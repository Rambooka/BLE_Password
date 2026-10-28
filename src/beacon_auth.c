/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * SHA-256 / HMAC-SHA256 and the rolling-beacon check (see beacon_auth.h).
 * SHA-256 follows FIPS 180-4; verified against the FIPS and RFC 4231 vectors
 * in tests/host/beacon_auth_test.c.
 */
#include "beacon_auth.h"

#include <string.h>

struct sha256_ctx {
	uint32_t h[8];
	uint8_t  block[64];
	size_t   used;     /* bytes in block */
	uint64_t total;    /* message length in bytes */
};

static const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
	0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
	0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
	0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
	0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
	0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
	0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
	0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
	0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr(uint32_t x, unsigned int n)
{
	return (x >> n) | (x << (32u - n));
}

static void sha256_compress(struct sha256_ctx *c, const uint8_t *p)
{
	uint32_t w[64];

	for (int i = 0; i < 16; i++) {
		w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
		       ((uint32_t)p[4 * i + 2] << 8) | (uint32_t)p[4 * i + 3];
	}
	for (int i = 16; i < 64; i++) {
		uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
		uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);

		w[i] = w[i - 16] + s0 + w[i - 7] + s1;
	}

	uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
	uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];

	for (int i = 0; i < 64; i++) {
		uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
		uint32_t ch = (e & f) ^ (~e & g);
		uint32_t t1 = h + s1 + ch + K[i] + w[i];
		uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
		uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
		uint32_t t2 = s0 + maj;

		h = g;
		g = f;
		f = e;
		e = d + t1;
		d = cc;
		cc = b;
		b = a;
		a = t1 + t2;
	}

	c->h[0] += a;
	c->h[1] += b;
	c->h[2] += cc;
	c->h[3] += d;
	c->h[4] += e;
	c->h[5] += f;
	c->h[6] += g;
	c->h[7] += h;
}

static void sha256_init(struct sha256_ctx *c)
{
	static const uint32_t iv[8] = {
		0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
	};

	memcpy(c->h, iv, sizeof(iv));
	c->used = 0;
	c->total = 0;
}

static void sha256_update(struct sha256_ctx *c, const uint8_t *p, size_t len)
{
	c->total += len;
	while (len > 0) {
		size_t n = sizeof(c->block) - c->used;

		if (n > len) {
			n = len;
		}
		memcpy(c->block + c->used, p, n);
		c->used += n;
		p += n;
		len -= n;
		if (c->used == sizeof(c->block)) {
			sha256_compress(c, c->block);
			c->used = 0;
		}
	}
}

static void sha256_final(struct sha256_ctx *c, uint8_t out[SHA256_DIGEST_LEN])
{
	uint64_t bits = c->total * 8u;
	uint8_t pad = 0x80;

	sha256_update(c, &pad, 1);
	pad = 0;
	while (c->used != 56) {
		sha256_update(c, &pad, 1);
	}
	uint8_t len_be[8];

	for (int i = 0; i < 8; i++) {
		len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha256_update(c, len_be, sizeof(len_be));

	for (int i = 0; i < 8; i++) {
		out[4 * i]     = (uint8_t)(c->h[i] >> 24);
		out[4 * i + 1] = (uint8_t)(c->h[i] >> 16);
		out[4 * i + 2] = (uint8_t)(c->h[i] >> 8);
		out[4 * i + 3] = (uint8_t)c->h[i];
	}
}

void sha256(const uint8_t *msg, size_t len, uint8_t out[SHA256_DIGEST_LEN])
{
	struct sha256_ctx c;

	sha256_init(&c);
	sha256_update(&c, msg, len);
	sha256_final(&c, out);
}

void hmac_sha256(const uint8_t *key, size_t key_len,
		 const uint8_t *msg, size_t msg_len,
		 uint8_t out[SHA256_DIGEST_LEN])
{
	uint8_t pad[64];
	uint8_t inner[SHA256_DIGEST_LEN];
	struct sha256_ctx c;

	if (key_len > sizeof(pad)) {
		key_len = sizeof(pad);   /* not used with long keys; see header */
	}

	memset(pad, 0x36, sizeof(pad));
	for (size_t i = 0; i < key_len; i++) {
		pad[i] ^= key[i];
	}
	sha256_init(&c);
	sha256_update(&c, pad, sizeof(pad));
	sha256_update(&c, msg, msg_len);
	sha256_final(&c, inner);

	memset(pad, 0x5c, sizeof(pad));
	for (size_t i = 0; i < key_len; i++) {
		pad[i] ^= key[i];
	}
	sha256_init(&c);
	sha256_update(&c, pad, sizeof(pad));
	sha256_update(&c, inner, sizeof(inner));
	sha256_final(&c, out);

	memset(pad, 0, sizeof(pad));
}

bool beacon_auth_is_beacon(const uint8_t *mfg, size_t len)
{
	return len == BEACON_MFG_LEN &&
	       mfg[0] == (uint8_t)(BEACON_COMPANY_ID & 0xFF) &&
	       mfg[1] == (uint8_t)(BEACON_COMPANY_ID >> 8) &&
	       mfg[2] == BEACON_MAGIC0 && mfg[3] == BEACON_MAGIC1 &&
	       mfg[4] == BEACON_VERSION;
}

bool beacon_auth_verify(const uint8_t key[BEACON_KEY_LEN],
			const uint8_t *mfg, size_t len, uint32_t *ctr)
{
	if (!beacon_auth_is_beacon(mfg, len)) {
		return false;
	}

	/* Signed part: magic, version, counter (7 bytes after the company ID). */
	uint8_t mac[SHA256_DIGEST_LEN];

	hmac_sha256(key, BEACON_KEY_LEN, &mfg[2], 7, mac);

	/* Constant-time compare. */
	uint8_t diff = 0;

	for (int i = 0; i < BEACON_TAG_LEN; i++) {
		diff |= (uint8_t)(mac[i] ^ mfg[9 + i]);
	}
	if (diff != 0) {
		return false;
	}

	*ctr = (uint32_t)mfg[5] | ((uint32_t)mfg[6] << 8) |
	       ((uint32_t)mfg[7] << 16) | ((uint32_t)mfg[8] << 24);
	return true;
}

void beacon_auth_key_id(const uint8_t key[BEACON_KEY_LEN],
			uint8_t id[BEACON_KEY_ID_LEN])
{
	static const uint8_t label[] = "BLEPW-KEYID";
	uint8_t mac[SHA256_DIGEST_LEN];

	hmac_sha256(key, BEACON_KEY_LEN, label, sizeof(label) - 1, mac);
	memcpy(id, mac, BEACON_KEY_ID_LEN);
}
