/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Encrypted config session (fw 1.2.0+): the secrets the app writes (passwords,
 * username 2, the beacon key) travel encrypted, not in the clear.
 *
 * AUTHENTICATED since fw 1.4.0 (static-ephemeral ECDH, Noise "NK" style): the
 * dongle has a persistent P-256 IDENTITY key pair, and the session key is
 * derived from TWO shared secrets, so only the real dongle (holding the identity
 * private key) can derive it, and the app pins the identity public key.
 *
 *   1. Per connection, the phone reads the dongle's EPHEMERAL public key (fresh
 *      per window) and its IDENTITY public key (persistent), and writes its own
 *      fresh ephemeral key (all 65-byte uncompressed points).
 *        z1 = ECDH(ephemeral_dongle, ephemeral_phone)   (forward secrecy)
 *        z2 = ECDH(identity_dongle,  ephemeral_phone)    (authenticates the dongle)
 *   2. HKDF-SHA256 (RFC 5869), salt "BLEPW-SESSION-v2", IKM = z1 || z2,
 *      info = id_pub || dongle_pub || phone_pub, L = 64 → k_enc (32) || k_mac (32).
 *   3. Each secret write is  seq[4, LE] || ct || tag[16]  where
 *        ct  = pt XOR (HMAC(k_enc, id || seq || 0) || HMAC(k_enc, id || seq || 1) || …)
 *        tag = HMAC(k_mac, id || seq || ct)[0..16]
 *      id = the characteristic's short ID (low byte of its UUID, e.g. 0x02),
 *      so a ciphertext can't be moved to another characteristic; seq must go
 *      up within a session, so it can't be replayed.
 *
 * This stops passive sniffing AND (with the app pinning the identity key) an
 * active relay/fake dongle: a relay can't compute z2 (it lacks the identity
 * private key), so it can't derive the session key.
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

/* Derive the session keys from the ephemeral (z1) and identity (z2) ECDH shared
 * secrets.  id_pub is the dongle's persistent identity public key. */
void cfg_session_derive(const uint8_t z1[CFG_SECRET_LEN],
			const uint8_t z2[CFG_SECRET_LEN],
			const uint8_t id_pub[CFG_PUB_LEN],
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
