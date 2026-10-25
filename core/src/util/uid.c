/* UUIDv7 (RFC 9562): 48-bit Unix ms timestamp, then random bits. Time-ordered ids keep
 * SQLite primary-key indexes append-mostly, which matters for big imports. */
#include "musiccore/musiccore.h"

#include "util/platform.h"

mc_status mc_uid_new(char out[MC_UID_STRLEN]) {
  static const char hex[] = "0123456789abcdef";
  unsigned char b[16];

  mc_status st = mc_random_bytes(b + 6, sizeof b - 6);
  if (st != MC_OK) return st;

  uint64_t ms = mc_wall_ms();
  for (int i = 0; i < 6; i++) b[i] = (unsigned char)(ms >> (40 - 8 * i));
  b[6] = (unsigned char)((b[6] & 0x0f) | 0x70); /* version 7 */
  b[8] = (unsigned char)((b[8] & 0x3f) | 0x80); /* RFC 4122 variant */

  char *p = out;
  for (int i = 0; i < 16; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) *p++ = '-';
    *p++ = hex[b[i] >> 4];
    *p++ = hex[b[i] & 0x0f];
  }
  *p = '\0';
  return MC_OK;
}
