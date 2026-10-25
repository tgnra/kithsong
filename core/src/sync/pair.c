#include "sync/pair.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/ecjpake.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#include "util/platform.h"

/* Room for one J-PAKE round: two (round 1) or one (round 2) points with their Schnorr
 * proofs on P-256, a few hundred bytes. */
#define JPAKE_BUF 1024

static int os_random(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  return mc_random_bytes(buf, len) == MC_OK ? 0 : -1;
}

static int valid_code(const char *code) {
  if (!code) return 0;
  for (int i = 0; i < MC_PAIR_CODE_DIGITS; i++)
    if (code[i] < '0' || code[i] > '9') return 0;
  return code[MC_PAIR_CODE_DIGITS] == '\0';
}

mc_status mc_pair_new_code(char code[MC_PAIR_CODE_DIGITS + 1]) {
  /* Rejection sampling: 2^32 isn't a multiple of 10^6, so draws from the incomplete top
   * block would favour low codes. */
  const uint32_t range = 1000000;
  const uint32_t limit = UINT32_MAX - UINT32_MAX % range; /* a multiple of range */
  uint32_t v;
  do {
    unsigned char b[4];
    mc_status st = mc_random_bytes(b, sizeof b);
    if (st != MC_OK) return st;
    v = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
  } while (v >= limit);
  snprintf(code, MC_PAIR_CODE_DIGITS + 1, "%06u", (unsigned)(v % range));
  return MC_OK;
}

/* Receives the clear-text message of the given type. */
static mc_status expect(mc_channel *ch, uint8_t want, const unsigned char **payload,
                        size_t *len) {
  uint8_t type;
  mc_status st = mc_channel_recv_clear(ch, &type, payload, len);
  if (st == MC_OK && type != want) st = MC_ERR_FORMAT;
  return st;
}

mc_status mc_pair_jpake(mc_channel *ch, const char *code, unsigned char psk[MC_NOISE_PSK_LEN],
                        int *code_used) {
  if (!ch || !valid_code(code) || !psk) return MC_ERR_INVALID_ARG;
  if (code_used) *code_used = 0;

  mbedtls_ecjpake_context ctx;
  mbedtls_ecjpake_init(&ctx);
  unsigned char buf[JPAKE_BUF], secret[32];
  const unsigned char *in;
  size_t n, in_len;
  mc_status st = MC_ERR_IO;
  mbedtls_ecjpake_role role =
    mc_channel_is_initiator(ch) ? MBEDTLS_ECJPAKE_CLIENT : MBEDTLS_ECJPAKE_SERVER;
  if (mbedtls_ecjpake_setup(&ctx, role, MBEDTLS_MD_SHA256, MBEDTLS_ECP_DP_SECP256R1,
                            (const unsigned char *)code, MC_PAIR_CODE_DIGITS) != 0)
    goto done;

  /* Both sides write each round before reading the peer's: the messages are small enough
   * for the socket buffers, so this can't deadlock, and it saves a round trip. */
  if (mbedtls_ecjpake_write_round_one(&ctx, buf, sizeof buf, &n, os_random, NULL) != 0) goto done;
  if ((st = mc_channel_send_clear(ch, MC_MSG_JPAKE_1, buf, n)) != MC_OK) goto done;
  if ((st = expect(ch, MC_MSG_JPAKE_1, &in, &in_len)) != MC_OK) goto done;
  st = MC_ERR_FORMAT;
  if (mbedtls_ecjpake_read_round_one(&ctx, in, in_len) != 0) goto done;

  st = MC_ERR_IO;
  if (mbedtls_ecjpake_write_round_two(&ctx, buf, sizeof buf, &n, os_random, NULL) != 0) goto done;
  if (code_used) *code_used = 1; /* round two depends on the code */
  if ((st = mc_channel_send_clear(ch, MC_MSG_JPAKE_2, buf, n)) != MC_OK) goto done;
  if ((st = expect(ch, MC_MSG_JPAKE_2, &in, &in_len)) != MC_OK) goto done;
  st = MC_ERR_FORMAT;
  if (mbedtls_ecjpake_read_round_two(&ctx, in, in_len) != 0) goto done;

  st = MC_ERR_IO;
  if (mbedtls_ecjpake_derive_secret(&ctx, secret, sizeof secret, &n, os_random, NULL) != 0 ||
      n != sizeof secret)
    goto done;
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (md && mbedtls_hkdf(md, NULL, 0, secret, sizeof secret,
                         (const unsigned char *)MC_PAIR_PSK_INFO, strlen(MC_PAIR_PSK_INFO), psk,
                         MC_NOISE_PSK_LEN) == 0)
    st = MC_OK;

done:
  mbedtls_ecjpake_free(&ctx);
  mbedtls_platform_zeroize(secret, sizeof secret);
  if (st != MC_OK) mbedtls_platform_zeroize(psk, MC_NOISE_PSK_LEN);
  return st;
}

