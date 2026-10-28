#include "util/text.h"

/* Base letters for U+00C0..U+00FF; '\0' is punctuation (× and ÷). */
static const char k_latin1[] = "aaaaaaaceeeeiiiidnooooo\0ouuuuyts"
                               "aaaaaaaceeeeiiiidnooooo\0ouuuuyty";

/* Base letters for U+0100..U+017F (Latin Extended-A). */
static const char k_latin_ext_a[] = "aaaaaaccccccccddddeeeeeeeeeegggggggghhhhiiiiiiiiiiiijj"
                                    "kkkllllllllllnnnnnnnnnoooooooorrrrrrsssssssstttttt"
                                    "uuuuuuuuuuuuwwyyyzzzzzzs";
_Static_assert(sizeof k_latin_ext_a == 0x80 + 1, "one letter per code point");

uint32_t mc_utf8_next(const unsigned char **s) {
  const unsigned char *p = *s;
  uint32_t c = *p++;
  int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
  if (extra) c &= 0x3Fu >> extra;
  while (extra-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3Fu);
  *s = p;
  return c;
}

static int one(uint32_t c, uint32_t out[3]) {
  out[0] = c;
  return 1;
}

int mc_text_fold(uint32_t c, uint32_t out[3]) {
  if (c < 0x80) {
    if (c >= 'A' && c <= 'Z') return one(c + 32, out);
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return one(c, out);
    if (c == '&') {
      out[0] = 'a', out[1] = 'n', out[2] = 'd';
      return 3;
    }
    if (c == '\'' || c == '.') return MC_FOLD_DROP;
    return c <= ' ' || c == 0x7F ? MC_FOLD_SPACE : MC_FOLD_PUNCT;
  }
  if (c < 0xC0) return c == 0xA0 ? MC_FOLD_SPACE : MC_FOLD_PUNCT; /* Latin-1 symbols */
  if (c <= 0xFF) return k_latin1[c - 0xC0] ? one((unsigned char)k_latin1[c - 0xC0], out) : MC_FOLD_PUNCT;
  if (c <= 0x17F) return one((unsigned char)k_latin_ext_a[c - 0x100], out);
  if (c >= 0x300 && c <= 0x36F) return MC_FOLD_DROP; /* combining accents, as in NFD text */
  if (c >= 0x391 && c <= 0x3A9) return one(c + 0x20, out); /* Greek capitals */
  if (c >= 0x400 && c <= 0x40F) return one(c + 0x50, out); /* Cyrillic Ѐ..Џ */
  if (c >= 0x410 && c <= 0x42F) return one(c + 0x20, out); /* Cyrillic А..Я */
  if (c >= 0x2000 && c <= 0x206F) {
    /* General punctuation: spaces, curly quotes, dashes, ellipsis. */
    if (c <= 0x200B || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F) return MC_FOLD_SPACE;
    return c == 0x2018 || c == 0x2019 ? MC_FOLD_DROP : MC_FOLD_PUNCT;
  }
  if (c == 0x3000) return MC_FOLD_SPACE;                    /* ideographic space */
  if (c >= 0x3001 && c <= 0x303F) return MC_FOLD_PUNCT;     /* CJK punctuation */
  if (c >= 0xFF01 && c <= 0xFF5E) return mc_text_fold(c - 0xFEE0, out); /* fullwidth ASCII */
  return one(c, out);
}
