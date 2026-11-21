#include "wstr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "kithcore/kithcore.h"

void wstr_init(void) {
  /* The display language first (the list is double-NUL-terminated; the first is the one in
   * use), else the regional format's. */
  wchar_t langs[256], name[LOCALE_NAME_MAX_LENGTH];
  ULONG count = 0, size = sizeof langs / sizeof langs[0];
  const wchar_t *tag = NULL;
  if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, langs, &size) && count && langs[0]) tag = langs;
  else if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)) tag = name;
  char utf8[LOCALE_NAME_MAX_LENGTH * 3];
  if (!tag || !WideCharToMultiByte(CP_UTF8, 0, tag, -1, utf8, sizeof utf8, NULL, NULL)) utf8[0] = '\0';
  mc_strings_set_locale(utf8);
}

wchar_t *wstr_dup_from_utf8(const char *s) {
  if (!s) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

/* As wstr_dup_from_utf8(), with "" for NULL. */
static wchar_t *widen(const char *s) {
  return wstr_dup_from_utf8(s ? s : "");
}

/* Copies w into buf, cut short at n - 1 characters without splitting a surrogate pair. */
static wchar_t *copy_into(wchar_t *buf, size_t n, const wchar_t *w) {
  if (!n) return buf;
  size_t len = w ? wcslen(w) : 0;
  if (len >= n) {
    len = n - 1;
    if (len && IS_HIGH_SURROGATE(w[len - 1])) len--;
  }
  if (len) wmemcpy(buf, w, len);
  buf[len] = L'\0';
  return buf;
}

/* The '&' form of GTK-style mnemonic text, malloc'd. */
static wchar_t *ampersand_form(const char *s) {
  wchar_t *w = widen(s);
  wchar_t *out = w ? malloc((wcslen(w) * 2 + 1) * sizeof *out) : NULL;
  if (out) {
    wchar_t *o = out;
    for (const wchar_t *p = w; *p; p++) {
      if (*p == L'_') *o++ = p[1] == L'_' ? *++p : L'&';
      else if (*p == L'&') *o++ = L'&', *o++ = L'&';
      else *o++ = *p;
    }
    *o = L'\0';
  }
  free(w);
  return out;
}

/* Converted strings, one per id, made on first use. Looked up from any thread, so a slot is
 * filled with a compare-and-swap and the loser of a race frees its copy. */
static wchar_t *plain[MC_STR_COUNT], *mnemonic[MC_STR_COUNT];

static const wchar_t *cached(wchar_t **slot, wchar_t *made) {
  if (!made) return L"";
  wchar_t *old = InterlockedCompareExchangePointer((void *volatile *)slot, made, NULL);
  if (!old) return made;
  free(made);
  return old;
}

const wchar_t *mc_wstr(enum mc_str_id id) {
  if ((unsigned)id >= MC_STR_COUNT) return L"";
  return plain[id] ? plain[id] : cached(&plain[id], widen(mc_str(id)));
}

const wchar_t *mc_wstr_mnemonic(enum mc_str_id id) {
  if ((unsigned)id >= MC_STR_COUNT) return L"";
  return mnemonic[id] ? mnemonic[id] : cached(&mnemonic[id], ampersand_form(mc_str_mnemonic(id)));
}

wchar_t *wstr_from_utf8(wchar_t *buf, size_t n, const char *s) {
  wchar_t *w = widen(s);
  copy_into(buf, n, w);
  free(w);
  return buf;
}

wchar_t *wstr_long_path(const wchar_t *path) {
  DWORD n = GetFullPathNameW(path, 0, NULL, NULL);
  wchar_t *full = n ? malloc((n + 8) * sizeof *full) : NULL;
  if (!full) return _wcsdup(path);
  DWORD got = GetFullPathNameW(path, n, full + 8, NULL);
  if (got == 0 || got >= n || got < MAX_PATH - 12 || (full[8] == L'\\' && full[9] == L'?')) {
    free(full);
    return _wcsdup(path);
  }
  /* "\\server\share" takes UNC in place of its first backslash. */
  int unc = full[8] == L'\\';
  const wchar_t *prefix = unc ? L"\\\\?\\UNC" : L"\\\\?\\";
  size_t len = wcslen(prefix);
  wchar_t *start = full + 8 + (unc ? 1 : 0) - len;
  memcpy(start, prefix, len * sizeof *start);
  memmove(full, start, (wcslen(start) + 1) * sizeof *full);
  return full;
}

char *wstr_to_utf8(const wchar_t *w) {
  if (!w) return NULL;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = n > 0 ? malloc((size_t)n) : NULL;
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}

wchar_t *wstr_size(wchar_t *buf, size_t n, int64_t bytes) {
  char text[64];
  mc_format_size(text, sizeof text, bytes);
  return wstr_from_utf8(buf, n, text);
}

wchar_t *mc_wformat_args(wchar_t *buf, size_t n, const char *fmt, const mc_arg *args, int nargs) {
  char *s = mc_format_alloc_args(fmt, args, nargs);
  wstr_from_utf8(buf, n, s);
  mc_free(s);
  return buf;
}

wchar_t *wstr_field_label(wchar_t *buf, size_t n, enum mc_str_id id) {
  char *s = mc_format_alloc(mc_str(MC_STR_WINDOWS_FIELD_LABEL), MC_S(mc_str_mnemonic(id)));
  wchar_t *w = ampersand_form(s);
  copy_into(buf, n, w);
  free(w);
  mc_free(s);
  return buf;
}

wchar_t *wstr_mnemonic_format(wchar_t *buf, size_t n, enum mc_str_id id, const char *arg) {
  /* The argument's underscores doubled, so the '&' form keeps them. */
  size_t len = arg ? strlen(arg) : 0, k = 0;
  char *escaped = malloc(len * 2 + 1);
  if (escaped) {
    for (size_t i = 0; i < len; i++) {
      if (arg[i] == '_') escaped[k++] = '_';
      escaped[k++] = arg[i];
    }
    escaped[k] = '\0';
  }
  char *s = mc_format_alloc(mc_str_mnemonic(id), MC_S(escaped ? escaped : ""));
  wchar_t *w = ampersand_form(s);
  copy_into(buf, n, w);
  free(w);
  mc_free(s);
  free(escaped);
  return buf;
}

wchar_t *wstr_menu_item(wchar_t *buf, size_t n, enum mc_str_id text, enum mc_str_id shortcut) {
  _snwprintf(buf, n, L"%ls\t%ls", mc_wstr_mnemonic(text), mc_wstr(shortcut));
  if (n) buf[n - 1] = L'\0';
  return buf;
}

wchar_t *wstr_tooltip(wchar_t *buf, size_t n, enum mc_str_id text, enum mc_str_id shortcut) {
  return mc_wformat(buf, n, mc_str(MC_STR_TOOLTIP_SHORTCUT), MC_S(mc_str(text)), MC_S(mc_str(shortcut)));
}

wchar_t *wstr_count(wchar_t *buf, size_t n, int64_t value) {
  wchar_t digits[32], *p = digits + 31;
  uint64_t v = value < 0 ? (uint64_t)-value : (uint64_t)value;
  *p = L'\0';
  do *--p = (wchar_t)(L'0' + v % 10);
  while ((v /= 10) && p > digits + 1);
  if (value < 0) *--p = L'-';
  NUMBERFMTW fmt = {.NumDigits = 0, .Grouping = 3, .lpDecimalSep = L".", .lpThousandSep = L","};
  wchar_t sep[8];
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8)) fmt.lpThousandSep = sep;
  if (!GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, p, &fmt, buf, (int)n)) lstrcpynW(buf, p, (int)n);
  return buf;
}
