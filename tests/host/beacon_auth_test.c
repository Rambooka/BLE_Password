/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit test for src/beacon_auth.c (run with tests/host/run.ps1).
 * SHA-256 vectors: FIPS 180-4 examples.  HMAC vector: RFC 4231 test case 2.
 * Beacon / key-ID vectors: computed with Python's hmac module and shared with
 * the Android unit test (BeaconCryptoTest.kt), so both ends agree on the format.
 */
#include "beacon_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void hex(const char *s, uint8_t *out, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned int b;

		sscanf(s + 2 * i, "%2x", &b);
		out[i] = (uint8_t)b;
	}
}

static void expect(const char *what, const uint8_t *got, const char *want_hex)
{
	size_t n = strlen(want_hex) / 2;
	uint8_t want[64];

	hex(want_hex, want, n);
	if (memcmp(got, want, n) != 0) {
		printf("FAIL %s\n", what);
		failures++;
	} else {
		printf("ok   %s\n", what);
	}
}

static void check(const char *what, int cond)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", what);
	if (!cond) {
		failures++;
	}
}

int main(void)
{
	uint8_t d[SHA256_DIGEST_LEN];

	sha256((const uint8_t *)"abc", 3, d);
	expect("sha256 abc", d,
	       "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	const char *two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

	sha256((const uint8_t *)two_blocks, strlen(two_blocks), d);
	expect("sha256 448-bit", d,
	       "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

	uint8_t *million = malloc(1000000);

	memset(million, 'a', 1000000);
	sha256(million, 1000000, d);
	free(million);
	expect("sha256 million a", d,
	       "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");

	const char *tc2 = "what do ya want for nothing?";

	hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)tc2, strlen(tc2), d);
	expect("hmac rfc4231 tc2", d,
	       "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");

	/* Beacon: key 00..0f, counter 0x6A3B1C2D. */
	uint8_t key[BEACON_KEY_LEN];

	for (int i = 0; i < BEACON_KEY_LEN; i++) {
		key[i] = (uint8_t)i;
	}
	uint8_t mfg[BEACON_MFG_LEN];

	hex("ffff4250012d1c3b6a009ddddfe4f67fd5", mfg, sizeof(mfg));

	uint32_t ctr = 0;

	check("beacon format recognised", beacon_auth_is_beacon(mfg, sizeof(mfg)));
	check("beacon verifies", beacon_auth_verify(key, mfg, sizeof(mfg), &ctr));
	check("beacon counter", ctr == 0x6A3B1C2Du);

	uint8_t bad[BEACON_MFG_LEN];

	memcpy(bad, mfg, sizeof(bad));
	bad[16] ^= 0x01;   /* tag bit flip */
	check("tampered tag rejected", !beacon_auth_verify(key, bad, sizeof(bad), &ctr));

	memcpy(bad, mfg, sizeof(bad));
	bad[5] ^= 0x01;    /* counter changed, tag not recomputed */
	check("tampered counter rejected", !beacon_auth_verify(key, bad, sizeof(bad), &ctr));

	uint8_t other[BEACON_KEY_LEN];

	memcpy(other, key, sizeof(other));
	other[0] ^= 0x80;
	check("other phone's key rejected", !beacon_auth_verify(other, mfg, sizeof(mfg), &ctr));
	check("short field rejected", !beacon_auth_verify(key, mfg, sizeof(mfg) - 1, &ctr));

	uint8_t id[BEACON_KEY_ID_LEN];

	beacon_auth_key_id(key, id);
	expect("key id", id, "2fcd11b7");

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
