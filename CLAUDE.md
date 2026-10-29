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
| **0xDC000–0xE0000** | `config_storage`: **unused since fw 1.3.0** (RAM-only config — nothing is persisted; the partition is still reserved in the pm_static maps). Up to 1.2.x it held the raw config record at 0xDC000 plus the beacon counter log at 0xDD000/0xDE000 |
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
Outputs per build dir: `dfu_application.zip` (signed app + manifest = BLE update package, bundled into the Android app), `merged.hex` (MCUboot + app, for J-Link), `BLE_Password/zephyr/zephyr.signed.bin`. Dongle build also makes `dongle_mcuboot_migration.zip` (Nordic-bootloader package containing **only MCUboot**). Version comes from `VERSION` (currently 1.3.0) → shown in the app and in the status dump. **Always bump it for a new image**: the app's updater silently skips the upload if the dongle already runs an image with the same hash, and two different images with the same version can't be told apart.

**Lint + unit tests — run after EVERY code change (firmware or app), before committing.** `lint.ps1` also runs `tests/host/run.ps1`, which builds every `tests/host/*_test.c` against the pure-C modules (`beacon_auth.c`, `cfg_crypto.c`) with Strawberry `gcc`, using FIPS/RFC/Python vectors. It also runs `:app:testDebugUnitTest` (`BeaconCryptoTest`, `ConfigSessionTest`). The firmware and app vectors are shared, so a change to the beacon or session format must update both sides' tests. New pure-C modules go in `run.ps1`'s `$modules` list.
```
powershell -ExecutionPolicy Bypass -File tools\lint.ps1             # both
powershell -ExecutionPolicy Bypass -File tools\lint.ps1 -Firmware   # after a west build of both boards
powershell -ExecutionPolicy Bypass -File tools\lint.ps1 -Android [-AndroidDir <app dir>]
```
- **Firmware = clang-tidy** (`C:\Program Files\LLVM\bin\clang-tidy.exe`, LLVM 18) on `src/*.c`, for **both** boards (main.c has board `#ifdef`s). The script reuses each build's `compile_commands.json`, strips GCC-only flags and adds `--target=arm-none-eabi`, so build first. Checks are in `.clang-tidy`: bugprone/cert/clang-analyzer/misc, with Zephyr noise off and the reason given for each. Target: **zero findings**. Fix real issues. Silence false positives with a targeted `NOLINTNEXTLINE(check): reason` (as on the `K_THREAD_DEFINE` lines), not by disabling checks globally.
- **App = Android Lint** (`gradlew :app:lintRelease`, report `app\build\reports\lint-results-release.html`). Target: **zero errors** (errors fail the task). Don't add new warnings. Known existing warnings (~19, as of 1.3.0): dependency/AGP upgrade suggestions (these drift upward over time as upstream releases, independent of our code), `OldTargetApi`, unused template colours, `UseKtx`/`UseTomlInstead`/`RedundantLabel` style. Upgrading dependencies is a separate, deliberate change, not a lint fix.
- First run (2026-10-29) found and fixed:
  - firmware: 4× `strcpy` → size-asserted `memcpy`, RSSI sum `int16_t` → `int32_t`;
  - app: the API 33+ `writeCharacteristic` result was compared to `GATT_SUCCESS` instead of `BluetoothStatusCodes.SUCCESS` (both 0, so it worked by luck); `remember { purgeSecrets() }` → `LaunchedEffect`.

**Release flow:**
1. Bump `VERSION`.
2. Build both boards.
3. Run `tools\lint.ps1`.
4. Build the release APK (it copies both `dfu_application.zip` in), then install and test on hardware.
5. Commit and push.
6. Annotated tag `vX.Y.Z` + `git push origin vX.Y.Z`.
7. Run `pwsh -File tools\make_release.ps1 -Tag vX.Y.Z -Notes <notes.md>`. It creates the GitHub Release via the REST API, using git's stored RambookaICT token; `gh` isn't installed. It uploads the APK, both DFU zips, the dongle migration zip + `zephyr.signed.bin`, and the MDBT50Q `merged.hex`. Use **pwsh 7**, not Windows PowerShell 5: PS5 garbles `git credential fill`'s stdin.

