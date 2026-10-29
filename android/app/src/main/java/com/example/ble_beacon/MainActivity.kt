package com.example.ble_beacon

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.BluetoothManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Checkbox
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity

class MainActivity : FragmentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            com.example.ble_beacon.ui.theme.BLE_BeaconTheme {
                val context = LocalContext.current

                // Permissions needed to advertise, connect and scan.
                val permissionsToRequest = remember {
                    mutableListOf<String>().apply {
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                            add(Manifest.permission.BLUETOOTH_ADVERTISE)
                            add(Manifest.permission.BLUETOOTH_CONNECT)
                            add(Manifest.permission.BLUETOOTH_SCAN)
                        } else {
                            add(Manifest.permission.ACCESS_FINE_LOCATION)
                        }
                        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                            add(Manifest.permission.POST_NOTIFICATIONS)
                        }
                    }.toTypedArray()
                }

                fun hasAllPermissions(): Boolean = permissionsToRequest.all {
                    ContextCompat.checkSelfPermission(context, it) == PackageManager.PERMISSION_GRANTED
                }

                // ---- form state ----
                // Credentials are never stored on the phone: they start blank, and
                // blank means "leave the dongle's current value alone".
                var password by remember { mutableStateOf("") }
                var passwordVisible by remember { mutableStateOf(false) }
                var username2 by remember { mutableStateOf("") }
                var password2 by remember { mutableStateOf("") }
                var password2Visible by remember { mutableStateOf(false) }
                var clearLogin2 by remember { mutableStateOf(false) }
                // Password keyboard: no suggestions / learning of what's typed.
                val secretKeyboard = KeyboardOptions(
                    autoCorrectEnabled = false,
                    keyboardType = KeyboardType.Password,
                )
                // This phone's beacon fingerprint (matches beacon_key= in the dongle's
                // status dump). Computing it creates the key on first launch.
                val beaconId = remember { BeaconKey.idHex(context) }
                var greenText by remember { mutableStateOf(Config.getGreenThresholdText(context)) }
                var blueText by remember { mutableStateOf(Config.getBlueThresholdText(context)) }
                var status by remember { mutableStateOf("") }
                var sending by remember { mutableStateOf(false) }
                // Whether a credential set is saved on this phone (biometric-sealed).
                var hasSavedCreds by remember { mutableStateOf(CredentialStore.hasCredentials(context)) }
                // Pause the beacon when the phone is still (anti-relay); persisted toggle.
                var motionGate by remember { mutableStateOf(Config.isMotionGateEnabled(context)) }
                // Set when a dongle presents an identity we haven't paired with: the
                // fingerprint to show + the callback that resumes or aborts the connection.
                var unknownDongle by remember { mutableStateOf<Pair<String, (Boolean) -> Unit>?>(null) }

                val launcher = rememberLauncherForActivityResult(
                    ActivityResultContracts.RequestMultiplePermissions()
                ) { /* result handled by hasAllPermissions() on next action */ }

                fun ensurePermissions(): Boolean {
                    if (hasAllPermissions()) return true
                    launcher.launch(permissionsToRequest)
                    return false
                }

                fun startBeacon() {
                    if (!ensurePermissions()) {
                        status = "Grant Bluetooth permissions, then try again."
                        return
                    }
                    val manager =
                        context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
                    val adapter = manager.adapter
                    if (adapter == null || !adapter.isEnabled) {
                        status = "Bluetooth is off — turn it on, then Start Beacon."
                        return
                    }
                    Config.setBeaconEnabled(context, true)
                    BeaconService.start(context)
                    status = "Secure beacon started (ID $beaconId)."
                    requestBatteryExemption(context)
                }

                // Push a credential set to the dongle (over the sealed session). The
                // dongle keeps it in RAM only, so this runs every time it's plugged in.
                fun doSend(creds: CredentialStore.Creds, green: Int, blue: Int) {
                    // Make sure the beacon is on, so the dongle finds this phone as soon
                    // as its config window closes.
                    Config.setBeaconEnabled(context, true)
                    BeaconService.start(context)
                    sending = true
                    status = "Sending…"
                    ConfigClient(
                        context,
                        onUnknownIdentity = { fp, decide -> unknownDongle = fp to decide },
                    ) { s, done, success ->
                        status = s
                        if (done) {
                            sending = false
                            if (success) {
                                Config.save(context, green, blue)
                                // Credentials are saved (biometric-sealed) on the phone,
                                // so clear the input fields now that they've been sent.
                                password = ""
                                username2 = ""
                                password2 = ""
                                clearLogin2 = false
                            }
                        }
                    }.send(creds.password, creds.username2, creds.password2,
                        clearLogin2 = false, green, blue)
                }

                fun saveAndSend() {
                    if (!ensurePermissions()) {
                        status = "Grant Bluetooth permissions, then try again."
                        return
                    }
                    val green = greenText.trim().toIntOrNull()
                    val blue = blueText.trim().toIntOrNull()
                    val activity = context.findFragmentActivity()
                    when {
                        activity == null ->
                            status = "Internal error: no activity for the fingerprint prompt."
                        password.length > Config.PASSWORD_MAX_LEN ->
                            status = "Password too long (max ${Config.PASSWORD_MAX_LEN})."
                        password2.length > Config.PASSWORD_MAX_LEN ->
                            status = "Password 2 too long (max ${Config.PASSWORD_MAX_LEN})."
                        username2.length > Config.PASSWORD_MAX_LEN ->
                            status = "Username 2 too long (max ${Config.PASSWORD_MAX_LEN})."
                        green == null || green !in -120..0 ->
                            status = "Green threshold must be an integer dBm (-120…0)."
                        blue == null || blue !in -120..0 ->
                            status = "Blue threshold must be an integer dBm (-120…0)."
                        green <= blue ->
                            status = "Green (close) must be a stronger signal than Blue (far), " +
                                "e.g. Green -71, Blue -100."
                        password.isNotEmpty() -> {
                            // New / changed credentials: seal them behind the fingerprint,
                            // then send. Typing the password replaces the whole saved set.
                            val creds = CredentialStore.Creds(
                                password = password,
                                username2 = if (clearLogin2) "" else username2,
                                password2 = if (clearLogin2) "" else password2,
                            )
                            sending = true
                            status = "Confirm your fingerprint to save…"
                            CredentialStore.save(activity, creds) { ok, err ->
                                if (!ok) {
                                    sending = false
                                    status = err ?: "Couldn't save the password."
                                } else {
                                    hasSavedCreds = true
                                    doSend(creds, green, blue)
                                }
                            }
                        }
                        CredentialStore.hasCredentials(context) -> {
                            // Reuse the saved credentials: unlock with the fingerprint,
                            // then send. This is the usual every-plug-in case.
                            sending = true
                            status = "Confirm your fingerprint to send…"
                            CredentialStore.load(activity) { creds, err ->
                                if (creds == null) {
                                    sending = false
                                    hasSavedCreds = CredentialStore.hasCredentials(context)
                                    status = err ?: "Couldn't read the saved password."
                                } else {
                                    doSend(creds, green, blue)
                                }
                            }
                        }
                        else ->
                            status = "Enter your password above. It's saved on this phone " +
                                "(unlocked by your fingerprint) and re-sent to the dongle each " +
                                "time you plug it in."
                    }
                }

                // ---- firmware update ----
                // Images bundled into this APK at build time (null = not bundled).
                val bundled = remember { Pcb.entries.associateWith { FirmwareBundle.load(context, it) } }
                var showFirmwarePicker by remember { mutableStateOf(false) }
                var pickedPcb by remember { mutableStateOf<Pcb?>(null) }
                var uploadProgress by remember { mutableStateOf<Float?>(null) }

                fun updateFirmware(firmware: FirmwareImage) {
                    if (!ensurePermissions()) {
                        status = "Grant Bluetooth permissions, then try again."
                        return
                    }
                    // Gated by the dongle's held button, not the beacon, so there's
                    // no need to start advertising here.
                    sending = true
                    uploadProgress = null
                    status = "Keep holding the dongle button…"
                    ConfigClient(context) { s, done, success ->
                        status = s
                        // On success the upload continues in onArmed below.
                        if (done && !success) sending = false
                    }.armDfu(firmware.pcb.board) { device ->
                        FirmwareUpdater(context, device) { s, progress, done, _ ->
                            status = s
                            uploadProgress = progress
                            if (done) sending = false
                        }.start(firmware)
                    }
                }

                if (showFirmwarePicker) {
                    AlertDialog(
                        onDismissRequest = { showFirmwarePicker = false },
                        title = { Text("Update dongle firmware") },
                        text = {
                            Column {
                                Text("Which dongle are you updating?")
                                Spacer(Modifier.height(8.dp))
                                Pcb.entries.forEach { pcb ->
                                    val fw = bundled[pcb]
                                    Row(
                                        verticalAlignment = Alignment.CenterVertically,
                                        modifier = Modifier
                                            .fillMaxWidth()
                                            .clickable(enabled = fw != null) { pickedPcb = pcb }
                                    ) {
                                        RadioButton(
                                            selected = pickedPcb == pcb,
                                            onClick = { pickedPcb = pcb },
                                            enabled = fw != null,
                                        )
                                        Column {
                                            Text(pcb.label)
                                            Text(
                                                if (fw != null) "Bundled firmware v${fw.version}"
                                                else "Not bundled in this app build",
                                                style = MaterialTheme.typography.bodySmall,
                                                color = MaterialTheme.colorScheme.onSurfaceVariant,
                                            )
                                        }
                                    }
                                }
                                Spacer(Modifier.height(12.dp))
                                Text(
                                    "Plug the dongle in (it opens its 1-minute config window), " +
                                        "HOLD its button, tap Update, and keep holding until " +
                                        "\"Update armed\" appears.",
                                    style = MaterialTheme.typography.bodySmall,
                                )
                            }
                        },
                        confirmButton = {
                            TextButton(
                                enabled = pickedPcb?.let { bundled[it] } != null,
                                onClick = {
                                    showFirmwarePicker = false
                                    pickedPcb?.let { bundled[it] }?.let { updateFirmware(it) }
                                },
                            ) { Text("Update") }
                        },
                        dismissButton = {
                            TextButton(onClick = { showFirmwarePicker = false }) { Text("Cancel") }
                        },
                    )
                }

                unknownDongle?.let { (fp, decide) ->
                    AlertDialog(
                        onDismissRequest = {
                            decide(false)
                            unknownDongle = null
                        },
                        title = { Text("New dongle?") },
                        text = {
                            Text(
                                "This dongle (identity $fp…) isn't one you've paired with on " +
                                    "this phone. If it's yours and you're holding its button, " +
                                    "pair it. If you weren't expecting this, cancel — it could " +
                                    "be an impostor trying to receive your password."
                            )
                        },
                        confirmButton = {
                            TextButton(onClick = {
                                decide(true)
                                unknownDongle = null
                            }) { Text("Pair this dongle") }
                        },
                        dismissButton = {
                            TextButton(onClick = {
                                decide(false)
                                unknownDongle = null
                            }) { Text("Cancel") }
                        },
                    )
                }

                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background,
                ) {
                    Column(
                        modifier = Modifier
                            .fillMaxSize()
                            .verticalScroll(rememberScrollState())
                            .padding(24.dp),
                        horizontalAlignment = Alignment.CenterHorizontally,
                    ) {
                        Text(
                            text = "BLE Beacon Controller",
                            style = MaterialTheme.typography.headlineMedium,
                            fontWeight = FontWeight.Bold,
                            modifier = Modifier.padding(vertical = 16.dp)
                        )

                        Text(
                            text = if (hasSavedCreds) {
                                "A password is saved on this phone, encrypted behind your " +
                                    "fingerprint. Leave the fields blank and tap below to unlock " +
                                    "and send it; or type to change the saved password."
                            } else {
                                "The dongle keeps your password in RAM only and forgets it when " +
                                    "unplugged, so the phone stores it — encrypted behind your " +
                                    "fingerprint — and re-sends it each time you plug the dongle in."
                            },
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(bottom = 8.dp)
                        )

                        OutlinedTextField(
                            value = password,
                            onValueChange = { password = it },
                            label = { Text("Password (short press)") },
                            singleLine = true,
                            keyboardOptions = secretKeyboard,
                            visualTransformation = if (passwordVisible)
                                VisualTransformation.None else PasswordVisualTransformation(),
                            trailingIcon = {
                                IconButton(onClick = { passwordVisible = !passwordVisible }) {
                                    Icon(
                                        imageVector = if (passwordVisible)
                                            Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                                        contentDescription =
                                            if (passwordVisible) "Hide password" else "Show password"
                                    )
                                }
                            },
                            modifier = Modifier.fillMaxWidth()
                        )

                        Spacer(Modifier.height(12.dp))

                        OutlinedTextField(
                            value = username2,
                            onValueChange = { username2 = it },
                            label = { Text("Username 2 (typed + TAB before Password 2)") },
                            singleLine = true,
                            enabled = !clearLogin2,
                            keyboardOptions = KeyboardOptions(
                                autoCorrectEnabled = false,
                                keyboardType = KeyboardType.Ascii,
                            ),
                            modifier = Modifier.fillMaxWidth()
                        )

                        Spacer(Modifier.height(12.dp))

                        OutlinedTextField(
                            value = password2,
                            onValueChange = { password2 = it },
                            label = { Text("Password 2 (1-second press)") },
                            singleLine = true,
                            enabled = !clearLogin2,
                            keyboardOptions = secretKeyboard,
                            visualTransformation = if (password2Visible)
                                VisualTransformation.None else PasswordVisualTransformation(),
                            trailingIcon = {
                                IconButton(onClick = { password2Visible = !password2Visible }) {
                                    Icon(
                                        imageVector = if (password2Visible)
                                            Icons.Filled.VisibilityOff else Icons.Filled.Visibility,
                                        contentDescription =
                                            if (password2Visible) "Hide password 2" else "Show password 2"
                                    )
                                }
                            },
                            modifier = Modifier.fillMaxWidth()
                        )

                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { clearLogin2 = !clearLogin2 }
                        ) {
                            Checkbox(checked = clearLogin2, onCheckedChange = { clearLogin2 = it })
                            Text("Remove Username 2 + Password 2 from the dongle")
                        }

                        Spacer(Modifier.height(12.dp))

                        Text(
                            text = "Secure beacon ID: $beaconId. Saving pairs the dongle with " +
                                "this phone; other people's phones can't trigger it.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 4.dp, bottom = 8.dp)
                        )

                        Row(modifier = Modifier.fillMaxWidth()) {
                            OutlinedTextField(
                                value = greenText,
                                onValueChange = { greenText = it },
                                label = { Text("Green dBm") },
                                singleLine = true,
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                                modifier = Modifier.weight(1f)
                            )
                            Spacer(Modifier.width(12.dp))
                            OutlinedTextField(
                                value = blueText,
                                onValueChange = { blueText = it },
                                label = { Text("Blue dBm") },
                                singleLine = true,
                                keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                                modifier = Modifier.weight(1f)
                            )
                        }

                        Text(
                            text = "Green = close (type password). Blue = out of range.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 4.dp)
                        )

                        Spacer(Modifier.height(20.dp))

                        Button(
                            onClick = { saveAndSend() },
                            enabled = !sending,
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(56.dp),
                            shape = RoundedCornerShape(16.dp),
                            colors = ButtonDefaults.buttonColors(containerColor = Color(0xFF1565C0))
                        ) {
                            Text(
                                when {
                                    sending -> "WORKING…"
                                    password.isEmpty() && hasSavedCreds -> "UNLOCK & SEND TO DONGLE"
                                    else -> "SAVE & SEND TO DONGLE"
                                },
                                fontWeight = FontWeight.Bold
                            )
                        }

                        if (status.isNotEmpty()) {
                            Text(
                                text = status,
                                style = MaterialTheme.typography.bodyMedium,
                                modifier = Modifier.padding(top = 12.dp)
                            )
                        }
                        uploadProgress?.let { p ->
                            LinearProgressIndicator(
                                progress = { p },
                                modifier = Modifier
                                    .fillMaxWidth()
                                    .padding(top = 8.dp)
                            )
                        }

                        Text(
                            text = "Plug the dongle in, HOLD its button, tap, then confirm your " +
                                "fingerprint. The dongle stays in its config window until it has " +
                                "received everything, so there's no rush.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier.padding(top = 4.dp)
                        )

                        Spacer(Modifier.height(16.dp))

                        Button(
                            onClick = {
                                pickedPcb = null
                                showFirmwarePicker = true
                            },
                            enabled = !sending,
                            modifier = Modifier
                                .fillMaxWidth()
                                .height(48.dp),
                            shape = RoundedCornerShape(16.dp),
                            colors = ButtonDefaults.buttonColors(
                                containerColor = Color(0xFFE65100)
                            )
                        ) {
                            Text("UPDATE DONGLE FIRMWARE", fontWeight = FontWeight.Bold)
                        }
                        Text(
                            text = "Uses the firmware built into this app. Hold the dongle " +
                                "button when you tap Update.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 4.dp)
                        )

                        Spacer(Modifier.height(28.dp))

                        Row(
                            verticalAlignment = Alignment.CenterVertically,
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable {
                                    val on = !motionGate
                                    motionGate = on
                                    Config.setMotionGateEnabled(context, on)
                                    // Re-apply immediately if the beacon is running.
                                    if (Config.isBeaconEnabled(context)) BeaconService.start(context)
                                }
                        ) {
                            Checkbox(
                                checked = motionGate,
                                onCheckedChange = { on ->
                                    motionGate = on
                                    Config.setMotionGateEnabled(context, on)
                                    if (Config.isBeaconEnabled(context)) BeaconService.start(context)
                                },
                            )
                            Text("Pause beacon when the phone is still for 5 min (anti-relay)")
                        }
                        Text(
                            text = "Off by default. When on, a phone that sits motionless (e.g. " +
                                "left behind) stops beaconing, so it can't be relayed — but a " +
                                "phone resting still by the PC will also pause and lock it until moved.",
                            style = MaterialTheme.typography.bodySmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                            modifier = Modifier
                                .fillMaxWidth()
                                .padding(top = 4.dp, bottom = 12.dp)
                        )

                        ThreeDButton(
                            text = "START BEACON",
                            color = Color(0xFF4CAF50),
                            onClick = { startBeacon() }
                        )
                        Spacer(modifier = Modifier.height(16.dp))
                        ThreeDButton(
                            text = "STOP BEACON",
                            color = Color(0xFFF44336),
                            onClick = {
                                Config.setBeaconEnabled(context, false)
                                context.stopService(Intent(context, BeaconService::class.java))
                            }
                        )
                    }
                }
            }
        }
    }
}

