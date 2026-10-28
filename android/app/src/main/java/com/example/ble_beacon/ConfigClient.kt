package com.example.ble_beacon

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.BluetoothLeScanner
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.UUID

/**
 * One-shot BLE client that finds the dongle (by its config service UUID),
 * connects, and writes the password / beacon name / RSSI thresholds, then
 * disconnects. All writes are sequential (BLE allows one outstanding op).
 *
 * Progress is reported via [listener] on the main thread.
 */
@SuppressLint("MissingPermission") // caller guarantees BLUETOOTH_SCAN/CONNECT granted
class ConfigClient(
    private val context: Context,
    private val listener: (status: String, done: Boolean, success: Boolean) -> Unit,
) {
    private val tag = "ConfigClient"
    private val main = Handler(Looper.getMainLooper())

    private var scanner: BluetoothLeScanner? = null
    private var gatt: BluetoothGatt? = null
    private var device: BluetoothDevice? = null
    private var finished = false

    // Set by armDfu(): check the dongle's board, arm DFU, then hand the device
    // to [onDfuArmed] for the SMP upload.
    private var dfuBoard: String? = null
    private var onDfuArmed: ((BluetoothDevice) -> Unit)? = null

    // Queue of (characteristic UUID -> value) to write in order. An [optional]
    // item is skipped when the dongle's firmware doesn't have that characteristic.
    private data class WriteItem(
        val uuid: UUID,
        val value: ByteArray,
        val optional: Boolean = false,
    )
    private val writeQueue = ArrayDeque<WriteItem>()

    private val scanTimeout = Runnable {
        if (gatt == null && !finished) {
            stopScan()
            finish("Dongle not found. Is it powered and in range?", false)
        }
    }

    // Covers the whole operation (connect / MTU / discovery / writes), not just
    // the scan — otherwise a stalled connection leaves the UI stuck forever.
    private val overallTimeout = Runnable {
        if (!finished) {
            finish(
                "Timed out talking to the dongle. Power-cycle it (settings are " +
                    "only accepted during its 1-minute config window) and try again.",
                false,
            )
        }
    }

    /**
     * Credentials left blank are NOT written, so the dongle keeps its current
     * ones (the phone doesn't store them). [clearLogin2] removes username 2 and
     * password 2 from the dongle instead.
     */
    fun send(
        password: String, username2: String, password2: String, clearLogin2: Boolean,
        beaconName: String, green: Int, blue: Int,
    ) {
        dfuBoard = null
        writeQueue.clear()
        if (password.isNotEmpty()) {
            writeQueue.add(WriteItem(Config.PASSWORD_UUID, password.toByteArray(Charsets.UTF_8)))
        }
        // Older firmware has no password 2 (<= 1.1.3) / username 2 (<= 1.1.4):
        // clearing is then a no-op (optional), but setting one must fail.
        if (clearLogin2) {
            writeQueue.add(WriteItem(Config.PASSWORD2_UUID, ByteArray(0), optional = true))
            writeQueue.add(WriteItem(Config.USERNAME2_UUID, ByteArray(0), optional = true))
        } else {
            if (password2.isNotEmpty()) {
                writeQueue.add(WriteItem(Config.PASSWORD2_UUID, password2.toByteArray(Charsets.UTF_8)))
            }
            if (username2.isNotEmpty()) {
                writeQueue.add(WriteItem(Config.USERNAME2_UUID, username2.toByteArray(Charsets.UTF_8)))
            }
        }
        writeQueue.add(WriteItem(Config.BEACON_NAME_UUID, beaconName.toByteArray(Charsets.UTF_8)))
        writeQueue.add(WriteItem(Config.GREEN_THRESHOLD_UUID, byteArrayOf(green.toByte())))
        writeQueue.add(WriteItem(Config.BLUE_THRESHOLD_UUID, byteArrayOf(blue.toByte())))
        beginScan()
    }

    /**
     * Connect to the dongle, check it is the [expectedBoard] PCB, and arm a
     * firmware update (unlocks SMP on the dongle for this phone). Like config
     * writes, the dongle only accepts it during its boot config window while
     * its button is physically held. On success the connection is closed and
     * [onArmed] gets the device so the SMP client can reconnect to it.
     */
    fun armDfu(expectedBoard: String, onArmed: (BluetoothDevice) -> Unit) {
        dfuBoard = expectedBoard
        onDfuArmed = onArmed
        writeQueue.clear()
        writeQueue.add(WriteItem(Config.DFU_ARM_UUID, byteArrayOf(0x01)))
        beginScan()
    }

    private fun beginScan() {
        val manager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val adapter = manager.adapter
        if (adapter == null || !adapter.isEnabled) {
            finish("Bluetooth is off.", false)
            return
        }
        scanner = adapter.bluetoothLeScanner
        if (scanner == null) {
            finish("BLE scanning not available.", false)
            return
        }

        report("Scanning for dongle…")
        // Scan WITHOUT a hardware filter and match in software: Samsung devices
        // are unreliable at offloading 128-bit service-UUID scan filters.
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        scanner?.startScan(null, settings, scanCallback)
        main.postDelayed(scanTimeout, 12_000)
        main.postDelayed(overallTimeout, 30_000)
    }

    private fun isDongle(result: ScanResult): Boolean {
        val rec = result.scanRecord
        val byUuid = rec?.serviceUuids?.any { it.uuid == Config.SERVICE_UUID } == true
        val byName = rec?.deviceName == Config.DONGLE_NAME
        return byUuid || byName
    }

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val device = result.device ?: return
            if (gatt != null || finished) return
            if (!isDongle(result)) return
            Log.i(tag, "Found dongle ${device.address}, connecting")
            stopScan()
            report("Connecting…")
            this@ConfigClient.device = device
            gatt = device.connectGatt(context, false, gattCallback)
        }

        override fun onScanFailed(errorCode: Int) {
            finish("BLE scan failed ($errorCode).", false)
        }
    }

    private fun stopScan() {
        main.removeCallbacks(scanTimeout)
        try {
            scanner?.stopScan(scanCallback)
        } catch (_: Exception) {
        }
        scanner = null
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            if (newState == BluetoothProfile.STATE_CONNECTED) {
                report("Connected, negotiating…")
                g.requestMtu(247)
            } else if (newState == BluetoothProfile.STATE_DISCONNECTED) {
                if (!finished) {
                    finish("Disconnected before finishing (status $status).", false)
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, mtu: Int, status: Int) {
            report("Discovering services…")
            g.discoverServices()
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            val service = g.getService(Config.SERVICE_UUID)
            if (service == null) {
                finish("Config service not found on dongle.", false)
                return
            }
            if (dfuBoard != null) {
                // Firmware update: first make sure the image matches this PCB.
                val boardCh = service.getCharacteristic(Config.BOARD_UUID)
                if (boardCh == null) {
                    finish(
                        "This dongle's firmware can't be updated over BLE yet. " +
                            "Install the MCUboot firmware once over USB/J-Link first.",
                        false,
                    )
                    return
                }
                report("Checking dongle type…")
                if (!g.readCharacteristic(boardCh)) {
                    finish("Failed to read the dongle type.", false)
                }
                return
            }
            report("Writing settings…")
            writeNext()
        }

        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            value: ByteArray,
            status: Int,
        ) {
            onBoardRead(value, status)
        }

        @Deprecated("Used below Android 13")
        @Suppress("DEPRECATION")
        override fun onCharacteristicRead(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) {
                onBoardRead(characteristic.value ?: ByteArray(0), status)
            }
        }

        override fun onCharacteristicWrite(
            g: BluetoothGatt,
            characteristic: BluetoothGattCharacteristic,
            status: Int,
        ) {
            if (status != BluetoothGatt.GATT_SUCCESS) {
                // Firmware returns an ATT error (e.g. 0x08) unless the dongle's
                // button is physically held down during the whole save.
                val hint = if (dfuBoard != null) {
                    "Hold the dongle button while starting the update."
                } else {
                    "Hold the dongle button while saving."
                }
                finish("Write rejected (ATT 0x%02x). %s".format(status, hint), false)
                return
            }
            writeNext()
        }
    }

    private fun onBoardRead(value: ByteArray, status: Int) {
        val expected = dfuBoard ?: return
        if (status != BluetoothGatt.GATT_SUCCESS) {
            finish("Failed to read the dongle type (status $status).", false)
            return
        }
        val actual = String(value, Charsets.UTF_8)
        if (actual != expected) {
            val name = Pcb.entries.firstOrNull { it.board == actual }?.label ?: "\"$actual\""
            finish("Wrong firmware: this dongle is the $name. Pick that one instead.", false)
            return
        }
        report("Arming firmware update…")
        writeNext()
    }

    private fun writeNext() {
        val g = gatt ?: return
        val item = writeQueue.removeFirstOrNull()
        if (item == null) {
            if (dfuBoard != null) {
                finish("Update armed — you can release the button.", true)
            } else {
                finish("Saved to dongle. Power-cycle it to apply.", true)
            }
            return
        }
        val service = g.getService(Config.SERVICE_UUID)
        val ch = service?.getCharacteristic(item.uuid)
        if (ch == null) {
            when {
                item.optional -> writeNext()
                item.uuid == Config.PASSWORD2_UUID -> finish(
                    "This dongle's firmware has no Password 2. Update the dongle " +
                        "firmware first.",
                    false,
                )
                item.uuid == Config.USERNAME2_UUID -> finish(
                    "This dongle's firmware has no Username 2. Update the dongle " +
                        "firmware first.",
                    false,
                )
                else -> finish("Characteristic ${item.uuid} missing.", false)
            }
            return
        }

        val ok: Boolean = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            g.writeCharacteristic(
                ch, item.value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
            ) == BluetoothStatusCodes.SUCCESS
        } else {
            @Suppress("DEPRECATION")
            run {
                ch.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                ch.value = item.value
                g.writeCharacteristic(ch)
            }
        }
        if (!ok) {
            finish("Failed to start write for ${item.uuid}.", false)
        }
    }

    private fun report(status: String) {
        main.post { listener(status, false, false) }
    }

    private fun finish(status: String, success: Boolean) {
        if (finished) return
        finished = true
        main.removeCallbacks(overallTimeout)
        stopScan()
        val g = gatt
        gatt = null
        main.post {
            try {
                g?.disconnect()
                g?.close()
            } catch (_: Exception) {
            }
            listener(status, true, success)
            val armed = onDfuArmed
            val dev = device
            if (success && armed != null && dev != null) {
                // Give the dongle a moment to see the disconnect and resume
                // advertising for the SMP client.
                main.postDelayed({ armed(dev) }, 800)
            }
        }
    }
}
