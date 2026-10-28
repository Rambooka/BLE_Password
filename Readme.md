# BLE_Password

A **proximity password typer** built on an nRF52840 USB dongle.

The dongle plugs into your PC and acts as a USB keyboard. It listens for a
Bluetooth LE beacon broadcast by your phone and estimates how close the phone
is from its signal strength:

- **Phone close + button pressed** → the dongle types your password and presses Enter.
- **Phone walks away** (or its beacon disappears) → the dongle presses **Win+L** and locks the PC.

A companion Android app (in [`android/`](android/)) broadcasts the beacon,
configures the dongle over BLE, and carries the dongle firmware so the dongle
can be updated over the air.

Built with Zephyr / nRF Connect SDK **v3.0.2** and MCUboot.

---

## Using it

### LEDs (official Nordic dongle)

| LED | Meaning |
|---|---|
| Blinking blue | Config window (first ~60 s after plug-in): the app can connect |
| Solid blue | Phone's beacon not seen |
| Solid red | Phone seen but too far away |
| Solid green | Phone is close: the button will type |
| Fast blink | Firmware update armed / in progress |
| 3 green blinks | Settings saved |
| 6 fast red blinks | Settings rejected (invalid values), nothing saved |

Zones use an averaged RSSI with 4 dB of hysteresis, so a borderline signal
doesn't flicker between zones.

The Raytac MDBT50Q-RX dongle has a single LED, so it shows the same states as
blink patterns: slow blink = config window, solid = close, fast blink = far,
off = not seen.

### Button (while green)

Timing is by how long you hold before releasing:

| Hold | Action |
|---|---|
| < 0.6 s | Type **password 1** + Enter |
| 0.6 – 2.5 s | Type **username 2** + Tab + **password 2** + Enter (a login form). The username part is skipped if not set. Nothing is typed if password 2 isn't set. |
| 2.5 – 5 s | Nothing (lets you back out) |
| 5 s | Type a **status dump** (see below) |

Leaving the green zone, or losing the beacon for 5 s, locks the PC with Win+L.

Typing uses a US keyboard layout map. Set the PC to a US layout, or avoid
characters that differ on yours.

### Status dump

Open a text editor and hold the button for 5 s. The dongle types:

```
=== BLE_Password ===
fw=1.1.6 board=nrf52840dongle confirmed=1
up_ms=... scan=1 adv=0 saved=1 hid=1
ads=... named=... ray=... match=...
beacon=<beacon name>
password=(hidden: phone not in range)
green=-76 blue=-100 state=RED seen_ago=120
averaged_rssi=-82
```

- Stored credentials are printed **only while the phone is in the green zone**.
- `averaged_rssi` is the running average the zones are based on. It reads `gone` once the beacon hasn't been seen for 5 s.
- `confirmed=0` means MCUboot will roll back to the previous firmware on the next reset.

### Configuring with the app

1. Plug in the dongle. It opens a **60 s config window** (blinking blue).
2. In the app, enter the beacon name, the green and blue thresholds (dBm), and any credentials you want to change.
3. **Hold the dongle's button** and tap **Save & Send to Dongle**. Writes are only accepted while the button is physically held. That's the proof that whoever is configuring the dongle owns it.

A short tap (< 0.5 s) on the button ends the config window early. It also ends
when the app disconnects, or after 60 s. Then the dongle starts scanning.

Credentials are **never stored on the phone**. The fields start blank and are
cleared after a successful send. A blank field means "keep the dongle's current
value". A checkbox removes username 2 and password 2 from the dongle.

---

## Hardware

| | Official Nordic nRF52840 Dongle (PCA10059) | Raytac MDBT50Q-RX dongle |
|---|---|---|
| Zephyr board | `nrf52840dongle/nrf52840` | `nrf52840dk/nrf52840` + overlay in `boards/` |
| LEDs | Green P0.06, RGB red P0.08, RGB blue P0.12 | One blue LED, P1.13 |
| Button | SW1 (P1.06) | P0.15 |
| First flash | USB, using the Nordic bootloader (one time) | J-Link / SWD |
| Updates | BLE from the app | BLE from the app |

> **MDBT50Q warning:** on this board **P0.19 is wired to nRESET**. Driving it
> low resets the chip, and doing that early in boot causes a reset loop that
> software can't recover from. The nRF52840 DK board definition puts its QSPI
> flash clock on P0.19. The overlays here disable QSPI and `gpio-as-nreset`.
> If you add any DK peripheral, check its pins against the Raytac schematic first.

### Flash layout (both boards)

| Range | Contents |
|---|---|
| 0x00000–0x10000 | MCUboot (on the official dongle, the Nordic MBR is at 0x0 and MCUboot at 0x1000) |
| 0x10000–0x76000 | Slot 0: running app |
| 0x76000–0xDC000 | Slot 1: update upload target |
| 0xDC000–0xE0000 | Saved config (raw flash record) |
| 0xE0000–0x100000 | Official dongle: Nordic bootloader (kept for last-resort recovery) |

---

## Building the firmware

Requires nRF Connect SDK **v3.0.2** and its toolchain. The app sits outside the
NCS workspace, so set `ZEPHYR_BASE` and `WEST_TOPDIR`, and put the toolchain on
`PATH`. Then:

```
west build -b nrf52840dongle/nrf52840 -d build_nRF52840 -p always --sysbuild .
west build -b nrf52840dk/nrf52840     -d build_mdbt50q  -p always --sysbuild .
```

Each build directory produces:

