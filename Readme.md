# BLE_Password

A **proximity password typer** built on an nRF52840 USB dongle.

The dongle plugs into your PC and acts as a USB keyboard. It listens for a
Bluetooth LE beacon broadcast by your phone and estimates how close the phone
is from its signal strength:

- **Phone close + short button press** → the dongle types your password 1 and presses Enter.
- **Phone close + long (~1 s) button press** → the dongle types your username 2 + Tab + password 2 and presses Enter.
- **Phone walks away** (or its beacon disappears) → the dongle presses **Win+L** and locks the PC.

A companion Android app (in [`android/`](android/)) broadcasts the beacon,
configures the dongle over BLE, and carries the dongle firmware so the dongle
can be updated over the air.

Built with Zephyr / nRF Connect SDK **v3.0.2** and MCUboot.

### Secure rolling beacon (firmware 1.2.0+)

The phone doesn't broadcast a name that anyone could copy. It broadcasts an
**authenticated, rolling beacon** instead:

```
manufacturer data, company ID 0xFFFF:   'B' 'P' 01  counter[4]  tag[8]
tag = first 8 bytes of HMAC-SHA256(key, 'B' 'P' 01 counter)
```

- **key** is 16 random bytes made by the phone and written to the dongle when you save (button held). On the phone it is stored encrypted under an Android Keystore key.
- **counter** is the phone's clock in seconds (UTC, so time zones and daylight saving don't affect it). It never goes backwards, even if the clock is set back.
- The dongle accepts a beacon only if the tag verifies with **its** key and the counter is fresh. After every lock, the counter must be newer than the last one accepted. This floor is also saved to flash, so it survives a reboot.

As a result:

