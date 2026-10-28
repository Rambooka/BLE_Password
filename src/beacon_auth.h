/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authenticated rolling beacon (fw 1.2.0+).
 *
 * The phone advertises manufacturer-specific data (company ID 0xFFFF, the
 * SIG's "internal use / testing" ID) laid out as:
 *
 *   FF FF  'B' 'P'  01  ctr[4, LE]  tag[8]          (17 bytes)
 *
 *   ctr = phone wall clock in seconds, strictly increasing (never repeats a
 *         lower value, even if the phone's clock is set back)
 *   tag = first 8 bytes of HMAC-SHA256(key, 'B' 'P' 01 ctr[4])
 *
 * key is a random 128-bit secret made by the phone and written to the dongle
 * once (button held, config window).  Each phone has its own key, so dongles
 * ignore every other phone's beacon; a recorded beacon is rejected because its
 * counter is stale.  This module is plain C with no Zephyr dependencies so it
 * can be unit-tested on the host (tests/host/).
 */
#ifndef BEACON_AUTH_H_
#define BEACON_AUTH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BEACON_COMPANY_ID   0xFFFFu
#define BEACON_MAGIC0       0x42u   /* 'B' */
#define BEACON_MAGIC1       0x50u   /* 'P' */
#define BEACON_VERSION      0x01u
#define BEACON_KEY_LEN      16
#define BEACON_TAG_LEN      8
#define BEACON_KEY_ID_LEN   4
/* Manufacturer data as the scanner sees it, company ID included. */
#define BEACON_MFG_LEN      (2 + 2 + 1 + 4 + BEACON_TAG_LEN)

#define SHA256_DIGEST_LEN   32

void sha256(const uint8_t *msg, size_t len, uint8_t out[SHA256_DIGEST_LEN]);

/* HMAC-SHA256 (RFC 2104).  key_len must be <= 64 (we only use 16-byte keys). */
void hmac_sha256(const uint8_t *key, size_t key_len,
		 const uint8_t *msg, size_t msg_len,
		 uint8_t out[SHA256_DIGEST_LEN]);

/* Check one manufacturer-data field (company ID first).  Returns true and sets
 * *ctr if it is our beacon format AND the tag verifies with key.  Freshness
 * (the counter check) is the caller's job. */
bool beacon_auth_verify(const uint8_t key[BEACON_KEY_LEN],
			const uint8_t *mfg, size_t len, uint32_t *ctr);

/* True if mfg is our beacon format (regardless of whose key made it). */
bool beacon_auth_is_beacon(const uint8_t *mfg, size_t len);

/* Short, non-secret identifier of a key: HMAC(key, "BLEPW-KEYID")[0..4].
 * Lets the app tell whether a dongle already holds its key without the key
 * ever being sent again. */
void beacon_auth_key_id(const uint8_t key[BEACON_KEY_LEN],
			uint8_t id[BEACON_KEY_ID_LEN]);

#endif /* BEACON_AUTH_H_ */
