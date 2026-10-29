package com.example.ble_beacon

import android.annotation.SuppressLint
import android.app.*
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.bluetooth.le.AdvertiseData
import android.bluetooth.le.AdvertisingSet
import android.bluetooth.le.AdvertisingSetCallback
import android.bluetooth.le.AdvertisingSetParameters
import android.bluetooth.le.BluetoothLeAdvertiser
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import androidx.core.content.edit

/**
 * Broadcasts the authenticated rolling beacon (see [BeaconCrypto]).
 *
 * The counter is the wall clock in seconds, never allowed to go backwards. It
 * is re-signed every second while the CPU is awake. When the phone sleeps the
 * controller keeps sending the last packet (the dongle accepts a repeated
 * counter while it can see us), but after the dongle locks it wants a NEWER
 * one — so an alarm and the screen-on broadcast keep the counter moving while
 * the phone is asleep.
 */
@SuppressLint("MissingPermission") // MainActivity starts us only with BLE permissions granted
class BeaconService : Service() {

    private val TAG = "BeaconService"
    private val main = Handler(Looper.getMainLooper())

    private var advertiser: BluetoothLeAdvertiser? = null
    private var advertisingSet: AdvertisingSet? = null
    private var running = false
    private lateinit var key: ByteArray
    private var ctr = 0L
    private var ticksSinceSave = 0

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        key = BeaconKey.get(this)
        createNotificationChannel()
        val notification = NotificationCompat.Builder(this, CHANNEL_ID)
            .setContentTitle("BLE Beacon")
            .setContentText("Secure beacon on (ID ${BeaconKey.idHex(this)})")
            .setSmallIcon(R.mipmap.ic_launcher)
            .build()

        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        } else {
            startForeground(1, notification)
        }

        if (!running) {
            running = true
            ctr = startCounter()
            startAdvertising()
            main.post(tickLoop)
            ContextCompat.registerReceiver(
                this, wakeReceiver,
                IntentFilter().apply {
                    addAction(ACTION_TICK)
                    addAction(Intent.ACTION_SCREEN_ON)
                    addAction(Intent.ACTION_USER_PRESENT)
                    addAction(BluetoothAdapter.ACTION_STATE_CHANGED)
                },
                ContextCompat.RECEIVER_NOT_EXPORTED,
            )
            scheduleAlarm()
        } else if (advertisingSet == null) {
            startAdvertising()   // e.g. Bluetooth was off when we first started
        }
        return START_STICKY
    }

    // ---- counter ----

    private fun prefs() = getSharedPreferences(PREFS, Context.MODE_PRIVATE)

    /** Past the last counter we may have sent (saved once a minute). */
    private fun startCounter(): Long =
        maxOf(nowSec(), prefs().getLong(KEY_CTR, 0) + SAVE_EVERY_TICKS + 1)

    private fun nowSec() = System.currentTimeMillis() / 1000

    private fun tick() {
        if (!running) return
        ctr = maxOf(ctr + 1, nowSec()) and 0xFFFF_FFFFL
        updateAdvertisingData()
        if (++ticksSinceSave >= SAVE_EVERY_TICKS) {
            ticksSinceSave = 0
            prefs().edit { putLong(KEY_CTR, ctr) }
        }
    }

    private val tickLoop = object : Runnable {
        override fun run() {
            tick()
            main.postDelayed(this, 1000)
        }
    }

    // Alarm (works while the CPU sleeps) + screen on / unlock tick the
    // counter. Bluetooth being switched off kills the advertising set, so
    // restart it when Bluetooth comes back on.
    private val wakeReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == BluetoothAdapter.ACTION_STATE_CHANGED) {
                when (intent.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                    BluetoothAdapter.STATE_ON -> startAdvertising()
                    BluetoothAdapter.STATE_OFF -> advertisingSet = null
                }
                return
            }
            tick()
            if (intent.action == ACTION_TICK) scheduleAlarm()
        }
    }

    private fun alarmIntent(): PendingIntent = PendingIntent.getBroadcast(
        this, 0, Intent(ACTION_TICK).setPackage(packageName),
        PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
    )

    private fun scheduleAlarm() {
        val am = getSystemService(AlarmManager::class.java) ?: return
        // Inexact (no special permission); roughly once a minute, also in Doze.
        am.setAndAllowWhileIdle(
            AlarmManager.ELAPSED_REALTIME_WAKEUP,
            SystemClock.elapsedRealtime() + ALARM_INTERVAL_MS,
            alarmIntent(),
        )
    }

    // ---- advertising ----

    private fun advertiseData(): AdvertiseData = AdvertiseData.Builder()
        .addManufacturerData(BeaconCrypto.COMPANY_ID, BeaconCrypto.beaconPayload(key, ctr))
        .build()

    private fun startAdvertising() {
        val adapter = (getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager).adapter
        if (adapter == null || !adapter.isEnabled) {
            Log.e(TAG, "Bluetooth not enabled or not supported")
            return
        }
        // Drop any old set first, so a restart doesn't re-register the callback.
        runCatching { advertiser?.stopAdvertisingSet(setCallback) }
        advertisingSet = null
        advertiser = adapter.bluetoothLeAdvertiser ?: run {
            Log.e(TAG, "Bluetooth LE Advertising not supported")
            return
        }
        try {
            val params = AdvertisingSetParameters.Builder()
                .setLegacyMode(true)       // plain advert every scanner understands
                .setConnectable(false)
                .setScannable(false)
                .setInterval(AdvertisingSetParameters.INTERVAL_LOW)  // ~100 ms
                .setTxPowerLevel(AdvertisingSetParameters.TX_POWER_HIGH)
                .build()
            advertiser?.startAdvertisingSet(params, advertiseData(), null, null, null, setCallback)
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to start advertising", e)
        }
    }

    private fun updateAdvertisingData() {
        try {
            advertisingSet?.setAdvertisingData(advertiseData())
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to update advertising", e)
        }
    }

    private val setCallback = object : AdvertisingSetCallback() {
        override fun onAdvertisingSetStarted(set: AdvertisingSet?, txPower: Int, status: Int) {
            if (status == ADVERTISE_SUCCESS) {
                advertisingSet = set
                Log.i(TAG, "Secure beacon started")
            } else {
                Log.e(TAG, "Advertising set failed: $status")
            }
        }

        override fun onAdvertisingSetStopped(set: AdvertisingSet?) {
            advertisingSet = null
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        running = false
        main.removeCallbacks(tickLoop)
        runCatching { unregisterReceiver(wakeReceiver) }
        getSystemService(AlarmManager::class.java)?.cancel(alarmIntent())
        prefs().edit { putLong(KEY_CTR, ctr) }
        try {
            advertiser?.stopAdvertisingSet(setCallback)
        } catch (e: SecurityException) {
            Log.e(TAG, "Permission denied to stop advertising", e)
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    private fun createNotificationChannel() {
        val serviceChannel = NotificationChannel(
            CHANNEL_ID,
            "Beacon Service Channel",
            NotificationManager.IMPORTANCE_LOW
        )
        getSystemService(NotificationManager::class.java).createNotificationChannel(serviceChannel)
    }

    companion object {
        const val CHANNEL_ID = "BeaconServiceChannel"

        fun start(ctx: Context) {
            ContextCompat.startForegroundService(ctx, Intent(ctx, BeaconService::class.java))
        }

        /** Advertising needs BLUETOOTH_ADVERTISE on Android 12+. */
        fun hasPermissions(ctx: Context): Boolean =
            Build.VERSION.SDK_INT < Build.VERSION_CODES.S ||
                ContextCompat.checkSelfPermission(ctx, android.Manifest.permission.BLUETOOTH_ADVERTISE) ==
                PackageManager.PERMISSION_GRANTED
        private const val PREFS = "ble_password_beacon"
        private const val KEY_CTR = "last_counter"
        private const val SAVE_EVERY_TICKS = 60
        private const val ALARM_INTERVAL_MS = 60_000L
        private const val ACTION_TICK = "com.example.ble_beacon.BEACON_TICK"
    }
}
