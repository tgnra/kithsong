#include "sync/noise.h"

#include <string.h>

#include <mbedtls/bignum.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

#include "util/platform.h"

/* --- Patterns --- */

/* Message tokens. A DH token "xy" combines the initiator's x key with the responder's y. */
enum { T_END, T_E, T_S, T_EE, T_ES, T_SE, T_SS, T_PSK };

#define MAX_MESSAGES 3
#define MAX_TOKENS 5

struct mc_noise_pattern_def {
  const char *name;
  /* Pre-messages: the initiator's (-> s) and the responder's (<- s) static keys, known to
   * the other side in advance. */
  int pre_initiator_s, pre_responder_s;
  /* Any psk token makes every "e" token also MixKey(e.public). */
  int psk;
  int messages; /* alternating, starting with the initiator */
  unsigned char tokens[MAX_MESSAGES][MAX_TOKENS];
};

static const struct mc_noise_pattern_def k_patterns[] = {
  [MC_NOISE_KK] = {"Noise_KK_25519_ChaChaPoly_SHA256", 1, 1, 0, 2,
                   {{T_E, T_ES, T_SS}, {T_E, T_EE, T_SE}}},
  [MC_NOISE_XXPSK3] = {"Noise_XXpsk3_25519_ChaChaPoly_SHA256", 0, 0, 1, 3,
                       {{T_E}, {T_E, T_EE, T_S, T_ES}, {T_S, T_SE, T_PSK}}},
};

/* --- 25519 --- */

static int os_random(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  return mc_random_bytes(buf, len) == MC_OK ? 0 : MBEDTLS_ERR_ECP_RANDOM_FAILED;
}

mc_status mc_noise_x25519(const unsigned char priv[MC_NOISE_KEY_LEN],
                          const unsigned char pub[MC_NOISE_KEY_LEN],
                          unsigned char out[MC_NOISE_KEY_LEN]) {
  /* Clamp as RFC 7748 decodeScalar25519 does; the point read masks u's top bit itself. */
  unsigned char k[MC_NOISE_KEY_LEN];
  memcpy(k, priv, sizeof k);
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;

  mbedtls_ecp_group grp;
  mbedtls_ecp_point q;
  mbedtls_mpi d, z;
  mbedtls_ecp_group_init(&grp);
  mbedtls_ecp_point_init(&q);
  mbedtls_mpi_init(&d);
  mbedtls_mpi_init(&z);
  int rc = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_CURVE25519);
  if (rc == 0) rc = mbedtls_mpi_read_binary_le(&d, k, sizeof k);
  if (rc == 0) rc = mbedtls_ecp_point_read_binary(&grp, &q, pub, MC_NOISE_KEY_LEN);
  /* Fails for a low-order point, whose result would be all zeros. The RNG only blinds. */
  if (rc == 0) rc = mbedtls_ecdh_compute_shared(&grp, &z, &q, &d, os_random, NULL);
  if (rc == 0) rc = mbedtls_mpi_write_binary_le(&z, out, MC_NOISE_KEY_LEN);
  mbedtls_mpi_free(&z);
  mbedtls_mpi_free(&d);
  mbedtls_ecp_point_free(&q);
  mbedtls_ecp_group_free(&grp);
  mbedtls_platform_zeroize(k, sizeof k);
  if (rc != 0) mbedtls_platform_zeroize(out, MC_NOISE_KEY_LEN);
  return rc == 0 ? MC_OK : MC_ERR_FORMAT;
}

mc_status mc_noise_public_key(const unsigned char priv[MC_NOISE_KEY_LEN],
                              unsigned char pub[MC_NOISE_KEY_LEN]) {
  static const unsigned char base[MC_NOISE_KEY_LEN] = {9};
  return mc_noise_x25519(priv, base, pub);
}

/* --- CipherState --- */

