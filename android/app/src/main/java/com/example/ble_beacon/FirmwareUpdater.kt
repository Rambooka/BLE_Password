package com.example.ble_beacon

import android.bluetooth.BluetoothDevice
import android.content.Context
import android.util.Log
import no.nordicsemi.android.mcumgr.ble.McuMgrBleTransport
import no.nordicsemi.android.mcumgr.dfu.FirmwareUpgradeCallback
import no.nordicsemi.android.mcumgr.dfu.FirmwareUpgradeController
import no.nordicsemi.android.mcumgr.dfu.mcuboot.FirmwareUpgradeManager
import no.nordicsemi.android.mcumgr.dfu.mcuboot.FirmwareUpgradeManager.State
import no.nordicsemi.android.mcumgr.exception.McuMgrException

/**
 * Uploads a signed MCUboot image to an (already armed, see
 * [ConfigClient.armDfu]) dongle over MCUmgr/SMP, marks it for test and resets
 * it. The new firmware confirms itself once it boots; if it doesn't, MCUboot
 * rolls back to the previous image on the next reset.
 *
 * [listener] runs on the main thread. `progress` is 0..1 during the upload,
 * null otherwise.
 */
class FirmwareUpdater(
    context: Context,
    device: BluetoothDevice,
    private val listener: (status: String, progress: Float?, done: Boolean, success: Boolean) -> Unit,
) {
    private val transport = McuMgrBleTransport(context, device)

    // The manager skips the upload (and still reports success) when the dongle
    // already runs this exact image, so remember whether bytes were sent.
    private var uploaded = false
    private var version = ""

    private val manager = FirmwareUpgradeManager(transport, object : FirmwareUpgradeCallback<State> {
        override fun onUpgradeStarted(controller: FirmwareUpgradeController) {
            listener("Connecting for upload…", null, false, false)
        }

        override fun onStateChanged(prevState: State, newState: State) {
            Log.i(TAG, "State $prevState -> $newState")
            if (newState == State.UPLOAD) uploaded = true
            when (newState) {
                State.VALIDATE -> listener("Checking current firmware…", null, false, false)
                State.UPLOAD -> listener("Uploading…", 0f, false, false)
                State.TEST, State.RESET -> listener("Restarting dongle…", null, false, false)
                else -> {}
            }
        }

        override fun onUploadProgressChanged(bytesSent: Int, imageSize: Int, timestamp: Long) {
            val pct = if (imageSize > 0) bytesSent.toFloat() / imageSize else 0f
            listener("Uploading… ${(pct * 100).toInt()}%", pct, false, false)
        }

        override fun onUpgradeCompleted() {
            Log.i(TAG, "Completed (uploaded=$uploaded)")
            transport.release()
            if (!uploaded) {
                listener(
                    "The dongle already has this firmware (v$version) — nothing to update.",
                    null, true, true,
                )
                return
            }
            listener(
                "Update sent. The dongle is installing it — wait ~30 s before " +
                    "unplugging. It boots into the new firmware's config window.",
                null, true, true,
            )
        }

        override fun onUpgradeFailed(state: State, error: McuMgrException) {
            Log.w(TAG, "Failed during $state", error)
            transport.release()
            listener("Update failed during $state: ${error.message}", null, true, false)
        }

        override fun onUpgradeCanceled(state: State) {
            Log.i(TAG, "Cancelled during $state")
            transport.release()
            listener("Update cancelled.", null, true, false)
        }
    }).apply {
        // Firmware auto-confirms after a good boot (main.c), so test-only is
        // enough and keeps MCUboot's automatic rollback.
        setMode(FirmwareUpgradeManager.Mode.TEST_ONLY)
    }

    fun start(firmware: FirmwareImage) {
        version = firmware.version
        val settings = FirmwareUpgradeManager.Settings.Builder()
            .setEstimatedSwapTime(30_000)
            .setWindowCapacity(3)
            .setMemoryAlignment(4)
            .build()
        try {
            manager.start(firmware.image, settings)
        } catch (e: McuMgrException) {
            transport.release()
            listener("Invalid firmware image: ${e.message}", null, true, false)
        }
    }

    private companion object {
        const val TAG = "FirmwareUpdater"
    }
}