Users then update from the app. Releases: https://github.com/Rambooka/BLE_Password/releases (v1.2.1 = first release, 2026-10-29).

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
- **RAM-only secrets (fw 1.3.0+):** the password, password 2, username 2 and the 16-byte beacon key live in **RAM only — never in flash**. A powered-off/unplugged dongle holds no secret at all. The price: the phone must re-send them (button held) after **every** power-up / reboot / firmware update. Only the two RSSI thresholds would be worth persisting, and even those aren't: **nothing is written to flash** (`config_storage` is unused), the phone re-sends the thresholds too, and the dongle boots with compiled defaults until paired. See "Hardware constraints" #4.
- **Boot config window:** advertise a connectable GATT config service; LED blinks blue (official). The Android app connects and writes settings. **Writes are only accepted while the dongle's button is physically held down** — the hold is the proof the writer owns the dongle. **The window stays open until the phone has delivered the essential secrets (password AND beacon key), then closes when the app disconnects** — it does NOT time out into a blank/useless normal mode, and a button tap no longer exits it. (A firmware-update arm still has its own 3-min deadline.) Completed pairing → 3 green blinks (safe to release the button); an invalid threshold combo → 6 fast red/ALERT blinks and the thresholds revert to defaults.
- **Rolling beacon (fw 1.2.0+, `src/beacon_auth.[ch]`):** the dongle no longer matches a beacon *name*. The phone advertises manufacturer data with company ID 0xFFFF, laid out as `'B' 'P' 01 ctr[4 LE] tag[8]`, where `tag = HMAC-SHA256(key, 'B' 'P' 01 ctr)[0..8]`.
  - **Key:** 16 random bytes per phone, written through GATT `…000A` with the button gate. **RAM only (1.3.0+): re-sent every boot** — the key-ID read returns empty after a power-cycle, so the app always re-pairs. Without a key the dongle never accepts any beacon.
  - **Counter:** wall-clock seconds (`currentTimeMillis`, UTC, so time zones and DST don't matter), strictly increasing on the phone.
  - **Acceptance:** valid tag, and `ctr >= last accepted`. Equal is allowed because a sleeping phone repeats its last packet. Also `ctr > floor`.
  - **Floor (RAM only, 1.3.0+):** set to the last accepted counter at **every lock** (left GREEN, or beacon lost); starts at 0 each boot. So within a session, after the owner leaves only a newer counter brings the dongle back; a recording can't. The counter log in flash was **removed in 1.3.0** — cross-reboot replay is covered instead by the RAM-only key: after a reboot no beacon is accepted until the owner re-provisions (present, with a ~now counter), which ratchets `last accepted` past any older recording.
  - **Coexistence:** colleagues' phones fail the tag check. They only increment `beacons=`.
  - **Config advert:** carries `FF FF 'B' 'P' 10 <flags>`, bit 0 = button held, so the app connects only to the dongle being held.
  - Self-implemented SHA-256 / HMAC, host-tested against FIPS 180-4, RFC 4231 and a Python vector shared with the Kotlin test.
- **Normal mode:** scan for the paired phone's rolling beacon. Proximity by averaged RSSI (4 dB hysteresis so a borderline signal doesn't flip-flop zones) → blue (not seen) / green (close, `>= green threshold`) / red (far). Button released while green, by press length (`MEDIUM_PRESS_MS` / `LONG_PRESS_MS`): **< 600 ms → password 1** + Enter; **600 ms–2.5 s → [username 2 + TAB +] password 2** + Enter (login forms; the username part only if set; nothing if password 2 isn't set); 2.5–5 s → nothing (back out). Leaving green (→ red **or** → blue) or losing the beacon (5 s) → Win+L.
- **Status:** hold button 5 s → types a full status/diagnostics dump over HID into a focused text editor. First line `fw=<VERSION> board=<CONFIG_BOARD> confirmed=<0|1>` (confirmed=0 → MCUboot rolls back on next reset). The second line has `prov=<0|1>` — whether this boot's secrets have been received (RAM-only); the old `saved=` field is gone (nothing is persisted). The credential lines (`password=`, `username2=`, `password2=`) appear only while GREEN, else `password=(hidden: phone not in range)`. **Since 1.2.2 passwords are masked**: first char + `***` + last char (`mask_secret`), or `***` if under 4 chars, or `(not set)`. Username 2 is shown in full. Never type a full password in the dump: anyone at the desk can hold the button while the owner is nearby. `boot=<power|software|pin|watchdog|other> watchdog=<on|OFF>` shows the last reset cause. `ads= beacons= match= stale=` (all adverts / rolling beacons from any phone / ours verified+fresh / ours with an old counter), `beacon_key=<id> ctr= floor=` (or `(not set: pair from the app)`). `averaged_rssi=` is the 20-sample running average (named so it isn't mistaken for a setting); it prints `gone` once the beacon hasn't been seen for `BEACON_TIMEOUT_MS` (5 s) — fw 1.1.6+ (older firmware printed a stale `rssi=`).
- **Firmware update (BLE, both boards):** the app reads the board characteristic (must match the chosen image), then writes `0x01` to DFU-arm (button held). That unlocks MCUmgr/SMP **for that phone's address only**, extends the window to 3 min, and re-advertises after the arming connection drops. LED fast-blinks while armed. The app reconnects over SMP, uploads to slot 1, marks it *test*, and resets. MCUboot swaps; the new app **self-confirms** at boot (`boot_write_img_confirmed()` before `bt_enable`) — if it crashes before that, MCUboot rolls back on the next reset. SMP commands are refused at all other times (`smp_gate_cb`, `MGMT_EVT_OP_CMD_RECV`).
- **Watchdog / fail closed (1.2.2+):**
  - **Watchdog:** hardware WDT (`wdt0`), 8 s timeout, fed every 1 s by a system-workqueue job and by every `send_key()`. The 5 s status dump runs on the workqueue and takes seconds, so it would starve the job otherwise.
  - **Lock on watchdog boot:** the app reads and then clears `hwinfo` reset cause at boot (RESETREAS accumulates). If the cause was the watchdog, the `ble_init` thread sends **Win+L** once HID is ready: the dongle may have missed the owner leaving.
  - **MCUboot:** it already feeds the WDT during swaps (`BOOT_WATCHDOG_FEED` + `NRFX_WDT`, on by default for nRF), so BLE updates are safe.
  - **Survives soft resets:** the nRF WDT can't be stopped and keeps running through a soft reset (SMP reset). A WDT reset clears it.
  - **What MCUboot clears:** MCUboot clears the reset cause only after a pin reset, so the app still sees `RESET_WATCHDOG`.
  - **Not covered:** the dongle being unplugged. Recommend Windows Dynamic Lock as a backup.
