package com.example.ble_beacon

import android.app.*
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseCallback
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertiseSettings
import android.bluetooth.le.BluetoothLeAdvertiser
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log
import androidx.core.app.NotificationCompat

class BeaconService : Service() {

    private var bluetoothLeAdvertiser: BluetoothLeAdvertiser? = null
    private val TAG = "BeaconService"

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val beaconName = Config.getBeaconNameForAdvertising(this)
        createNotificationChannel()
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("BLE Beacon")
            .setContentText("Advertising as $beaconName")
            .setSmallIcon(R.mipmap.ic_launcher)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(1, notification)
        }
        
        startAdvertising()
        return START_STICKY
    }

    private fun startAdvertising() {
        val bluetoothManager = getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
        val bluetoothAdapter = bluetoothManager.adapter

        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) {
            Log.e(TAG, "Bluetooth not enabled or not supported")
            return
        }

        bluetoothLeAdvertiser = bluetoothAdapter.bluetoothLeAdvertiser

        if (bluetoothLeAdvertiser == null) {
            Log.e(TAG, "Bluetooth LE Advertising not supported")
            return
        }

        // Stop any previous advertising so a restart picks up a changed name.
        try {
            bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback)
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to stop advertising", e)
        }

        val settings = AdvertiseSettings.Builder()
            .setAdvertiseMode(AdvertiseSettings.ADVERTISE_MODE_LOW_LATENCY)
            .setConnectable(false)
            .setTimeout(0)
            .setTxPowerLevel(AdvertiseSettings.ADVERTISE_TX_POWER_HIGH)
            .build()

        val data = AdvertiseData.Builder()
            .setIncludeDeviceName(true)
            .build()

        try {
            bluetoothAdapter.name = Config.getBeaconNameForAdvertising(this)
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to set name", e)
        }

        try {
            bluetoothLeAdvertiser?.startAdvertising(settings, data, advertiseCallback)
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to start advertising", e)
        }
    }

    private val advertiseCallback = object : AdvertiseCallback() {
        override fun onStartSuccess(settingsInEffect: AdvertiseSettings) {
            Log.i(TAG, "LE Advertise Started.")
        }

        override fun onStartFailure(errorCode: Int) {
            Log.e(TAG, "LE Advertise Failed: $errorCode")
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        try {
            bluetoothLeAdvertiser?.stopAdvertising(advertiseCallback)
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to stop advertising", e)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val serviceChannel = NotificationChannel(
                CHANNEL_ID,
                "Beacon Service Channel",
                NotificationManager.IMPORTANCE_LOW
            )
            val manager = getSystemService(NotificationManager::class.java)
            manager.createNotificationChannel(serviceChannel)
        }
    }

    companion object {
        const val CHANNEL_ID = "BeaconServiceChannel"
    }
}
