/* The build's Last.fm API key and secret, decoded from lastfm_key.h when a request needs
 * them. They're stored obscured only so they don't sit in the binary as plain strings; that
 * isn't real protection (anyone with the binary can decode them as this does), and Last.fm
 * expects as much of desktop apps. Callers decode into their own buffer just before use and
 * wipe it straight after (mc_lastfm_wipe()). */
#include "scrobble/lastfm.h"

#include <mbedtls/platform_util.h>

#include "lastfm_key.h"

int mc_lastfm_build_key_present(void) {
  return MC_LASTFM_KEY_LEN > 0 && MC_LASTFM_SECRET_LEN > 0;
}

/* Undoes the mask and the even/odd split. The volatile reads keep the compiler from
 * folding the decoding into plain text at build time. */
static int decode(const unsigned char *even, const unsigned char *odd, int len, char *out,
                  size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (len <= 0 || (size_t)len >= cap) return 0;
  const volatile unsigned char *mask = k_lastfm_mask;
  int mask_len = (int)sizeof k_lastfm_mask;
  for (int i = 0; i < len; i++) {
    const volatile unsigned char *half = i % 2 ? odd : even;
    out[i] = (char)(half[i / 2] ^ mask[i % mask_len]);
  }
  out[len] = '\0';
  return 1;
}

int mc_lastfm_build_key(char *out, size_t cap) {
  if (!mc_lastfm_build_key_present()) return out && cap ? (out[0] = '\0', 0) : 0;
  return decode(k_lastfm_key_even, k_lastfm_key_odd, MC_LASTFM_KEY_LEN, out, cap);
}

int mc_lastfm_build_secret(char *out, size_t cap) {
  if (!mc_lastfm_build_key_present()) return out && cap ? (out[0] = '\0', 0) : 0;
  return decode(k_lastfm_secret_even, k_lastfm_secret_odd, MC_LASTFM_SECRET_LEN, out, cap);
}

void mc_lastfm_wipe(void *buf, size_t len) {
  if (buf && len) mbedtls_platform_zeroize(buf, len);
}
