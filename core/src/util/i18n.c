/* Looking up UI strings in the generated tables (util/i18n_table.h), plural rules, and
 * {0}-style formatting. */
#include "musiccore/i18n.h"

#include <stdlib.h>
#include <string.h>

#include "util/i18n_table.h"

/* The languages to look a string up in, most specific first: for fr-CA, fr-CA then fr
 * then English (index 0, always last). */
#define MAX_CHAIN 4
static int chain[MAX_CHAIN] = {0};
static int chain_len = 1;

static int lower(int c) {
  return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

/* Whether the first n characters of tag are the language code, ignoring case, with '_'
 * the same as '-'. */
static int tag_equal(const char *tag, size_t n, const char *code) {
  for (size_t i = 0; i < n; i++, tag++, code++) {
    int a = *tag == '_' ? '-' : lower((unsigned char)*tag);
    if (!*code || a != lower((unsigned char)*code)) return 0;
  }
  return *code == '\0';
}

void mc_strings_set_locale(const char *locale) {
  chain_len = 0;
  /* A POSIX locale's encoding and modifier aren't part of the language. */
  size_t n = locale ? strcspn(locale, ".@") : 0;
  if ((n == 1 && locale[0] == 'C') || (n == 5 && !strncmp(locale, "POSIX", 5))) n = 0;
  /* fr-CA-x, then fr-CA, then fr. */
  while (n > 0 && chain_len < MAX_CHAIN - 1) {
    for (int i = 1; i < mc_str_lang_count; i++) {
      if (tag_equal(locale, n, mc_str_langs[i].code)) {
        chain[chain_len++] = i;
        break;
      }
    }
    while (n > 0 && locale[n - 1] != '-' && locale[n - 1] != '_') n--;
    if (n > 0) n--; /* the separator */
  }
  chain[chain_len++] = 0;
}

const char *mc_strings_locale(void) {
  return mc_str_langs[chain[0]].code;
}

static const mc_str_entry *entry(int lang, enum mc_str_id id) {
  return &mc_str_langs[lang].entries[id];
}

/* The first language in the chain that has the string. English has them all. */
static const mc_str_entry *find(enum mc_str_id id) {
  for (int i = 0; i < chain_len - 1; i++)
    if (entry(chain[i], id)->text) return entry(chain[i], id);
  return entry(0, id);
}

const char *mc_str(enum mc_str_id id) {
  if ((int)id < 0 || id >= MC_STR_COUNT) return "";
  return find(id)->text;
}

const char *mc_str_mnemonic(enum mc_str_id id) {
  if ((int)id < 0 || id >= MC_STR_COUNT) return "";
  const mc_str_entry *e = find(id);
  return e->mnemonic ? e->mnemonic : e->text;
}

#define LANG(a, b) ((a) << 8 | (b))

/* The CLDR plural category of the integer n in language code. A language whose rule isn't
 * here gets English's; add a case for each new language whose rule differs. Rules:
 * https://www.unicode.org/cldr/charts/latest/supplemental/language_plural_rules.html */
static int plural_category(const char *code, long n) {
  unsigned long i = n < 0 ? 0UL - (unsigned long)n : (unsigned long)n; /* CLDR uses |n| */
  int two_letter = code[0] && code[1] && (code[2] == '\0' || code[2] == '-');
  switch (two_letter ? LANG(lower(code[0]), lower(code[1])) : 0) {
    case LANG('j', 'a'):
    case LANG('k', 'o'):
    case LANG('z', 'h'):
      return MC_PLURAL_OTHER; /* no plurals */
    case LANG('f', 'r'):
      if (i <= 1) return MC_PLURAL_ONE; /* 0 is singular too */
      return i % 1000000 == 0 ? MC_PLURAL_MANY : MC_PLURAL_OTHER; /* "1 000 000 d'items" */
    default: /* en */
      return i == 1 ? MC_PLURAL_ONE : MC_PLURAL_OTHER;
  }
}

const char *mc_str_plural(enum mc_str_id id, long n) {
  if ((int)id < 0 || id >= MC_STR_COUNT) return "";
  /* The first language with the plural, by that language's rule. */
  for (int k = 0; k < chain_len; k++) {
    const mc_str_entry *e = entry(chain[k], id);
    if (!e->forms[MC_PLURAL_OTHER]) continue;
    const char *s = e->forms[plural_category(mc_str_langs[chain[k]].code, n)];
    return s ? s : e->forms[MC_PLURAL_OTHER];
  }
  return mc_str(id); /* not a plural */
}

/* --- Formatting --- */

typedef struct out {
  char *buf;
  size_t size;    /* of buf, NUL included; 0 to only measure */
  size_t written; /* bytes in buf */
  size_t len;     /* bytes wanted */
  int full;       /* a character didn't fit, so nothing more is written */
} out;

static size_t utf8_char_len(unsigned char c) {
  return c < 0x80 ? 1 : c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc0 ? 2 : 1;
}

/* Appends n bytes a whole UTF-8 character at a time, leaving room for the NUL. */
static void put(out *o, const char *s, size_t n) {
  for (size_t k = 0; k < n;) {
    size_t c = utf8_char_len((unsigned char)s[k]);
    if (c > n - k) c = n - k;
    if (!o->full && o->written + c < o->size) {
      memcpy(o->buf + o->written, s + k, c);
      o->written += c;
    } else {
      o->full = 1;
    }
    o->len += c;
    k += c;
  }
}

static void put_int(out *o, long long v) {
  char tmp[24];
  size_t n = 0;
  unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
  do tmp[sizeof tmp - 1 - n++] = (char)('0' + u % 10);
  while (u /= 10);
  if (v < 0) tmp[sizeof tmp - 1 - n++] = '-';
  put(o, tmp + sizeof tmp - n, n);
}

size_t mc_format_args(char *buf, size_t size, const char *fmt, const mc_arg *args, int nargs) {
  out o = {buf, buf ? size : 0, 0, 0, 0};
  if (!args) nargs = 0;
  const char *p = fmt ? fmt : "";
  while (*p) {
    if ((p[0] == '{' && p[1] == '{') || (p[0] == '}' && p[1] == '}')) {
      put(&o, p, 1);
      p += 2;
      continue;
    }
    if (*p == '{') {
      /* {N} or {N:d}; anything else is copied as is. */
      const char *q = p + 1;
      long idx = 0;
      int digits = 0;
      while (*q >= '0' && *q <= '9' && digits < 6) idx = idx * 10 + (*q++ - '0'), digits++;
      if (digits && q[0] == ':' && q[1] == 'd') q += 2;
      if (digits && *q == '}' && idx < nargs) {
        const mc_arg *a = &args[idx];
        if (a->kind == MC_ARG_INT) put_int(&o, a->i);
        else if (a->s) put(&o, a->s, strlen(a->s));
        p = q + 1;
        continue;
      }
    }
    size_t n = strcspn(p + 1, "{}") + 1;
    put(&o, p, n);
    p += n;
  }
  if (o.size) buf[o.written] = '\0';
  return o.len;
}

char *mc_format_alloc_args(const char *fmt, const mc_arg *args, int nargs) {
  size_t n = mc_format_args(NULL, 0, fmt, args, nargs);
  char *s = malloc(n + 1);
  if (s) mc_format_args(s, n + 1, fmt, args, nargs);
  return s;
}
