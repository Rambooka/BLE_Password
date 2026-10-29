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

    private val z1 = ByteArray(32) { 0x11 }
    private val z2 = ByteArray(32) { 0x44 }
    private val ip = byteArrayOf(4) + ByteArray(64) { 0x55 }
    private val dp = byteArrayOf(4) + ByteArray(64) { 0x22 }
    private val pp = byteArrayOf(4) + ByteArray(64) { 0x33 }

    @Test
    fun hkdfMatchesFirmwareVector() {
        val (kEnc, kMac) = ConfigSession.derive(z1, z2, ip, dp, pp)
        assertArrayEquals(unhex("6ab9ce9cbad04d216affe112127e29e38173f1c067a7cdd9e260606555655280"), kEnc)
        assertArrayEquals(unhex("ab7586a4b7d4ec34eb8c2adf87edbd36c4c66602f07ad9425de96e444f06ce4e"), kMac)
    }

    @Test
    fun sealMatchesFirmwareVectors() {
        val (kEnc, kMac) = ConfigSession.derive(z1, z2, ip, dp, pp)
        assertArrayEquals(
            unhex("010000003f9611d1dcf0b6cf524e0a527e78047c8f0d4dfdb3d2f1c84f19"),
            ConfigSession.seal(kEnc, kMac, 0x02, 1, "Secret-pw1".toByteArray()),
        )
        assertArrayEquals(
            unhex(
                "07000000e3713fe194218679393ce494e9150f088a4b3da0298fbdcf69eb714e6b1ffe02" +
                    "7ea53c3e90a9911e1512dea12d6bd6028ec9378d8083a8d7"
            ),
            ConfigSession.seal(kEnc, kMac, 0x0A, 7, ByteArray(40) { it.toByte() }),
        )
    }

    @Test
    fun ecdhAgreesWithAStandInDongle() {
        val kpg = KeyPairGenerator.getInstance("EC").apply { initialize(ECGenParameterSpec("secp256r1")) }
        fun fixed(v: BigInteger) = v.toByteArray().let { b ->
            if (b.size > 32) b.copyOfRange(b.size - 32, b.size) else ByteArray(32 - b.size) + b
        }
        fun pubBytes(kp: java.security.KeyPair): ByteArray {
            val w = (kp.public as ECPublicKey).w
            return byteArrayOf(4) + fixed(w.affineX) + fixed(w.affineY)
        }

        // Play the dongle: an ephemeral (per-window) key AND a persistent identity key.
        val ephemeral = kpg.generateKeyPair()
        val identity = kpg.generateKeyPair()
        val ephemeralPub = pubBytes(ephemeral)
        val identityPub = pubBytes(identity)

        val session = ConfigSession.start(ephemeralPub, identityPub)
        assertEquals(ConfigSession.PUB_LEN, session.phonePublicKey.size)

        // Dongle side: ECDH the phone's key against both its keys, same KDF, check a seal.
        val pk = session.phonePublicKey
        val phonePub = KeyFactory.getInstance("EC").generatePublic(
            ECPublicKeySpec(
                ECPoint(BigInteger(1, pk.copyOfRange(1, 33)), BigInteger(1, pk.copyOfRange(33, 65))),
                (ephemeral.public as ECPublicKey).params,
            )
        )
        fun ecdh(priv: java.security.PrivateKey) = KeyAgreement.getInstance("ECDH").run {
            init(priv); doPhase(phonePub, true); generateSecret()
        }
        val (kEnc, kMac) = ConfigSession.derive(ecdh(ephemeral.private), ecdh(identity.private), identityPub, ephemeralPub, pk)
        val msg = "hunter2".toByteArray()
        assertArrayEquals(
            ConfigSession.seal(kEnc, kMac, ConfigSession.ID_PASSWORD, 1, msg),
            session.seal(ConfigSession.ID_PASSWORD, msg),
        )
    }
}
