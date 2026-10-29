package com.example.ble_beacon

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/**
 * Brings the beacon back after the phone reboots or the app is updated, if
 * the user had it switched on. Without this the dongle stops seeing the
 * phone until the app is opened again.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED ->
                if (Config.isBeaconEnabled(context) && BeaconService.hasPermissions(context)) {
                    BeaconService.start(context)
                }
        }
    }
}
