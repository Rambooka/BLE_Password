package com.example.ble_beacon

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.biometric.BiometricManager
import androidx.biometric.BiometricPrompt
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.fragment.app.FragmentActivity
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.DataInputStream
import java.io.DataOutputStream
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * The owner's credentials (password, and the optional Username 2 / Password 2),
 * stored on the phone encrypted under a BIOMETRIC-BOUND Android Keystore key.
 *
 * Why store them at all: the dongle keeps secrets in RAM only (fw 1.3.0+), so it
 * forgets them on every power-cycle and the phone must re-send them each time it
 * is plugged in. Retyping the password every boot would be unworkable, so the
 * phone holds it — but only sealed behind the user's fingerprint.
 *
 * The wrapping key is created with [KeyGenParameterSpec.Builder.setUserAuthenticationRequired]
 * and PER-OPERATION auth (a fresh biometric for every encrypt/decrypt via a
 * [BiometricPrompt.CryptoObject]), and [setInvalidatedByBiometricEnrollment], so:
 *   - at rest the ciphertext is useless without the hardware key, which even
 *     root can't export;
 *   - the key can't be used without a fresh fingerprint (no time window a
 *     background process could ride);
 *   - enrolling a new fingerprint destroys the key (and the stored secrets).
 * Root can still read the plaintext from app memory at the instant it is sent
 * (that is inherent to root); biometric protects it at rest and gates its use.
 */
object CredentialStore {
    data class Creds(val password: String, val username2: String, val password2: String)

    private const val PREFS = "ble_password_creds"
    private const val KEY_BLOB = "creds_blob"          // base64(iv(12) | ciphertext+tag)
    private const val ALIAS = "ble_password_creds_key"
    private const val GCM_TAG_BITS = 128
    private const val IV_LEN = 12

    private fun prefs(ctx: Context) = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** True once a credential set has been saved on this phone. */
    fun hasCredentials(ctx: Context): Boolean = prefs(ctx).contains(KEY_BLOB)

    /** Forget the saved credentials (and the Keystore key that sealed them). */
    fun clear(ctx: Context) {
        prefs(ctx).edit { remove(KEY_BLOB) }
        runCatching {
            KeyStore.getInstance("AndroidKeyStore").apply { load(null) }.deleteEntry(ALIAS)
        }
    }

    /** Whether the device can do a strong biometric auth, with a human-readable reason. */
    fun biometricStatus(ctx: Context): String? {
        return when (BiometricManager.from(ctx)
            .canAuthenticate(BiometricManager.Authenticators.BIOMETRIC_STRONG)) {
            BiometricManager.BIOMETRIC_SUCCESS -> null
            BiometricManager.BIOMETRIC_ERROR_NONE_ENROLLED ->
                "No fingerprint enrolled. Add one in Settings to store passwords securely."
            BiometricManager.BIOMETRIC_ERROR_NO_HARDWARE,
            BiometricManager.BIOMETRIC_ERROR_HW_UNAVAILABLE ->
                "This phone has no usable fingerprint sensor."
            else -> "Biometric authentication isn't available on this phone."
        }
    }

