/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE Beacon Proximity Password Typer
 *
 * Scans for the paired phone's authenticated rolling beacon (beacon_auth.h:
 * HMAC-tagged counter, so other phones and recordings are ignored) and controls
 * LEDs by RSSI zone (thresholds configurable):
 *   Not detected / RSSI < -100 dBm : Blue  LED solid (phone not seen)
 *   -100 to -71 dBm                : Red   LED solid (phone too far away)
 *   RSSI >= -71 dBm                : Green LED solid (phone beacon is close)
 *
 * Pressing SW1 while green solid types the stored password over USB HID:
 * a short press types password 1, a medium press (~1 s) types password 2
 * (preceded by its username + TAB when one is set, for login forms).
 *
 * Remote configuration (companion Android app):
 *   The dongle is also a connectable BLE peripheral exposing a small GATT
 *   "config" service during the boot config window.  The phone writes the
 *   passwords, beacon name and thresholds; they apply immediately and are
 *   saved to a raw flash record (config_storage) that survives reboots and
 *   firmware updates.
 *
 *   A write is only accepted while (a) the boot config window is open AND
 *   (b) the dongle's own button is physically held down at the instant the
 *   write arrives.  Holding the button is the proof that whoever is writing the
 *   settings has physical possession of the dongle (the owner).
 *
 *   Firmware update (both boards run under MCUboot): the same service exposes
 *   a "DFU arm" characteristic.  Writing 0x01 to it (under the same gate)
 *   unlocks MCUmgr/SMP over BLE for that phone and extends the config window,
 *   so the companion app can upload a signed image and reset into it.  SMP is
 *   refused at all other times.  A read-only "board" characteristic reports
 *   CONFIG_BOARD so the app can refuse an image built for the other PCB.
 */

#include <sample_usbd.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/att.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/fatal.h>
#include <zephyr/logging/log.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/mgmt/mcumgr/mgmt/callbacks.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <pm_config.h>
#include <app_version.h>
#include <string.h>
#include <stdio.h>

#include <psa/crypto.h>

#include "beacon_auth.h"
#include "cfg_crypto.h"

LOG_MODULE_REGISTER(ble_password, LOG_LEVEL_INF);

/* ---- Configuration ---------------------------------------------------- */

#define DEFAULT_PASSWORD     "12345678"   /* ENTER is appended automatically */

#define DEFAULT_RSSI_GREEN_THRESHOLD (-71)   /* avg RSSI >= this → GREEN (close:  -10 to -77)  */
#define DEFAULT_RSSI_BLUE_THRESHOLD  (-100)  /* avg RSSI >= this → RED   (far:    -78 to -100) */
                                             /* avg RSSI <  this → BLUE  (gone: below -100)    */
#define BEACON_TIMEOUT_MS    5000    /* reset to BLUE after this many ms with no sighting */

/* -> Beacon rate = 100mS from Android phone */
#define RSSI_SAMPLES         20      /* number of samples in the running average */

#define PASSWORD_MAX_LEN     64
#define BEACON_NAME_MAX_LEN  31      /* only for the (retired) record field */

/* Button press lengths (normal mode, released while GREEN):
 *   < MEDIUM_PRESS_MS                  → type password 1
 *   MEDIUM_PRESS_MS … LONG_PRESS_MS    → type password 2 (if one is set)
 *   LONG_PRESS_MS … 5 s                → nothing (lets you back out)
 *   held 5 s                           → status dump */
#define MEDIUM_PRESS_MS      600
#define LONG_PRESS_MS        2500

/* RSSI sanity bounds (dBm) and zone hysteresis (dB).  The hysteresis is a guard
 * band so an average hovering on a threshold doesn't flip-flop between zones
 * (which would otherwise re-lock the PC repeatedly). */
#define RSSI_MIN_DBM        (-120)
#define RSSI_MAX_DBM        (0)
#define RSSI_HYSTERESIS     (4)

/* ---- Runtime configuration (loaded from flash at boot, read-only after) -- */

static char password_str[PASSWORD_MAX_LEN + 1]    = DEFAULT_PASSWORD;
static char password2_str[PASSWORD_MAX_LEN + 1];   /* optional; "" = not set */
static char username2_str[PASSWORD_MAX_LEN + 1];   /* optional; typed + TAB before password 2 */
/* Rolling-beacon key shared with the owner's phone (see beacon_auth.h).  No key
 * = no beacon is ever accepted: the dongle stays BLUE until the app pairs it. */
static uint8_t beacon_key[BEACON_KEY_LEN];
static bool beacon_key_set;
static bool beacon_key_changed;   /* new key written this window → reset counters */
static int8_t rssi_green_threshold = DEFAULT_RSSI_GREEN_THRESHOLD;
static int8_t rssi_blue_threshold  = DEFAULT_RSSI_BLUE_THRESHOLD;

/* ---- GATT config service UUIDs ----------------------------------------- */
/* Service:  9f1b0001-5c3a-4e8e-9c2a-1a2b3c4d5e6f
 * Password: 9f1b0002-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write, UTF-8 string)
 * (0003 was the beacon name; retired in 1.2.0 by the rolling beacon key)
 * Green th:9f1b0004-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write, 1 signed byte, dBm)
 * Blue th:  9f1b0005-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write, 1 signed byte, dBm)
 * DFU arm:  9f1b0006-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write 0x01 → unlock SMP upload)
 * Board:    9f1b0007-5c3a-4e8e-9c2a-1a2b3c4d5e6f (read, CONFIG_BOARD string)
 * Password2:9f1b0008-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write, UTF-8; empty = clear)
 * Username2:9f1b0009-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write, UTF-8; empty = clear)
 * BeaconKey:9f1b000a-5c3a-4e8e-9c2a-1a2b3c4d5e6f (write, 16 bytes: rolling-beacon key)
 * KeyId:    9f1b000b-5c3a-4e8e-9c2a-1a2b3c4d5e6f (read, 4 bytes; empty = no key)
 * Session:  9f1b000c-5c3a-4e8e-9c2a-1a2b3c4d5e6f (read: dongle's ECDH public key;
 *           write: phone's) — Password, Password2, Username2 and BeaconKey
 *           writes must be sealed with the resulting session (cfg_crypto.h).
 */
#define CFG_SVC_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0001, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_PW_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0002, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_GREEN_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0004, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_BLUE_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0005, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_BOOT_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0006, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_BOARD_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0007, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_PW2_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0008, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_USER2_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b0009, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_KEY_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b000a, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_KEYID_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b000b, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)
#define CFG_SESSION_UUID_VAL \
	BT_UUID_128_ENCODE(0x9f1b000c, 0x5c3a, 0x4e8e, 0x9c2a, 0x1a2b3c4d5e6f)

/* Short IDs bound into each sealed write (low byte of the UUID). */
#define SEAL_ID_PASSWORD   0x02
#define SEAL_ID_PASSWORD2  0x08
#define SEAL_ID_USERNAME2  0x09
#define SEAL_ID_BEACON_KEY 0x0A

static const struct bt_uuid_128 cfg_svc_uuid    = BT_UUID_INIT_128(CFG_SVC_UUID_VAL);
static const struct bt_uuid_128 cfg_pw_uuid     = BT_UUID_INIT_128(CFG_PW_UUID_VAL);
static const struct bt_uuid_128 cfg_green_uuid  = BT_UUID_INIT_128(CFG_GREEN_UUID_VAL);
static const struct bt_uuid_128 cfg_blue_uuid   = BT_UUID_INIT_128(CFG_BLUE_UUID_VAL);
static const struct bt_uuid_128 cfg_boot_uuid   = BT_UUID_INIT_128(CFG_BOOT_UUID_VAL);
static const struct bt_uuid_128 cfg_board_uuid  = BT_UUID_INIT_128(CFG_BOARD_UUID_VAL);
static const struct bt_uuid_128 cfg_pw2_uuid    = BT_UUID_INIT_128(CFG_PW2_UUID_VAL);
static const struct bt_uuid_128 cfg_user2_uuid  = BT_UUID_INIT_128(CFG_USER2_UUID_VAL);
static const struct bt_uuid_128 cfg_key_uuid    = BT_UUID_INIT_128(CFG_KEY_UUID_VAL);
static const struct bt_uuid_128 cfg_keyid_uuid  = BT_UUID_INIT_128(CFG_KEYID_UUID_VAL);
static const struct bt_uuid_128 cfg_session_uuid = BT_UUID_INIT_128(CFG_SESSION_UUID_VAL);

/* ---- LEDs --------------------------------------------------------------- */
/* Official nRF52840 Dongle has 3 LEDs (blue/green/red).  The 3rd-party Raytac
 * MDBT50Q-RX dongle has a SINGLE blue LED (on led0 = P1.13), so that build uses
 * blink patterns on one LED instead of colours. */