- **Several people can work side by side.** Each phone has its own key, so a dongle ignores every other phone. When dongles are in their config window, each one advertises whether its button is held, and the app connects only to the dongle you're holding.
- **Copying or replaying the beacon doesn't work.** A spoofed beacon has no valid tag, and a recording carries an old counter.
- A *live* relay (forwarding your phone's packets in real time) is still possible. Stopping that needs distance bounding (BLE Channel Sounding), which the nRF52840 doesn't have.

When the phone sleeps, it keeps sending its last counter, which the dongle
accepts while it's still seeing you. A wake-up alarm (about once a minute) and
the screen turning on both advance the counter. **If the LED stays blue when
you sit back down, wake your phone's screen.**

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
fw=1.2.0 board=nrf52840dongle confirmed=1
up_ms=... scan=1 adv=0 saved=1 hid=1
ads=... beacons=... match=... stale=...
beacon_key=2fcd11b7 ctr=... floor=...
password=(hidden: phone not in range)
green=-76 blue=-100 state=RED seen_ago=120
averaged_rssi=-82
```

- Stored credentials are printed **only while the phone is in the green zone**.
- `beacons` counts rolling beacons from any phone. `match` counts yours, verified and fresh. `stale` counts yours with an old counter (a sleeping phone, or a replay).
- `beacon_key` is the paired phone's ID. The app shows the same ID.
- `averaged_rssi` is the running average the zones are based on. It reads `gone` once the beacon hasn't been seen for 5 s.
- `confirmed=0` means MCUboot will roll back to the previous firmware on the next reset.

### Configuring with the app

1. Plug in the dongle. It opens a **60 s config window** (blinking blue).
2. In the app, enter the green and blue thresholds (dBm), and any credentials you want to change.
3. **Hold the dongle's button** and tap **Save & Send to Dongle**. Writes are only accepted while the button is physically held and the dongle is within its config period. That's the proof that whoever is configuring the dongle owns it. The first save also **pairs** the dongle with your phone's beacon key. A dongle that isn't paired never sees any phone. One phone can be paired with several dongles, but each dongle has one phone.

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
| 0xDC000–0xE0000 | Saved config (raw flash record at 0xDC000) + beacon counter log (0xDD000, 0xDE000) |
| 0xE0000–0x100000 | Official dongle: Nordic bootloader (kept for last-resort recovery) |

### Flash wear

The nRF52840's flash is rated for at least **10,000 erase cycles per 4 KB
page**. Wear comes from erasing a page; appending a small write into
already-erased space barely counts. The dongle writes flash in only three
places:

| What | When it writes | Typical rate |
|---|---|---|
| Saved settings (0xDC000) | Only when you save from the app: 1 erase + 1 write | A few times a year |
| Beacon counter log (0xDD000 + 0xDE000) | One 4-byte entry per lock, plus one every 5 min while your phone is in range | ~100–300 entries/day |
| Firmware slots | Only during a firmware update | A few times a year |

The counter log is the only part written daily, and it is built to avoid
wear:

- Entries are appended into erased space. Each page holds 1,024 entries and is erased only when full, after which logging moves to the other page. One erase covers 1,024 entries, and the two pages share the erases.
- Each word is written only once between erases.

**Worst case:** the phone sits next to the dongle 24 hours a day. That's
about 300 entries a day, so each page is erased about once a week (~54
times a year). The flash would then last **10,000 ÷ 54 ≈ 185 years**. A
normal office day (~150 entries) gives about **370 years**. Flash wear is
not a practical limit.

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

This runs:
- clang-tidy on `src/` for both boards, using each build's compile database (so build first);
- the host unit tests in `tests/host/` (SHA-256, HMAC, beacon and config-session vectors; needs a host `gcc`);
- Android Lint and the app's JVM unit tests.

The firmware and app tests share the same beacon test vectors, so both ends
are checked to agree on the format.

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

Settings survive updates from 1.2.0 on. The config record is versioned and only ever extended. Since 1.2.1 the dongle no longer loads records from before 1.2, which have no beacon key, so a dongle coming from older firmware has to be set up again.

---

## Android app

Kotlin / Jetpack Compose, in [`android/`](android/), for **Android 8.0 or newer**. It uses Nordic's
`mcumgr-ble` for updates.

```
cd android
gradlew.bat :app:assembleRelease
adb install -r app\build\outputs\apk\release\app-release.apk
```

- The build copies both boards' `dfu_application.zip` from `../build_*` into the APK. Build the firmware first; a missing image only gives a warning. Override the location with `-PfirmwareDir=...`.
- Release signing reads `~/.android/ble_beacon_keystore.properties`, which you create yourself (`storeFile`, `storePassword`, `keyAlias`, `keyPassword`). Without it the release APK is unsigned. Debug builds work as usual.
- Backups are disabled (`allowBackup=false`, plus data-extraction rules), and the app stores only the thresholds, its beacon counter, and its beacon key (encrypted under an Android Keystore key). Passwords are never stored.

---

## BLE config service

Service `9f1b0001-5c3a-4e8e-9c2a-1a2b3c4d5e6f`. The dongle advertises as `BLE_Password`.

| UUID suffix | Characteristic | Access |
|---|---|---|
| `…0002` | Password 1 (UTF-8) | write* |
| `…0003` | *(retired in 1.2.0: was the beacon name)* | |
| `…0004` | Green threshold (int8 dBm) | write* |
| `…0005` | Blue threshold (int8 dBm) | write* |
| `…0006` | DFU arm (`0x01`): unlocks SMP for this phone | write* |
| `…0007` | Board name (`nrf52840dongle` / `nrf52840dk`) | read |
| `…0008` | Password 2 (UTF-8, empty = clear) | write* |
| `…0009` | Username 2 (UTF-8, empty = clear) | write* |
| `…000A` | Beacon key (16 bytes): pairs the dongle with a phone | write* |
| `…000B` | Key ID (4 bytes; empty = not paired). The app sends the key only if this doesn't match. | read |
| `…000C` | Session: read the dongle's ECDH public key, write the phone's (65-byte uncompressed P-256 points) | read / write |

\* Accepted only during the config window **and while the button is held**.
Password 1, password 2, username 2 and the beacon key must be **sealed** with
the session (`seq[4] || ciphertext || tag[16]`, see `src/cfg_crypto.h`).

In its config window the dongle advertises manufacturer data
`FF FF 'B' 'P' 10 <flags>`, where bit 0 of flags means its button is held.
Values are range-checked (−120…0 dBm, green > blue, non-empty password 1).
The standard MCUmgr SMP service is also present, but it refuses all commands
unless DFU has been armed.

---

## Security notes

- **Passwords are write-only over BLE.** The password, username 2, password 2 and beacon-key characteristics accept writes only. The dongle has no way to read them back out over Bluetooth, not even for the paired phone or with the button held. The firmware-update service (SMP) handles only firmware images, not the config storage. The only way they ever leave the dongle is typed over USB: the password when you press the button, and the 5 s status dump. Both happen only while your phone is in the green zone.
- Passwords are stored **unencrypted** in the dongle's flash. Anyone with the dongle and an SWD debugger (or the MDBT50Q's pads) can read them. Treat the dongle like a key.
- Builds are signed with **NCS's default MCUboot development key**, which is public. Physical access (the button hold) is what protects updates. For anything serious, generate your own key and set `SB_CONFIG_BOOT_SIGNATURE_KEY_FILE`. After that, MCUboot must be reflashed once on every board.
- The status dump only reveals credentials while the phone is in range.
- The beacon can't be spoofed or replayed (see *Secure rolling beacon*). A **live relay** can still fake proximity. This is a convenience tool, not a hardened authenticator.
- The secrets the app sends (passwords, username 2, and the beacon key on first pairing) are **encrypted**. Each connection runs a P-256 ECDH exchange, a fresh phone key against the dongle's per-window key, then derives two keys with HKDF-SHA256. Each secret write is encrypted and authenticated with HMAC-SHA256, bound to its characteristic, and numbered so it can't be replayed (`src/cfg_crypto.h`, `ConfigSession.kt`). The dongle refuses unencrypted secrets. A BLE sniffer sees only ciphertext. The exchange is *unauthenticated*, so an attacker actively relaying the connection during the 60 s window, while you hold the button, could still read it.

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
| `src/beacon_auth.*`, `src/cfg_crypto.*` | Rolling-beacon and encrypted-config crypto (plain C) |
| `tests/host/` | Host unit tests for those modules (`run.ps1`) |
| `tools/lint.ps1`, `.clang-tidy` | Lint + unit tests |
| `tools/make_release.ps1` | Publishes a GitHub Release for a tag, with the build outputs attached |
| `android/` | Companion Android app |
| `CLAUDE.md` | Detailed engineering notes and hard-won hardware findings |
