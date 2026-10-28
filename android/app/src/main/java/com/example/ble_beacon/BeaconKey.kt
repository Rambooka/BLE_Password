package com.example.ble_beacon

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.core.content.edit
import java.security.KeyStore
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * This phone's rolling-beacon key (16 random bytes), made on first use.
 *
 * Unlike the passwords it has to live on the phone — the beacon is signed with
 * it all day — so it is stored encrypted with an AES-GCM key held in the
 * Android Keystore (hardware-backed where available), never in plain text.
 * The raw key is kept recoverable (not a non-exportable Keystore HMAC key) so
 * the same phone can be paired with several dongles, e.g. home and work.
 */
object BeaconKey {
    private const val PREFS = "ble_password_key"
    private const val KEY_WRAPPED = "beacon_key_wrapped"   // base64(iv | ciphertext)
    private const val WRAP_ALIAS = "ble_password_beacon_wrap"
    private const val GCM_TAG_BITS = 128

    @Volatile private var cached: ByteArray? = null

    /** The key, creating and storing a new one on first use. */
    @Synchronized
    fun get(ctx: Context): ByteArray {
        cached?.let { return it }
        val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val stored = prefs.getString(KEY_WRAPPED, null)
        val key = stored?.let { runCatching { unwrap(it) }.getOrNull() }
            ?: ByteArray(BeaconCrypto.KEY_LEN).also {
                // No key yet (or the Keystore entry is gone): make a new one.
                // Any dongle paired with the old key must be paired again.
                SecureRandom().nextBytes(it)
                prefs.edit { putString(KEY_WRAPPED, wrap(it)) }
            }
        cached = key
        return key
    }

    /** Short hex fingerprint shown in the app and in the dongle's status dump. */
    fun idHex(ctx: Context): String = BeaconCrypto.hex(BeaconCrypto.keyId(get(ctx)))

    private fun wrapKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(WRAP_ALIAS, null) as? SecretKey)?.let { return it }
        val gen = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        gen.init(
            KeyGenParameterSpec.Builder(
                WRAP_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build()
        )
        return gen.generateKey()
    }

    private fun wrap(raw: ByteArray): String {
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, wrapKey())
        return Base64.encodeToString(cipher.iv + cipher.doFinal(raw), Base64.NO_WRAP)
    }

    private fun unwrap(stored: String): ByteArray {
        val blob = Base64.decode(stored, Base64.NO_WRAP)
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(
            Cipher.DECRYPT_MODE, wrapKey(),
            GCMParameterSpec(GCM_TAG_BITS, blob, 0, 12),
        )
        return cipher.doFinal(blob, 12, blob.size - 12)
    }
}