#if defined(CONFIG_BOARD_NRF52840DK)
#define SINGLE_LED 1
#else
#define SINGLE_LED 0
#endif

static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
#if !SINGLE_LED
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios);
static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
#define ALERT_LED led_red
#else
/* Only one LED exists; use it for everything. */
#define ALERT_LED led_green
#endif

/* ---- HID keyboard report ---------------------------------------------- */

static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

enum kb_report_idx {
	KB_MOD_KEY = 0,
	KB_RESERVED,
	KB_KEY_CODE1,
	KB_KEY_CODE2,
	KB_KEY_CODE3,
	KB_KEY_CODE4,
	KB_KEY_CODE5,
	KB_KEY_CODE6,
	KB_REPORT_COUNT,
};

UDC_STATIC_BUF_DEFINE(hid_report, KB_REPORT_COUNT);

/* ---- Application state ------------------------------------------------- */

typedef enum {
	STATE_BLUE = 0,
	STATE_GREEN,
	STATE_RED,
} beacon_state_t;

static atomic_t current_state = ATOMIC_INIT(STATE_BLUE);
static bool hid_ready;
static const struct device *hid_dev;

/* Last time (uptime ms) the configured target beacon was seen. Initialised far
 * in the past so writes are rejected until the beacon is actually observed. */
static volatile int64_t last_beacon_seen_ms = -1000000;

/* BLE role status (for the button-hold status dump) */
static volatile bool ble_scan_running;
static volatile bool ble_adv_running;
static volatile int  ble_adv_err = -999;  /* last bt_le_adv_start() return */

/* Config window: this controller won't run connectable advertising and scanning
 * at the same time, so we advertise for a window at boot (config mode) then
 * switch to scanning (normal mode). */
static volatile bool config_mode;
/* True while the dongle button is physically held down.  During the config
 * window this is the "authorize write" gate (see the GATT write handlers):
 * holding the button proves the writer physically owns the dongle. */
static volatile bool button_held;
static struct bt_conn *cfg_conn;
K_SEM_DEFINE(config_done_sem, 0, 1);
#define CONFIG_WINDOW_MS 60000
/* A button tap shorter than this (ms) during the config window exits it early;
 * a longer hold is the "authorize write" gate instead. */
#define CFG_EXIT_TAP_MS  500

/* Firmware update.  MCUmgr/SMP is locked until the DFU-arm characteristic is
 * written (button held).  Arming remembers the phone's address, keeps the
 * config window open for up to DFU_ARM_WINDOW_MS, and re-advertises after the
 * arming connection drops so the phone's SMP client can reconnect.  Only that
 * same address is let back in; the SMP session ending closes the window. */
#define DFU_ARM_WINDOW_MS 180000
static volatile bool dfu_armed;
static volatile bool dfu_arm_conn_open;  /* the GATT connection that armed us */
static volatile bool dfu_conn_foreign;   /* current conn is not the armed phone */
static volatile int64_t dfu_deadline_ms;
static bt_addr_le_t dfu_peer;

/* Diagnostics counters */
static atomic_t scan_total_count;  /* every advertisement seen (any device) */
static atomic_t scan_fmt_count;    /* rolling beacons from ANY phone (our format) */
static atomic_t scan_match_count;  /* beacons that verified with our key and were fresh */
static atomic_t scan_stale_count;  /* verified, but counter not newer (replay / repeat) */

/* Rolling-beacon counter (replay protection).  A beacon is accepted only if its
 * counter is >= the last accepted one (the phone repeats a value while its CPU
 * sleeps) AND > the floor.  The floor jumps to the last accepted counter at
 * every lock (left GREEN / beacon lost), so after the owner walks away only a
 * NEWER counter brings the dongle back — a recording can't.  The floor is also
 * persisted (append-only log of uint32 words in two ping-pong pages after the
 * config record; see ctr_log_*()) so that holds across a reboot. */
static volatile uint32_t beacon_ctr_last;  /* highest accepted counter */
static uint32_t beacon_ctr_floor;          /* from flash at boot */
static volatile uint32_t ctr_logged;       /* highest counter written to flash */
#define CTR_LOG_INTERVAL_S 300   /* also log every 5 min while the phone is present */

/* ---- RSSI running average (ring buffer, BT thread only) ---------------- */

static int8_t  rssi_buf[RSSI_SAMPLES];
static uint8_t rssi_head;       /* next write index */
static uint8_t rssi_count;      /* valid samples (0..RSSI_SAMPLES) */
static volatile int8_t rssi_last_avg; /* last computed average, readable from any context */

static void rssi_reset(void)
{
	rssi_head  = 0;
	rssi_count = 0;
}

static int8_t rssi_avg_update(int8_t sample)
{
	rssi_buf[rssi_head] = sample;
	rssi_head = (rssi_head + 1) % RSSI_SAMPLES;
	if (rssi_count < RSSI_SAMPLES) {
		rssi_count++;
	}

	int32_t sum = 0;
	for (uint8_t i = 0; i < rssi_count; i++) {
		sum += rssi_buf[i];
	}
	int8_t avg = (int8_t)(sum / rssi_count);
	rssi_last_avg = avg;
	return avg;
}

/* ---- Semaphore: button press while green triggers typing --------------- */

K_SEM_DEFINE(type_trigger, 0, 1);
static atomic_t type_which;   /* 1 or 2: which password the trigger types */

/* ---- LED update timer (500 ms period) ---------------------------------- */

/* LED timer tick = 100 ms (patterns derived from a tick counter). */
#define LED_TICK_MS 100

static void led_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	static uint32_t tick;
	tick++;

#if SINGLE_LED
	/* Single blue LED — encode states as blink patterns:
	 *   config window : slow blink (200 ms on / 800 ms)
	 *   close (green) : solid on
	 *   far   (red)   : fast blink (200 / 200 ms)
	 *   not seen      : off
	 */
	bool on;
	if (config_mode) {
		/* DFU armed: fast blink so the user knows an update is in progress. */
		on = dfu_armed ? (tick % 2) == 0 : (tick % 10) < 2;
	} else {
		switch ((beacon_state_t)atomic_get(&current_state)) {
		case STATE_GREEN: on = true;           break;
		case STATE_RED:   on = (tick % 4) < 2; break;
		default:          on = false;          break;
		}
	}
	gpio_pin_set_dt(&led_green, on ? 1 : 0);
#else
	/* Config window: blink BLUE (~500 ms) so the mode is obvious; fast blink
	 * while a firmware update is armed. */
	if (config_mode) {
		bool on = dfu_armed ? (tick % 2) == 0 : (tick % 10) < 5;
		gpio_pin_set_dt(&led_blue,  on ? 1 : 0);
		gpio_pin_set_dt(&led_green, 0);
		gpio_pin_set_dt(&led_red,   0);
		return;
	}

	switch ((beacon_state_t)atomic_get(&current_state)) {
	case STATE_BLUE:
		gpio_pin_set_dt(&led_blue,  1);
		gpio_pin_set_dt(&led_green, 0);
		gpio_pin_set_dt(&led_red,   0);
		break;
	case STATE_GREEN:
		gpio_pin_set_dt(&led_blue,  0);
		gpio_pin_set_dt(&led_green, 1);
		gpio_pin_set_dt(&led_red,   0);
		break;
	case STATE_RED:
		gpio_pin_set_dt(&led_blue,  0);
		gpio_pin_set_dt(&led_green, 0);
		gpio_pin_set_dt(&led_red,   1);
		break;
	}
#endif
}

K_TIMER_DEFINE(led_timer, led_timer_fn, NULL);

static void send_key(uint8_t modifier, uint8_t keycode); /* forward declaration */
static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *key); /* forward decl */
static void type_string(const char *s); /* forward declaration */
static void type_full_status(void); /* forward declaration */
static void ctr_log_request(void);  /* forward declaration */

/* ---- Win+L lock work (runs in system work queue, safe to sleep) -------- */

static void lock_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (hid_ready) {
		send_key(HID_KBD_MODIFIER_LEFT_UI, HID_KEY_L);
	}
}

K_WORK_DEFINE(lock_work, lock_fn);

/* ---- Beacon lost timeout ----------------------------------------------- */

static void beacon_lost_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	rssi_reset();
	atomic_set(&current_state, STATE_BLUE);
	LOG_INF("Beacon lost — locking computer");
	k_work_submit(&lock_work);
	/* From now on only a NEWER counter counts (a sleeping phone repeats its
	 * last one, and a recording of it must not unlock anything), and persist
	 * it so that also holds across a reboot. */
	beacon_ctr_floor = beacon_ctr_last;
	ctr_log_request();
}

K_WORK_DELAYABLE_DEFINE(beacon_lost_work, beacon_lost_fn);

/* ---- Status report on 5-second button hold ----------------------------- */

static bool rssi_hold_fired;

/* 5-second button hold: type out current status + settings over HID so they
 * can be read without a serial console. */
