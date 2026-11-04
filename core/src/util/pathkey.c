#include "util/pathkey.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util/path_fold_table.h"
#include "util/text.h"

#define HANGUL_FIRST 0xAC00u
#define HANGUL_LAST 0xD7A3u

static int is_mark(uint32_t c) {
  size_t lo = 0, hi = sizeof k_path_marks / sizeof k_path_marks[0];
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c > k_path_marks[mid].last) lo = mid + 1;
    else if (c < k_path_marks[mid].first) hi = mid;
    else return 1;
  }
  return 0;
}

/* c's row of k_path_fold, or -1. */
static int fold_row(uint32_t c) {
  size_t lo = 0, hi = sizeof k_path_fold / sizeof k_path_fold[0];
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (c > k_path_fold[mid].cp) lo = mid + 1;
    else if (c < k_path_fold[mid].cp) hi = mid;
    else return (int)mid;
  }
  return -1;
}

static char *put(char *o, uint32_t c) {
  if (c < 0x80) {
    *o++ = (char)c;
  } else if (c < 0x800) {
    *o++ = (char)(0xC0 | c >> 6);
    *o++ = (char)(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    *o++ = (char)(0xE0 | c >> 12);
    *o++ = (char)(0x80 | (c >> 6 & 0x3F));
    *o++ = (char)(0x80 | (c & 0x3F));
  } else {
    *o++ = (char)(0xF0 | (c >> 18 & 7));
    *o++ = (char)(0x80 | (c >> 12 & 0x3F));
    *o++ = (char)(0x80 | (c >> 6 & 0x3F));
    *o++ = (char)(0x80 | (c & 0x3F));
  }
  return o;
}

char *mc_path_key(const char *path) {
  if (!path) return NULL;
  /* A code point of one byte stays one; a longer one becomes at most three of four bytes. */
  char *out = malloc(strlen(path) * 6 + 1), *o = out;
  if (!out) return NULL;
  const unsigned char *p = (const unsigned char *)path;
  while (*p) {
    uint32_t c = mc_utf8_next(&p);
    if (c == '\\') {
      *o++ = '/';
    } else if (c < 0x80) {
      *o++ = (char)(c >= 'A' && c <= 'Z' ? c + 32 : c);
    } else if (c >= HANGUL_FIRST && c <= HANGUL_LAST) {
      uint32_t s = c - HANGUL_FIRST;
      o = put(o, 0x1100 + s / 588);
      o = put(o, 0x1161 + s % 588 / 28);
      if (s % 28) o = put(o, 0x11A7 + s % 28);
    } else if (!is_mark(c)) {
      int row = fold_row(c);
      if (row < 0) {
        o = put(o, c);
      } else {
        for (int i = 0; i < 3 && k_path_fold[row].to[i]; i++) o = put(o, k_path_fold[row].to[i]);
      }
    }
  }
  *o = '\0';
  return out;
}
