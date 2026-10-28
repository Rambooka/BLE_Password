package com.example.ble_beacon

import android.annotation.SuppressLint
import android.content.Context
import java.util.UUID

/**
 * Persistent configuration shared between the UI, the beacon advertiser and the
 * GATT config client. Values are stored in SharedPreferences so they survive an
 * app restart, and are pushed to the nRF52840 over BLE on demand.
 *
 * The UUIDs and defaults MUST match the dongle firmware (src/main.c).
 */
object Config {
    // ---- GATT config service / characteristic UUIDs (match firmware) ----
    val SERVICE_UUID: UUID = UUID.fromString("9f1b0001-5c3a-4e8e-9c2a-1a2b3c4d5e6f")
    val PASSWORD_UUID: UUID = UUID.fromString("9f1b0002-5c3a-4e8e-9c2a-1a2b3c4d5e6f")
    // (…0003 was the beacon name; replaced by the rolling-beacon key in fw 1.2.0.)
    val GREEN_THRESHOLD_UUID: UUID = UUID.fromString("9f1b0004-5c3a-4e8e-9c2a-1a2b3c4d5e6f")
    val BLUE_THRESHOLD_UUID: UUID = UUID.fromString("9f1b0005-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // Write 0x01 (button held, during the config window) → dongle unlocks
    // MCUmgr/SMP firmware upload for this phone.
    val DFU_ARM_UUID: UUID = UUID.fromString("9f1b0006-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // Read → the dongle firmware's Zephyr board name (e.g. "nrf52840dongle").
    val BOARD_UUID: UUID = UUID.fromString("9f1b0007-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // Password 2, typed on a ~1 s press (firmware 1.1.4+). Empty = clear it.
    val PASSWORD2_UUID: UUID = UUID.fromString("9f1b0008-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // Username typed before Password 2, then TAB (firmware 1.1.5+). Empty = clear it.
    val USERNAME2_UUID: UUID = UUID.fromString("9f1b0009-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // Write: this phone's 16-byte rolling-beacon key (fw 1.2.0+). Read KEY_ID
    // first and only write when the dongle doesn't already hold this key.
    val BEACON_KEY_UUID: UUID = UUID.fromString("9f1b000a-5c3a-4e8e-9c2a-1a2b3c4d5e6f")
    // Read: 4-byte fingerprint of the dongle's key (empty = not paired).
    val KEY_ID_UUID: UUID = UUID.fromString("9f1b000b-5c3a-4e8e-9c2a-1a2b3c4d5e6f")
    // Read: the dongle's ECDH public key; write: ours (see ConfigSession). The
    // passwords and the beacon key are only accepted sealed with that session.
    val SESSION_UUID: UUID = UUID.fromString("9f1b000c-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // The dongle's advertised name (CONFIG_BT_DEVICE_NAME in firmware).
    const val DONGLE_NAME = "BLE_Password"

    // ---- Defaults (match firmware compiled defaults) ----
    const val DEFAULT_PASSWORD = "12345678"
    const val DEFAULT_GREEN_THRESHOLD = -71
    const val DEFAULT_BLUE_THRESHOLD = -100

    // ---- Firmware limits (match firmware) ----
    const val PASSWORD_MAX_LEN = 64

    private const val PREFS = "ble_password_cfg"
    private const val KEY_GREEN = "green_threshold"
    private const val KEY_BLUE = "blue_threshold"

    // Credentials are NEVER stored on the phone: they only live in the UI until
    // they've been sent to the dongle. These keys are only here so purgeSecrets()
    // can delete what older app versions saved in plain text.
    private val LEGACY_SECRET_KEYS = listOf("password", "password2", "username2")

    private fun prefs(ctx: Context) =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Remove credentials saved by older app versions (call once at startup). */
    @SuppressLint("ApplySharedPref") // commit(): wipe the plaintext now, not in the background
    fun purgeSecrets(ctx: Context) {
        val p = prefs(ctx)
        if (LEGACY_SECRET_KEYS.any { p.contains(it) }) {
            p.edit().apply { LEGACY_SECRET_KEYS.forEach { remove(it) } }.commit()
        }
    }

    // When nothing has been saved yet, the getters return EMPTY strings so the
    // UI fields start blank (the user fills them in), rather than pre-filling the
    // firmware defaults.

    /** Threshold as text for the UI: "" when not previously set. */
    fun getGreenThresholdText(ctx: Context): String =
        if (prefs(ctx).contains(KEY_GREEN)) prefs(ctx).getInt(KEY_GREEN, 0).toString() else ""

    fun getBlueThresholdText(ctx: Context): String =
        if (prefs(ctx).contains(KEY_BLUE)) prefs(ctx).getInt(KEY_BLUE, 0).toString() else ""

    /** Persist the non-secret settings (so the form shows them next time). */
    fun save(ctx: Context, green: Int, blue: Int) {
        prefs(ctx).edit()
            .remove("beacon_name")   // pre-1.2 setting, no longer used
            .putInt(KEY_GREEN, green)
            .putInt(KEY_BLUE, blue)
            .apply()
    }
}