static void status_report_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	rssi_hold_fired = true;

	/* Visual confirmation that the 5-second hold fired (independent of USB/
	 * HID/focus): blink green 3x.  Pause the LED state machine meanwhile. */
	k_timer_stop(&led_timer);
#if !SINGLE_LED
	gpio_pin_set_dt(&led_blue, 0);
	gpio_pin_set_dt(&led_red, 0);
#endif
	for (int i = 0; i < 3; i++) {
		gpio_pin_set_dt(&led_green, 1);
		k_msleep(120);
		gpio_pin_set_dt(&led_green, 0);
		k_msleep(120);
	}
	k_timer_start(&led_timer, K_MSEC(LED_TICK_MS), K_MSEC(LED_TICK_MS));

	type_full_status();
}

K_WORK_DELAYABLE_DEFINE(status_report_work, status_report_fn);

/* ---- ASCII -> HID keycode (US keyboard layout) ------------------------- */

#define SHIFT HID_KBD_MODIFIER_LEFT_SHIFT

static bool ascii_to_hid(char c, uint8_t *mod, uint8_t *key)
{
	*mod = 0;

	if (c >= 'a' && c <= 'z') {
		*key = HID_KEY_A + (uint8_t)(c - 'a');
		return true;
	}
	if (c >= 'A' && c <= 'Z') {
		*mod = SHIFT;
		*key = HID_KEY_A + (uint8_t)(c - 'A');
		return true;
	}
	if (c >= '1' && c <= '9') {
		*key = HID_KEY_1 + (uint8_t)(c - '1');
		return true;
	}
	if (c == '0') {
		*key = HID_KEY_0;
		return true;
	}

	switch (c) {
	/* unshifted symbols */
	case ' ':  *key = HID_KEY_SPACE;      return true;
	case '-':  *key = HID_KEY_MINUS;      return true;
	case '=':  *key = HID_KEY_EQUAL;      return true;
	case '[':  *key = HID_KEY_LEFTBRACE;  return true;
	case ']':  *key = HID_KEY_RIGHTBRACE; return true;
	case '\\': *key = HID_KEY_BACKSLASH;  return true;
	case ';':  *key = HID_KEY_SEMICOLON;  return true;
	case '\'': *key = HID_KEY_APOSTROPHE; return true;
	case '`':  *key = HID_KEY_GRAVE;      return true;
	case ',':  *key = HID_KEY_COMMA;      return true;
	case '.':  *key = HID_KEY_DOT;        return true;
	case '/':  *key = HID_KEY_SLASH;      return true;
	/* shifted symbols */
	case '!':  *mod = SHIFT; *key = HID_KEY_1;          return true;
	case '@':  *mod = SHIFT; *key = HID_KEY_2;          return true;
	case '#':  *mod = SHIFT; *key = HID_KEY_3;          return true;
	case '$':  *mod = SHIFT; *key = HID_KEY_4;          return true;
	case '%':  *mod = SHIFT; *key = HID_KEY_5;          return true;
	case '^':  *mod = SHIFT; *key = HID_KEY_6;          return true;
	case '&':  *mod = SHIFT; *key = HID_KEY_7;          return true;
	case '*':  *mod = SHIFT; *key = HID_KEY_8;          return true;
	case '(':  *mod = SHIFT; *key = HID_KEY_9;          return true;
	case ')':  *mod = SHIFT; *key = HID_KEY_0;          return true;
	case '_':  *mod = SHIFT; *key = HID_KEY_MINUS;      return true;
	case '+':  *mod = SHIFT; *key = HID_KEY_EQUAL;      return true;
	case '{':  *mod = SHIFT; *key = HID_KEY_LEFTBRACE;  return true;
	case '}':  *mod = SHIFT; *key = HID_KEY_RIGHTBRACE; return true;
	case '|':  *mod = SHIFT; *key = HID_KEY_BACKSLASH;  return true;
	case ':':  *mod = SHIFT; *key = HID_KEY_SEMICOLON;  return true;
	case '"':  *mod = SHIFT; *key = HID_KEY_APOSTROPHE; return true;
	case '~':  *mod = SHIFT; *key = HID_KEY_GRAVE;      return true;
	case '<':  *mod = SHIFT; *key = HID_KEY_COMMA;      return true;
	case '>':  *mod = SHIFT; *key = HID_KEY_DOT;        return true;
	case '?':  *mod = SHIFT; *key = HID_KEY_SLASH;      return true;
	default:
		return false;
	}
}

/* Type an ASCII string over HID ('\n' becomes ENTER). */
static void type_string(const char *s)
{
	for (; *s != '\0'; s++) {
		uint8_t mod, key;

		if (*s == '\n') {
			send_key(0, HID_KEY_ENTER);
		} else if (ascii_to_hid(*s, &mod, &key)) {
			send_key(mod, key);
		}
	}
}

/* ---- BLE scan ---------------------------------------------------------- */

struct beacon_parse_ctx {
	bool     ours;     /* a rolling beacon (any phone's) */
	bool     valid;    /* ...whose tag verified with our key */
	uint32_t ctr;
};

static bool ad_beacon_cb(struct bt_data *data, void *user_data)
{
	struct beacon_parse_ctx *ctx = user_data;

	if (data->type != BT_DATA_MANUFACTURER_DATA ||
	    !beacon_auth_is_beacon(data->data, data->data_len)) {
		return true;   /* keep looking */
	}
	ctx->ours = true;
	ctx->valid = beacon_key_set &&
		     beacon_auth_verify(beacon_key, data->data, data->data_len, &ctx->ctr);
	return false;
}

static void scan_recv(const struct bt_le_scan_recv_info *info,
		      struct net_buf_simple *buf)
{
	struct beacon_parse_ctx ctx = {0};
	struct net_buf_simple_state saved;

	atomic_inc(&scan_total_count);
	if (!beacon_key_set) {
		return;   /* not paired with a phone yet */
	}

	net_buf_simple_save(buf, &saved);
	bt_data_parse(buf, ad_beacon_cb, &ctx);
	net_buf_simple_restore(buf, &saved);

	if (!ctx.ours) {
		return;
	}
	atomic_inc(&scan_fmt_count);
	if (!ctx.valid) {
		return;   /* another phone's beacon (or a forgery) */
	}
	/* Fresh = not older than the newest accepted one (a sleeping phone repeats
	 * its last counter) and newer than the floor (raised at every lock, restored
	 * from flash at boot). */
	if (ctx.ctr <= beacon_ctr_floor || ctx.ctr < beacon_ctr_last) {
		atomic_inc(&scan_stale_count);
		return;
	}
	beacon_ctr_last = ctx.ctr;
	atomic_inc(&scan_match_count);
	if (ctx.ctr - ctr_logged >= CTR_LOG_INTERVAL_S) {
		ctr_log_request();
	}

	/* Record that the configured beacon is present (gates config writes). */
	last_beacon_seen_ms = k_uptime_get();

	int8_t avg = rssi_avg_update(info->rssi);
	beacon_state_t prev_state = (beacon_state_t)atomic_get(&current_state);
	beacon_state_t new_state;

	/* Apply hysteresis: once in a "closer" zone, require the average to fall a
	 * few dB below the threshold before dropping back, so a borderline RSSI
	 * doesn't oscillate between zones (and spam the PC lock). */
	int8_t green_th = rssi_green_threshold;
	int8_t blue_th  = rssi_blue_threshold;
	if (prev_state == STATE_GREEN) {
		green_th -= RSSI_HYSTERESIS;
	}
	if (prev_state == STATE_GREEN || prev_state == STATE_RED) {
		blue_th -= RSSI_HYSTERESIS;
	}

	if (avg >= green_th) {
		new_state = STATE_GREEN;
	} else if (avg >= blue_th) {
		new_state = STATE_RED;
	} else {
		new_state = STATE_BLUE;
	}

	atomic_set(&current_state, new_state);

	/* Lock whenever the phone leaves the close (GREEN) zone — whether it steps
	 * back to RED or drops straight to BLUE. */
	if (prev_state == STATE_GREEN && new_state != STATE_GREEN) {
		LOG_INF("Left GREEN (state=%d) — locking computer", new_state);
		k_work_submit(&lock_work);
		beacon_ctr_floor = beacon_ctr_last;   /* see beacon_lost_fn */
		ctr_log_request();
	}

	k_work_reschedule(&beacon_lost_work, K_MSEC(BEACON_TIMEOUT_MS));

	LOG_DBG("beacon ctr=%u RSSI=%d avg=%d state=%d", ctx.ctr, info->rssi, avg,
		new_state);
}

static struct bt_le_scan_cb scan_cbs = {
	.recv = scan_recv,
};

/* ---- Config state ------------------------------------------------------ */

/* GATT write handlers update the live variables directly (safe: during the
 * config window nothing else reads them) and set config_dirty; the flash save
 * happens at the end of the config window with the radio off. */
