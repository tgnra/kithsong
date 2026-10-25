#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/chachapoly.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

#include "musiccore/musiccore.h"
#include "sync/noise.h"
#include "test.h"
#include "util/platform.h"

/* Decodes hex into out (cap bytes); returns the length, or -1. */
static int unhex(const char *hex, unsigned char *out, size_t cap) {
  size_t n = strlen(hex);
  if (n % 2 || n / 2 > cap) return -1;
  for (size_t i = 0; i < n / 2; i++) {
    unsigned v;
    if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
    out[i] = (unsigned char)v;
  }
  return (int)(n / 2);
}

static int equals_hex(const unsigned char *data, size_t len, const char *hex) {
  unsigned char want[1024];
  int n = unhex(hex, want, sizeof want);
  return n >= 0 && (size_t)n == len && memcmp(data, want, len) == 0;
}

/* --- Primitives: RFC 7748, RFC 8439, RFC 5869 --- */

static void test_x25519(void) {
  unsigned char k[32], u[32], out[32];
  /* RFC 7748 5.2, first vector. */
  unhex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, sizeof k);
  unhex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, sizeof u);
  CHECK(mc_noise_x25519(k, u, out) == MC_OK);
  CHECK(equals_hex(out, 32, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"));

  /* RFC 7748 6.1: both public keys, and the shared secret from either side. */
  unsigned char a[32], b[32], a_pub[32], b_pub[32], s1[32], s2[32];
  unhex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, sizeof a);
  unhex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, sizeof b);
  CHECK(mc_noise_public_key(a, a_pub) == MC_OK);
  CHECK(mc_noise_public_key(b, b_pub) == MC_OK);
  CHECK(equals_hex(a_pub, 32, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
  CHECK(equals_hex(b_pub, 32, "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
  CHECK(mc_noise_x25519(a, b_pub, s1) == MC_OK);
  CHECK(mc_noise_x25519(b, a_pub, s2) == MC_OK);
  CHECK(equals_hex(s1, 32, "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
  CHECK(memcmp(s1, s2, 32) == 0);

  /* A low-order point (u = 0) gives an all-zero secret, which is refused. */
  memset(u, 0, sizeof u);
  CHECK(mc_noise_x25519(a, u, out) != MC_OK);
}

static void test_chachapoly(void) {
  /* RFC 8439 2.8.2. */
  static const char plain[] = "Ladies and Gentlemen of the class of '99: If I could offer you only "
                              "one tip for the future, sunscreen would be it.";
  unsigned char key[32], nonce[12], aad[12], ct[sizeof plain], tag[16], back[sizeof plain];
  size_t len = sizeof plain - 1;
  unhex("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, sizeof key);
  unhex("070000004041424344454647", nonce, sizeof nonce);
  unhex("50515253c0c1c2c3c4c5c6c7", aad, sizeof aad);
  mbedtls_chachapoly_context ctx;
  mbedtls_chachapoly_init(&ctx);
  CHECK(mbedtls_chachapoly_setkey(&ctx, key) == 0);
  CHECK(mbedtls_chachapoly_encrypt_and_tag(&ctx, len, nonce, aad, sizeof aad,
                                           (const unsigned char *)plain, ct, tag) == 0);
  CHECK(equals_hex(ct, len,
                   "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca967"
                   "1282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee32809"
                   "1b58fab324e4fad675945585808b4831d7bc3ff4def08e4b7a9de576d26586cec64b6116"));
  CHECK(equals_hex(tag, 16, "1ae10b594f09e26a7e902ecbd0600691"));
  CHECK(mbedtls_chachapoly_auth_decrypt(&ctx, len, nonce, aad, sizeof aad, tag, ct, back) == 0);
  CHECK(memcmp(back, plain, len) == 0);
  mbedtls_chachapoly_free(&ctx);
}

static void test_hkdf(void) {
  const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  unsigned char ikm[22], salt[13], info[10], okm[42];
  memset(ikm, 0x0b, sizeof ikm);
  /* RFC 5869 A.1. */
  unhex("000102030405060708090a0b0c", salt, sizeof salt);
  unhex("f0f1f2f3f4f5f6f7f8f9", info, sizeof info);
  CHECK(mbedtls_hkdf(md, salt, sizeof salt, ikm, sizeof ikm, info, sizeof info, okm, 42) == 0);
  CHECK(equals_hex(okm, 42,
                   "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887"
                   "185865"));
  /* A.3: empty salt and info, the shape of Noise's HKDF (which always has a salt). */
  CHECK(mbedtls_hkdf(md, NULL, 0, ikm, sizeof ikm, NULL, 0, okm, 42) == 0);
  CHECK(equals_hex(okm, 42,
                   "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b6"
                   "1a96c8"));
}

/* --- Noise test vectors --- */

/* From cacophony's vectors/cacophony.txt (github.com/haskell-cryptography/cacophony),
 * the Noise test-vector format: messages alternate initiator and responder, through the
 * handshake and on into transport messages. */
typedef struct vector {
  const char *name;
  mc_noise_pattern pattern;
  const char *prologue, *psk;
  const char *init_static, *init_ephemeral, *resp_static, *resp_ephemeral;
  const char *init_remote_static; /* KK: the responder's public key, known in advance */
  const char *handshake_hash;
  const char *payload[6], *ciphertext[6];
} vector;

#define PAYLOADS                                                                         \
  {"4c756477696720766f6e204d69736573", "4d757272617920526f746862617264",                 \
   "462e20412e20486179656b", "4361726c204d656e676572", "4a65616e2d426170746973746520536179", \
   "457567656e2042f6686d20766f6e2042617765726b"}

static const vector k_vectors[] = {
  {"Noise_KK_25519_ChaChaPoly_SHA256", MC_NOISE_KK, "4a6f686e2047616c74", NULL,
   "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1",
   "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
   "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893",
   "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b",
   "31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62",
   "24c6b51ecb76277140ca018b5985bc9f03de321dae2d34dcae433dafef0131d9",
   PAYLOADS,
   {"ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79440177015efc1fe7a37c629af712"
    "0a96274e6ab7afcc9261901d0e09ae32a5bb96",
    "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843b274d3429adc47ca093ba63ef9"
    "0f8da89fda108db471dccfa4894aa7b00003",
    "966b05bc69ec01b8454d3160a214e6f24a3d884eb31ec2408af63f",
    "0ad887fba4f611bbb4afe44ba3556b8164332ca7d5934634d63d80",
    "012b28ae646ae7830e2c5472cb023eab071c1db3d8413ec69b513b83832f974c2d",
    "bb3e6a48160d9c5971d37f975727294e0d868342db31832e54d07191ab0ca3c3703b5ed3d9"}},
  {"Noise_XXpsk3_25519_ChaChaPoly_SHA256", MC_NOISE_XXPSK3, "4a6f686e2047616c74",
   "54686973206973206d7920417573747269616e20706572737065637469766521",
   "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1",
   "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a",
   "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893",
   "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b", NULL,
   "a477edf6a131bbdb54707f6ea30eab6cd935d9b560f0e5fd1f053a95a99669fb",
   PAYLOADS,
   {"ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c7944c9f5ff0e8079630cb7e270c20b"
    "bf480821b77a384a645c71a2fd9b3db1c16a5f",
    "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843b123def17f71e6ae8e57e0e1de"
    "c5949c5f7415c6f33517398747d821a06dc23ad430aa1fd7381d46195c378a819fd574425462cbb2d4ca339e73"
    "8a0b7001dc91423fbf55a99af0c6f1df21012ceb2f",
    "52187316111b118d4c060364f7b975dc0809b2590779aff2d63113c564f11744493384db7bf32d5ae6686df6ab"
    "06d508d2e07caaf1d6afc010b978735fc78900e71ae1d314130d042e729a",
    "eaedc672d4c21e0e2955758756fb98f194c4e90d5deb5b6cf30b27",
    "522d543c5fe799d09a3d9da7ff54d0dc03c8af1dc7751d2ff708339d2290943e98",
    "7d4e2c3873eef6a213b04e72f9df60a91666072d3544c5d96c34a09e2329b5030bee796741"}},
};

static void run_vector(const vector *v) {
  unsigned char prologue[64], psk[32], is[32], is_pub[32], ie[32], rs[32], rs_pub[32], re[32];
  int prologue_len = unhex(v->prologue, prologue, sizeof prologue);
  unhex(v->init_static, is, sizeof is);
  unhex(v->init_ephemeral, ie, sizeof ie);
  unhex(v->resp_static, rs, sizeof rs);
  unhex(v->resp_ephemeral, re, sizeof re);
  if (v->psk) unhex(v->psk, psk, sizeof psk);
  CHECK(mc_noise_public_key(is, is_pub) == MC_OK);
  CHECK(mc_noise_public_key(rs, rs_pub) == MC_OK);
  if (v->init_remote_static) CHECK(equals_hex(rs_pub, 32, v->init_remote_static));

  int kk = v->pattern == MC_NOISE_KK;
  mc_noise_handshake init, resp;
  CHECK(mc_noise_handshake_init(&init, v->pattern, 1, prologue, (size_t)prologue_len, is, is_pub,
                                kk ? rs_pub : NULL, v->psk ? psk : NULL) == MC_OK);
  CHECK(mc_noise_handshake_init(&resp, v->pattern, 0, prologue, (size_t)prologue_len, rs, rs_pub,
                                kk ? is_pub : NULL, v->psk ? psk : NULL) == MC_OK);
  CHECK(mc_noise_handshake_set_ephemeral(&init, ie) == MC_OK);
  CHECK(mc_noise_handshake_set_ephemeral(&resp, re) == MC_OK);

  mc_noise_cipher init_send, init_recv, resp_send, resp_recv;
  int split = 0;
  for (int i = 0; i < 6; i++) {
    unsigned char payload[256], msg[512], back[512];
    int payload_len = unhex(v->payload[i], payload, sizeof payload);
    int from_init = i % 2 == 0;
    size_t n = 0, m = 0;
    if (!split) {
      mc_noise_handshake *w = from_init ? &init : &resp, *r = from_init ? &resp : &init;
      CHECK(mc_noise_handshake_my_turn(w) && !mc_noise_handshake_my_turn(r));
      CHECK(mc_noise_write_message(w, payload, (size_t)payload_len, msg, sizeof msg, &n) == MC_OK);
      CHECK(mc_noise_read_message(r, msg, n, back, sizeof back, &m) == MC_OK);
      if (mc_noise_handshake_complete(&init) && mc_noise_handshake_complete(&resp)) {
        CHECK(equals_hex(mc_noise_handshake_hash(&init), 32, v->handshake_hash));
        CHECK(memcmp(mc_noise_handshake_hash(&resp), mc_noise_handshake_hash(&init), 32) == 0);
        CHECK(memcmp(mc_noise_remote_static(&init), rs_pub, 32) == 0);
        CHECK(memcmp(mc_noise_remote_static(&resp), is_pub, 32) == 0);
        CHECK(mc_noise_split(&init, &init_send, &init_recv) == MC_OK);
        CHECK(mc_noise_split(&resp, &resp_send, &resp_recv) == MC_OK);
        split = 1;
      }
    } else {
      mc_noise_cipher *enc = from_init ? &init_send : &resp_send;
      mc_noise_cipher *dec = from_init ? &resp_recv : &init_recv;
      CHECK(mc_noise_encrypt(enc, NULL, 0, payload, (size_t)payload_len, msg) == MC_OK);
      n = (size_t)payload_len + MC_NOISE_TAG_LEN;
      CHECK(mc_noise_decrypt(dec, NULL, 0, msg, n, back) == MC_OK);
      m = n - MC_NOISE_TAG_LEN;
    }
    if (!equals_hex(msg, n, v->ciphertext[i])) {
      fprintf(stderr, "%s: message %d differs\n", v->name, i);
      test_failures++;
    }
    CHECK(m == (size_t)payload_len && memcmp(back, payload, m) == 0);
  }
  CHECK(split);
  mc_noise_handshake_wipe(&init);
  mc_noise_handshake_wipe(&resp);
}

/* --- Round trips and tampering, with fresh keys --- */

typedef struct party {
  unsigned char priv[32], pub[32];
} party;

static void new_party(party *p) {
  mc_random_bytes(p->priv, sizeof p->priv);
  mc_noise_public_key(p->priv, p->pub);
}

/* Runs a handshake between i and r; flip >= 0 flips a byte of message flip in transit.
 * Returns the status of the first failing step, MC_OK if it completed. */
static mc_status handshake(mc_noise_handshake *i, mc_noise_handshake *r, int flip) {
  for (int k = 0; !mc_noise_handshake_complete(i) || !mc_noise_handshake_complete(r); k++) {
    mc_noise_handshake *w = mc_noise_handshake_my_turn(i) ? i : r;
    mc_noise_handshake *rd = w == i ? r : i;
    unsigned char msg[256], back[256];
    size_t n, m;
    mc_status st = mc_noise_write_message(w, (const unsigned char *)"hi", 2, msg, sizeof msg, &n);
    if (st != MC_OK) return st;
    if (k == flip) msg[n / 2] ^= 0x01;
    if ((st = mc_noise_read_message(rd, msg, n, back, sizeof back, &m)) != MC_OK) return st;
    if (m != 2 || memcmp(back, "hi", 2) != 0) return MC_ERR_FORMAT;
  }
  return MC_OK;
}

static void test_round_trips(void) {
  party a, b, c;
  new_party(&a);
  new_party(&b);
  new_party(&c);
  static const unsigned char pro[] = {'M', 'A', 1, 2};
  unsigned char psk[32], other_psk[32];
  mc_random_bytes(psk, sizeof psk);
  memcpy(other_psk, psk, sizeof psk);
  other_psk[31] ^= 0x80;
  mc_noise_handshake i, r;

  /* KK both ways, then transport. */
  CHECK(mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pro, 4, a.priv, a.pub, b.pub, NULL) == MC_OK);
  CHECK(mc_noise_handshake_init(&r, MC_NOISE_KK, 0, pro, 4, b.priv, b.pub, a.pub, NULL) == MC_OK);
  CHECK(handshake(&i, &r, -1) == MC_OK);
  mc_noise_cipher is, ir, rs, rr;
  CHECK(mc_noise_split(&i, &is, &ir) == MC_OK && mc_noise_split(&r, &rs, &rr) == MC_OK);
  unsigned char msg[64], back[64];
  CHECK(mc_noise_encrypt(&is, NULL, 0, (const unsigned char *)"ping", 4, msg) == MC_OK);
  CHECK(mc_noise_decrypt(&rr, NULL, 0, msg, 4 + MC_NOISE_TAG_LEN, back) == MC_OK);
  CHECK(memcmp(back, "ping", 4) == 0);
  /* A replayed message fails: the receiver's nonce has moved on. */
  CHECK(mc_noise_decrypt(&rr, NULL, 0, msg, 4 + MC_NOISE_TAG_LEN, back) == MC_ERR_FORMAT);
  /* A flipped byte fails, ciphertext or tag. */
  CHECK(mc_noise_encrypt(&rs, NULL, 0, (const unsigned char *)"pong", 4, msg) == MC_OK);
  msg[1] ^= 0x01;
  CHECK(mc_noise_decrypt(&ir, NULL, 0, msg, 4 + MC_NOISE_TAG_LEN, back) == MC_ERR_FORMAT);
  msg[1] ^= 0x01;
  msg[4 + MC_NOISE_TAG_LEN - 1] ^= 0x01;
  CHECK(mc_noise_decrypt(&ir, NULL, 0, msg, 4 + MC_NOISE_TAG_LEN, back) == MC_ERR_FORMAT);
  msg[4 + MC_NOISE_TAG_LEN - 1] ^= 0x01;
  CHECK(mc_noise_decrypt(&ir, NULL, 0, msg, 4 + MC_NOISE_TAG_LEN, back) == MC_OK); /* intact */
  /* In-place encryption and decryption. */
  memcpy(msg, "in place", 8);
  CHECK(mc_noise_encrypt(&is, NULL, 0, msg, 8, msg) == MC_OK);
  CHECK(mc_noise_decrypt(&rr, NULL, 0, msg, 8 + MC_NOISE_TAG_LEN, msg) == MC_OK);
  CHECK(memcmp(msg, "in place", 8) == 0);
  mc_noise_cipher_wipe(&is);
  mc_noise_cipher_wipe(&ir);
  mc_noise_cipher_wipe(&rs);
  mc_noise_cipher_wipe(&rr);

  /* Each handshake message of KK and XXpsk3 fails when tampered with. */
  for (int flip = 0; flip < 3; flip++) {
    if (flip < 2) {
      mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pro, 4, a.priv, a.pub, b.pub, NULL);
      mc_noise_handshake_init(&r, MC_NOISE_KK, 0, pro, 4, b.priv, b.pub, a.pub, NULL);
      CHECK(handshake(&i, &r, flip) == MC_ERR_FORMAT);
    }
    mc_noise_handshake_init(&i, MC_NOISE_XXPSK3, 1, pro, 4, a.priv, a.pub, NULL, psk);
    mc_noise_handshake_init(&r, MC_NOISE_XXPSK3, 0, pro, 4, b.priv, b.pub, NULL, psk);
    CHECK(handshake(&i, &r, flip) == MC_ERR_FORMAT);
  }

  /* KK: the responder expects a different initiator. */
  mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pro, 4, c.priv, c.pub, b.pub, NULL);
  mc_noise_handshake_init(&r, MC_NOISE_KK, 0, pro, 4, b.priv, b.pub, a.pub, NULL);
  CHECK(handshake(&i, &r, -1) == MC_ERR_FORMAT);
  CHECK(mc_noise_handshake_my_turn(&r) == 0 && mc_noise_handshake_complete(&r) == 0);

  /* KK: a different prologue (here, the mode byte) fails. */
  static const unsigned char pairing[] = {'M', 'A', 1, 1};
  mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pairing, 4, a.priv, a.pub, b.pub, NULL);
  mc_noise_handshake_init(&r, MC_NOISE_KK, 0, pro, 4, b.priv, b.pub, a.pub, NULL);
  CHECK(handshake(&i, &r, -1) == MC_ERR_FORMAT);

  /* XXpsk3: both learn each other's key with the same psk... */
  mc_noise_handshake_init(&i, MC_NOISE_XXPSK3, 1, pro, 4, a.priv, a.pub, NULL, psk);
  mc_noise_handshake_init(&r, MC_NOISE_XXPSK3, 0, pro, 4, b.priv, b.pub, NULL, psk);
  CHECK(mc_noise_remote_static(&i) == NULL);
  CHECK(handshake(&i, &r, -1) == MC_OK);
  CHECK(memcmp(mc_noise_remote_static(&i), b.pub, 32) == 0);
  CHECK(memcmp(mc_noise_remote_static(&r), a.pub, 32) == 0);

  /* ...and a wrong psk fails at the responder, on the third message. */
  mc_noise_handshake_init(&i, MC_NOISE_XXPSK3, 1, pro, 4, a.priv, a.pub, NULL, psk);
  mc_noise_handshake_init(&r, MC_NOISE_XXPSK3, 0, pro, 4, b.priv, b.pub, NULL, other_psk);
  CHECK(handshake(&i, &r, -1) == MC_ERR_FORMAT);
  CHECK(mc_noise_handshake_complete(&i) && !mc_noise_handshake_complete(&r));
  mc_noise_cipher dummy1, dummy2;
  CHECK(mc_noise_split(&r, &dummy1, &dummy2) == MC_ERR_INVALID_ARG);

  /* Misuse: writing out of turn, missing keys. */
  mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pro, 4, a.priv, a.pub, b.pub, NULL);
  size_t n;
  CHECK(mc_noise_read_message(&i, msg, 48, back, sizeof back, &n) == MC_ERR_INVALID_ARG);
  CHECK(mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pro, 4, a.priv, a.pub, NULL, NULL) ==
        MC_ERR_INVALID_ARG);
  CHECK(mc_noise_handshake_init(&i, MC_NOISE_XXPSK3, 1, pro, 4, a.priv, a.pub, NULL, NULL) ==
        MC_ERR_INVALID_ARG);

  /* The largest transport message fits; one byte more doesn't. */
  CHECK(mc_noise_handshake_init(&i, MC_NOISE_KK, 1, pro, 4, a.priv, a.pub, b.pub, NULL) == MC_OK);
  CHECK(mc_noise_handshake_init(&r, MC_NOISE_KK, 0, pro, 4, b.priv, b.pub, a.pub, NULL) == MC_OK);
  CHECK(handshake(&i, &r, -1) == MC_OK);
  CHECK(mc_noise_split(&i, &is, &ir) == MC_OK && mc_noise_split(&r, &rs, &rr) == MC_OK);
  unsigned char *big = malloc(MC_NOISE_MAX_MSG + 1);
  if (big) {
    for (size_t k = 0; k < MC_NOISE_MAX_PLAINTEXT; k++) big[k] = (unsigned char)k;
    CHECK(mc_noise_encrypt(&is, NULL, 0, big, MC_NOISE_MAX_PLAINTEXT + 1, big) ==
          MC_ERR_INVALID_ARG);
    CHECK(mc_noise_encrypt(&is, NULL, 0, big, MC_NOISE_MAX_PLAINTEXT, big) == MC_OK);
    CHECK(mc_noise_decrypt(&rr, NULL, 0, big, MC_NOISE_MAX_MSG, big) == MC_OK);
    int ok = 1;
    for (size_t k = 0; k < MC_NOISE_MAX_PLAINTEXT; k++) ok &= big[k] == (unsigned char)k;
    CHECK(ok);
    free(big);
  }
  mc_noise_handshake_wipe(&i);
  mc_noise_handshake_wipe(&r);
}

int main(void) {
  test_x25519();
  test_chachapoly();
  test_hkdf();
  for (size_t k = 0; k < sizeof k_vectors / sizeof k_vectors[0]; k++) run_vector(&k_vectors[k]);
  test_round_trips();
  return TEST_RESULT();
}
