/* The Noise Protocol Framework (revision 34) with 25519, ChaChaPoly and SHA256, on mbedTLS:
 * CipherState, SymmetricState and HandshakeState, for the two patterns sync uses.
 *  - KK: sessions between group members, who already know each other's static key.
 *  - XXpsk3: pairing, where the psk comes from the J-PAKE exchange (see pair.h).
 * Messages are driven by a small token table rather than per-pattern code. Internal to
 * core; see "Noise" in docs/sync-protocol.md.
 *
 * Authentication failures (a bad tag, a wrong psk, the wrong static key) return
 * MC_ERR_FORMAT. A handshake that has failed refuses every further call. */
#ifndef MC_SYNC_NOISE_H
#define MC_SYNC_NOISE_H

#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

#define MC_NOISE_KEY_LEN 32  /* DHLEN, and the cipher key length */
#define MC_NOISE_HASH_LEN 32 /* HASHLEN */
#define MC_NOISE_TAG_LEN 16
#define MC_NOISE_PSK_LEN 32
/* Every Noise message, handshake or transport, is at most this long. */
#define MC_NOISE_MAX_MSG 65535
#define MC_NOISE_MAX_PLAINTEXT (MC_NOISE_MAX_MSG - MC_NOISE_TAG_LEN)

typedef enum mc_noise_pattern {
  MC_NOISE_KK,     /* Noise_KK_25519_ChaChaPoly_SHA256 */
  MC_NOISE_XXPSK3, /* Noise_XXpsk3_25519_ChaChaPoly_SHA256 */
} mc_noise_pattern;

/* A key plus a 64-bit nonce. Rekey isn't used. */
typedef struct mc_noise_cipher {
  unsigned char k[MC_NOISE_KEY_LEN];
  uint64_t n;
  int has_key;
} mc_noise_cipher;

/* One handshake. Plain data, so it can live on the stack; wipe it when done. */
typedef struct mc_noise_handshake {
  const struct mc_noise_pattern_def *pattern;
  int initiator;
  int message; /* index of the next handshake message */
  int failed;
  /* SymmetricState */
  unsigned char ck[MC_NOISE_HASH_LEN];
  unsigned char h[MC_NOISE_HASH_LEN];
  mc_noise_cipher cipher;
  /* Keys: s, e, rs, re, and the psk. */
  unsigned char s_priv[MC_NOISE_KEY_LEN], s_pub[MC_NOISE_KEY_LEN];
  unsigned char e_priv[MC_NOISE_KEY_LEN], e_pub[MC_NOISE_KEY_LEN];
  unsigned char rs[MC_NOISE_KEY_LEN], re[MC_NOISE_KEY_LEN];
  unsigned char psk[MC_NOISE_PSK_LEN];
  int has_e, has_rs, has_re;
} mc_noise_handshake;

/* Initialize(). Both patterns need the local static key pair. KK needs rs, the peer's
 * static public key, before the first message; XXpsk3 learns it during the handshake and
 * needs the 32-byte psk. Pass NULL for whichever the pattern doesn't use. */
mc_status mc_noise_handshake_init(mc_noise_handshake *hs, mc_noise_pattern pattern, int initiator,
                                  const unsigned char *prologue, size_t prologue_len,
                                  const unsigned char s_priv[MC_NOISE_KEY_LEN],
                                  const unsigned char s_pub[MC_NOISE_KEY_LEN],
                                  const unsigned char *rs, const unsigned char *psk);
/* Fixes the ephemeral private key instead of drawing a random one. For test vectors only;
 * call before the first message. */
mc_status mc_noise_handshake_set_ephemeral(mc_noise_handshake *hs,
                                           const unsigned char e_priv[MC_NOISE_KEY_LEN]);
/* Whether the next handshake message is ours to write (else ours to read). */
int mc_noise_handshake_my_turn(const mc_noise_handshake *hs);
int mc_noise_handshake_complete(const mc_noise_handshake *hs);

/* WriteMessage(): the next handshake message, carrying payload, into out (cap bytes).
 * MC_ERR_INVALID_ARG if it isn't our turn or the message would be too long. */
mc_status mc_noise_write_message(mc_noise_handshake *hs, const unsigned char *payload,
                                 size_t payload_len, unsigned char *out, size_t cap,
                                 size_t *out_len);
/* ReadMessage(): processes the peer's next handshake message, writing its payload into
 * payload (cap bytes; msg_len always suffices). payload may not overlap msg. */
mc_status mc_noise_read_message(mc_noise_handshake *hs, const unsigned char *msg, size_t msg_len,
                                unsigned char *payload, size_t cap, size_t *payload_len);

/* The peer's static public key, or NULL while it isn't known. */
const unsigned char *mc_noise_remote_static(const mc_noise_handshake *hs);
/* h, which binds everything the handshake has seen. Final once the handshake is complete. */
const unsigned char *mc_noise_handshake_hash(const mc_noise_handshake *hs);

/* Split(), for a complete handshake: the CipherStates for sending and receiving, already
 * oriented for this side. Wipes the handshake's secrets; the remote static key and the
 * handshake hash stay readable. */
mc_status mc_noise_split(mc_noise_handshake *hs, mc_noise_cipher *send, mc_noise_cipher *recv);
void mc_noise_handshake_wipe(mc_noise_handshake *hs);

/* Transport messages. encrypt writes len + MC_NOISE_TAG_LEN bytes; decrypt reads
 * ct_len >= MC_NOISE_TAG_LEN bytes and writes ct_len - MC_NOISE_TAG_LEN. in and out may be
 * the same buffer. The nonce advances on success only. */
mc_status mc_noise_encrypt(mc_noise_cipher *c, const unsigned char *ad, size_t ad_len,
                           const unsigned char *plain, size_t len, unsigned char *out);
mc_status mc_noise_decrypt(mc_noise_cipher *c, const unsigned char *ad, size_t ad_len,
                           const unsigned char *ct, size_t ct_len, unsigned char *out);
void mc_noise_cipher_wipe(mc_noise_cipher *c);

/* The 25519 primitives, exposed for key handling and known-answer tests. */
/* X25519(priv, pub) per RFC 7748. MC_ERR_FORMAT for a low-order pub (all-zero result). */
mc_status mc_noise_x25519(const unsigned char priv[MC_NOISE_KEY_LEN],
                          const unsigned char pub[MC_NOISE_KEY_LEN],
                          unsigned char out[MC_NOISE_KEY_LEN]);
/* X25519(priv, 9): the public key of priv. */
mc_status mc_noise_public_key(const unsigned char priv[MC_NOISE_KEY_LEN],
                              unsigned char pub[MC_NOISE_KEY_LEN]);

#endif