static volatile bool config_dirty;  /* set when config changed, cleared after flash save */
static volatile bool cfg_loaded;    /* true if valid config was loaded from flash */

/* ---- Raw-flash persistence --------------------------------------------- */
/* One raw record in the 'config_storage' partition, pinned by the static
 * partition maps (pm_static_*.yml) at 0xDC000 on both boards: after MCUboot's
 * two image slots and, on the official dongle, below the Nordic bootloader
 * (0xE0000).  NVS/settings are not used (see CLAUDE.md). */
/* Only the current layout is accepted (pre-1.2 records were dropped in 1.2.1:
 * they hold no beacon key, so such a dongle must be set up again anyway).  If
 * the layout ever changes, APPEND fields and bump the magic. */
#define CFG_MAGIC      0xB1E50006u
#define STORAGE_OFFSET PM_CONFIG_STORAGE_ADDRESS
#define STORAGE_ERASE  4096
BUILD_ASSERT(PM_CONFIG_STORAGE_ADDRESS == 0xDC000,
	     "config_storage moved — existing dongles would lose their saved config");

struct persist_cfg {
	uint32_t magic;
	int8_t   green;
	int8_t   blue;
	char     password[PASSWORD_MAX_LEN + 1];    /* 65 */
	char     beacon[BEACON_NAME_MAX_LEN + 1];   /* 32, unused since v6 (was the beacon name) */
	char     password2[PASSWORD_MAX_LEN + 1];   /* 65, new in v4 */
	char     username2[PASSWORD_MAX_LEN + 1];   /* 65, new in v5 */
	uint8_t  beacon_key[BEACON_KEY_LEN];        /* 16, new in v6 */
	uint8_t  beacon_key_set;                    /* new in v6 */
};

BUILD_ASSERT(sizeof(struct persist_cfg) % 4 == 0, "persist_cfg must be 4-aligned");

static const struct device *flash_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_flash_controller));

/* Validate the live config: non-empty password, thresholds in a sane dBm
 * window, and green strictly stronger (greater) than blue.  Used to reject bad
 * persisted data on load and bad settings before a flash save.  (No beacon key
 * is valid too: the dongle just never sees its phone until it is paired.) */
static bool config_is_sane(void)
{
	if (password_str[0] == '\0') {
		return false;
	}
	if (rssi_green_threshold < RSSI_MIN_DBM || rssi_green_threshold > RSSI_MAX_DBM) {
		return false;
	}
	if (rssi_blue_threshold < RSSI_MIN_DBM || rssi_blue_threshold > RSSI_MAX_DBM) {
		return false;
	}
	if (rssi_green_threshold <= rssi_blue_threshold) {
		return false;
	}
	return true;
}

/* Load persisted config into the live variables.  Read-only, call before
 * bt_enable(). */
static void config_load_from_flash(void)
{
	struct persist_cfg cfg;

	if (!device_is_ready(flash_dev)) {
		LOG_ERR("flash device not ready");
		return;
	}
	int rc = flash_read(flash_dev, STORAGE_OFFSET, &cfg, sizeof(cfg));
	if (rc != 0) {
		LOG_ERR("flash_read failed (%d)", rc);
		return;
	}
	if (cfg.magic != CFG_MAGIC) {
		LOG_INF("No saved config (magic=0x%08x) — using defaults", cfg.magic);
		return;
	}

	/* Record fields and live buffers are the same size, so a terminated
	 * whole-buffer copy is bounded by construction. */
	BUILD_ASSERT(sizeof(password_str) == sizeof(cfg.password));
	BUILD_ASSERT(sizeof(password2_str) == sizeof(cfg.password2));
	BUILD_ASSERT(sizeof(username2_str) == sizeof(cfg.username2));
	BUILD_ASSERT(sizeof(beacon_key) == sizeof(cfg.beacon_key));
	cfg.password[PASSWORD_MAX_LEN] = '\0';
	cfg.password2[PASSWORD_MAX_LEN] = '\0';
	cfg.username2[PASSWORD_MAX_LEN] = '\0';
	memcpy(password_str, cfg.password, sizeof(password_str));
	memcpy(password2_str, cfg.password2, sizeof(password2_str));
	memcpy(username2_str, cfg.username2, sizeof(username2_str));
	beacon_key_set = (cfg.beacon_key_set == 1);
	if (beacon_key_set) {
		memcpy(beacon_key, cfg.beacon_key, sizeof(beacon_key));
	}
	rssi_green_threshold = cfg.green;
	rssi_blue_threshold = cfg.blue;
	memset(&cfg, 0, sizeof(cfg));   /* don't leave secrets on the stack */

	/* Guard against corrupt / out-of-range persisted data — fall back to the
	 * compiled defaults rather than running with nonsense thresholds. */
	if (!config_is_sane()) {
		LOG_WRN("Persisted config invalid — reverting to defaults");
		strcpy(password_str, DEFAULT_PASSWORD);
		password2_str[0] = '\0';
		username2_str[0] = '\0';
		memset(beacon_key, 0, sizeof(beacon_key));
		beacon_key_set = false;
		rssi_green_threshold = DEFAULT_RSSI_GREEN_THRESHOLD;
		rssi_blue_threshold = DEFAULT_RSSI_BLUE_THRESHOLD;
		return;
	}

	cfg_loaded = true;
	LOG_INF("Loaded config from flash (beacon key %s)",
		beacon_key_set ? "set" : "not set");
}

/* Save the live config to flash.  Erase+write — MUST be called with the radio
 * fully off (bt_disable), because flash is un-synced and the erase stalls the
 * CPU. */
static void config_save_to_flash(void)
{
	struct persist_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.magic = CFG_MAGIC;
	cfg.green = rssi_green_threshold;
	cfg.blue  = rssi_blue_threshold;
	strncpy(cfg.password, password_str, PASSWORD_MAX_LEN);
	strncpy(cfg.password2, password2_str, PASSWORD_MAX_LEN);
	strncpy(cfg.username2, username2_str, PASSWORD_MAX_LEN);
	memcpy(cfg.beacon_key, beacon_key, sizeof(cfg.beacon_key));
	cfg.beacon_key_set = beacon_key_set ? 1 : 0;

	if (!device_is_ready(flash_dev)) {
		LOG_ERR("flash device not ready");
		return;
	}

	int rc = flash_erase(flash_dev, STORAGE_OFFSET, STORAGE_ERASE);
	if (rc == 0) {
		rc = flash_write(flash_dev, STORAGE_OFFSET, &cfg, sizeof(cfg));
	}
	memset(&cfg, 0, sizeof(cfg));
	LOG_INF("Saved config to flash rc=%d", rc);
}

/* ---- Beacon counter log (replay protection across reboots) ------------- */
/* Two 4 KB pages after the config record hold an append-only list of uint32
 * counters (erased flash = 0xFFFFFFFF = empty slot).  The floor is the largest
 * value in either page.  When the active page fills, the OTHER page is erased
 * and written first, so a power cut mid-switch still leaves the old maximum.
 * ~1 entry per lock event plus one per 5 min of presence → a page lasts days,
 * far inside the flash's 10k erase cycles. */
#define CTR_LOG_PAGE_A  (STORAGE_OFFSET + 0x1000)
#define CTR_LOG_PAGE_B  (STORAGE_OFFSET + 0x2000)
#define CTR_LOG_WORDS   (STORAGE_ERASE / sizeof(uint32_t))
#define CTR_EMPTY       0xFFFFFFFFu
BUILD_ASSERT(STORAGE_OFFSET + 0x3000 <= PM_CONFIG_STORAGE_ADDRESS + PM_CONFIG_STORAGE_SIZE,
	     "counter log must fit in config_storage");

static off_t ctr_log_page = CTR_LOG_PAGE_A;  /* active page */
static size_t ctr_log_next;                  /* next free word in it */

/* Scan one page: returns its largest entry and the number of used words. */
static uint32_t ctr_log_scan(off_t page, size_t *used)
{
	uint32_t chunk[64];
	uint32_t max = 0;

	*used = CTR_LOG_WORDS;
	for (size_t base = 0; base < CTR_LOG_WORDS; base += ARRAY_SIZE(chunk)) {
		if (flash_read(flash_dev, page + (off_t)(base * sizeof(uint32_t)),
			       chunk, sizeof(chunk)) != 0) {
			*used = 0;
			return 0;
		}
		for (size_t i = 0; i < ARRAY_SIZE(chunk); i++) {
			if (chunk[i] == CTR_EMPTY) {
				if (*used == CTR_LOG_WORDS) {
					*used = base + i;
				}
			} else if (chunk[i] > max) {
				max = chunk[i];
			}
		}
	}
	return max;
}

