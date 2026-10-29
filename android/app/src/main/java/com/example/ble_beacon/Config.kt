package com.example.ble_beacon

import android.content.Context
import androidx.core.content.edit
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
    // Read: the dongle's persistent identity public key (fw 1.4.0+). The app pins
    // it at first pairing and verifies it every session (authenticates the dongle).
    val IDENTITY_UUID: UUID = UUID.fromString("9f1b000d-5c3a-4e8e-9c2a-1a2b3c4d5e6f")

    // The dongle's advertised name (CONFIG_BT_DEVICE_NAME in firmware).
    const val DONGLE_NAME = "BLE_Password"

    // ---- Firmware limits (match firmware) ----
    const val PASSWORD_MAX_LEN = 64

    // Credentials are NEVER stored on the phone: they only live in the UI until
    // they've been sent (sealed) to the dongle. Only the thresholds are saved.
    private const val PREFS = "ble_password_cfg"
    private const val KEY_GREEN = "green_threshold"
    private const val KEY_BLUE = "blue_threshold"
    private const val KEY_BEACON_ON = "beacon_enabled"
    private const val KEY_MOTION_GATE = "motion_gate_enabled"
    // Trusted dongle identity public keys (hex), pinned at first pairing so the
    // app can recognise its own dongle(s) and refuse an impostor (fw 1.4.0+).
    private const val KEY_TRUSTED_IDS = "trusted_dongle_ids"

    private fun prefs(ctx: Context) =
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /**
     * Pause the beacon when the phone hasn't moved for a while (anti-relay, like
     * a car key fob). A persisted toggle so it can be changed without a new build.
     * Default OFF: a phone sitting still next to the PC would otherwise lock it.
     */
    fun isMotionGateEnabled(ctx: Context): Boolean =
        prefs(ctx).getBoolean(KEY_MOTION_GATE, false)

    fun setMotionGateEnabled(ctx: Context, on: Boolean) {
        prefs(ctx).edit { putBoolean(KEY_MOTION_GATE, on) }
    }

    /** Is this dongle identity (hex public key) one we've paired with before? */
    fun isTrustedIdentity(ctx: Context, idHex: String): Boolean =
        prefs(ctx).getStringSet(KEY_TRUSTED_IDS, emptySet())?.contains(idHex) == true

    /** Pin a dongle identity as trusted (trust-on-first-use, button held). */
    fun addTrustedIdentity(ctx: Context, idHex: String) {
        val cur = prefs(ctx).getStringSet(KEY_TRUSTED_IDS, emptySet()) ?: emptySet()
        prefs(ctx).edit { putStringSet(KEY_TRUSTED_IDS, cur + idHex) }
    }

    // When nothing has been saved yet, the getters return EMPTY strings so the
    // UI fields start blank (the user fills them in), rather than pre-filling the
    // firmware defaults.

    /** Threshold as text for the UI: "" when not previously set. */
    fun getGreenThresholdText(ctx: Context): String =
        if (prefs(ctx).contains(KEY_GREEN)) prefs(ctx).getInt(KEY_GREEN, 0).toString() else ""

    fun getBlueThresholdText(ctx: Context): String =
        if (prefs(ctx).contains(KEY_BLUE)) prefs(ctx).getInt(KEY_BLUE, 0).toString() else ""

    /** Whether the user wants the beacon on (restored after a reboot / update). */
    fun isBeaconEnabled(ctx: Context): Boolean = prefs(ctx).getBoolean(KEY_BEACON_ON, false)

    fun setBeaconEnabled(ctx: Context, on: Boolean) {
        prefs(ctx).edit { putBoolean(KEY_BEACON_ON, on) }
    }

    /** Persist the non-secret settings (so the form shows them next time). */
    fun save(ctx: Context, green: Int, blue: Int) {
        prefs(ctx).edit {
            putInt(KEY_GREEN, green)
            putInt(KEY_BLUE, blue)
        }
    }
}