    /**
     * Encrypt and store [creds], gated by a biometric prompt. [onResult] is called
     * on the main thread with success or a message.
     */
    fun save(
        activity: FragmentActivity,
        creds: Creds,
        onResult: (ok: Boolean, err: String?) -> Unit,
    ) {
        biometricStatus(activity)?.let { onResult(false, it); return }

        val cipher = Cipher.getInstance(TRANSFORM)
        try {
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey(recreate = false))
        } catch (_: KeyPermanentlyInvalidatedException) {
            // Biometrics changed since the key was made: make a fresh key.
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey(recreate = true))
        }
        val iv = cipher.iv

        authenticate(
            activity, cipher,
            title = "Save password",
            subtitle = "Confirm your fingerprint to store it on this phone",
            onError = { onResult(false, it) },
            onSuccess = { authed ->
                runCatching {
                    val ct = authed.doFinal(serialize(creds))
                    prefs(activity).edit {
                        putString(KEY_BLOB, Base64.encodeToString(iv + ct, Base64.NO_WRAP))
                    }
                }.fold(
                    onSuccess = { onResult(true, null) },
                    onFailure = { onResult(false, "Couldn't store the password (${it.message}).") },
                )
            },
        )
    }

    /**
     * Decrypt the stored credentials, gated by a biometric prompt. [onResult] gets
     * the credentials on success, or null plus a message.
     */
    fun load(
        activity: FragmentActivity,
        onResult: (creds: Creds?, err: String?) -> Unit,
    ) {
        val stored = prefs(activity).getString(KEY_BLOB, null)
        if (stored == null) {
            onResult(null, "No password saved on this phone yet. Enter one above.")
            return
        }
        biometricStatus(activity)?.let { onResult(null, it); return }

        val blob = Base64.decode(stored, Base64.NO_WRAP)
        val key = existingKey()
        if (key == null || blob.size <= IV_LEN) {
            clear(activity)
            onResult(null, "Saved password is unreadable. Please enter it again.")
            return
        }
        val cipher = Cipher.getInstance(TRANSFORM)
        try {
            cipher.init(Cipher.DECRYPT_MODE, key, GCMParameterSpec(GCM_TAG_BITS, blob, 0, IV_LEN))
        } catch (_: KeyPermanentlyInvalidatedException) {
            // A new fingerprint was enrolled: the old secret can't be recovered.
            clear(activity)
            onResult(null, "Fingerprints changed, so the saved password was cleared. Enter it again.")
            return
        }

        authenticate(
            activity, cipher,
            title = "Send password",
            subtitle = "Confirm your fingerprint to send it to the dongle",
            onError = { onResult(null, it) },
            onSuccess = { authed ->
                runCatching { deserialize(authed.doFinal(blob, IV_LEN, blob.size - IV_LEN)) }.fold(
                    onSuccess = { onResult(it, null) },
                    onFailure = { onResult(null, "Couldn't read the saved password (${it.message}).") },
                )
            },
        )
    }

    // ---- internals ----

    private const val TRANSFORM = "AES/GCM/NoPadding"

    private fun authenticate(
        activity: FragmentActivity,
        cipher: Cipher,
        title: String,
        subtitle: String,
        onError: (String) -> Unit,
        onSuccess: (Cipher) -> Unit,
    ) {
        val prompt = BiometricPrompt(
            activity,
            ContextCompat.getMainExecutor(activity),
            object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationError(code: Int, msg: CharSequence) {
                    onError("Fingerprint cancelled ($msg).")
                }

                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                    val c = result.cryptoObject?.cipher
                    if (c == null) onError("Biometric session had no cipher.") else onSuccess(c)
                }
            },
        )
        val info = BiometricPrompt.PromptInfo.Builder()
            .setTitle(title)
            .setSubtitle(subtitle)
            .setAllowedAuthenticators(BiometricManager.Authenticators.BIOMETRIC_STRONG)
            .setNegativeButtonText("Cancel")
            .build()
        prompt.authenticate(info, BiometricPrompt.CryptoObject(cipher))
    }

    private fun existingKey(): SecretKey? {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        return ks.getKey(ALIAS, null) as? SecretKey
    }

    private fun getOrCreateKey(recreate: Boolean): SecretKey {
        if (!recreate) existingKey()?.let { return it }
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        if (ks.containsAlias(ALIAS)) ks.deleteEntry(ALIAS)

        val gen = javax.crypto.KeyGenerator.getInstance(
            KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore"
        )
        val spec = KeyGenParameterSpec.Builder(
            ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .setUserAuthenticationRequired(true)
            .setInvalidatedByBiometricEnrollment(true)
            // Per-operation auth: a fresh fingerprint for every use (minSdk 30).
            .setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG)
            .build()
        gen.init(spec)
        return gen.generateKey()
    }

    private fun serialize(c: Creds): ByteArray {
        val bos = ByteArrayOutputStream()
        DataOutputStream(bos).use { out ->
            for (s in listOf(c.password, c.username2, c.password2)) {
                val b = s.toByteArray(Charsets.UTF_8)
                out.writeShort(b.size)
                out.write(b)
            }
        }
        return bos.toByteArray()
    }

    private fun deserialize(data: ByteArray): Creds {
        DataInputStream(ByteArrayInputStream(data)).use { ins ->
            fun next(): String {
                val n = ins.readUnsignedShort()
                val b = ByteArray(n)
                ins.readFully(b)
                return String(b, Charsets.UTF_8)
            }
            return Creds(next(), next(), next())
        }
    }
}