/* Boot: load the floor and pick the page to append to.  Call before bt_enable. */
static void ctr_log_load(void)
{
	size_t used_a, used_b;
	uint32_t max_a = ctr_log_scan(CTR_LOG_PAGE_A, &used_a);
	uint32_t max_b = ctr_log_scan(CTR_LOG_PAGE_B, &used_b);

	if (max_b > max_a) {
		ctr_log_page = CTR_LOG_PAGE_B;
		ctr_log_next = used_b;
		beacon_ctr_floor = max_b;
	} else {
		ctr_log_page = CTR_LOG_PAGE_A;
		ctr_log_next = used_a;
		beacon_ctr_floor = max_a;
	}
	ctr_logged = beacon_ctr_floor;
	beacon_ctr_last = beacon_ctr_floor;
}

/* New phone key: the old phone's counters mean nothing now.  Radio must be off
 * (called from the config save path, inside bt_disable()). */
static void ctr_log_reset(void)
{
	(void)flash_erase(flash_dev, CTR_LOG_PAGE_A, STORAGE_ERASE);
	(void)flash_erase(flash_dev, CTR_LOG_PAGE_B, STORAGE_ERASE);
	ctr_log_page = CTR_LOG_PAGE_A;
	ctr_log_next = 0;
	beacon_ctr_floor = 0;
	beacon_ctr_last = 0;
	ctr_logged = 0;
}

/* Workqueue: append the newest accepted counter if it isn't logged yet.  Flash
 * uses MPSL radio sync, so this is safe while scanning. */
static void ctr_log_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	uint32_t v = beacon_ctr_last;

	if (v <= ctr_logged || v == CTR_EMPTY) {
		return;
	}
	if (ctr_log_next >= CTR_LOG_WORDS) {
		off_t other = (ctr_log_page == CTR_LOG_PAGE_A) ? CTR_LOG_PAGE_B : CTR_LOG_PAGE_A;

		if (flash_erase(flash_dev, other, STORAGE_ERASE) != 0) {
			return;
		}
		ctr_log_page = other;
		ctr_log_next = 0;
	}
	if (flash_write(flash_dev, ctr_log_page + (off_t)(ctr_log_next * sizeof(uint32_t)),
			&v, sizeof(v)) == 0) {
		ctr_log_next++;
		ctr_logged = v;
	}
}

K_WORK_DEFINE(ctr_log_work, ctr_log_fn);

static void ctr_log_request(void)
{
	k_work_submit(&ctr_log_work);
}

/* ---- Encrypted config session (ECDH, see cfg_crypto.h) ----------------- */
/* One dongle key pair per config window; a fresh session (phone key pair →
 * shared secret → k_enc/k_mac) per connection.  Secret writes are refused
 * until the session exists. */
static psa_key_id_t sess_priv;
static uint8_t sess_pub[CFG_PUB_LEN];
static bool sess_pub_ok;
static volatile bool sess_ready;
static uint8_t sess_enc[CFG_SUBKEY_LEN];
static uint8_t sess_mac[CFG_SUBKEY_LEN];
static uint32_t sess_seq;

/* Config window opening: make this window's ECDH key pair (CC310). */
static void session_keypair_new(void)
{
	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	size_t len = 0;

	sess_pub_ok = false;
	if (psa_crypto_init() != PSA_SUCCESS) {
		LOG_ERR("psa_crypto_init failed");
		return;
	}
	psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
	psa_set_key_bits(&attr, 256);
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
	psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
	if (psa_generate_key(&attr, &sess_priv) != PSA_SUCCESS) {
		LOG_ERR("ECDH key generation failed");
		return;
	}
	if (psa_export_public_key(sess_priv, sess_pub, sizeof(sess_pub), &len) != PSA_SUCCESS ||
	    len != CFG_PUB_LEN) {
		LOG_ERR("ECDH public key export failed");
		(void)psa_destroy_key(sess_priv);
		return;
	}
	sess_pub_ok = true;
}

static void session_clear(void)
{
	sess_ready = false;
	sess_seq = 0;
	memset(sess_enc, 0, sizeof(sess_enc));
	memset(sess_mac, 0, sizeof(sess_mac));
}

/* Config window closing: forget everything. */
static void session_keypair_destroy(void)
{
	session_clear();
	if (sess_pub_ok) {
		(void)psa_destroy_key(sess_priv);
		sess_pub_ok = false;
	}
}

static ssize_t read_session(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 sess_pub, sess_pub_ok ? sizeof(sess_pub) : 0);
}

/* The phone's public key: run ECDH and derive this connection's keys.  No
 * button gate needed — it carries no secret; the secret writes stay gated. */
static ssize_t write_session(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len,
			     uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);
	uint8_t z[CFG_SECRET_LEN];
	size_t zlen = 0;

	if (!config_mode || !sess_pub_ok) {
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != CFG_PUB_LEN || ((const uint8_t *)buf)[0] != 0x04) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	session_clear();
	/* PSA validates that the point is on the curve. */
	if (psa_raw_key_agreement(PSA_ALG_ECDH, sess_priv, buf, len,
				  z, sizeof(z), &zlen) != PSA_SUCCESS ||
	    zlen != sizeof(z)) {
		LOG_WRN("ECDH failed (bad phone key?)");
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	cfg_session_derive(z, sess_pub, buf, sess_enc, sess_mac);
	memset(z, 0, sizeof(z));
	sess_ready = true;
	LOG_INF("Config session established");
	return len;
}

/* Unseal a secret write into out.  Returns the plaintext length, or a
 * BT_GATT_ERR (no session, bad tag, replay, too long). */
