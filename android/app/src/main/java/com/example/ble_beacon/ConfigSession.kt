package com.example.ble_beacon

import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPair
import java.security.KeyPairGenerator
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.KeyAgreement
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Encrypted config session with the dongle — must match src/cfg_crypto.h.
 *
 * Per connection: P-256 ECDH (fresh phone key pair, the dongle's key from the
 * Session characteristic) → HKDF-SHA256 → k_enc / k_mac. Secret writes are
 * sealed as  seq[4 LE] || ct || tag[16]  with an HMAC keystream and an HMAC
 * tag, bound to the characteristic's short ID. Stops passive sniffing of the
 * passwords and the beacon key; it's unauthenticated ECDH, so not an active
 * man-in-the-middle during the config window.
 *
 * Plain JCA, so it runs in a JVM unit test.
 */
class ConfigSession private constructor(
    val phonePublicKey: ByteArray,
    private val kEnc: ByteArray,
    private val kMac: ByteArray,
) {
    private var seq = 0L

    /** Seal [plaintext] for the characteristic with short ID [id]. */
    fun seal(id: Int, plaintext: ByteArray): ByteArray = seal(kEnc, kMac, id, ++seq, plaintext)

    companion object {
        const val PUB_LEN = 65
        private val SALT = "BLEPW-SESSION-v1".toByteArray(Charsets.US_ASCII)
        private const val TAG_LEN = 16

        // Short IDs (low byte of each characteristic's UUID; see firmware).
        const val ID_PASSWORD = 0x02
        const val ID_PASSWORD2 = 0x08
        const val ID_USERNAME2 = 0x09
        const val ID_BEACON_KEY = 0x0A

        /** Run ECDH against the dongle's public key and derive the session. */
        fun start(donglePublicKey: ByteArray): ConfigSession {
            require(donglePublicKey.size == PUB_LEN && donglePublicKey[0] == 0x04.toByte()) {
                "bad dongle public key"
            }
            val kpg = KeyPairGenerator.getInstance("EC")
            kpg.initialize(ECGenParameterSpec("secp256r1"))
            val pair: KeyPair = kpg.generateKeyPair()
            val phonePub = encodePoint(pair.public as ECPublicKey)

            val params = (pair.public as ECPublicKey).params
            val peer = KeyFactory.getInstance("EC").generatePublic(
                ECPublicKeySpec(
                    ECPoint(
                        BigInteger(1, donglePublicKey.copyOfRange(1, 33)),
                        BigInteger(1, donglePublicKey.copyOfRange(33, 65)),
                    ),
                    params,
                )
            )
            val ka = KeyAgreement.getInstance("ECDH")
            ka.init(pair.private)
            ka.doPhase(peer, true)
            val z = ka.generateSecret()   // 32-byte x-coordinate
            val (kEnc, kMac) = derive(z, donglePublicKey, phonePub)
            z.fill(0)
            return ConfigSession(phonePub, kEnc, kMac)
        }

        /** HKDF-SHA256(salt, Z, info = dongle_pub || phone_pub), L = 64. */
        fun derive(z: ByteArray, donglePub: ByteArray, phonePub: ByteArray): Pair<ByteArray, ByteArray> {
            val prk = hmac(SALT, z)
            val info = donglePub + phonePub
            val t1 = hmac(prk, info + byteArrayOf(1))
            val t2 = hmac(prk, t1 + info + byteArrayOf(2))
            return t1 to t2
        }

        fun seal(kEnc: ByteArray, kMac: ByteArray, id: Int, seq: Long, pt: ByteArray): ByteArray {
            val seqLe = byteArrayOf(seq.toByte(), (seq shr 8).toByte(), (seq shr 16).toByte(), (seq shr 24).toByte())
            val ct = pt.copyOf()
            var block = 0
            var off = 0
            while (off < ct.size) {
                val ks = hmac(kEnc, byteArrayOf(id.toByte()) + seqLe + byteArrayOf(block.toByte()))
                for (i in ks.indices) {
                    if (off + i >= ct.size) break
                    ct[off + i] = (ct[off + i].toInt() xor ks[i].toInt()).toByte()
                }
                off += ks.size
                block++
            }
            val tag = hmac(kMac, byteArrayOf(id.toByte()) + seqLe + ct).copyOf(TAG_LEN)
            return seqLe + ct + tag
        }

        private fun encodePoint(pub: ECPublicKey): ByteArray =
            byteArrayOf(0x04) + fixed32(pub.w.affineX) + fixed32(pub.w.affineY)

        // BigInteger → exactly 32 big-endian bytes (strip sign byte / left-pad).
        private fun fixed32(v: BigInteger): ByteArray {
            val b = v.toByteArray()
            return when {
                b.size == 32 -> b
                b.size > 32 -> b.copyOfRange(b.size - 32, b.size)
                else -> ByteArray(32 - b.size) + b
            }
        }

        private fun hmac(key: ByteArray, msg: ByteArray): ByteArray =
            Mac.getInstance("HmacSHA256").run {
                init(SecretKeySpec(key, "HmacSHA256"))
                doFinal(msg)
            }
    }
}