/* ChaChaPoly's nonce: 32 bits of zeros, then n as a little-endian 64-bit integer. */
static void nonce_bytes(uint64_t n, unsigned char out[12]) {
  memset(out, 0, 4);
  for (int i = 0; i < 8; i++) out[4 + i] = (unsigned char)(n >> (8 * i));
}

/* EncryptWithAd(). Without a key, the plaintext passes through (it may alias out). */
static mc_status cipher_encrypt(mc_noise_cipher *c, const unsigned char *ad, size_t ad_len,
                                const unsigned char *plain, size_t len, unsigned char *out) {
  if (!c->has_key) {
    if (len) memmove(out, plain, len);
    return MC_OK;
  }
  if (c->n == UINT64_MAX) return MC_ERR_INVALID_ARG; /* 2^64-1 is reserved */
  unsigned char nonce[12];
  nonce_bytes(c->n, nonce);
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, c->k);
  if (rc == 0) rc = mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce, ad, ad_len, plain, out,
                                                      out + len);
  mbedtls_chachapoly_free(&ctx);
  if (rc != 0) return MC_ERR_IO;
  c->n++;
  return MC_OK;
}

/* DecryptWithAd(); ct_len includes the tag when there is a key. */
static mc_status cipher_decrypt(mc_noise_cipher *c, const unsigned char *ad, size_t ad_len,
                                const unsigned char *ct, size_t ct_len, unsigned char *out) {
  if (!c->has_key) {
    if (ct_len) memmove(out, ct, ct_len);
    return MC_OK;
  }
  if (ct_len < MC_NOISE_TAG_LEN) return MC_ERR_FORMAT;
  if (c->n == UINT64_MAX) return MC_ERR_INVALID_ARG;
  size_t len = ct_len - MC_NOISE_TAG_LEN;
  unsigned char nonce[12];
  nonce_bytes(c->n, nonce);
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  int rc = mbedtls_chachapoly_setkey(&ctx, c->k);
  if (rc == 0) rc = mbedtls_chachapoly_auth_decrypt(&ctx, len, nonce, ad, ad_len, ct + len, ct,
                                                   out);
  mbedtls_chachapoly_free(&ctx);
  if (rc != 0) {
    if (len) mbedtls_platform_zeroize(out, len);
    return MC_ERR_FORMAT;
  }
  c->n++;
  return MC_OK;
}

static void cipher_set_key(mc_noise_cipher *c, const unsigned char k[MC_NOISE_KEY_LEN]) {
  memcpy(c->k, k, MC_NOISE_KEY_LEN);
  c->n = 0;
  c->has_key = 1;
}

mc_status mc_noise_encrypt(mc_noise_cipher *c, const unsigned char *ad, size_t ad_len,
                           const unsigned char *plain, size_t len, unsigned char *out) {
  if (!c || !c->has_key || (!plain && len) || !out || len > MC_NOISE_MAX_PLAINTEXT)
    return MC_ERR_INVALID_ARG;
  return cipher_encrypt(c, ad, ad_len, plain, len, out);
}

mc_status mc_noise_decrypt(mc_noise_cipher *c, const unsigned char *ad, size_t ad_len,
                           const unsigned char *ct, size_t ct_len, unsigned char *out) {
  if (!c || !c->has_key || !ct || !out || ct_len > MC_NOISE_MAX_MSG) return MC_ERR_INVALID_ARG;
  return cipher_decrypt(c, ad, ad_len, ct, ct_len, out);
}

void mc_noise_cipher_wipe(mc_noise_cipher *c) {
  if (c) mbedtls_platform_zeroize(c, sizeof *c);
}

/* --- SymmetricState --- */

static void sha256_two(const unsigned char *a, size_t a_len, const unsigned char *b, size_t b_len,
                       unsigned char out[MC_NOISE_HASH_LEN]) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, a, a_len);
  if (b_len) mbedtls_sha256_update(&ctx, b, b_len);
  mbedtls_sha256_finish(&ctx, out);
  mbedtls_sha256_free(&ctx);
}

