package com.example.ble_beacon

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.math.BigInteger
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.interfaces.ECPublicKey
import java.security.spec.ECGenParameterSpec
import java.security.spec.ECPoint
import java.security.spec.ECPublicKeySpec
import javax.crypto.KeyAgreement

/**
 * Same vectors as the firmware's host test (tests/host/cfg_crypto_test.c),
 * computed with Python's hmac module; plus a real ECDH round trip.
 */
class ConfigSessionTest {
    private fun unhex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    private val z = ByteArray(32) { 0x11 }
    private val dp = byteArrayOf(4) + ByteArray(64) { 0x22 }
    private val pp = byteArrayOf(4) + ByteArray(64) { 0x33 }

    @Test
    fun hkdfMatchesFirmwareVector() {
        val (kEnc, kMac) = ConfigSession.derive(z, dp, pp)
        assertArrayEquals(unhex("f1ff356b00d96635697c5d651a968295ec6701cd24cf6d8c9b9d1097b326f70c"), kEnc)
        assertArrayEquals(unhex("706e5fd2260f0bd13613acd40411c82c65dc7dec26a3ecdbaa5bffb9b5407df7"), kMac)
    }

    @Test
    fun sealMatchesFirmwareVectors() {
        val (kEnc, kMac) = ConfigSession.derive(z, dp, pp)
        assertArrayEquals(
            unhex("0100000021762fa163c2ab8f77e7e7f07a1c892c14a9396541cbf9121546"),
            ConfigSession.seal(kEnc, kMac, 0x02, 1, "Secret-pw1".toByteArray()),
        )
        assertArrayEquals(
            unhex(
                "07000000cd80d3ec0c280ca8748d939261bfa9b4835c4cc98a4c88053026b10ccf515daf" +
                    "33f2ecd4b129465c0e3963c499b3b7627569f0dd49b310c4"
            ),
            ConfigSession.seal(kEnc, kMac, 0x0A, 7, ByteArray(40) { it.toByte() }),
        )
    }

    @Test
    fun ecdhAgreesWithAStandInDongle() {
        // Play the dongle: its own P-256 key pair, uncompressed public point.
        val kpg = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }
        val dongle = kpg.generateKeyPair()
        val w = (dongle.public as ECPublicKey).w
        fun fixed(v: BigInteger) = v.toByteArray().let { b ->
            if (b.size > 32) b.copyOfRange(b.size - 32, b.size) else ByteArray(32 - b.size) + b
        }
        val donglePub = byteArrayOf(4) + fixed(w.affineX) + fixed(w.affineY)

        val session = ConfigSession.start(donglePub)
        assertEquals(ConfigSession.PUB_LEN, session.phonePublicKey.size)

        // Dongle side: ECDH with the phone's key, same KDF, then check a seal.
        val pk = session.phonePublicKey
        val phonePub = KeyFactory.getInstance("EC").generatePublic(
            ECPublicKeySpec(
                ECPoint(BigInteger(1, pk.copyOfRange(1, 33)), BigInteger(1, pk.copyOfRange(33, 65))),
                (dongle.public as ECPublicKey).params,
            )
        )
        val zDongle = KeyAgreement.getInstance("ECDH").run {
            init(dongle.private); doPhase(phonePub, true); generateSecret()
        }
        val (kEnc, kMac) = ConfigSession.derive(zDongle, donglePub, pk)
        val msg = "hunter2".toByteArray()
        assertArrayEquals(
            ConfigSession.seal(kEnc, kMac, ConfigSession.ID_PASSWORD, 1, msg),
            session.seal(ConfigSession.ID_PASSWORD, msg),
        )
    }
}
