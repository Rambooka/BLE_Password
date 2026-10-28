package com.example.ble_beacon

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * Same vectors as the firmware's host test (tests/host/beacon_auth_test.c),
 * computed independently with Python's hmac module: if both pass, the phone
 * and the dongle agree on the beacon format.
 */
class BeaconCryptoTest {
    private val key = ByteArray(16) { it.toByte() }

    private fun unhex(s: String) = ByteArray(s.length / 2) { s.substring(2 * it, 2 * it + 2).toInt(16).toByte() }

    @Test
    fun beaconPayloadMatchesFirmwareVector() {
        // Full manufacturer field is ffff + payload.
        assertArrayEquals(
            unhex("4250012d1c3b6a009ddddfe4f67fd5"),
            BeaconCrypto.beaconPayload(key, 0x6A3B1C2DL),
        )
    }

    @Test
    fun keyIdMatchesFirmwareVector() {
        assertEquals("2fcd11b7", BeaconCrypto.hex(BeaconCrypto.keyId(key)))
    }

    @Test
    fun counterIsLittleEndianAndTruncatedTo32Bits() {
        val p = BeaconCrypto.beaconPayload(key, 0x1_0000_0001L)
        assertArrayEquals(byteArrayOf(1, 0, 0, 0), p.copyOfRange(3, 7))
    }

    @Test
    fun dongleConfigAdvert() {
        assertEquals(true, BeaconCrypto.dongleButtonHeld(unhex("42501001")))
        assertEquals(false, BeaconCrypto.dongleButtonHeld(unhex("42501000")))
        assertNull(BeaconCrypto.dongleButtonHeld(unhex("4250010000"))) // a phone beacon
        assertNull(BeaconCrypto.dongleButtonHeld(null))                // old firmware
    }
}