static ssize_t open_secret(uint8_t id, const void *buf, uint16_t len,
			   uint8_t *out, size_t out_max)
{
	if (!sess_ready) {
		LOG_WRN("Secret write rejected: no encrypted session");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	int n = cfg_open(sess_enc, sess_mac, id, &sess_seq, buf, len, out, out_max);

	if (n < 0) {
		LOG_WRN("Secret write rejected: bad seal");
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}
	return n;
}

/* ---- GATT config service ----------------------------------------------- */

static ssize_t write_password(struct bt_conn *conn,
			      const struct bt_gatt_attr *attr,
			      const void *buf, uint16_t len,
			      uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (!config_mode) {
		LOG_WRN("Password write rejected: not in config window");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (!button_held) {
		LOG_WRN("Password write rejected: button not held");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	uint8_t pt[PASSWORD_MAX_LEN];
	ssize_t n = open_secret(SEAL_ID_PASSWORD, buf, len, pt, sizeof(pt));

	if (n < 0) {
		return n;
	}
	if (n == 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(password_str, pt, (size_t)n);
	password_str[n] = '\0';
	memset(pt, 0, sizeof(pt));
	config_dirty = true;

	LOG_INF("Password write accepted (%d chars)", (int)n);
	return len;
}

/* Password 2 and its username (medium press) are optional: an empty write
 * clears them.  `live` is a PASSWORD_MAX_LEN + 1 buffer. */
static ssize_t write_optional_str(const void *buf, uint16_t len,
				  uint16_t offset, uint8_t seal_id, char *live,
				  const char *what)
{
	if (!config_mode) {
		LOG_WRN("%s write rejected: not in config window", what);
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (!button_held) {
		LOG_WRN("%s write rejected: button not held", what);
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	uint8_t pt[PASSWORD_MAX_LEN];
	ssize_t n = open_secret(seal_id, buf, len, pt, sizeof(pt));

	if (n < 0) {
		return n;
	}

	memcpy(live, pt, (size_t)n);
	live[n] = '\0';
	memset(pt, 0, sizeof(pt));
	config_dirty = true;

	LOG_INF("%s write accepted (%d chars)", what, (int)n);
	return len;
}

static ssize_t write_password2(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len,
			       uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	return write_optional_str(buf, len, offset, SEAL_ID_PASSWORD2, password2_str,
				  "Password2");
}

static ssize_t write_username2(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len,
			       uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	return write_optional_str(buf, len, offset, SEAL_ID_USERNAME2, username2_str,
				  "Username2");
}

/* Pair with a phone: its 16-byte rolling-beacon key.  Same physical gate as
 * every other write.  The app only sends it when the key-ID read shows the
 * dongle doesn't already hold this phone's key. */
static ssize_t write_beacon_key(struct bt_conn *conn,
				const struct bt_gatt_attr *attr,
				const void *buf, uint16_t len,
				uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (!config_mode) {
		LOG_WRN("Beacon-key write rejected: not in config window");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (!button_held) {
		LOG_WRN("Beacon-key write rejected: button not held");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}

	uint8_t key[BEACON_KEY_LEN];
	ssize_t n = open_secret(SEAL_ID_BEACON_KEY, buf, len, key, sizeof(key));

	if (n < 0) {
		return n;
	}
	if (n != BEACON_KEY_LEN) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	if (!beacon_key_set || memcmp(beacon_key, key, BEACON_KEY_LEN) != 0) {
		beacon_key_changed = true;
	}
	memcpy(beacon_key, key, BEACON_KEY_LEN);
	memset(key, 0, sizeof(key));
	beacon_key_set = true;
	config_dirty = true;

	LOG_INF("Beacon key written");
	return len;
}

/* Non-secret ID of the stored key (empty when unpaired), so the app can skip
 * re-sending a key the dongle already has.  No gate: it reveals nothing. */
static ssize_t read_key_id(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   void *buf, uint16_t len, uint16_t offset)
{
	uint8_t id[BEACON_KEY_ID_LEN] = {0};

	if (!beacon_key_set) {
		return bt_gatt_attr_read(conn, attr, buf, len, offset, id, 0);
	}
	beacon_auth_key_id(beacon_key, id);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, id, sizeof(id));
}

static ssize_t write_threshold(struct bt_conn *conn,
			       const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len,
			       uint16_t offset, uint8_t flags,
			       int8_t *live, const char *what)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (!config_mode) {
		LOG_WRN("%s write rejected: not in config window", what);
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (!button_held) {
		LOG_WRN("%s write rejected: button not held", what);
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len != sizeof(int8_t)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	int8_t v = *(const int8_t *)buf;
	if (v < RSSI_MIN_DBM || v > RSSI_MAX_DBM) {
		LOG_WRN("%s out of range (%d dBm)", what, v);
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	*live = v;
	config_dirty = true;

	LOG_INF("%s write accepted (%d dBm)", what, *live);
	return len;
}

static ssize_t write_green(struct bt_conn *conn,
			   const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len,
			   uint16_t offset, uint8_t flags)
{
	return write_threshold(conn, attr, buf, len, offset, flags,
			       &rssi_green_threshold, "Green threshold");
}

static ssize_t write_blue(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  const void *buf, uint16_t len,
			  uint16_t offset, uint8_t flags)
{
	return write_threshold(conn, attr, buf, len, offset, flags,
			       &rssi_blue_threshold, "Blue threshold");
}

/* ---- Firmware update (MCUboot + MCUmgr/SMP) ---------------------------- */

static ssize_t write_dfu_arm(struct bt_conn *conn,
			     const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len,
			     uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (!config_mode) {
		LOG_WRN("DFU arm rejected: not in config window");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (!button_held) {
		LOG_WRN("DFU arm rejected: button not held");
		return BT_GATT_ERR(BT_ATT_ERR_AUTHORIZATION);
	}
	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	/* Require the magic byte 0x01 so a stray/short write can't arm us. */
	if (len != 1 || *(const uint8_t *)buf != 0x01) {
		return BT_GATT_ERR(BT_ATT_ERR_VALUE_NOT_ALLOWED);
	}

	bt_addr_le_copy(&dfu_peer, bt_conn_get_dst(conn));
	dfu_deadline_ms = k_uptime_get() + DFU_ARM_WINDOW_MS;
	dfu_arm_conn_open = true;
	dfu_armed = true;
	LOG_INF("DFU armed — SMP image upload unlocked for this phone");
	return len;
}

static ssize_t read_board(struct bt_conn *conn,
			  const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	static const char board[] = CONFIG_BOARD;

	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 board, sizeof(board) - 1);
}

/* Every SMP command passes through here first: refuse all of them unless the
 * owner armed DFU with the button held. */
static enum mgmt_cb_return smp_gate_cb(uint32_t event,
				       enum mgmt_cb_return prev_status,
				       int32_t *rc, uint16_t *group,
				       bool *abort_more, void *data,
				       size_t data_size)
{
	ARG_UNUSED(event);
	ARG_UNUSED(prev_status);
	ARG_UNUSED(group);
	ARG_UNUSED(abort_more);
	ARG_UNUSED(data);
	ARG_UNUSED(data_size);

	if (dfu_armed && config_mode) {
		return MGMT_CB_OK;
	}
	*rc = MGMT_ERR_EACCESSDENIED;
	return MGMT_CB_ERROR_RC;
}

static struct mgmt_callback smp_gate = {
	.callback = smp_gate_cb,
	.event_id = MGMT_EVT_OP_CMD_RECV,
};

BT_GATT_SERVICE_DEFINE(cfg_svc,
	BT_GATT_PRIMARY_SERVICE(&cfg_svc_uuid),
	BT_GATT_CHARACTERISTIC(&cfg_pw_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_password, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_green_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_green, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_blue_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_blue, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_boot_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_dfu_arm, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_board_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_board, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_pw2_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_password2, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_user2_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_username2, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_key_uuid.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, write_beacon_key, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_keyid_uuid.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       read_key_id, NULL, NULL),
	BT_GATT_CHARACTERISTIC(&cfg_session_uuid.uuid,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
			       read_session, write_session, NULL),
);

/* ---- Connectable advertising ------------------------------------------- */

/* Manufacturer data in the config-window advert: FF FF 'B' 'P' 10 <flags>.
 * flags bit 0 = button held right now.  With several dongles in config mode
 * nearby (an office), the app connects only to the one whose owner is holding
 * its button, instead of the first one it hears. */
#define CFG_ADV_TYPE      0x10
#define CFG_ADV_HELD      0x01
static uint8_t adv_mfg[] = { 0xFF, 0xFF, BEACON_MAGIC0, BEACON_MAGIC1,
			     CFG_ADV_TYPE, 0x00 };

static const struct bt_data adv_ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, CFG_SVC_UUID_VAL),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, adv_mfg, sizeof(adv_mfg)),
};

static const struct bt_data adv_sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int start_advertising(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2,
				  adv_ad, ARRAY_SIZE(adv_ad),
				  adv_sd, ARRAY_SIZE(adv_sd));
	if (err) {
		LOG_ERR("Advertising start failed (%d)", err);
	} else {
		LOG_INF("Advertising as '%s' (config service)",
			CONFIG_BT_DEVICE_NAME);
	}
	return err;
}

/* While DFU is armed, advertising is restarted after each disconnect so the
 * phone's SMP client can (re)connect.  Done from the workqueue once the old
 * connection object is recycled (with one connection allowed, advertising
 * can't restart before then). */
static volatile bool adv_restart_pending;

static void adv_restart_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!config_mode || !dfu_armed) {
		return;
	}
	ble_adv_err = start_advertising();
	if (ble_adv_err == -EALREADY) {
		ble_adv_err = 0;
	}
	ble_adv_running = (ble_adv_err == 0);
}

K_WORK_DEFINE(adv_restart_work, adv_restart_fn);

/* Button pressed/released in the config window: refresh the "held" flag in the
 * advert (from the workqueue — the button callback runs in interrupt context).
 * While connected the controller isn't advertising and the update just fails;
 * the flag is still current for the next (DFU) restart. */
static void adv_flag_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	adv_mfg[5] = button_held ? CFG_ADV_HELD : 0;
	if (config_mode && ble_adv_running) {
		(void)bt_le_adv_update_data(adv_ad, ARRAY_SIZE(adv_ad),
					    adv_sd, ARRAY_SIZE(adv_sd));
	}
}

K_WORK_DEFINE(adv_flag_work, adv_flag_fn);

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("Connection failed (0x%02x)", err);
		if (config_mode && dfu_armed) {
			k_work_submit(&adv_restart_work);
		}
		return;
	}
	/* Once DFU is armed, only the phone that armed it may reconnect. */
	if (config_mode && dfu_armed &&
	    !bt_addr_le_eq(bt_conn_get_dst(conn), &dfu_peer)) {
		LOG_WRN("DFU armed for another phone — dropping connection");
		dfu_conn_foreign = true;
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	}
	cfg_conn = conn;
	LOG_INF("Config client connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	cfg_conn = NULL;
	session_clear();   /* a new connection must do its own ECDH */
	LOG_INF("Config client disconnected (0x%02x)", reason);
	if (!config_mode) {
		return;
	}
	/* DFU armed: the arming GATT session (or a refused stranger) going away
	 * is expected — keep the window open and advertise for the SMP client. */
	if (dfu_armed && (dfu_arm_conn_open || dfu_conn_foreign)) {
		dfu_arm_conn_open = false;
		dfu_conn_foreign = false;
		adv_restart_pending = true;
		return;
	}
	/* End the config window early once the phone is done and disconnects. */
	k_sem_give(&config_done_sem);
}

static void on_recycled(void)
{
	if (adv_restart_pending) {
		adv_restart_pending = false;
		k_work_submit(&adv_restart_work);
	}
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = on_connected,
	.disconnected = on_disconnected,
	.recycled     = on_recycled,
};

/* ---- Button (input subsystem, INPUT_KEY_0 = SW1) ----------------------- */

