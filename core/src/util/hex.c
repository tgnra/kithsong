#include "util/hex.h"

#include <stdlib.h>
#include <string.h>

void mc_hex(const void *bytes, size_t n, char *out) {
  static const char digits[] = "0123456789abcdef";
  const unsigned char *b = bytes;
  for (size_t i = 0; i < n; i++) {
    *out++ = digits[b[i] >> 4];
    *out++ = digits[b[i] & 15];
  }
  *out = '\0';
}

char *mc_percent_encode(const char *s, const char *keep) {
  static const char digits[] = "0123456789ABCDEF";
  char *out = malloc(strlen(s) * 3 + 1), *o = out;
  if (!out) return NULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
        strchr("-_.~", *p) || (keep && strchr(keep, *p))) {
      *o++ = (char)*p;
    } else {
      *o++ = '%';
      *o++ = digits[*p >> 4];
      *o++ = digits[*p & 15];
    }
  }
  *o = '\0';
  return out;
}