static void mix_hash(mc_noise_handshake *hs, const unsigned char *data, size_t len) {
  sha256_two(hs->h, sizeof hs->h, data, len, hs->h);
}

/* Noise's HKDF(ck, ikm, n) is RFC 5869 HKDF with salt = ck and empty info, so mbedTLS's
 * does it: output i is bytes [32i, 32i + 32). */
static mc_status hkdf(const unsigned char ck[MC_NOISE_HASH_LEN], const unsigned char *ikm,
                      size_t ikm_len, unsigned char *out, int outputs) {
  static const unsigned char empty[1] = {0};
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md) return MC_ERR_IO;
  int rc = mbedtls_hkdf(md, ck, MC_NOISE_HASH_LEN, ikm_len ? ikm : empty, ikm_len, NULL, 0, out,
                        (size_t)outputs * MC_NOISE_HASH_LEN);
  return rc == 0 ? MC_OK : MC_ERR_IO;
}

static mc_status mix_key(mc_noise_handshake *hs, const unsigned char *ikm, size_t len) {
  unsigned char out[2 * MC_NOISE_HASH_LEN];
  mc_status st = hkdf(hs->ck, ikm, len, out, 2);
  if (st == MC_OK) {
    memcpy(hs->ck, out, MC_NOISE_HASH_LEN);
    cipher_set_key(&hs->cipher, out + MC_NOISE_HASH_LEN); /* HASHLEN = 32: no truncation */
  }
  mbedtls_platform_zeroize(out, sizeof out);
  return st;
}

static mc_status mix_key_and_hash(mc_noise_handshake *hs, const unsigned char *ikm, size_t len) {
  unsigned char out[3 * MC_NOISE_HASH_LEN];
  mc_status st = hkdf(hs->ck, ikm, len, out, 3);
  if (st == MC_OK) {
    memcpy(hs->ck, out, MC_NOISE_HASH_LEN);
    mix_hash(hs, out + MC_NOISE_HASH_LEN, MC_NOISE_HASH_LEN);
    cipher_set_key(&hs->cipher, out + 2 * MC_NOISE_HASH_LEN);
  }
  mbedtls_platform_zeroize(out, sizeof out);
  return st;
}

/* EncryptAndHash(): out gets len (+ tag, once there is a key) bytes. */
static mc_status encrypt_and_hash(mc_noise_handshake *hs, const unsigned char *plain, size_t len,
                                  unsigned char *out, size_t *out_len) {
  mc_status st = cipher_encrypt(&hs->cipher, hs->h, sizeof hs->h, plain, len, out);
  if (st != MC_OK) return st;
  *out_len = len + (hs->cipher.has_key ? MC_NOISE_TAG_LEN : 0);
  mix_hash(hs, out, *out_len);
  return MC_OK;
}

/* DecryptAndHash(). The new h is computed before decrypting, which needs the old one as
 * associated data, so ct may be overwritten in place. */
static mc_status decrypt_and_hash(mc_noise_handshake *hs, const unsigned char *ct, size_t ct_len,
                                  unsigned char *out) {
  unsigned char next_h[MC_NOISE_HASH_LEN];
  sha256_two(hs->h, sizeof hs->h, ct, ct_len, next_h);
  mc_status st = cipher_decrypt(&hs->cipher, hs->h, sizeof hs->h, ct, ct_len, out);
  if (st == MC_OK) memcpy(hs->h, next_h, sizeof next_h);
  return st;
}

/* --- HandshakeState --- */