static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->code != INPUT_KEY_0) {
		return;
	}

	bool pressed = (evt->value == 1);

	/* During the config window the button has two roles:
	 *   - held down  = the physical "authorize write" gate (BLE config writes
	 *     are only accepted while it is held; see the GATT write handlers);
	 *   - short tap (< CFG_EXIT_TAP_MS) = exit the config window early into
	 *     normal mode.
	 * It does not type the password or arm the status dump in this mode. */
	if (config_mode) {
		static int64_t press_ms;

		if (pressed) {
			press_ms = k_uptime_get();
			button_held = true;
		} else {
			button_held = false;
			if ((k_uptime_get() - press_ms) < CFG_EXIT_TAP_MS) {
				k_sem_give(&config_done_sem);
			}
		}
		k_work_submit(&adv_flag_work);
		return;
	}

	static int64_t down_ms;

	if (pressed) {
		/* press: arm 5-second hold timer */
		down_ms = k_uptime_get();
		rssi_hold_fired = false;
		k_work_reschedule(&status_report_work, K_SECONDS(5));
	} else {
		/* release: cancel hold timer; the press length picks the password
		 * (see MEDIUM_PRESS_MS / LONG_PRESS_MS) */
		k_work_cancel_delayable(&status_report_work);
		int64_t held = k_uptime_get() - down_ms;

		if (rssi_hold_fired || held >= LONG_PRESS_MS ||
		    (beacon_state_t)atomic_get(&current_state) != STATE_GREEN) {
			return;
		}
		if (held < MEDIUM_PRESS_MS) {
			atomic_set(&type_which, 1);
		} else if (password2_str[0] != '\0') {
			atomic_set(&type_which, 2);
		} else {
			return;
		}
		k_sem_give(&type_trigger);
	}
}

INPUT_CALLBACK_DEFINE(NULL, button_cb, NULL);

/* ---- HID device callbacks ---------------------------------------------- */

static void kb_iface_ready(const struct device *dev, const bool ready)
{
	LOG_INF("HID device %s interface %s", dev->name,
		ready ? "ready" : "not ready");
	hid_ready = ready;
}

static int kb_get_report(const struct device *dev,
			 const uint8_t type, const uint8_t id,
			 const uint16_t len, uint8_t *const buf)
{
	ARG_UNUSED(dev);
	LOG_WRN("Get Report not implemented, Type %u ID %u", type, id);
	return 0;
}

static int kb_set_report(const struct device *dev,
			 const uint8_t type, const uint8_t id,
			 const uint16_t len, const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(len);
	if (type != HID_REPORT_TYPE_OUTPUT) {
		return -ENOTSUP;
	}
	return 0;
}

static void kb_set_idle(const struct device *dev,
			const uint8_t id, const uint32_t duration)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(duration);
}

static uint32_t kb_get_idle(const struct device *dev, const uint8_t id)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	return 0;
}

static void kb_set_protocol(const struct device *dev, const uint8_t proto)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(proto);
}

static void kb_output_report(const struct device *dev, const uint16_t len,
			     const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);
}

static struct hid_device_ops kb_ops = {
	.iface_ready   = kb_iface_ready,
	.get_report    = kb_get_report,
	.set_report    = kb_set_report,
	.set_idle      = kb_set_idle,
	.get_idle      = kb_get_idle,
	.set_protocol  = kb_set_protocol,
	.output_report = kb_output_report,
};

/* ---- USB message callback ---------------------------------------------- */

static void usb_msg_cb(struct usbd_context *const usbd_ctx,
		       const struct usbd_msg *const msg)
{
	LOG_INF("USBD: %s", usbd_msg_type_string(msg->type));

	if (usbd_can_detect_vbus(usbd_ctx)) {
		if (msg->type == USBD_MSG_VBUS_READY) {
			if (usbd_enable(usbd_ctx)) {
				LOG_ERR("Failed to enable USB");
			}
		}
		if (msg->type == USBD_MSG_VBUS_REMOVED) {
			if (usbd_disable(usbd_ctx)) {
				LOG_ERR("Failed to disable USB");
			}
		}
	}
}

/* ---- HID key send ------------------------------------------------------ */

static void send_key(uint8_t modifier, uint8_t keycode)
{
	memset(hid_report, 0, KB_REPORT_COUNT);
	hid_report[KB_MOD_KEY]   = modifier;
	hid_report[KB_KEY_CODE1] = keycode;
	hid_device_submit_report(hid_dev, KB_REPORT_COUNT, hid_report);
	k_msleep(10);

	memset(hid_report, 0, KB_REPORT_COUNT);
	hid_device_submit_report(hid_dev, KB_REPORT_COUNT, hid_report);
	k_msleep(10);
}

/* ---- Password typing thread -------------------------------------------- */

/* Type a stored credential verbatim (unlike type_string, '\n' isn't ENTER). */
static void type_secret(const char *s)
{
	for (; *s != '\0'; s++) {
		uint8_t mod, key;

		if (ascii_to_hid(*s, &mod, &key)) {
			send_key(mod, key);
		}
	}
}

static void type_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (true) {
		k_sem_take(&type_trigger, K_FOREVER);

		if (!hid_ready) {
			LOG_WRN("HID not ready, skipping");
			continue;
		}

		/* send up-arrow to wake Windows */
		send_key(0, HID_KEY_UP);
		k_msleep(500);

		bool second = (atomic_get(&type_which) == 2);
		const char *pw = second ? password2_str : password_str;

		LOG_INF("Typing password %d", second ? 2 : 1);
		/* Login form: username, TAB to the password field, password. */
		if (second && username2_str[0] != '\0') {
			type_secret(username2_str);
			send_key(0, HID_KEY_TAB);
			k_msleep(100);   /* let the form move focus */
		}
		type_secret(pw);
		send_key(0, HID_KEY_ENTER);
	}
}

/* NOLINTNEXTLINE(bugprone-branch-clone,misc-misplaced-const): Zephyr macro internals */
K_THREAD_DEFINE(type_tid, 1024, type_thread_fn, NULL, NULL, NULL, 7, 0, 0);

/* ---- Fatal error indicator --------------------------------------------- */
/* Halts and blinks the RED LED a fixed number of times, forever, so a boot
 * failure is diagnosable without a serial console:
 *   1 blink  = Bluetooth enable failed
 *   2 blinks = BLE scan (observer) failed to start
 * A *blinking* red is always abnormal (normal "far" state is red SOLID).
 */
/* Any fatal fault (hard fault, assert, stack overflow, …) fast-flashes RED
 * forever, so a crash is instantly distinguishable from a hang (LEDs frozen)
 * or normal operation. Runs with the scheduler dead, so it busy-waits. */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(reason);
	ARG_UNUSED(esf);

	(void)gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
#if !SINGLE_LED
	(void)gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_INACTIVE);
	(void)gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_INACTIVE);
#endif

	while (1) {
		gpio_pin_set_dt(&ALERT_LED, 1);
		for (volatile uint32_t i = 0; i < 300000; i++) {
		}
		gpio_pin_set_dt(&ALERT_LED, 0);
		for (volatile uint32_t i = 0; i < 300000; i++) {
		}
	}
}

static void error_halt_blink(int count)
{
	k_timer_stop(&led_timer);
	gpio_pin_set_dt(&led_green, 0);
#if !SINGLE_LED
	gpio_pin_set_dt(&led_blue,  0);
	gpio_pin_set_dt(&led_red,   0);
#endif

	while (true) {
		for (int i = 0; i < count; i++) {
			gpio_pin_set_dt(&ALERT_LED, 1);
			k_msleep(200);
			gpio_pin_set_dt(&ALERT_LED, 0);
			k_msleep(200);
		}
		k_msleep(1200);
	}
}

/* ---- Deferred BLE bring-up --------------------------------------------- */
/* bt_enable() + NVS flash access + scan start are CPU/flash heavy and, on a
 * cold plug-in, would run during the USB enumeration window and starve it
 * (Windows then reports "USB device not recognized").  So USB is brought up
 * first in main(); this thread waits for enumeration to settle, then starts
 * Bluetooth. */