/**
 * Ask Android (once per start, until granted) not to battery-optimise the app:
 * otherwise Doze / Samsung's "sleeping apps" can kill the beacon service and
 * the dongle stops seeing the phone. Sideloaded app, so the Play policy on
 * this permission doesn't apply.
 */
@SuppressLint("BatteryLife")
private fun requestBatteryExemption(context: Context) {
    val pm = context.getSystemService(PowerManager::class.java) ?: return
    if (pm.isIgnoringBatteryOptimizations(context.packageName)) return
    runCatching {
        context.startActivity(
            Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
                .setData(Uri.parse("package:${context.packageName}"))
        )
    }
}

/** Unwrap a (possibly themed) Context to the hosting FragmentActivity, for BiometricPrompt. */
private tailrec fun Context.findFragmentActivity(): FragmentActivity? = when (this) {
    is FragmentActivity -> this
    is android.content.ContextWrapper -> baseContext.findFragmentActivity()
    else -> null
}

@Composable
fun ThreeDButton(
    text: String,
    color: Color,
    onClick: () -> Unit
) {
    Box(
        modifier = Modifier
            .fillMaxWidth()
            .height(90.dp)
            .clickable(onClick = onClick)
    ) {
        Surface(
            color = color.copy(alpha = 0.6f),
            shape = RoundedCornerShape(16.dp),
            modifier = Modifier
                .fillMaxSize()
                .padding(top = 8.dp)
        ) {}
        Surface(
            color = color,
            shape = RoundedCornerShape(16.dp),
            modifier = Modifier.fillMaxSize(),
            shadowElevation = 4.dp
        ) {
            Box(contentAlignment = Alignment.Center) {
                Text(
                    text = text,
                    style = MaterialTheme.typography.titleLarge,
                    color = Color.White,
                    fontWeight = FontWeight.ExtraBold,
                    letterSpacing = 2.sp
                )
            }
        }
    }
}
