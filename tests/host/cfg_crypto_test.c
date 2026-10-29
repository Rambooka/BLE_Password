/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit test for src/cfg_crypto.c (run with tests/host/run.ps1).
 * Vectors computed with Python's hmac module and shared with the Android
 * unit test (ConfigSessionTest.kt).  Z / public keys are fixed dummy bytes:
 * this tests the KDF + sealing; the ECDH itself is PSA (dongle) / JCA (phone).
 */
#include "cfg_crypto.h"

#include <stdio.h>
#include <string.h>

static int failures;

static size_t hex(const char *s, uint8_t *out)
{
	size_t n = strlen(s) / 2;

	for (size_t i = 0; i < n; i++) {
		unsigned int b;

		sscanf(s + 2 * i, "%2x", &b);
		out[i] = (uint8_t)b;
	}
	return n;
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
	uint8_t z1[32], z2[32], ip[65], dp[65], pp[65];
	uint8_t k_enc[32], k_mac[32], want[128], buf[128], pt[128];

	memset(z1, 0x11, sizeof(z1));
	memset(z2, 0x44, sizeof(z2));
	ip[0] = 0x04;
	memset(ip + 1, 0x55, 64);
	dp[0] = 0x04;
	memset(dp + 1, 0x22, 64);
	pp[0] = 0x04;
	memset(pp + 1, 0x33, 64);

	cfg_session_derive(z1, z2, ip, dp, pp, k_enc, k_mac);
	hex("6ab9ce9cbad04d216affe112127e29e38173f1c067a7cdd9e260606555655280", want);
	check("hkdf k_enc", memcmp(k_enc, want, 32) == 0);
	hex("ab7586a4b7d4ec34eb8c2adf87edbd36c4c66602f07ad9425de96e444f06ce4e", want);
	check("hkdf k_mac", memcmp(k_mac, want, 32) == 0);

	/* seal matches the Python vector */
	size_t n = cfg_seal(k_enc, k_mac, 0x02, 1, (const uint8_t *)"Secret-pw1", 10, buf);
	size_t wn = hex("010000003f9611d1dcf0b6cf524e0a527e78047c8f0d4dfdb3d2f1c84f19", want);

	check("seal vector", n == wn && memcmp(buf, want, n) == 0);

	/* open it back */
	uint32_t last = 0;
	int r = cfg_open(k_enc, k_mac, 0x02, &last, want, wn, pt, sizeof(pt));

	check("open vector", r == 10 && memcmp(pt, "Secret-pw1", 10) == 0 && last == 1);
	check("replay rejected", cfg_open(k_enc, k_mac, 0x02, &last, want, wn, pt, sizeof(pt)) < 0);

	last = 0;
	check("wrong characteristic rejected",
	      cfg_open(k_enc, k_mac, 0x08, &last, want, wn, pt, sizeof(pt)) < 0);
	want[6] ^= 0x01;
	check("tampered ciphertext rejected",
	      cfg_open(k_enc, k_mac, 0x02, &last, want, wn, pt, sizeof(pt)) < 0);

	uint8_t other_mac[32];

	memcpy(other_mac, k_mac, 32);
	other_mac[0] ^= 1;
	n = cfg_seal(k_enc, k_mac, 0x02, 2, (const uint8_t *)"x", 1, buf);
	check("other session's key rejected",
	      cfg_open(k_enc, other_mac, 0x02, &last, buf, n, pt, sizeof(pt)) < 0);

	/* 40 bytes = two keystream blocks */
	uint8_t pt40[40];

	for (int i = 0; i < 40; i++) {
		pt40[i] = (uint8_t)i;
	}
	n = cfg_seal(k_enc, k_mac, 0x0a, 7, pt40, sizeof(pt40), buf);
	wn = hex("07000000e3713fe194218679393ce494e9150f088a4b3da0298fbdcf69eb714e6b1ffe02"
		 "7ea53c3e90a9911e1512dea12d6bd6028ec9378d8083a8d7", want);
	check("seal 2-block vector", n == wn && memcmp(buf, want, n) == 0);
	last = 6;
	r = cfg_open(k_enc, k_mac, 0x0a, &last, buf, n, pt, sizeof(pt));
	check("open 2-block", r == 40 && memcmp(pt, pt40, 40) == 0);

	check("short input rejected", cfg_open(k_enc, k_mac, 0x02, &last, buf, 19, pt, sizeof(pt)) < 0);
	check("output too small rejected",
	      cfg_open(k_enc, k_mac, 0x0a, &last, buf, n, pt, 39) < 0);

	printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
