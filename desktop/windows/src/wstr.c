#include "wstr.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "musiccore/musiccore.h"

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

/* s as a malloc'd UTF-16 string; NULL if out of memory. NULL gives "". */
static wchar_t *widen(const char *s) {
  if (!s) s = "";
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
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

char *wstr_to_utf8(const wchar_t *w) {
  if (!w) return NULL;
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = n > 0 ? malloc((size_t)n) : NULL;
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
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

wchar_t *wstr_menu_item(wchar_t *buf, size_t n, enum mc_str_id text, enum mc_str_id shortcut) {
  _snwprintf(buf, n, L"%ls\t%ls", mc_wstr_mnemonic(text), mc_wstr(shortcut));
  if (n) buf[n - 1] = L'\0';
  return buf;
}
