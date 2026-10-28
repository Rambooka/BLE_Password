package com.example.ble_beacon

import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/**
 * Rolling-beacon format shared with the dongle firmware (src/beacon_auth.h).
 *
 * Manufacturer data, company ID 0xFFFF:  'B' 'P' 01 ctr[4, LE] tag[8]
 * where tag = HMAC-SHA256(key, 'B' 'P' 01 ctr)[0..8]. Each phone has its own
 * random key, so a dongle ignores every other phone's beacon, and a recording
 * is useless because the dongle only accepts newer counters.
 *
 * Pure functions (no Android APIs) so they run in a JVM unit test.
 */
object BeaconCrypto {
    const val COMPANY_ID = 0xFFFF
    const val KEY_LEN = 16
    private const val MAGIC0: Byte = 0x42 // 'B'
    private const val MAGIC1: Byte = 0x50 // 'P'
    private const val VERSION: Byte = 0x01
    private const val TAG_LEN = 8
    private const val KEY_ID_LEN = 4

    // The dongle's own config-window advert: 'B' 'P' 0x10 <flags>, bit 0 =
    // its button is held (so we connect to the right dongle in an office).
    private const val CONFIG_ADV_TYPE: Byte = 0x10
    private const val CONFIG_ADV_HELD = 0x01

    /** Manufacturer-data payload (after the company ID) for counter [ctr]. */
    fun beaconPayload(key: ByteArray, ctr: Long): ByteArray {
        val signed = byteArrayOf(
            MAGIC0, MAGIC1, VERSION,
            ctr.toByte(), (ctr shr 8).toByte(), (ctr shr 16).toByte(), (ctr shr 24).toByte(),
        )
        return signed + hmac(key, signed).copyOf(TAG_LEN)
    }

    /** Non-secret key fingerprint; the dongle reports the same for its key. */
    fun keyId(key: ByteArray): ByteArray =
        hmac(key, "BLEPW-KEYID".toByteArray(Charsets.US_ASCII)).copyOf(KEY_ID_LEN)

    fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it) }

    /**
     * Parse a dongle's config-window manufacturer data (company ID 0xFFFF).
     * Returns whether its button is held, or null if it isn't that advert
     * (older firmware has none).
     */
    fun dongleButtonHeld(mfg: ByteArray?): Boolean? {
        if (mfg == null || mfg.size != 4) return null
        if (mfg[0] != MAGIC0 || mfg[1] != MAGIC1 || mfg[2] != CONFIG_ADV_TYPE) return null
        return (mfg[3].toInt() and CONFIG_ADV_HELD) != 0
    }

    private fun hmac(key: ByteArray, msg: ByteArray): ByteArray =
        Mac.getInstance("HmacSHA256").run {
            init(SecretKeySpec(key, "HmacSHA256"))
            doFinal(msg)
        }
}