- **Crash = fast red flash** (`k_sys_fatal_error_handler`, interrupts off). Nothing feeds the WDT, so within 8 s it resets and that boot locks the PC. A boot BLE-init failure shows a red blink code (1 = bt_enable, 2 = scan) 5 times, then `sys_reboot` retries.
- **ASCII→HID**: US-layout map, so any typed password works.

## GATT config service (UUIDs must match the Android app)
- Service `9f1b0001-5c3a-4e8e-9c2a-1a2b3c4d5e6f`
- Password (write, UTF-8) `...0002`. (`...0003` was the beacon name; removed in 1.2.0.)
- Green threshold (write, 1 signed byte dBm) `...0004`; Blue threshold `...0005`
- DFU arm (write `0x01`) `...0006` — unlocks SMP upload (see above). Same button-held gate.
- Board (read, string = `CONFIG_BOARD`: `nrf52840dongle` / `nrf52840dk`) `...0007` — no gate. Matches the `board` field in `dfu_application.zip`'s manifest.
- Password 2 (write, UTF-8, **empty write = clear**; optional) `...0008` — fw 1.1.4+. Username 2 (same rules) `...0009` — fw 1.1.5+. The app treats each as optional when blank.
- Beacon key (write, exactly 16 bytes, gated) `...000A` and Key ID (read, 4 bytes = `HMAC(key,"BLEPW-KEYID")[0..4]`, empty = unpaired, no gate) `...000B`, both fw 1.2.0+. The app reads the key ID first and writes the key only if it differs. **Since 1.3.0 the key is RAM-only, so after every power-cycle the key ID reads empty and the app re-sends the key** (each boot, not once per dongle). Missing `...000B` → the app refuses to configure ("older than 1.2").
- **Encrypted session `...000C` (fw 1.2.0+, `src/cfg_crypto.[ch]` + `ConfigSession.kt`):**
  - **Key exchange:** read returns the dongle's P-256 public key, 65 bytes uncompressed. It's generated once per config window with PSA: `psa_generate_key`, ECDH via Nordic **Oberon** in software, TRNG from CC3XX. Write takes the phone's fresh public key (also 65 bytes). The dongle then runs `psa_raw_key_agreement` (which validates the point) and derives `HKDF-SHA256(salt "BLEPW-SESSION-v1", Z, info dongle_pub‖phone_pub)` → `k_enc ‖ k_mac`.
  - **Sealed writes:** Password `0x02`, Password2 `0x08`, Username2 `0x09` and BeaconKey `0x0A` must arrive as `seq[4 LE] ‖ ct ‖ tag[16]`. The ciphertext is XORed with an `HMAC(k_enc, id‖seq‖blk)` keystream; `tag = HMAC(k_mac, id‖seq‖ct)[0..16]`. Seq must rise within the session. Anything else is rejected with ATT 0x13. Not `0x0F`: Insufficient Encryption would make Android start its own pairing.
  - **Lifetime:** the session is cleared on disconnect, and the key pair is destroyed when the window closes.
  - **Plaintext:** thresholds and the DFU arm stay unencrypted.
  - **Threat model:** this defeats passive sniffing, but not an active relay during the window (unauthenticated ECDH).
  - **Threads:** ECDH runs in the BT RX thread, so `CONFIG_BT_RX_STACK_SIZE=4096`, the system workqueue is 4096, and the `ble_init` thread is 4 KB.
  - **Size:** the dongle app is now 52.6% of the 408 KB slot and uses 29% of RAM.
