/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encrypted config session (fw 1.2.0+): the secrets the app writes (passwords,
 * username 2, the beacon key) travel encrypted, not in the clear.
 *
 *   1. Per connection, the phone reads the dongle's P-256 public key and writes
 *      its own fresh one (both 65-byte uncompressed points).  Each side computes
 *      the ECDH shared secret Z (32-byte x-coordinate).
 *   2. HKDF-SHA256 (RFC 5869), salt "BLEPW-SESSION-v1", IKM Z,
 *      info = dongle_pub || phone_pub, L = 64  →  k_enc (32) || k_mac (32).
 *   3. Each secret write is  seq[4, LE] || ct || tag[16]  where
 *        ct  = pt XOR (HMAC(k_enc, id || seq || 0) || HMAC(k_enc, id || seq || 1) || …)
 *        tag = HMAC(k_mac, id || seq || ct)[0..16]
 *      id = the characteristic's short ID (low byte of its UUID, e.g. 0x02),
 *      so a ciphertext can't be moved to another characteristic; seq must go
 *      up within a session, so it can't be replayed.
 *
 * This stops passive sniffing of the config connection.  It is unauthenticated
 * ECDH, so an attacker actively relaying the connection during the 60 s window
 * (while the owner holds the button) could still read it.
 *
 * Plain C, no Zephyr dependencies (the ECDH itself is PSA, in main.c), so it is
 * unit-tested on the host (tests/host/).
 */
#ifndef CFG_CRYPTO_H_
#define CFG_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>

#define CFG_PUB_LEN        65   /* 0x04 || X || Y */
#define CFG_SECRET_LEN     32   /* ECDH x-coordinate */
#define CFG_SUBKEY_LEN     32
#define CFG_SEAL_TAG_LEN   16
#define CFG_SEAL_OVERHEAD  (4 + CFG_SEAL_TAG_LEN)

void cfg_session_derive(const uint8_t z[CFG_SECRET_LEN],
			const uint8_t dongle_pub[CFG_PUB_LEN],
			const uint8_t phone_pub[CFG_PUB_LEN],
			uint8_t k_enc[CFG_SUBKEY_LEN],
			uint8_t k_mac[CFG_SUBKEY_LEN]);

/* Verify and decrypt one sealed write.  *last_seq is the highest seq accepted
 * this session (start at 0) and is updated on success.  Returns the plaintext
 * length, or -1 if the tag is wrong, seq isn't newer, or out is too small. */
int cfg_open(const uint8_t k_enc[CFG_SUBKEY_LEN], const uint8_t k_mac[CFG_SUBKEY_LEN],
	     uint8_t id, uint32_t *last_seq,
	     const uint8_t *in, size_t in_len, uint8_t *out, size_t out_max);

/* The phone's side (used by the host test to round-trip).  out must hold
 * pt_len + CFG_SEAL_OVERHEAD bytes; returns that length. */
size_t cfg_seal(const uint8_t k_enc[CFG_SUBKEY_LEN], const uint8_t k_mac[CFG_SUBKEY_LEN],
		uint8_t id, uint32_t seq,
		const uint8_t *pt, size_t pt_len, uint8_t *out);

#endif /* CFG_CRYPTO_H_ */