mc_status mc_noise_handshake_init(mc_noise_handshake *hs, mc_noise_pattern pattern, int initiator,
                                  const unsigned char *prologue, size_t prologue_len,
                                  const unsigned char s_priv[MC_NOISE_KEY_LEN],
                                  const unsigned char s_pub[MC_NOISE_KEY_LEN],
                                  const unsigned char *rs, const unsigned char *psk) {
  if (!hs) return MC_ERR_INVALID_ARG;
  memset(hs, 0, sizeof *hs);
  if ((unsigned)pattern >= sizeof k_patterns / sizeof k_patterns[0] || !s_priv || !s_pub ||
      (!prologue && prologue_len))
    return MC_ERR_INVALID_ARG;
  const struct mc_noise_pattern_def *p = &k_patterns[pattern];
  int needs_rs = initiator ? p->pre_responder_s : p->pre_initiator_s;
  if ((needs_rs && !rs) || (p->psk && !psk)) return MC_ERR_INVALID_ARG;

  hs->pattern = p;
  hs->initiator = initiator ? 1 : 0;
  memcpy(hs->s_priv, s_priv, MC_NOISE_KEY_LEN);
  memcpy(hs->s_pub, s_pub, MC_NOISE_KEY_LEN);
  if (needs_rs) {
    memcpy(hs->rs, rs, MC_NOISE_KEY_LEN);
    hs->has_rs = 1;
  }
  if (p->psk) memcpy(hs->psk, psk, MC_NOISE_PSK_LEN);

  /* InitializeSymmetric(): a name of up to HASHLEN bytes is used as h directly, padded with
   * zeros; a longer one is hashed. */
  size_t name_len = strlen(p->name);
  if (name_len <= MC_NOISE_HASH_LEN)
    memcpy(hs->h, p->name, name_len);
  else
    sha256_two((const unsigned char *)p->name, name_len, NULL, 0, hs->h);
  memcpy(hs->ck, hs->h, sizeof hs->ck);
  mix_hash(hs, prologue, prologue_len);

  /* Pre-messages, the initiator's first. */
  if (p->pre_initiator_s) mix_hash(hs, initiator ? hs->s_pub : hs->rs, MC_NOISE_KEY_LEN);
  if (p->pre_responder_s) mix_hash(hs, initiator ? hs->rs : hs->s_pub, MC_NOISE_KEY_LEN);
  return MC_OK;
}

mc_status mc_noise_handshake_set_ephemeral(mc_noise_handshake *hs,
                                           const unsigned char e_priv[MC_NOISE_KEY_LEN]) {
  if (!hs || !hs->pattern || hs->message != 0 || !e_priv) return MC_ERR_INVALID_ARG;
  memcpy(hs->e_priv, e_priv, MC_NOISE_KEY_LEN);
  mc_status st = mc_noise_public_key(hs->e_priv, hs->e_pub);
  hs->has_e = st == MC_OK;
  return st;
}

int mc_noise_handshake_complete(const mc_noise_handshake *hs) {
  return hs->pattern && !hs->failed && hs->message >= hs->pattern->messages;
}

int mc_noise_handshake_my_turn(const mc_noise_handshake *hs) {
  return hs->pattern && !hs->failed && !mc_noise_handshake_complete(hs) &&
         (hs->message % 2 == 0) == (hs->initiator == 1);
}

/* A DH token: MixKey(DH(local key, remote key)). */
static mc_status mix_dh(mc_noise_handshake *hs, int token) {
  int initiator_s = token == T_SE || token == T_SS;
  int responder_s = token == T_ES || token == T_SS;
  int local_s = hs->initiator ? initiator_s : responder_s;
  int remote_s = hs->initiator ? responder_s : initiator_s;
  if (!local_s && !hs->has_e) return MC_ERR_INVALID_ARG;
  if (remote_s ? !hs->has_rs : !hs->has_re) return MC_ERR_INVALID_ARG;

  unsigned char shared[MC_NOISE_KEY_LEN];
  mc_status st = mc_noise_x25519(local_s ? hs->s_priv : hs->e_priv, remote_s ? hs->rs : hs->re,
                                 shared);
  if (st == MC_OK) st = mix_key(hs, shared, sizeof shared);
  mbedtls_platform_zeroize(shared, sizeof shared);
  return st;
}