- `dfu_application.zip`: the signed update package. The Android app bundles it.
- `merged.hex`: MCUboot plus the app, for J-Link.
- `BLE_Password/zephyr/zephyr.signed.bin`: the signed app image.
- `dongle_mcuboot_migration.zip` (dongle build only): MCUboot packaged for the Nordic bootloader.

The firmware version comes from [`VERSION`](VERSION). **Bump it for every new
image.** The app's updater skips the upload if the dongle already runs an
image with the same hash.

### Lint

After every change, run:

```
powershell -ExecutionPolicy Bypass -File tools\lint.ps1
```

This runs clang-tidy on `src/` for both boards, using each build's compile
database (so build first), and Android Lint on the app.

---

## First-time flashing

After the first flash, every update goes over BLE from the app.

### Official dongle: one-time migration to MCUboot

1. Press the dongle's **RESET** button. The red LED pulses and the Nordic bootloader appears as a COM port. Then run `flash_dongle.bat COMxx`, which installs **only MCUboot** through `nrfutil`.
2. Unplug, then **hold SW1 while plugging back in**. MCUboot starts its USB serial recovery and a new COM port appears. Upload the app:
   ```
   pip install smpmgr
   smpmgr --port COMyy image upload build_nRF52840\BLE_Password\zephyr\zephyr.signed.bin
   smpmgr --port COMyy os reset
   ```
3. Replug. The app runs.

Only MCUboot goes through the Nordic bootloader. The Nordic bootloader
CRC-checks what it flashed on every boot, so an app installed that way would
break after the first MCUboot slot swap.

### Raytac MDBT50Q-RX (J-Link on the SWD pads)

```
nrfjprog -f NRF52 --recover
nrfjprog -f NRF52 --program build_mdbt50q\merged.hex --sectorerase --verify --reset
```

Run `--recover` first on a new unit: it erases the factory image and leaves
debug access enabled. Use `--sectorerase` (not `--chiperase`) so UICR is preserved.

---

## Updating over BLE

1. Plug in the dongle (config window).
2. In the app, tap **Update dongle firmware**, pick the board, and **hold the dongle's button** to start.
3. The app checks that the dongle's board matches the image. It then arms the update, uploads to slot 1 over SMP, and resets the dongle.
4. MCUboot swaps the images (the LED is dark for ~15 s). The new firmware confirms itself at boot. If it crashes before that, MCUboot rolls back on the next reset.

Settings survive updates. The config record is versioned and only ever extended.

---

## Android app

Kotlin / Jetpack Compose, in [`android/`](android/). It uses Nordic's
`mcumgr-ble` for updates.

```
cd android
gradlew.bat :app:assembleRelease
adb install -r app\build\outputs\apk\release\app-release.apk
```

- The build copies both boards' `dfu_application.zip` from `../build_*` into the APK. Build the firmware first; a missing image only gives a warning. Override the location with `-PfirmwareDir=...`.
- Release signing reads `~/.android/ble_beacon_keystore.properties`, which you create yourself (`storeFile`, `storePassword`, `keyAlias`, `keyPassword`). Without it the release APK is unsigned. Debug builds work as usual.
- Backups are disabled (`allowBackup=false`, plus data-extraction rules), and the app stores only the beacon name and thresholds.

---

## BLE config service

Service `9f1b0001-5c3a-4e8e-9c2a-1a2b3c4d5e6f`. The dongle advertises as `BLE_Password`.

| UUID suffix | Characteristic | Access |
|---|---|---|
| `…0002` | Password 1 (UTF-8) | write* |
| `…0003` | Beacon name (UTF-8) | write* |
| `…0004` | Green threshold (int8 dBm) | write* |
| `…0005` | Blue threshold (int8 dBm) | write* |
| `…0006` | DFU arm (`0x01`): unlocks SMP for this phone | write* |
| `…0007` | Board name (`nrf52840dongle` / `nrf52840dk`) | read |
| `…0008` | Password 2 (UTF-8, empty = clear) | write* |
| `…0009` | Username 2 (UTF-8, empty = clear) | write* |

\* Accepted only during the config window **and while the button is held**.
Values are range-checked (−120…0 dBm, green > blue, non-empty password 1).
The standard MCUmgr SMP service is also present, but it refuses all commands
unless DFU has been armed.

---

## Security notes

- Passwords are stored **unencrypted** in the dongle's flash. Anyone with the dongle and an SWD debugger (or the MDBT50Q's pads) can read them. Treat the dongle like a key.
- Builds are signed with **NCS's default MCUboot development key**, which is public. Physical access (the button hold) is what protects updates. For anything serious, generate your own key and set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`. After that, MCUboot must be reflashed once on every board.
- The status dump only reveals credentials while the phone is in range.
- Proximity comes from signal strength, which an attacker relaying your phone's beacon could fake. This is a convenience tool, not a hardened authenticator.

---

## Repo layout

| Path | What |
|---|---|
| `src/main.c` | Firmware |
| `prj.conf`, `sysbuild.conf`, `sysbuild.cmake` | Zephyr / sysbuild config. `sysbuild.cmake` makes the dongle migration package. |
| `boards/` | MDBT50Q overlay and config (built as `nrf52840dk`) |
| `sysbuild/mcuboot/` | MCUboot config per board |
| `pm_static_*.yml` | Fixed flash layout |
| `flash_dongle.bat` | One-time official-dongle migration |
| `tools/lint.ps1`, `.clang-tidy` | Lint |
| `android/` | Companion Android app |
| `CLAUDE.md` | Detailed engineering notes and hard-won hardware findings |
