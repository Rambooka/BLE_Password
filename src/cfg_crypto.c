/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encrypted config session: HKDF + HMAC-based encrypt-then-MAC (see cfg_crypto.h).
 */
#include "cfg_crypto.h"
#include "beacon_auth.h"

#include <string.h>

/* Largest sealed plaintext we handle (a 64-char password + NUL headroom). */
#define CFG_MAX_PT 80

void cfg_session_derive(const uint8_t z[CFG_SECRET_LEN],
			const uint8_t dongle_pub[CFG_PUB_LEN],
			const uint8_t phone_pub[CFG_PUB_LEN],
			uint8_t k_enc[CFG_SUBKEY_LEN],
			uint8_t k_mac[CFG_SUBKEY_LEN])
{
	static const uint8_t salt[] = "BLEPW-SESSION-v1";
	uint8_t prk[SHA256_DIGEST_LEN];
	/* T(2) input is the longest: T(1) || info || 0x02 */
	uint8_t buf[SHA256_DIGEST_LEN + 2 * CFG_PUB_LEN + 1];
	size_t n;

	/* HKDF-Extract */
	hmac_sha256(salt, sizeof(salt) - 1, z, CFG_SECRET_LEN, prk);

	/* HKDF-Expand: T(1) = HMAC(PRK, info || 01) */
	memcpy(buf, dongle_pub, CFG_PUB_LEN);
	memcpy(buf + CFG_PUB_LEN, phone_pub, CFG_PUB_LEN);
	buf[2 * CFG_PUB_LEN] = 0x01;
	hmac_sha256(prk, sizeof(prk), buf, 2 * CFG_PUB_LEN + 1, k_enc);

	/* T(2) = HMAC(PRK, T(1) || info || 02) */
	memcpy(buf, k_enc, SHA256_DIGEST_LEN);
	n = SHA256_DIGEST_LEN;
	memcpy(buf + n, dongle_pub, CFG_PUB_LEN);
	n += CFG_PUB_LEN;
	memcpy(buf + n, phone_pub, CFG_PUB_LEN);
	n += CFG_PUB_LEN;
	buf[n++] = 0x02;
	hmac_sha256(prk, sizeof(prk), buf, n, k_mac);

	memset(prk, 0, sizeof(prk));
	memset(buf, 0, sizeof(buf));
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)v;
	p[1] = (uint8_t)(v >> 8);
	p[2] = (uint8_t)(v >> 16);
	p[3] = (uint8_t)(v >> 24);
}

/* XOR data with the keystream for (id, seq). */
static void cfg_xor_stream(const uint8_t k_enc[CFG_SUBKEY_LEN], uint8_t id,
			   const uint8_t seq_le[4], uint8_t *data, size_t len)
{
	uint8_t msg[6];
	uint8_t block[SHA256_DIGEST_LEN];

	msg[0] = id;
	memcpy(&msg[1], seq_le, 4);
	for (size_t off = 0, j = 0; off < len; off += sizeof(block), j++) {
		msg[5] = (uint8_t)j;
		hmac_sha256(k_enc, CFG_SUBKEY_LEN, msg, sizeof(msg), block);
		for (size_t i = 0; i < sizeof(block) && off + i < len; i++) {
			data[off + i] ^= block[i];
		}
	}
	memset(block, 0, sizeof(block));
}

static void cfg_tag(const uint8_t k_mac[CFG_SUBKEY_LEN], uint8_t id,
		    const uint8_t seq_le[4], const uint8_t *ct, size_t ct_len,
		    uint8_t tag[SHA256_DIGEST_LEN])
{
	uint8_t msg[1 + 4 + CFG_MAX_PT];

	msg[0] = id;
	memcpy(&msg[1], seq_le, 4);
	memcpy(&msg[5], ct, ct_len);
	hmac_sha256(k_mac, CFG_SUBKEY_LEN, msg, 5 + ct_len, tag);
}

int cfg_open(const uint8_t k_enc[CFG_SUBKEY_LEN], const uint8_t k_mac[CFG_SUBKEY_LEN],
	     uint8_t id, uint32_t *last_seq,
	     const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max)
{
	if (in_len < CFG_SEAL_OVERHEAD) {
		return -1;
	}
	size_t ct_len = in_len - CFG_SEAL_OVERHEAD;

	if (ct_len > out_max || ct_len > CFG_MAX_PT) {
		return -1;
	}

	const uint8_t *seq_le = in;
	const uint8_t *ct = in + 4;
	const uint8_t *tag_in = in + 4 + ct_len;
	uint8_t tag[SHA256_DIGEST_LEN];
	uint8_t diff = 0;

	cfg_tag(k_mac, id, seq_le, ct, ct_len, tag);
	for (int i = 0; i < CFG_SEAL_TAG_LEN; i++) {
		diff |= (uint8_t)(tag[i] ^ tag_in[i]);
	}
	if (diff != 0) {
		return -1;
	}

	uint32_t seq = (uint32_t)seq_le[0] | ((uint32_t)seq_le[1] << 8) |
		       ((uint32_t)seq_le[2] << 16) | ((uint32_t)seq_le[3] << 24);

	if (seq <= *last_seq) {
		return -1;   /* replayed within this session */
	}
	*last_seq = seq;

	memcpy(out, ct, ct_len);
	cfg_xor_stream(k_enc, id, seq_le, out, ct_len);
	return (int)ct_len;
}

size_t cfg_seal(const uint8_t k_enc[CFG_SUBKEY_LEN], const uint8_t k_mac[CFG_SUBKEY_LEN],
		uint8_t id, uint32_t seq,
		const uint8_t *pt, size_t pt_len, uint8_t *out)
{
	uint8_t tag[SHA256_DIGEST_LEN];

	put_le32(out, seq);
	memcpy(out + 4, pt, pt_len);
	cfg_xor_stream(k_enc, id, out, out + 4, pt_len);
	cfg_tag(k_mac, id, out, out + 4, pt_len, tag);
	memcpy(out + 4 + pt_len, tag, CFG_SEAL_TAG_LEN);
	return pt_len + CFG_SEAL_OVERHEAD;
}