/* J-PAKE, then XXpsk3 in this channel's role. */
static mc_status pair(mc_channel *ch, const char *code, const unsigned char *s_priv,
                      const unsigned char *s_pub, unsigned char *peer_key, int *code_used) {
  unsigned char psk[MC_NOISE_PSK_LEN];
  mc_status st = mc_pair_jpake(ch, code, psk, code_used);
  if (st == MC_OK) st = mc_channel_handshake_xxpsk3(ch, s_priv, s_pub, psk);
  if (st == MC_OK) memcpy(peer_key, mc_channel_peer_key(ch), MC_NOISE_KEY_LEN);
  mbedtls_platform_zeroize(psk, sizeof psk);
  return st;
}

mc_status mc_pair_initiate(mc_channel *ch, const char *code,
                           const unsigned char s_priv[MC_NOISE_KEY_LEN],
                           const unsigned char s_pub[MC_NOISE_KEY_LEN],
                           unsigned char peer_key[MC_NOISE_KEY_LEN]) {
  if (!ch || !mc_channel_is_initiator(ch) || mc_channel_mode(ch) != MC_MODE_PAIRING ||
      !valid_code(code) || !s_priv || !s_pub || !peer_key)
    return MC_ERR_INVALID_ARG;
  return pair(ch, code, s_priv, s_pub, peer_key, NULL);
}

mc_status mc_pair_responder_init(mc_pair_responder *r) {
  if (!r) return MC_ERR_INVALID_ARG;
  r->failures = 0;
  return mc_pair_new_code(r->code);
}

mc_status mc_pair_respond(mc_pair_responder *r, mc_channel *ch,
                          const unsigned char s_priv[MC_NOISE_KEY_LEN],
                          const unsigned char s_pub[MC_NOISE_KEY_LEN],
                          unsigned char peer_key[MC_NOISE_KEY_LEN], int *new_code) {
  if (new_code) *new_code = 0;
  if (!r || !ch || mc_channel_is_initiator(ch) || mc_channel_mode(ch) != MC_MODE_PAIRING ||
      !valid_code(r->code) || !s_priv || !s_pub || !peer_key)
    return MC_ERR_INVALID_ARG;

  int used = 0;
  mc_status st = pair(ch, r->code, s_priv, s_pub, peer_key, &used);
  if (st == MC_OK) {
    r->failures = 0;
  } else if (used && ++r->failures >= MC_PAIR_MAX_FAILURES) {
    /* Whatever went wrong, the peer may have learnt one guess's worth. */
    mc_status gen = mc_pair_new_code(r->code);
    if (gen != MC_OK) return gen;
    r->failures = 0;
    if (new_code) *new_code = 1;
  }
  return st;
}

void mc_pair_responder_wipe(mc_pair_responder *r) {
  if (r) mbedtls_platform_zeroize(r, sizeof *r);
}
