# BLE_Password — project notes (CLAUDE.md)

BLE beacon **proximity password typer** on an nRF52840 USB dongle (Zephyr / nRF
Connect SDK). The dongle is a USB **HID keyboard**; it scans for a phone's BLE
beacon and, by signal strength, decides proximity. When the phone is close and
the button is pressed, it types a stored password over USB; when the beacon
leaves, it locks the PC (Win+L). A companion Android app broadcasts the beacon,
configures the dongle over BLE, and **carries the dongle firmware** for BLE updates.

## Repos / paths
- Firmware: `c:\Users\rabram\Source\nRFConnect\BLE_Password` (this folder). App code in `src/main.c`. Git repo → **https://github.com/Rambooka/BLE_Password (PUBLIC)**. Never commit real passwords, keys, or build output (`.gitignore`: `build*/`, `*.7z`, `.claude/`). Local git's stored GitHub credential is for the **RambookaICT** account.
- LED/button bring-up test (3rd-party board): `c:\Users\rabram\Source\nRFConnect\led_test`.
- Android app: `android/` in this repo (Kotlin/Compose; moved from `C:\Users\rabram\Source\Android\BLE_Beacon` 2026-10-29 — that old copy is stale). Advertises the beacon, GATT config client, SMP firmware updater.
- Datasheets / 3rd-party board info: `C:\_Datasheets\BLE Modules\RayTac\` (see `Rays Notes.txt`).

## Hardware targets
Two different boards — **different LED/button pins**:

| | Official Nordic Dongle (pca10059) | 3rd-party Raytac **MDBT50Q-RX** dongle |
|---|---|---|
| Board id | `nrf52840dongle/nrf52840` | (no board def; build as `nrf52840dk/nrf52840` + overlay) |
| Flashing | BLE from the app; one-time USB migration (below) | BLE from the app; **J-Link / SWD** (`nrfjprog`) on pads T5=SWDIO, T6=SWDCLK, T3=3V3/VTREF, T4=GND |
| LEDs | led0 green P0.06, led1 RGB-red P0.08, led3 RGB-blue P0.12 (active low) | **ONE blue LED on P1.13** (active low). D2/P1.11 pad is **not populated** → single-LED UI (blink patterns) |
| Button | SW1 = P1.06 (INPUT_KEY_0) | **P0.15**, active low, needs pull-up |
| Bootloader | Factory Nordic Open USB-DFU bootloader @ 0xE0000 → chains to **MCUboot @ 0x1000** | **MCUboot @ 0x0** |
| Board quirks | P0.18 = RESET button | **P0.19 is wired to P0.18/nRESET** (like PCA10059's self-reset pin) → never drive P0.19 low or the chip resets; 32.768 kHz crystal **is fitted** |

## Flash layout (both boards, `pm_static_<board>_nrf52840.yml`)
| Range | Contents |
|---|---|
| 0x00000 | dongle: Nordic MBR (0x0–0x1000) then MCUboot (0x1000–0x10000, USB serial recovery) · MDBT50Q: MCUboot (0x0–0x10000) |
| 0x10000–0x76000 | slot 0 — running app (MCUboot header at 0x10000, app at 0x10200) |
| 0x76000–0xDC000 | slot 1 — SMP upload target |
| **0xDC000–0xE0000** | `config_storage` — raw persisted config (`PM_CONFIG_STORAGE_ADDRESS`; BUILD_ASSERTed == 0xDC000) |
| 0xE0000–0x100000 | dongle: Nordic bootloader + settings (reserved) · MDBT50Q: unused |

## Build environment (PowerShell)
NCS **v3.0.2**, toolchain `C:\ncs\toolchains\0b393f9e1b`. Set env then `west`:
- PATH prepend (under toolchain root): `.`, `mingw64/bin`, `bin`, `opt/bin`, `opt/bin/Scripts`, `opt/nanopb/generator-bin`, `opt/zephyr-sdk/arm-zephyr-eabi/bin`, `opt/zephyr-sdk/riscv64-zephyr-elf/bin`
- `PYTHONPATH` = toolchain `opt/bin`, `opt/bin/Lib`, `opt/bin/Lib/site-packages`
- `ZEPHYR_TOOLCHAIN_VARIANT=zephyr`, `ZEPHYR_SDK_INSTALL_DIR=<tc>\opt\zephyr-sdk`, `ZEPHYR_BASE=C:\ncs\v3.0.2\zephyr`, `WEST_TOPDIR=C:\ncs\v3.0.2`
- `west build` needs `ZEPHYR_BASE`/`WEST_TOPDIR` (app is freestanding, outside the NCS workspace).
- Put `C:\Tools` on PATH too (legacy nrfutil v6.1.7, used by `sysbuild.cmake`).

**Builds (MCUboot is on for both via `sysbuild.conf`):**
```
west build -b nrf52840dongle/nrf52840 -d build_nRF52840 -p always --sysbuild .
west build -b nrf52840dk/nrf52840     -d build_mdbt50q  -p always --sysbuild .
```
Outputs per build dir: `dfu_application.zip` (signed app + manifest = BLE update package, bundled into the Android app), `merged.hex` (MCUboot + app, for J-Link), `BLE_Password/zephyr/zephyr.signed.bin`. Dongle build also makes `dongle_mcuboot_migration.zip` (Nordic-bootloader package containing **only MCUboot**). Version comes from `VERSION` (currently 1.2.0) → shown in the app and in the status dump. **Always bump it for a new image**: the app's updater silently skips the upload if the dongle already runs an image with the same hash, and two different images with the same version can't be told apart.

**Lint + unit tests — run after EVERY code change (firmware or app), before committing.** `lint.ps1` also runs `tests/host/run.ps1`, which builds the pure-C `beacon_auth.c` with Strawberry `gcc` against the FIPS/RFC/Python vectors, plus `:app:testDebugUnitTest`. The firmware and app vectors are shared, so a beacon-format change must update both tests.
```
powershell -ExecutionPolicy Bypass -File tools\lint.ps1             # both
powershell -ExecutionPolicy Bypass -File tools\lint.ps1 -Firmware   # after a west build of both boards
powershell -ExecutionPolicy Bypass -File tools\lint.ps1 -Android [-AndroidDir <app dir>]
```
- **Firmware = clang-tidy** (`C:\Program Files\LLVM\bin\clang-tidy.exe`, LLVM 18) on `src/*.c`, for **both** boards (main.c has board `#ifdef`s). The script reuses each build's `compile_commands.json`, strips GCC-only flags and adds `--target=arm-none-eabi`, so build first. Checks are in `.clang-tidy`: bugprone/cert/clang-analyzer/misc, with Zephyr noise off and the reason given for each. Target: **zero findings**. Fix real issues. Silence false positives with a targeted `NOLINTNEXTLINE(check): reason` (as on the `K_THREAD_DEFINE` lines), not by disabling checks globally.
- **App = Android Lint** (`gradlew :app:lintRelease`, report `app\build\reports\lint-results-release.html`). Target: **zero errors** (errors fail the task). Don't add new warnings. Known existing warnings (20, 2026-10-29): dependency upgrades, `OldTargetApi`, unused template colours, `UseKtx`/`UseTomlInstead`/`RedundantLabel` style. Upgrading dependencies is a separate, deliberate change, not a lint fix.
- First run (2026-10-29) found and fixed:
  - firmware: 4× `strcpy` → size-asserted `memcpy`, RSSI sum `int16_t` → `int32_t`;
  - app: the API 33+ `writeCharacteristic` result was compared to `GATT_SUCCESS` instead of `BluetoothStatusCodes.SUCCESS` (both 0, so it worked by luck); `remember { purgeSecrets() }` → `LaunchedEffect`.

**Release flow:** bump `VERSION` → build both boards → build the Android app (it copies both `dfu_application.zip` in) → users update from the app.

**Official dongle — ONE-TIME migration to MCUboot** (`flash_dongle.bat`):
1. Dongle in Nordic DFU (RESET, or SW1+replug → pulsing red; enumerates `nRF52 SDFU USB`, e.g. COM20) → `nrfutil dfu usb-serial -pkg build_nRF52840\dongle_mcuboot_migration.zip -p COM20`. I can't put it in DFU — ask the user.
2. Hold **SW1 while plugging in** (not RESET) → MCUboot USB serial recovery ("MCUBOOT" COM port) → upload `build_nRF52840\BLE_Password\zephyr\zephyr.signed.bin` with an SMP serial client (`smpmgr`, `nrfutil mcu-manager`, or `mcumgr`).
3. Replug → app runs; all later updates go over BLE.
SMP serial client: `pip install smpmgr` (not installed globally), then `smpmgr --port COMxx image upload <signed.bin>` and `smpmgr --port COMxx os reset`. MCUboot's recovery port enumerates as VID 0x2FE3 "USB Serial Device"; smpmgr's "ENOTSUP reading MCUMgr parameters" warning is harmless.
Why only MCUboot goes through the Nordic bootloader: it CRC-checks everything it flashed on every power-on, so if it also carried the app, the first MCUboot slot swap would break the CRC and strand the dongle in Nordic DFU. The Nordic bootloader (RESET button) remains as a last-resort recovery.

**3rd-party board first flash (J-Link)** — recover FIRST on a new unit (erases the factory test code and sets UICR.APPROTECT=HwDisabled so our firmware keeps SWD open), then program with `--sectorerase` so that UICR is preserved:
```
nrfjprog -f NRF52 --recover
nrfjprog -f NRF52 --program build_mdbt50q\merged.hex --sectorerase --verify --reset
```
Then confirm it booted over RTT before unplugging (`JLinkRTTLogger -Device NRF52840_XXAA -If SWD -Speed 4000 -RTTChannel 0 <file>`; expect "Image confirmed", config window, scanning). J-Link probe present (nrfjprog `--ids`); `JLink.exe`, `JLinkRTTLogger.exe` and `nrfjprog` 10.24.2 (J-Link DLL 8.96) installed.

## Firmware behaviour
- **Boot config window (~60 s):** advertise a connectable GATT config service; LED blinks blue (official). The Android app connects and writes settings. **Writes are only accepted while the dongle's button is physically held down** — the hold is the proof the writer owns the dongle. A **short button tap (<500 ms) exits the window early**; the window also ends when the app disconnects or 60 s elapses. Valid save → 3 green blinks; invalid values → 6 fast red/ALERT blinks and nothing is saved.
- **Rolling beacon (fw 1.2.0+, `src/beacon_auth.[ch]`):** the dongle no longer matches a beacon *name*. The phone advertises manufacturer data with company ID 0xFFFF, laid out as `'B' 'P' 01 ctr[4 LE] tag[8]`, where `tag = HMAC-SHA256(key, 'B' 'P' 01 ctr)[0..8]`.
  - **Key:** 16 random bytes per phone, written once through GATT `…000A`, with the button gate. Without a key the dongle never accepts any beacon.
  - **Counter:** wall-clock seconds (`currentTimeMillis`, UTC, so time zones and DST don't matter), strictly increasing on the phone.
  - **Acceptance:** valid tag, and `ctr >= last accepted`. Equal is allowed because a sleeping phone repeats its last packet. Also `ctr > floor`.
  - **Floor:** set to the last accepted counter at **every lock** (left GREEN, or beacon lost). So after the owner leaves, only a newer counter brings the dongle back; a recording can't. The floor is also appended to a **counter log in flash**: two ping-pong pages at 0xDD000 / 0xDE000 of `uint32` words, logged on each lock and every 5 min of presence, loaded at boot. Writing a new key erases the log.
  - **Coexistence:** colleagues' phones fail the tag check. They only increment `beacons=`.
  - **Config advert:** carries `FF FF 'B' 'P' 10 <flags>`, bit 0 = button held, so the app connects only to the dongle being held.
  - Self-implemented SHA-256 / HMAC, host-tested against FIPS 180-4, RFC 4231 and a Python vector shared with the Kotlin test.
- **Normal mode:** scan for the paired phone's rolling beacon. Proximity by averaged RSSI (4 dB hysteresis so a borderline signal doesn't flip-flop zones) → blue (not seen) / green (close, `>= green threshold`) / red (far). Button released while green, by press length (`MEDIUM_PRESS_MS` / `LONG_PRESS_MS`): **< 600 ms → password 1** + Enter; **600 ms–2.5 s → [username 2 + TAB +] password 2** + Enter (login forms; the username part only if set; nothing if password 2 isn't set); 2.5–5 s → nothing (back out). Leaving green (→ red **or** → blue) or losing the beacon (5 s) → Win+L.
- **Status:** hold button 5 s → types a full status/diagnostics dump over HID into a focused text editor. First line `fw=<VERSION> board=<CONFIG_BOARD> confirmed=<0|1>` (confirmed=0 → MCUboot rolls back on next reset). The credentials (`password=`, `username2=`, `password2=`) are only printed while GREEN (phone in range), else `password=(hidden: phone not in range)`. `ads= beacons= match= stale=` (all adverts / rolling beacons from any phone / ours verified+fresh / ours with an old counter), `beacon_key=<id> ctr= floor=` (or `(not set: pair from the app)`). `averaged_rssi=` is the 20-sample running average (named so it isn't mistaken for a setting); it prints `gone` once the beacon hasn't been seen for `BEACON_TIMEOUT_MS` (5 s) — fw 1.1.6+ (older firmware printed a stale `rssi=`).
- **Firmware update (BLE, both boards):** the app reads the board characteristic (must match the chosen image), then writes `0x01` to DFU-arm (button held). That unlocks MCUmgr/SMP **for that phone's address only**, extends the window to 3 min, and re-advertises after the arming connection drops. LED fast-blinks while armed. The app reconnects over SMP, uploads to slot 1, marks it *test*, and resets. MCUboot swaps; the new app **self-confirms** at boot (`boot_write_img_confirmed()` before `bt_enable`) — if it crashes before that, MCUboot rolls back on the next reset. SMP commands are refused at all other times (`smp_gate_cb`, `MGMT_EVT_OP_CMD_RECV`).
- **Crash = fast red flash** (`k_sys_fatal_error_handler`); boot BLE-init failure = repeating red blink code (1=bt_enable, 2=scan).
- **ASCII→HID**: US-layout map, so any typed password works.

## GATT config service (UUIDs must match the Android app)
- Service `9f1b0001-5c3a-4e8e-9c2a-1a2b3c4d5e6f`
- Password (write, UTF-8) `...0002`. (`...0003` was the beacon name; removed in 1.2.0.)
- Green threshold (write, 1 signed byte dBm) `...0004`; Blue threshold `...0005`
- DFU arm (write `0x01`) `...0006` — unlocks SMP upload (see above). Same button-held gate.
- Board (read, string = `CONFIG_BOARD`: `nrf52840dongle` / `nrf52840dk`) `...0007` — no gate. Matches the `board` field in `dfu_application.zip`'s manifest.
- Password 2 (write, UTF-8, **empty write = clear**; optional) `...0008` — fw 1.1.4+. Username 2 (same rules) `...0009` — fw 1.1.5+. The app treats each as optional when blank.
- Beacon key (write, exactly 16 bytes, gated) `...000A` and Key ID (read, 4 bytes = `HMAC(key,"BLEPW-KEYID")[0..4]`, empty = unpaired, no gate) `...000B`, both fw 1.2.0+. The app reads the key ID first and writes the key only if it differs, so the key crosses the air once per dongle. Missing `...000B` → the app refuses to configure ("older than 1.2, update first"); a firmware update still works.
- **Encrypted session `...000C` (fw 1.2.0+, `src/cfg_crypto.[ch]` + `ConfigSession.kt`):**
  - **Key exchange:** read returns the dongle's P-256 public key, 65 bytes uncompressed. It's generated once per config window with PSA: `psa_generate_key`, ECDH via Nordic **Oberon** in software, TRNG from CC3XX. Write takes the phone's fresh public key (also 65 bytes). The dongle then runs `psa_raw_key_agreement` (which validates the point) and derives `HKDF-SHA256(salt "BLEPW-SESSION-v1", Z, info dongle_pub‖phone_pub)` → `k_enc ‖ k_mac`.
  - **Sealed writes:** Password `0x02`, Password2 `0x08`, Username2 `0x09` and BeaconKey `0x0A` must arrive as `seq[4 LE] ‖ ct ‖ tag[16]`. The ciphertext is XORed with an `HMAC(k_enc, id‖seq‖blk)` keystream; `tag = HMAC(k_mac, id‖seq‖ct)[0..16]`. Seq must rise within the session. Anything else is rejected with ATT 0x13. Not `0x0F`: Insufficient Encryption would make Android start its own pairing.
  - **Lifetime:** the session is cleared on disconnect, and the key pair is destroyed when the window closes.
  - **Plaintext:** thresholds and the DFU arm stay unencrypted.
  - **Threat model:** this defeats passive sniffing, but not an active relay during the window (unauthenticated ECDH).
  - **Threads:** ECDH runs in the BT RX thread, so `CONFIG_BT_RX_STACK_SIZE=4096`, the system workqueue is 4096, and the `ble_init` thread is 4 KB.
  - **Size:** the dongle app is now 52.6% of the 408 KB slot and uses 29% of RAM.
- Plus the standard MCUmgr SMP service (locked unless armed).
- Dongle advertises name `BLE_Password` (scan response) plus the button-held manufacturer data. App scans unfiltered and matches by UUID **or** name (Samsung offloaded 128-bit filters are unreliable). It connects only to a dongle advertising "held". If only old-firmware dongles (no flag) are seen, it connects to the first after 3 s.
- Writes only accepted during the config window **and while the button is held** (physical-ownership gate). Values are sanity-checked (−120…0 dBm, green > blue, non-empty password) on each write, again at save time (bad combo → not persisted), and on load (corrupt flash → compiled defaults).

## Hardware constraints discovered (IMPORTANT — the hard-won bits)
1. **No serial/RTT on the official dongle** (HID-only USB, no debugger) → debugging was via LED patterns + a HID "type the status" dump.
2. **NVS/settings "hang"** — root cause was almost certainly that Partition Manager put `storage` at 0xFE000, *inside* the dongle's protected Nordic bootloader region. Still don't use NVS/settings; persistence is raw flash.
3. **Scan + connectable advertising cannot run together** — second role returns `-EINVAL`. Hence the **boot config window** (advertise, then switch to scanning).
4. **Persistence = raw flash in `config_storage` (0xDC000)** via `DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller))`, `flash_read/erase/write`. **Load before `bt_enable()`**. **Save still wraps `bt_disable()`/`bt_enable()`** (proven safe). Flash now uses the default **MPSL radio sync** (`CONFIG_SOC_FLASH_NRF_RADIO_SYNC_NONE` removed) because SMP must erase/write slot 1 *during* a BLE connection — with RADIO_SYNC_NONE the ~85 ms erase stall faults the controller. One record at 0xDC000: `struct persist_cfg { uint32 magic; int8 green; int8 blue; char password[65]; char beacon[32] (unused since v6); char password2[65]; char username2[65]; uint8 beacon_key[16]; uint8 beacon_key_set; }`, magic `0xB1E50006`. Only ever **append** fields. The loader also accepts the older magics, so updates keep the saved config:
   - `CFG_MAGIC_V3` = `0xB1E50003`: ≤ 1.1.3, no password2;
   - `V4` = `0xB1E50004`: 1.1.4, no username2;
   - `V5` = `0xB1E50005`: 1.1.5–1.1.6, no key. After a 1.2.0 update, **pair again**.

   The pages at 0xDD000 and 0xDE000 hold the beacon counter log (see "Rolling beacon"). It's written by the workqueue *while scanning*; MPSL sync makes that safe.
5. **CDC-ACM serial logging is unstable** composited with HID on this dongle (crashes terminal apps) — keep the app USB HID-only. (MCUboot's own USB recovery uses CDC-ACM alone, which is fine.)
6. **USB enumeration vs BLE init:** bring up USB/HID first in `main()`, defer `bt_enable()`+flash to a thread ~3 s later, or cold-plug enumeration fails ("USB device not recognized").
7. **Signing key:** builds use NCS's default (public) MCUboot dev key — the warning in the build log. The button-held arm gate is what actually protects updates (the user accepts this). For production, generate a private key and set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE` (then every board needs MCUboot rebuilt/reflashed once).
8. **MDBT50Q is built as `nrf52840dk`, which drags in the DK's peripherals** — disable anything the Raytac board doesn't have. The DK's QSPI flash (SCK = P0.19, high-drive) plus the DK's `gpio-as-nreset` (boot code writes UICR.PSELRESET = P0.18) **reset-looped and bricked two units**: P0.19 is wired to nRESET on this board, so QSPI init pulled reset low before `main()` → no LED/USB, J-Link "Found SW-DP / Failed to power up DAP" (CTRL/STAT 0xD0000040: CDBGPWRUPACK never set, so CTRL-AP/`--recover` can't work either). Fixed in both overlays (`&qspi`/`&mx25r64` disabled, `/delete-property/ gpio-as-nreset`). Before adding any DK-inherited peripheral, check its pins against the Raytac schematic (`C:\_Datasheets\BLE Modules\RayTac\Dongle Sch.png`).

## Key config files
- `prj.conf`: nrf_security/PSA (ECDH P-256, key generate/export/derive, random), BT observer+peripheral, MTU 498 / ACL 502 / DLE 251, USB next stack + HID, GPIO/INPUT, `CONFIG_FLASH`, MCUmgr (`ZCBOR`, `NET_BUF`, `MCUMGR_TRANSPORT_BT(+REASSEMBLY, CONN_PARAM_CONTROL)`, `GRP_IMG`, `GRP_OS`, notification + SMP command hooks), `IMG_MANAGER`, `REBOOT`; logging OFF.
- `sysbuild.conf`: `SB_CONFIG_BOOTLOADER_MCUBOOT=y`. `sysbuild.cmake`: dongle migration package.
- `sysbuild/mcuboot/`: MCUboot's config dir (copy of its stock `prj.conf` + per-board: dongle = stock USB CDC serial recovery on SW1; DK = `.conf` QSPI driver off + RC LF clock, `.overlay` QSPI node off + no `gpio-as-nreset`).
- `pm_static_nrf52840dongle_nrf52840.yml`, `pm_static_nrf52840dk_nrf52840.yml`: flash layout above.

## Android app
Compose UI: Password (short press), Username 2 + Password 2 (1-second press, optional) (passwords have eye show/hide), secure beacon ID text, Green/Blue dBm, "Save & Send to Dongle", **"Update dongle firmware"** (dialog: pick Official dongle / MDBT50Q, shows the bundled version; progress bar), Start/Stop beacon. `BeaconService` advertises the rolling beacon. It uses an `AdvertisingSet` and re-signs it every 1 s while the CPU is awake. When asleep it relies on a `setAndAllowWhileIdle` alarm (~60 s) and `SCREEN_ON`/`USER_PRESENT`. The counter is `max(ctr+1, now)`, saved every 60 s, and restarts at `saved+61`. The app **no longer changes the phone's Bluetooth name**. `BeaconCrypto` holds the pure format (JVM-tested). `BeaconKey` holds the key, AES-GCM-wrapped by an Android Keystore key in prefs `ble_password_key`; it's recoverable, so one phone can pair several dongles. `ConfigClient` scans for the held dongle and connects. Then either:
- `send`: read key ID, read the Session public key → `ConfigSession.start` (ECDH), then write the phone's public key, then the sealed key if needed, then the sealed credentials and the thresholds;
- `armDfu`: read the board and arm DFU. `FirmwareUpdater` = Nordic `mcumgr-ble` 3.4.1 `FirmwareUpgradeManager` in `TEST_ONLY` mode. `Firmware.kt` loads `assets/firmware/{nrf52840dongle,mdbt50q}.zip` and checks the manifest board. **The dongle's button must be held while saving / starting an update** (gate).
- Firmware bundling: `bundleFirmware` task in `app/build.gradle.kts` copies `<repo root>/build_{nRF52840,mdbt50q}/dfu_application.zip` (default `firmwareDir` = `android/..`) into generated assets (override with `-PfirmwareDir=...`). Missing zip → warning, that PCB shows "not bundled".
- mcumgr 3.x requires **compileSdk 37** and **Kotlin 2.4.10** (both set; targetSdk still 36).
- **Credentials are never stored on the phone** (2026-10-28): the password fields start blank and are cleared after a successful send. Blank = "keep the dongle's current value" (not written). The "Remove Username 2 + Password 2" checkbox sends empty writes to clear login 2. Only the thresholds are saved in SharedPreferences, plus the beacon counter and the wrapped beacon key. `Config.purgeSecrets()` deletes plaintext credentials that older app versions saved. Password fields use `KeyboardType.Password` (no IME learning).
- **No backup:** `allowBackup="false"`, and `data_extraction_rules.xml` / `backup_rules.xml` exclude everything (allowBackup=false alone doesn't stop Android 12+ device-to-device transfer).
- Build: **release**, `gradlew.bat :app:assembleRelease` (JAVA_HOME = Android Studio JBR); install `adb install -r app\build\outputs\apk\release\app-release.apk`. A release build is not debuggable, so `adb shell run-as` can't read the app's data. That route was verified readable on the old debug build, without root. Signing key: `%USERPROFILE%\.android\ble_beacon_release.jks` plus `ble_beacon_keystore.properties` (passwords). Both are **outside the repo — back them up**. A lost key means uninstall + reinstall, and the beacon/threshold settings must be re-entered. Override the location with `-PreleaseKeystoreProps=...`. Debug builds (`assembleDebug`) are signed with a different key, so switching between debug and release requires `adb uninstall com.example.ble_beacon` first.

## Status / TODO
- [x] MCUboot + BLE SMP update on both boards; app bundles both images.
- [x] **Official dongle (2026-10-28): fully verified on hardware.** Migrated to MCUboot (nrfutil on COM20, then smpmgr over MCUboot recovery on COM75). BLE updates from the app (Galaxy S21 Ultra) 1.1.0 → 1.1.1 → 1.1.2 → 1.1.3 all worked: progress bar, ~15 s dark during the MCUboot swap, then the config window; `confirmed=1`; saved config survived every update and a power cycle (so the Nordic bootloader → MCUboot → app chain is fine). **Now running v1.1.4** (second password); password-hiding when not GREEN verified.
- [x] **MDBT50Q: both units are bricked (scrap)** by the QSPI/reset-pin reset loop (constraint 8). Unit 1 by the original standalone app; unit 2 by `merged.hex` v1.1.3. Software recovery is impossible (CTRL-AP unreachable); the P0.19↔RESET link is between module pads 40 and 42 **under the module shield**, so only hot-air removal of the module could save them. Details below.
- [ ] **MDBT50Q: flash a NEW unit** with the fixed build (recover first — see "3rd-party board first flash"), verify over RTT, then test a BLE update from the app. The fixed image (QSPI off, no `gpio-as-nreset`) has not run on hardware yet.
- Investigation notes (2nd unit, 2026-10-28): J-Link reached it fine (FICR read OK), `merged.hex` v1.1.3 programmed + verified with `--sectorerase`, then after reset: no LED, no USB, J-Link "Found SW-DP ID 0x2BA01477 / Failed to power up DAP", `--recover` times out (also at 1 MHz). Ruled out: LF clock (RC in both app + MCUboot), UICR in the hex (none), supply (T3–T4 steady 3.3 V), old tools (nrfjprog 10.24.2 / J-Link 8.96). DCDC REG0/REG1 identical in working `led_test`. **Board quirk: P0.19 is tied to P0.18/nRESET on the dongle PCB** — same as Nordic's PCA10059 (`BSP_SELF_PINRESET_PIN` = P0.19, lets firmware pin-reset itself), confirmed by a Nordic DevZone answer (thread 96060) citing Raytac's reference circuit; in the schematic the box under pins 41/42 joins pin 40 (RESET) to 42 (P0.19). The DK's QSPI flash has SCK on P0.19 (high-drive), and the DK DT sets `gpio-as-nreset` (boot code writes UICR.PSELRESET=P0.18) → the app's QSPI init held reset low → reset loop before `main()` (no LED/USB, "Failed to power up DAP"). **Fixed** (built + bundled in the app, not yet run on hardware): `&qspi`/`&mx25r64` disabled and `gpio-as-nreset` deleted in `boards/nrf52840dk_nrf52840.overlay` and `sysbuild/mcuboot/boards/nrf52840dk_nrf52840.overlay`. **Software recovery of the reset-looping units FAILED (2026-10-28):** DP answers (IDR 0x2BA01477) but CTRL/STAT stays 0xD0000040 = CSYSPWRUPACK set, **CDBGPWRUPACK never set**, so CTRL-AP is unreachable (its IDR reads 0) and ERASEALL can't be issued — tried nrfjprog `--recover` (also 1 MHz), pyOCD `erase --mass`, and raw CTRL-AP via J-Link Commander and pyOCD register-level (`scratchpad ctrlap_unlock.py`). Recovery needs the loop stopped in hardware (break the P0.19↔P0.18 link) — but per the module pad map (`Raytac_MDBT50Q.pdf` p7), pins 40 (RESET) and 42 (P0.19) are **adjacent pads in the inner right-hand column, under the module shield**; photos of both PCB sides (`RayTac\PCB_Top/Bottom.heic`) show no exposed link. Only fix is hot-air removal of the module → the two looping units are effectively scrap. (Note: J-Link Commander's raw `SWDReadAP` returned 0 for everything here because the debug domain was never powered — its "writes" never landed.) APPROTECT (rev 3) was also on (`APPROTECTSTATUS` 0) — our firmware only SW-unlocks SWD when UICR.APPROTECT=HwDisabled, hence recover first on new units. The Gerber in `MDBT50Q-RX Gerber for Programming Jig (210205)` is the pogo-pin jig (no copper), not the dongle layout; Raytac doesn't publish the dongle's PCB files (ask Raytac support). Raytac's MDBT50Q-RX approval sheet (Ver. E, 2020-06-19, §8 Reference Circuit) draws the same 41–42 tie, and says **"External 32.768 kHz is already inside the dongle"** — so the crystal IS fitted, and the earlier "no LED" on unit 1 was most likely this QSPI contention, not the LFXO (the RC clock setting is harmless; keep or revert). Same sheet: REG0 DC/DC + LDO disabled by default (VDDH tied to VDD = normal-voltage mode).
- [x] **v1.1.4 = second password** (medium ~1 s press), built for both boards; the app (Password 2 field) is built with both 1.1.4 images bundled. The MDBT50Q 1.1.4 image keeps the QSPI/reset fix (verified in `.config`).
- [x] **Verified on the official dongle (2026-10-28):** app installed, BLE update 1.1.3 → 1.1.4, and Password 2 set from the app. Short press types password 1 and a ~1 s press types password 2 — user confirmed "works great".
- [x] **v1.1.5 = Username 2** (medium press types username → TAB → password 2 → Enter, for a database login form). Built for both boards (MDBT50Q still has no QSPI/pin-reset), and the app with the Username 2 field is installed on the phone.
- [ ] Verify 1.1.5 on the official dongle: after the BLE update the dump shows `fw=1.1.5` and `username2=(not set)` with the passwords kept (v4→v5 record), then set Username 2 and test the login.
- [ ] **NEXT (left off 2026-10-28):**
  - **(a) GitHub push:** the local commits are not pushed. The push got a 403 because local git is signed in as RambookaICT. Either add RambookaICT as a collaborator on Rambooka/BLE_Password, or the user pushes signed in as Rambooka.
  - [x] **(b) Android app is in `android/` (2026-10-29).** The folder couldn't be moved: something held a handle on the `BLE_Beacon` root folder itself. No file in it was open, and no visible process had it as its current directory, even after the Gradle and Kotlin daemons were stopped. So it was **copied** and verified by SHA-256 (58/58 files). Done: `firmwareDir` now defaults to the repo root, `android/.gitignore` ignores `.idea/`, `.kotlin/`, `app/google-services.json` and keystores, and the build and lint run from the new location. **The old copy `C:\Users\rabram\Source\Android\BLE_Beacon` is stale. Delete it once it's unlocked (e.g. after a reboot). Don't edit it.**
  - [x] Removed Android Studio's unused "Baking with Gemini" template screen, along with Firebase, the google-services plugin and viewmodel-compose (2026-10-29). Verified that a fresh `git clone` builds with no `google-services.json`. The local `android/app/google-services.json` is now unused but still git-ignored, because it contains a Firebase API key.
  - [x] **(c) `Readme.md` rewritten for the public repo (2026-10-29).** Keep it in step with behaviour changes: button timings, LEDs, status dump fields, GATT table.
- [x] **v1.1.6:** the status dump prints `averaged_rssi=` on its own line (`gone` after the 5 s beacon timeout).
- [x] **v1.2.0 = secure rolling beacon + office coexistence (2026-10-29).** Built for both boards (MDBT50Q still has QSPI/pin-reset off), host tests and Android unit tests pass, lint clean, and the app is built with 1.2.0 bundled. **Not yet run on hardware.**
- [x] **1.2.0 partly verified on the official dongle (2026-10-29):**
  - BLE update 1.1.x → 1.2.0, `confirmed=1`.
  - Save with the button held paired the dongle (`beacon_key=07abd2e3`) and set the passwords and username 2 through the **ECDH-sealed** writes. So PSA ECDH works on hardware, and `saved=1`.
  - Dump: `beacons=110 match=110 stale=0`, GREEN at −57 dBm.
  - Stopping the beacon → BLUE + lock in ~10 s (5 s timeout plus Android's stop latency). Restarting it → GREEN in a few seconds (fresh counter > floor); the button typed and unlocked the PC.
  - **Real walk-away with the phone in a pocket (2026-10-29):** the PC locked, and on return the dongle went GREEN and unlocked. So the counter advances fast enough while the phone sleeps (alarm / screen-on).
  - **Unplug/replug (2026-10-29):** after a reboot (tap to skip the config window), the dump showed `floor=1793218868`, restored from the flash counter log rather than 0. `ctr=1793219016` was accepted, `match=28 stale=0`.
- [ ] Still to test on 1.2.0 (optional): a second phone running the app doesn't trigger it (`beacons=` rises, `match=` doesn't).
- [ ] (Original 1.2.0 test plan, kept for reference.) The order matters:
  1. Install the new app (it stops sending the beacon name).
  2. Update the dongle to 1.2.0. The dongle stays BLUE until it's paired.
  3. Hold the button and Save: the app pairs, then 3 green blinks.
  4. Expect GREEN, and the status dump shows `beacon_key=` matching the app's ID, with `match` rising.

  Then test:
  - walk away → lock;
  - come back → GREEN without touching the phone (if it stays blue, check `stale` and how fast the phone ticks);
  - a second phone running the app doesn't trigger it (`beacons` rises, `match` doesn't);
  - unplug/replug → `floor` restored and the phone still accepted.
- [x] **Encrypted config session (ECDH, 2026-10-29), part of 1.2.0.** Host tests (12 KDF/seal vectors) and Android tests pass, including a real JCA ECDH round trip against a stand-in dongle. **The firmware's PSA ECDH has not run on hardware yet.** First test:
  - a Save that sets a password should succeed;
  - if it fails with "invalid session key" or write rejected 0x13, suspect PSA or Oberon;
  - a crash (fast red flash) on Save would point to a stack overflow in BT RX.

  This was chosen over BLE LE Secure Connections, which would mean a pairing popup on every save and needs bond storage (no NVS here).
- [ ] Possible follow-up: authenticate the ECDH (e.g. LED-blink or app-code comparison) to also stop an active relay during the config window.
- [ ] Decide whether to drop RTT/logging on the MDBT50Q production build.
- [ ] Production signing key (constraint 7) — deferred; the button gate is accepted for now.

MDBT50Q board files: `boards/nrf52840dk_nrf52840.overlay` (LED P1.13 as led0, button P0.15 as sw0, HID node, USB on, DK's own buttons disabled, **QSPI flash disabled, `gpio-as-nreset` deleted**) and `boards/nrf52840dk_nrf52840.conf` (RC LF clock + RTT logging). The RC clock was added on the wrong theory that the module has no 32 kHz crystal — it does; RC works fine, so keep it or switch back to the crystal (better accuracy, lower power). `src/main.c` uses single-LED blink patterns via `CONFIG_BOARD_NRF52840DK` → `SINGLE_LED`.

### Note on APPROTECT / "Failed to power up DAP" (nRF52840)
- **Locked but running chip** (rev 3 APPROTECT): J-Link reports protection; `nrfjprog --recover` works (full erase, sets UICR.APPROTECT=HwDisabled). After recover use `--program ... --sectorerase` (not `--chiperase`, which erases UICR) so the board stays debuggable.
- **"Found SW-DP" but "Failed to power up DAP" and `--recover` times out**: the chip is reset-looping (debug domain never acks power-up) — not APPROTECT. On the MDBT50Q that was the P0.19→RESET loop (constraint 8). Software can't fix it; the loop must be stopped in hardware.