static mc_status write_tokens(mc_noise_handshake *hs, const unsigned char *payload,
                              size_t payload_len, unsigned char *out, size_t cap, size_t *out_len) {
  const unsigned char *tokens = hs->pattern->tokens[hs->message];
  size_t pos = 0, n;
  mc_status st;
  for (int i = 0; i < MAX_TOKENS && tokens[i] != T_END; i++) {
    switch (tokens[i]) {
    case T_E:
      if (cap - pos < MC_NOISE_KEY_LEN) return MC_ERR_INVALID_ARG;
      if (!hs->has_e) {
        if ((st = mc_random_bytes(hs->e_priv, MC_NOISE_KEY_LEN)) != MC_OK ||
            (st = mc_noise_public_key(hs->e_priv, hs->e_pub)) != MC_OK)
          return st;
        hs->has_e = 1;
      }
      memcpy(out + pos, hs->e_pub, MC_NOISE_KEY_LEN);
      pos += MC_NOISE_KEY_LEN;
      mix_hash(hs, hs->e_pub, MC_NOISE_KEY_LEN);
      if (hs->pattern->psk && (st = mix_key(hs, hs->e_pub, MC_NOISE_KEY_LEN)) != MC_OK) return st;
      break;
    case T_S:
      n = MC_NOISE_KEY_LEN + (hs->cipher.has_key ? MC_NOISE_TAG_LEN : 0);
      if (cap - pos < n) return MC_ERR_INVALID_ARG;
      if ((st = encrypt_and_hash(hs, hs->s_pub, MC_NOISE_KEY_LEN, out + pos, &n)) != MC_OK)
        return st;
      pos += n;
      break;
    case T_PSK:
      if ((st = mix_key_and_hash(hs, hs->psk, MC_NOISE_PSK_LEN)) != MC_OK) return st;
      break;
    default:
      if ((st = mix_dh(hs, tokens[i])) != MC_OK) return st;
      break;
    }
  }
  n = payload_len + (hs->cipher.has_key ? MC_NOISE_TAG_LEN : 0);
  if (cap - pos < n || pos + n > MC_NOISE_MAX_MSG) return MC_ERR_INVALID_ARG;
  if ((st = encrypt_and_hash(hs, payload, payload_len, out + pos, &n)) != MC_OK) return st;
  *out_len = pos + n;
  return MC_OK;
}

static mc_status read_tokens(mc_noise_handshake *hs, const unsigned char *msg, size_t len,
                             unsigned char *payload, size_t cap, size_t *payload_len) {
  const unsigned char *tokens = hs->pattern->tokens[hs->message];
  size_t pos = 0, n;
  mc_status st;
  for (int i = 0; i < MAX_TOKENS && tokens[i] != T_END; i++) {
    switch (tokens[i]) {
    case T_E:
      if (len - pos < MC_NOISE_KEY_LEN) return MC_ERR_FORMAT;
      memcpy(hs->re, msg + pos, MC_NOISE_KEY_LEN);
      hs->has_re = 1;
      pos += MC_NOISE_KEY_LEN;
      mix_hash(hs, hs->re, MC_NOISE_KEY_LEN);
      if (hs->pattern->psk && (st = mix_key(hs, hs->re, MC_NOISE_KEY_LEN)) != MC_OK) return st;
      break;
    case T_S:
      n = MC_NOISE_KEY_LEN + (hs->cipher.has_key ? MC_NOISE_TAG_LEN : 0);
      if (len - pos < n) return MC_ERR_FORMAT;
      if ((st = decrypt_and_hash(hs, msg + pos, n, hs->rs)) != MC_OK) return st;
      hs->has_rs = 1;
      pos += n;
      break;
    case T_PSK:
      if ((st = mix_key_and_hash(hs, hs->psk, MC_NOISE_PSK_LEN)) != MC_OK) return st;
      break;
    default:
      if ((st = mix_dh(hs, tokens[i])) != MC_OK) return st;
      break;
    }
  }
  n = len - pos;
  size_t plain = n;
  if (hs->cipher.has_key) {
    if (n < MC_NOISE_TAG_LEN) return MC_ERR_FORMAT;
    plain = n - MC_NOISE_TAG_LEN;
  }
  if (plain > cap) return MC_ERR_INVALID_ARG;
  if ((st = decrypt_and_hash(hs, msg + pos, n, payload)) != MC_OK) return st;
  *payload_len = plain;
  return MC_OK;
}

