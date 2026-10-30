/* UUIDv7 (RFC 9562): 48-bit Unix ms timestamp, then random bits. Time-ordered ids keep
 * SQLite primary-key indexes append-mostly, which matters for big imports. */
#include "util/uid.h"

#include <stdio.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "util/platform.h"

static void format_uid(const unsigned char b[16], char out[MC_UID_STRLEN]) {
  static const char hex[] = "0123456789abcdef";
  char *p = out;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
    *p++ = hex[b[i] >> 4];
    *p++ = hex[b[i] & 0x0f];
  }
  *p = '\0';
}

mc_status mc_uid_new(char out[MC_UID_STRLEN]) {
  unsigned char b[16];

  mc_status st = mc_random_bytes(b + 6, sizeof b - 6);
  if (st != MC_OK) return st;

  uint64_t ms = mc_wall_ms();
  for (int i = 0; i < 6; i++) b[i] = (unsigned char)(ms >> (40 - 8 * i));
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x70); /* version 7 */
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); /* RFC 4122 variant */
  format_uid(b, out);
  return MC_OK;
}

mc_status mc_uid_derive(const char *owner, int position, char out[MC_UID_STRLEN]) {
  if (!owner || !out) return MC_ERR_INVALID_ARG;
  char pos[16];
  int n = snprintf(pos, sizeof pos, "/%d", position);
  unsigned char digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  int rc = mbedtls_sha256_starts(&ctx, 0);
  if (rc == 0) rc = mbedtls_sha256_update(&ctx, (const unsigned char *)owner, strlen(owner));
  if (rc == 0) rc = mbedtls_sha256_update(&ctx, (const unsigned char *)pos, (size_t)n);
  if (rc == 0) rc = mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  if (rc != 0) return MC_ERR_IO;
  digest[6] = (unsigned char)((digest[6] & 0x0f) | 0x80); /* version 8 */
  digest[8] = (unsigned char)((digest[8] & 0x3f) | 0x80); /* RFC 4122 variant */
  format_uid(digest, out);
  return MC_OK;
}