- Plus the standard MCUmgr SMP service (locked unless armed).
- Dongle advertises name `BLE_Password` (scan response) plus the button-held manufacturer data. App scans unfiltered and matches by UUID **or** name (Samsung offloaded 128-bit filters are unreliable). It connects **only** to a dongle advertising "held" (1.2+). The old 3 s fallback to an unflagged dongle was removed, so pre-1.2 dongles can't be configured or updated from the app any more; use J-Link / USB recovery for those.
- **Secrets are write-only over BLE and RAM-only on the dongle (1.3.0+):** the password, password 2, username 2 and beacon-key characteristics have no read handler, and SMP has only the IMG/OS groups (no fs/settings/memory read). Secrets live only in RAM and leave the dongle only as HID typing while GREEN. Keep it that way: never add a read handler, never write a secret to flash, and never add an SMP group that can read RAM/flash.
- Writes only accepted during the config window **and while the button is held** (physical-ownership gate). Thresholds are sanity-checked (−120…0 dBm per value on each write; green > blue at window close — a bad combo reverts to the compiled defaults). Nothing is persisted, so there is no save-time or load-time validation any more.

## Hardware constraints discovered (IMPORTANT — the hard-won bits)
1. **No serial/RTT on the official dongle** (HID-only USB, no debugger) → debugging was via LED patterns + a HID "type the status" dump.
2. **NVS/settings "hang"** — root cause was almost certainly that Partition Manager put `storage` at 0xFE000, *inside* the dongle's protected Nordic bootloader region. Still don't use NVS/settings. (As of 1.3.0 the app persists nothing at all — see #4.)
3. **Scan + connectable advertising cannot run together** — second role returns `-EINVAL`. Hence the **boot config window** (advertise, then switch to scanning).
4. **No persistence (RAM-only config, 1.3.0+).** The app writes nothing to flash: all secrets AND the thresholds live in RAM and are re-sent by the phone every boot (that's the whole point — a lost/unplugged dongle holds no secret). `config_storage` (0xDC000) is left reserved but unused; `src/main.c` no longer includes `flash.h`/`pm_config.h` or touches the flash controller. **`CONFIG_FLASH` stays on** because MCUmgr/SMP still writes slot 1 during a BLE firmware update, and for the same reason flash keeps the default **MPSL radio sync** (`CONFIG_SOC_FLASH_NRF_RADIO_SYNC_NONE` not set) — the ~85 ms erase stall would otherwise fault the controller mid-connection.
   - History: 1.2.x persisted a raw `persist_cfg` record (magic `0xB1E50006`: thresholds + password/password2/username2 + beacon key) at 0xDC000, plus a beacon counter-log (ping-pong pages 0xDD000/0xDE000) for cross-reboot replay protection. 1.3.0 removed all of it. If you ever reintroduce persistence, do NOT store secrets; a reserved partition and the old append-only-record discipline are still the pattern to follow.
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
Compose UI: Password (short press), Username 2 + Password 2 (1-second press, optional) (passwords have eye show/hide), secure beacon ID text, Green/Blue dBm, "Save & Send to Dongle" / "Unlock & Send to Dongle", **"Update dongle firmware"** (dialog: pick Official dongle / MDBT50Q, shows the bundled version; progress bar), Start/Stop beacon. `MainActivity` is a `FragmentActivity` (needed by `BiometricPrompt`). `BeaconService` advertises the rolling beacon. It uses an `AdvertisingSet` and re-signs it every 1 s while the CPU is awake. When asleep it relies on a `setAndAllowWhileIdle` alarm (~60 s) and `SCREEN_ON`/`USER_PRESENT`. The counter is `max(ctr+1, now)`, saved every 60 s, and restarts at `saved+61`. The app **no longer changes the phone's Bluetooth name**.

Staying on (1.2.2):
- The beacon's on/off choice is a pref (`Config.isBeaconEnabled`).
- `BootReceiver` (BOOT_COMPLETED, MY_PACKAGE_REPLACED; exported, but only protected system broadcasts) restarts the service with `BeaconService.start`, using `startForegroundService`. The connectedDevice FGS type is allowed from boot.
- The service listens for `BluetoothAdapter.ACTION_STATE_CHANGED`: STATE_ON → `startAdvertising()`, which first stops any old set so the callback isn't double-registered.
- Start Beacon asks for `REQUEST_IGNORE_BATTERY_OPTIMIZATIONS` (`@SuppressLint("BatteryLife")`: sideloaded, so the Play policy doesn't apply). On Samsung, also set battery to "Unrestricted". `BeaconCrypto` holds the pure format (JVM-tested). `BeaconKey` holds the key, AES-GCM-wrapped by an Android Keystore key in prefs `ble_password_key`; it's recoverable, so one phone can pair several dongles. `ConfigClient` scans for the held dongle and connects. Then either:
- `send`: read key ID, read the Session public key → `ConfigSession.start` (ECDH), then write the phone's public key, then the sealed key if needed, then the sealed credentials and the thresholds;
- `armDfu`: read the board and arm DFU. `FirmwareUpdater` = Nordic `mcumgr-ble` 3.4.1 `FirmwareUpgradeManager` in `TEST_ONLY` mode. `Firmware.kt` loads `assets/firmware/{nrf52840dongle,mdbt50q}.zip` and checks the manifest board. **The dongle's button must be held while saving / starting an update** (gate).
- Firmware bundling: `bundleFirmware` task in `app/build.gradle.kts` copies `<repo root>/build_{nRF52840,mdbt50q}/dfu_application.zip` (default `firmwareDir` = `android/..`) into generated assets (override with `-PfirmwareDir=...`). Missing zip → warning, that PCB shows "not bundled".
- mcumgr 3.x requires **compileSdk 37** and **Kotlin 2.4.10** (both set; targetSdk still 36).
- **Credentials stored on the phone behind a fingerprint (1.3.0, `CredentialStore.kt`):** because the dongle is now RAM-only and forgets its secrets every power-cycle, the phone must re-send them each boot — so it holds them, encrypted. `CredentialStore` serialises password + username 2 + password 2 into one blob and seals it with an **AES-GCM Android Keystore key created with `setUserAuthenticationRequired(true)` + per-operation auth** (`setUserAuthenticationParameters(0, AUTH_BIOMETRIC_STRONG)` on API 30+, else `setUserAuthenticationValidityDurationSeconds(-1)`) and `setInvalidatedByBiometricEnrollment(true)`. Save and send each require a fresh `BiometricPrompt` (BIOMETRIC_STRONG) wrapping the `Cipher` in a `CryptoObject`. UI model: **type a password = save-new (replaces the whole saved set) + send; leave blank with a set saved = unlock & send** (the usual every-plug-in case). At rest even root can't export the key; root can still read the plaintext from app memory at the instant it's sent (inherent to root). This reverses the earlier "credentials never stored on the phone" design — the trade for not retyping the password every boot. The **beacon key** stays wrapped by a *non-auth* Keystore key (`BeaconKey.kt`), because the beacon must sign in the background with no user present. Thresholds + beacon counter are still plain SharedPreferences. **minSdk 26** (Android 8): `AdvertisingSet` only; the Android 7 advertising restart path and the `mipmap-anydpi` (non-adaptive) icons are gone. Password fields use `KeyboardType.Password` (no IME learning).
- **No backup:** `allowBackup="false"`, and `data_extraction_rules.xml` / `backup_rules.xml` exclude everything (allowBackup=false alone doesn't stop Android 12+ device-to-device transfer).
- Build: **release**, `gradlew.bat :app:assembleRelease` (JAVA_HOME = Android Studio JBR); install `adb install -r app\build\outputs\apk\release\app-release.apk`. A release build is not debuggable, so `adb shell run-as` can't read the app's data. That route was verified readable on the old debug build, without root. Signing key: `%USERPROFILE%\.android\ble_beacon_release.jks` plus `ble_beacon_keystore.properties` (passwords). Both are **outside the repo — back them up**. A lost key means uninstall + reinstall. That also wipes the app's Keystore-wrapped **beacon key** AND the biometric-sealed **credential store**, so the phone gets a new beacon ID and forgets the passwords: re-enter the credentials, re-pair every dongle (hold the button + Save), and re-enter the thresholds. Override the location with `-PreleaseKeystoreProps=...`. Debug builds (`assembleDebug`) are signed with a different key, so switching between debug and release requires `adb uninstall com.example.ble_beacon` first (same consequence: re-pair the dongles).

## Status / TODO
- [x] MCUboot + BLE SMP update on both boards; app bundles both images.
- [x] **Official dongle (2026-10-28): fully verified on hardware.** Migrated to MCUboot (nrfutil on COM20, then smpmgr over MCUboot recovery on COM75). BLE updates from the app (Galaxy S21 Ultra) 1.1.0 → 1.1.1 → 1.1.2 → 1.1.3 all worked: progress bar, ~15 s dark during the MCUboot swap, then the config window; `confirmed=1`; saved config survived every update and a power cycle (so the Nordic bootloader → MCUboot → app chain is fine). Password-hiding when not GREEN verified. **Now running v1.2.1** (see below).
- [x] **MDBT50Q: both units are bricked (scrap)** by the QSPI/reset-pin reset loop (constraint 8). Unit 1 by the original standalone app; unit 2 by `merged.hex` v1.1.3. Software recovery is impossible (CTRL-AP unreachable); the P0.19↔RESET link is between module pads 40 and 42 **under the module shield**, so only hot-air removal of the module could save them. Details below.
- [ ] **MDBT50Q: flash a NEW unit** with the fixed build (recover first — see "3rd-party board first flash"), verify over RTT, then test a BLE update from the app. The fixed image (QSPI off, no `gpio-as-nreset`) has not run on hardware yet.
- Investigation notes (2nd unit, 2026-10-28): J-Link reached it fine (FICR read OK), `merged.hex` v1.1.3 programmed + verified with `--sectorerase`, then after reset: no LED, no USB, J-Link "Found SW-DP ID 0x2BA01477 / Failed to power up DAP", `--recover` times out (also at 1 MHz). Ruled out: LF clock (RC in both app + MCUboot), UICR in the hex (none), supply (T3–T4 steady 3.3 V), old tools (nrfjprog 10.24.2 / J-Link 8.96). DCDC REG0/REG1 identical in working `led_test`. **Board quirk: P0.19 is tied to P0.18/nRESET on the dongle PCB** — same as Nordic's PCA10059 (`BSP_SELF_PINRESET_PIN` = P0.19, lets firmware pin-reset itself), confirmed by a Nordic DevZone answer (thread 96060) citing Raytac's reference circuit; in the schematic the box under pins 41/42 joins pin 40 (RESET) to 42 (P0.19). The DK's QSPI flash has SCK on P0.19 (high-drive), and the DK DT sets `gpio-as-nreset` (boot code writes UICR.PSELRESET=P0.18) → the app's QSPI init held reset low → reset loop before `main()` (no LED/USB, "Failed to power up DAP"). **Fixed** (built + bundled in the app, not yet run on hardware): `&qspi`/`&mx25r64` disabled and `gpio-as-nreset` deleted in `boards/nrf52840dk_nrf52840.overlay` and `sysbuild/mcuboot/boards/nrf52840dk_nrf52840.overlay`. **Software recovery of the reset-looping units FAILED (2026-10-28):** DP answers (IDR 0x2BA01477) but CTRL/STAT stays 0xD0000040 = CSYSPWRUPACK set, **CDBGPWRUPACK never set**, so CTRL-AP is unreachable (its IDR reads 0) and ERASEALL can't be issued — tried nrfjprog `--recover` (also 1 MHz), pyOCD `erase --mass`, and raw CTRL-AP via J-Link Commander and pyOCD register-level (`scratchpad ctrlap_unlock.py`). Recovery needs the loop stopped in hardware (break the P0.19↔P0.18 link) — but per the module pad map (`Raytac_MDBT50Q.pdf` p7), pins 40 (RESET) and 42 (P0.19) are **adjacent pads in the inner right-hand column, under the module shield**; photos of both PCB sides (`RayTac\PCB_Top/Bottom.heic`) show no exposed link. Only fix is hot-air removal of the module → the two looping units are effectively scrap. (Note: J-Link Commander's raw `SWDReadAP` returned 0 for everything here because the debug domain was never powered — its "writes" never landed.) APPROTECT (rev 3) was also on (`APPROTECTSTATUS` 0) — our firmware only SW-unlocks SWD when UICR.APPROTECT=HwDisabled, hence recover first on new units. The Gerber in `MDBT50Q-RX Gerber for Programming Jig (210205)` is the pogo-pin jig (no copper), not the dongle layout; Raytac doesn't publish the dongle's PCB files (ask Raytac support). Raytac's MDBT50Q-RX approval sheet (Ver. E, 2020-06-19, §8 Reference Circuit) draws the same 41–42 tie, and says **"External 32.768 kHz is already inside the dongle"** — so the crystal IS fitted, and the earlier "no LED" on unit 1 was most likely this QSPI contention, not the LFXO (the RC clock setting is harmless; keep or revert). Same sheet: REG0 DC/DC + LDO disabled by default (VDDH tied to VDD = normal-voltage mode).
- [x] **v1.1.4 = second password** (medium ~1 s press), built for both boards; the app (Password 2 field) is built with both 1.1.4 images bundled. The MDBT50Q 1.1.4 image keeps the QSPI/reset fix (verified in `.config`).
- [x] **Verified on the official dongle (2026-10-28):** app installed, BLE update 1.1.3 → 1.1.4, and Password 2 set from the app. Short press types password 1 and a ~1 s press types password 2 — user confirmed "works great".
- [x] **v1.1.5 = Username 2** (medium press types username → TAB → password 2 → Enter, for a database login form). Built for both boards (MDBT50Q still has no QSPI/pin-reset), and the app with the Username 2 field is installed on the phone.
- [x] 1.1.5's username 2 was never tested on its own; it's superseded and verified working on 1.2.x (username 2 + TAB + password 2).
- [x] **Repo housekeeping (2026-10-28/29):**
  - [x] **(a) GitHub push done (2026-10-29).** RambookaICT is now a collaborator on Rambooka/BLE_Password, and `main` tracks `origin/main`.
  - [x] **(b) Android app is in `android/` (2026-10-29).** The folder couldn't be moved: something held a handle on the `BLE_Beacon` root folder itself. No file in it was open, and no visible process had it as its current directory, even after the Gradle and Kotlin daemons were stopped. So it was **copied** and verified by SHA-256 (58/58 files). Done: `firmwareDir` now defaults to the repo root, `android/.gitignore` ignores `.idea/`, `.kotlin/`, `app/google-services.json` and keystores, and the build and lint run from the new location. The old copy at `C:\Users\rabram\Source\Android\BLE_Beacon` is stale (see the open item below).
  - [x] Removed Android Studio's unused "Baking with Gemini" template screen, along with Firebase, the google-services plugin and viewmodel-compose (2026-10-29). Verified that a fresh `git clone` builds with no `google-services.json`. The local `android/app/google-services.json` is now unused but still git-ignored, because it contains a Firebase API key.
  - [x] **(c) `Readme.md` rewritten for the public repo (2026-10-29).** Keep it in step with behaviour changes: button timings, LEDs, status dump fields, GATT table.
- [x] **v1.1.6:** the status dump prints `averaged_rssi=` on its own line (`gone` after the 5 s beacon timeout).
- [x] **v1.2.0 = secure rolling beacon + office coexistence (2026-10-29).** Built for both boards (MDBT50Q still has QSPI/pin-reset off), host tests and Android unit tests pass, lint clean, and the app is built with 1.2.0 bundled.
- [x] **1.2.0 partly verified on the official dongle (2026-10-29):**
  - BLE update 1.1.x → 1.2.0, `confirmed=1`.
  - Save with the button held paired the dongle (`beacon_key=07abd2e3`) and set the passwords and username 2 through the **ECDH-sealed** writes. So PSA ECDH works on hardware, and `saved=1`.
  - Dump: `beacons=110 match=110 stale=0`, GREEN at −57 dBm.
  - Stopping the beacon → BLUE + lock in ~10 s (5 s timeout plus Android's stop latency). Restarting it → GREEN in a few seconds (fresh counter > floor); the button typed and unlocked the PC.
  - **Real walk-away with the phone in a pocket (2026-10-29):** the PC locked, and on return the dongle went GREEN and unlocked. So the counter advances fast enough while the phone sleeps (alarm / screen-on).
  - **Unplug/replug (2026-10-29):** after a reboot (tap to skip the config window), the dump showed `floor=1793218868`, restored from the flash counter log rather than 0. `ctr=1793219016` was accepted, `match=28 stale=0`.
- [x] **v1.2.1 = legacy removal (2026-10-29), verified on the official dongle.**
  - Firmware loads only v6 records. The app connects only to "held" dongles, needs minSdk 26, and dropped `purgeSecrets` and the beacon-name pref.
  - BLE update 1.2.0 → 1.2.1 from the new app worked.
  - Dump: `fw=1.2.1 confirmed=1`, `saved=1`, pairing kept (`beacon_key=07abd2e3`), floor restored, `match=54 stale=0`, GREEN.
  - Save **without** holding the button → the app said "Found a dongle, but its button isn't held…". Holding it → saved. So the held-flag advert and the app's held-only connect both work.
- [x] **Encrypted config session (ECDH, 2026-10-29), part of 1.2.0.** Host tests (12 KDF/seal vectors) and Android tests pass, including a real JCA ECDH round trip against a stand-in dongle. **Works on hardware**: the 1.2.0 pairing save went through the sealed writes. If it ever breaks:
  - "invalid session key" or write rejected 0x13 → suspect PSA or Oberon;
  - a fast red flash on Save → a BT RX stack overflow.

  This was chosen over BLE LE Secure Connections, which would mean a pairing popup on every save and needs bond storage (no NVS here).
- [x] **GitHub tag + Release v1.2.1 (2026-10-29):** https://github.com/Rambooka/BLE_Password/releases/tag/v1.2.1, with 6 assets (see "Release flow").
- [ ] **v1.2.2 (2026-10-29), "gaps" batch:** masked passwords in the dump, hardware watchdog + lock on watchdog boot, BLE-init failure reboots, and in the app: beacon restart on boot/update/Bluetooth-on plus the battery-optimisation request. Built and linted. **To verify on hardware:**
  - the dump shows `password=X***Y`, `boot=software watchdog=on` after the update;
  - the BLE update to 1.2.2 itself (MCUboot feeds the WDT during the swap);
  - phone reboot → beacon back without opening the app;
  - Bluetooth off/on → beacon back;
  - battery prompt appears on Start.

  The watchdog path isn't easy to trigger by hand. There's no test command; it was reviewed in code only.
- [x] **v1.3.0 = RAM-only secrets + biometric phone storage — verified on the official dongle (2026-10-29).** The dongle keeps the password/password2/username2/beacon-key in **RAM only** — nothing is written to flash (`config_storage` unused), so a lost/unplugged dongle holds no secret. The phone re-sends everything each boot; the config window stays open until it has the password + beacon key (button held), with no timeout/tap exit. The Android app **stores the credentials behind a per-operation biometric-bound Keystore key** (`CredentialStore.kt`) and prompts for a fingerprint to save/send. Both boards build, clang-tidy clean, host + Android unit tests pass, Android Lint 0 errors. Tag `PasswordsStoresInDongleCPUFlash` marks the last flash-storing commit (066b6c4).
  - BLE update 1.2.1 → 1.3.0 over the app (release APK installed with `adb install -r`, so the phone's beacon key `07abd2e3` was preserved): ~15 s dark swap, back to config window, dump `fw=1.3.0 confirmed=1`.
  - Dump after pairing (type password + fingerprint → save + sealed send): `prov=1`, no `saved=` field, `boot=software watchdog=on`, `beacon_key=07abd2e3 floor=0`, `match=73 stale=0`, `state=GREEN`, masked `password=B***)` / `password2=B***@`, `username2=rabram`.
  - Typing (short → password 1; ~1 s → username 2 + TAB + password 2), walk-away lock, and the headline test all worked: **unplug/replug → blank/BLUE/`prov=0`, then "Unlock & Send" (blank fields + fingerprint, reusing the saved credentials) re-paired and went GREEN.**
  - A fresh-boot dump confirmed the blank state: `prov=0`, `beacon_key=(not set)`, and `green=-71 blue=-100` (compiled defaults, so thresholds aren't persisted either). Harmless quirk: holding the button through power-up arms the 5 s dump timer during the first ~3 s before `config_mode` is set, so the dump can fire once while the config window is open (state is BLUE then, so no secret is shown). The dump is not armed by a press made after the window has opened.
- [x] **GitHub tag + Release v1.3.0 (2026-10-29):** https://github.com/Rambooka/BLE_Password/releases/tag/v1.3.0, 6 assets (see "Release flow"). Commit `2a7a930` on `main`; restore tag `PasswordsStoresInDongleCPUFlash` (066b6c4) marks the last flash-storing build (local only — push it if you want it on the remote).
- [ ] Optional test: a second phone running the app doesn't trigger the dongle (`beacons=` rises, `match=` doesn't).
- [ ] **Security follow-up (highest value): authenticate the dongle to the app.** The ECDH is unauthenticated, and the "held" flag can be faked. So a **fake dongle** advertising the service + "held" near you while you send would receive, and could decrypt, your passwords and beacon key. An active relay during the window is the same class of attack. **1.3.0 makes this MORE relevant, not less:** the sealed credential transfer now happens on *every* boot (not once per pairing), so the unauthenticated-dongle window recurs constantly. (1.3.0 *did* close the separate lost-dongle-at-rest leak — there's no secret on a powered-off dongle any more.) Intended fix was TOFU: a long-term dongle identity key pair the app pins at first pairing. **Complication from the RAM-only change:** there's no flash to persist that identity key in now. Options to weigh: (a) reintroduce a *minimal* persisted identity keypair only (its private key is a smaller at-rest exposure than the password — leaking it only lets someone impersonate that dongle *while you're present and pairing*); (b) derive a non-secret identity from FICR and accept it can't prove possession; (c) accept the active-relay risk. Decide before relying on this in a hostile RF environment.
- [ ] Delete the stale old app folder `C:\Users\rabram\Source\Android\BLE_Beacon` once it's unlocked (e.g. after a reboot). Don't edit it.
- [ ] No `LICENSE` file yet. The sources say Apache-2.0, so add a matching LICENSE if others should be able to reuse the code.
- [ ] Decide whether to drop RTT/logging on the MDBT50Q production build.
- [ ] Production signing key (constraint 7) — deferred; the button gate is accepted for now.

MDBT50Q board files: `boards/nrf52840dk_nrf52840.overlay` (LED P1.13 as led0, button P0.15 as sw0, HID node, USB on, DK's own buttons disabled, **QSPI flash disabled, `gpio-as-nreset` deleted**) and `boards/nrf52840dk_nrf52840.conf` (RC LF clock + RTT logging). The RC clock was added on the wrong theory that the module has no 32 kHz crystal — it does; RC works fine, so keep it or switch back to the crystal (better accuracy, lower power). `src/main.c` uses single-LED blink patterns via `CONFIG_BOARD_NRF52840DK` → `SINGLE_LED`.

### Note on APPROTECT / "Failed to power up DAP" (nRF52840)
- **Locked but running chip** (rev 3 APPROTECT): J-Link reports protection; `nrfjprog --recover` works (full erase, sets UICR.APPROTECT=HwDisabled). After recover use `--program ... --sectorerase` (not `--chiperase`, which erases UICR) so the board stays debuggable.
- **"Found SW-DP" but "Failed to power up DAP" and `--recover` times out**: the chip is reset-looping (debug domain never acks power-up) — not APPROTECT. On the MDBT50Q that was the P0.19→RESET loop (constraint 8). Software can't fix it; the loop must be stopped in hardware.
