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
    val BEACON_NAME_UUID: UUID = UUID.fromString("9f1b0003-5c3a-4e8e-9c2a-1a2b3c4d5e6f")
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

    // The dongle's advertised name (CONFIG_BT_DEVICE_NAME in firmware).
    const val DONGLE_NAME = "BLE_Password"

    // ---- Defaults (match firmware compiled defaults) ----
    const val DEFAULT_BEACON_NAME = "Ray's 21 Ultra"
    const val DEFAULT_PASSWORD = "12345678"
    const val DEFAULT_GREEN_THRESHOLD = -71
    const val DEFAULT_BLUE_THRESHOLD = -100

    // Name the phone advertises when the user hasn't set a beacon name yet.
    const val FALLBACK_BEACON_NAME = "Default"

    // ---- Firmware limits (match firmware) ----
    const val PASSWORD_MAX_LEN = 64
    const val BEACON_NAME_MAX_LEN = 31

    private const val PREFS = "ble_password_cfg"
    private const val KEY_BEACON = "beacon_name"
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

    fun getBeaconName(ctx: Context): String =
        prefs(ctx).getString(KEY_BEACON, "") ?: ""

    /**
     * Name to actually advertise. Falls back to [FALLBACK_BEACON_NAME] when the
     * user hasn't set one, so the phone never broadcasts a blank name (which
     * would also wipe the phone's global Bluetooth name).
     */
    fun getBeaconNameForAdvertising(ctx: Context): String =
        getBeaconName(ctx).ifBlank { FALLBACK_BEACON_NAME }

    /** Threshold as text for the UI: "" when not previously set. */
    fun getGreenThresholdText(ctx: Context): String =
        if (prefs(ctx).contains(KEY_GREEN)) prefs(ctx).getInt(KEY_GREEN, 0).toString() else ""

    fun getBlueThresholdText(ctx: Context): String =
        if (prefs(ctx).contains(KEY_BLUE)) prefs(ctx).getInt(KEY_BLUE, 0).toString() else ""

    /** Persist the non-secret settings (the beacon needs them to advertise). */
    fun save(ctx: Context, beaconName: String, green: Int, blue: Int) {
        prefs(ctx).edit()
            .putString(KEY_BEACON, beaconName)
            .putInt(KEY_GREEN, green)
            .putInt(KEY_BLUE, blue)
            .apply()
    }
}