static void ble_init_thread_fn(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* Let USB finish enumerating before we touch the radio or flash. */
	k_msleep(3000);

	/* LOAD persisted config BEFORE bt_enable(), i.e. with the radio fully off
	 * (flash is un-synced).  Read-only, so it cannot hang. */
	config_load_from_flash();
	ctr_log_load();

	/* MCUboot: if this image was just installed as a test swap, mark it good
	 * now that it has booted this far.  If it crashes before reaching here,
	 * MCUboot reverts to the previous image on the next reset. */
	if (!boot_is_img_confirmed()) {
		int rc = boot_write_img_confirmed();

		LOG_INF("Image confirmed (rc=%d)", rc);
	}

	mgmt_callback_register(&smp_gate);

	int ret = bt_enable(NULL);
	if (ret) {
		LOG_ERR("BLE enable failed (%d)", ret);
		error_halt_blink(1);
	}

	/* ---- CONFIG WINDOW: advertise connectable so the app can connect and
	 * write settings (applied live).  No scanning during this window — this
	 * controller won't do connectable-advertising + scanning together. */
	session_keypair_new();
	config_mode = true;
	ble_adv_err = start_advertising();
	ble_adv_running = (ble_adv_err == 0);
	LOG_INF("Config window open (%d s)", CONFIG_WINDOW_MS / 1000);

	/* Wait for the window to elapse, or end early when the app disconnects or
	 * the button is tapped.  Arming DFU extends the deadline (re-checked each
	 * time the wait times out). */
	int64_t window_end = k_uptime_get() + CONFIG_WINDOW_MS;

	while (true) {
		int64_t deadline = window_end;

		if (dfu_armed && dfu_deadline_ms > deadline) {
			deadline = dfu_deadline_ms;
		}
		int64_t left = deadline - k_uptime_get();

		if (left <= 0 ||
		    k_sem_take(&config_done_sem, K_MSEC(left)) == 0) {
			break;
		}
	}

	config_mode = false;
	dfu_armed = false;
	session_keypair_destroy();
	if (ble_adv_running) {
		bt_le_adv_stop();
		ble_adv_running = false;
	}
	if (cfg_conn) {
		bt_conn_disconnect(cfg_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		k_msleep(500);
		cfg_conn = NULL;
	}

	/* SAVE to flash.  The radio must be FULLY off during the flash erase/write
	 * (un-synced flash + a ~85 ms CPU stall faults the running controller), so
	 * bt_disable() first, then re-enable for scanning. */
	if (config_dirty) {
		config_dirty = false;

		if (!config_is_sane()) {
			/* Bad combination written during the window (out-of-range,
			 * green<=blue, or empty password): do NOT persist — the next
			 * boot keeps the previously-saved config.  Signal rejection
			 * with a fast 6x red blink (3x green = accepted). */
			LOG_WRN("Config invalid — not saving");
			k_timer_stop(&led_timer);
			for (int i = 0; i < 6; i++) {
				gpio_pin_set_dt(&ALERT_LED, 1);
				k_msleep(80);
				gpio_pin_set_dt(&ALERT_LED, 0);
				k_msleep(80);
			}
			k_timer_start(&led_timer, K_MSEC(LED_TICK_MS), K_MSEC(LED_TICK_MS));
		} else {
			bt_disable();
			config_save_to_flash();
			if (beacon_key_changed) {
				beacon_key_changed = false;
				ctr_log_reset();
			}
			ret = bt_enable(NULL);
			if (ret) {
				LOG_ERR("BLE re-enable failed (%d)", ret);
				error_halt_blink(1);
			}
			/* Confirm save with 3 green blinks so it's safe to power-cycle. */
			k_timer_stop(&led_timer);
			for (int i = 0; i < 3; i++) {
				gpio_pin_set_dt(&led_green, 1);
				k_msleep(150);
				gpio_pin_set_dt(&led_green, 0);
				k_msleep(150);
			}
			k_timer_start(&led_timer, K_MSEC(LED_TICK_MS), K_MSEC(LED_TICK_MS));
		}
	}

	/* ---- NORMAL MODE: scan for the target beacon (proximity). */
	bt_le_scan_cb_register(&scan_cbs);

	ret = -1;
	for (int attempt = 0; attempt < 5; attempt++) {
		ret = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
		if (ret == 0 || ret == -EALREADY) {
			ret = 0;
			break;
		}
		LOG_ERR("BLE scan start failed (%d), retrying", ret);
		k_msleep(300);
	}
	ble_scan_running = (ret == 0);

	LOG_INF("BLE Password initialized — scanning for the paired phone (key %s)",
		beacon_key_set ? "set" : "NOT set");
}

/* NOLINTNEXTLINE(bugprone-branch-clone,misc-misplaced-const): Zephyr macro internals */
K_THREAD_DEFINE(ble_init_tid, 4096, ble_init_thread_fn,   /* 4 KB: ECDH keygen */ NULL, NULL, NULL,
		7, 0, 0);

/* ---- HID status dump (triggered by 5-second button hold) --------------- */

static void type_full_status(void)
{
	if (!hid_ready) {
		return;
	}

	char line[160];
	int64_t since = k_uptime_get() - last_beacon_seen_ms;
	if (since > 9999999) {
		since = 9999999;
	}
	beacon_state_t st = (beacon_state_t)atomic_get(&current_state);
	const char *stn = (st == STATE_GREEN) ? "GREEN" :
			  (st == STATE_RED)   ? "RED" : "BLUE";

	type_string("=== BLE_Password ===\n");
	/* confirmed=0 means MCUboot will roll back to the old image on reset. */
	snprintf(line, sizeof(line), "fw=%s board=%s confirmed=%d\n",
		 APP_VERSION_STRING, CONFIG_BOARD, (int)boot_is_img_confirmed());
	type_string(line);
	snprintf(line, sizeof(line),
		 "up_ms=%lld scan=%d adv=%d saved=%d hid=%d\n",
		 (long long)k_uptime_get(), (int)ble_scan_running,
		 (int)ble_adv_running, (int)cfg_loaded, (int)hid_ready);
	type_string(line);
	/* ads = everything heard; beacons = rolling beacons from any phone;
	 * match = ours, verified + fresh; stale = ours but an old counter. */
	snprintf(line, sizeof(line), "ads=%ld beacons=%ld match=%ld stale=%ld\n",
		 (long)atomic_get(&scan_total_count),
		 (long)atomic_get(&scan_fmt_count),
		 (long)atomic_get(&scan_match_count),
		 (long)atomic_get(&scan_stale_count));
	type_string(line);
	if (beacon_key_set) {
		uint8_t id[BEACON_KEY_ID_LEN];

		beacon_auth_key_id(beacon_key, id);
		snprintf(line, sizeof(line),
			 "beacon_key=%02x%02x%02x%02x ctr=%lu floor=%lu\n",
			 id[0], id[1], id[2], id[3],
			 (unsigned long)beacon_ctr_last, (unsigned long)beacon_ctr_floor);
	} else {
		snprintf(line, sizeof(line), "beacon_key=(not set: pair from the app)\n");
	}
	type_string(line);
	/* Same rule as typing the password: only reveal it while the owner's
	 * phone is in range (GREEN). */
	if (st == STATE_GREEN) {
		snprintf(line, sizeof(line), "password=%s\n", password_str);
		type_string(line);
		snprintf(line, sizeof(line), "username2=%s\n",
			 username2_str[0] ? username2_str : "(not set)");
		type_string(line);
		snprintf(line, sizeof(line), "password2=%s\n",
			 password2_str[0] ? password2_str : "(not set)");
	} else {
		snprintf(line, sizeof(line), "password=(hidden: phone not in range)\n");
	}
	type_string(line);
	snprintf(line, sizeof(line), "green=%d blue=%d state=%s seen_ago=%lld\n",
		 rssi_green_threshold, rssi_blue_threshold, stn, (long long)since);
	type_string(line);
	/* "averaged_" so it isn't mistaken for a setting.  Once the beacon has
	 * timed out the last average is stale, so say "gone" instead. */
	if (since >= BEACON_TIMEOUT_MS) {
		snprintf(line, sizeof(line), "averaged_rssi=gone\n");
	} else {
		snprintf(line, sizeof(line), "averaged_rssi=%d\n", (int)rssi_last_avg);
	}
	type_string(line);
}

/* ---- main -------------------------------------------------------------- */

int main(void)
{
	int ret;

	/* Configure LEDs. */
	(void)gpio_pin_configure_dt(&led_green, GPIO_OUTPUT_INACTIVE);
#if !SINGLE_LED
	(void)gpio_pin_configure_dt(&led_blue,  GPIO_OUTPUT_ACTIVE);  /* power-on blue */
	(void)gpio_pin_configure_dt(&led_red,   GPIO_OUTPUT_INACTIVE);
#endif

	/* Normal proximity LED state machine. */
	k_timer_start(&led_timer, K_MSEC(LED_TICK_MS), K_MSEC(LED_TICK_MS));

	/* HID + USB come up first and alone, so enumeration isn't starved by the
	 * radio/flash bring-up (which runs later in ble_init_thread_fn). */
	hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);
	if (hid_dev && device_is_ready(hid_dev)) {
		ret = hid_device_register(hid_dev, hid_report_desc,
					  sizeof(hid_report_desc), &kb_ops);
		if (ret) {
			LOG_ERR("HID register failed (%d)", ret);
		}

		struct usbd_context *sample_usbd =
			sample_usbd_init_device(usb_msg_cb);
		if (!sample_usbd) {
			LOG_ERR("USB init failed");
		} else if (!usbd_can_detect_vbus(sample_usbd)) {
			ret = usbd_enable(sample_usbd);
			if (ret) {
				LOG_ERR("USB enable failed (%d)", ret);
			}
		}
	} else {
		LOG_ERR("HID device not ready");
	}

	return 0;
}