mc_status mc_noise_write_message(mc_noise_handshake *hs, const unsigned char *payload,
                                 size_t payload_len, unsigned char *out, size_t cap,
                                 size_t *out_len) {
  if (!hs || !out || !out_len || (!payload && payload_len) || !mc_noise_handshake_my_turn(hs))
    return MC_ERR_INVALID_ARG;
  *out_len = 0;
  mc_status st = write_tokens(hs, payload, payload_len, out, cap, out_len);
  if (st != MC_OK) {
    hs->failed = 1;
    *out_len = 0;
    return st;
  }
  hs->message++;
  return MC_OK;
}

mc_status mc_noise_read_message(mc_noise_handshake *hs, const unsigned char *msg, size_t msg_len,
                                unsigned char *payload, size_t cap, size_t *payload_len) {
  if (!hs || !msg || !payload_len || (!payload && cap) || !hs->pattern || hs->failed ||
      mc_noise_handshake_complete(hs) || mc_noise_handshake_my_turn(hs))
    return MC_ERR_INVALID_ARG;
  *payload_len = 0;
  if (msg_len > MC_NOISE_MAX_MSG) {
    hs->failed = 1;
    return MC_ERR_FORMAT;
  }
  mc_status st = read_tokens(hs, msg, msg_len, payload, cap, payload_len);
  if (st != MC_OK) {
    hs->failed = 1;
    *payload_len = 0;
    return st;
  }
  hs->message++;
  return MC_OK;
}

const unsigned char *mc_noise_remote_static(const mc_noise_handshake *hs) {
  return hs && hs->has_rs ? hs->rs : NULL;
}

const unsigned char *mc_noise_handshake_hash(const mc_noise_handshake *hs) {
  return hs->h;
}

mc_status mc_noise_split(mc_noise_handshake *hs, mc_noise_cipher *send, mc_noise_cipher *recv) {
  if (!hs || !send || !recv || !mc_noise_handshake_complete(hs)) return MC_ERR_INVALID_ARG;
  unsigned char out[2 * MC_NOISE_HASH_LEN];
  mc_status st = hkdf(hs->ck, NULL, 0, out, 2);
  if (st == MC_OK) {
    /* The first key is for initiator -> responder. */
    memset(send, 0, sizeof *send);
    memset(recv, 0, sizeof *recv);
    cipher_set_key(hs->initiator ? send : recv, out);
    cipher_set_key(hs->initiator ? recv : send, out + MC_NOISE_HASH_LEN);
  }
  mbedtls_platform_zeroize(out, sizeof out);
  /* Done with everything secret; rs and h stay for the caller. */
  mbedtls_platform_zeroize(hs->ck, sizeof hs->ck);
  mc_noise_cipher_wipe(&hs->cipher);
  mbedtls_platform_zeroize(hs->s_priv, sizeof hs->s_priv);
  mbedtls_platform_zeroize(hs->e_priv, sizeof hs->e_priv);
  mbedtls_platform_zeroize(hs->psk, sizeof hs->psk);
  return st;
}

void mc_noise_handshake_wipe(mc_noise_handshake *hs) {
  if (hs) mbedtls_platform_zeroize(hs, sizeof *hs);
}
