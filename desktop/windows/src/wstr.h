/* UI text as UTF-16, from the core's string table (musiccore/i18n.h, strings/en.json). */
#ifndef KITHSONG_WSTR_H
#define KITHSONG_WSTR_H

#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#include "musiccore/i18n.h"

/* Picks the strings' language from the user's Windows display language. Call it at startup,
 * before other threads start. */
void wstr_init(void);

/* mc_str(id) as UTF-16, without its mnemonic marker. Converted once and kept: the pointer
 * stays valid for the life of the process. */
const wchar_t *mc_wstr(enum mc_str_id id);

/* The string in Windows' '&' form, for buttons, menus and labels that show an access key:
 * "S_kip" -> "S&kip", "__" -> "_", and a literal '&' doubled. Kept like mc_wstr(). */
const wchar_t *mc_wstr_mnemonic(enum mc_str_id id);

/* Converts UTF-8 s into buf (n wide characters including the NUL), cut short if it
 * doesn't fit; NULL gives "". Returns buf. */
wchar_t *wstr_from_utf8(wchar_t *buf, size_t n, const char *s);

/* w as a malloc'd UTF-8 string, for passing UI text to mc_format(); NULL for NULL or out of
 * memory. Free with free(). */
char *wstr_to_utf8(const wchar_t *w);

/* s as a malloc'd UTF-16 string; NULL for NULL or out of memory. Free with free(). */
wchar_t *wstr_dup_from_utf8(const char *s);

/* "12.5 MB": bytes in the strings' units (KB, MB, GB, TB, of 1024), into buf (n wide
 * characters including the NUL). Returns buf. */
wchar_t *wstr_size(wchar_t *buf, size_t n, int64_t bytes);

/* mc_format() into a UTF-16 buffer: fmt (UTF-8, usually mc_str(..) or mc_str_plural(..))
 * with the tagged arguments, converted into buf (n wide characters including the NUL) and
 * cut short if it doesn't fit. Returns buf.
 *
 *   wchar_t text[64];
 *   mc_wformat(text, 64, mc_str_plural(MC_STR_TRACKS, n), MC_D(n));
 */
wchar_t *mc_wformat_args(wchar_t *buf, size_t n, const char *fmt, const mc_arg *args, int nargs);
#define mc_wformat(buf, n, fmt, ...) \
  mc_wformat_args((buf), (n), (fmt), MC_ARGS_(__VA_ARGS__), MC_NARGS_(__VA_ARGS__))

/* "Port:", a text box's label with its mnemonic: windows.field_label around
 * mc_str_mnemonic(id), in '&' form, into buf. Returns buf. */
wchar_t *wstr_field_label(wchar_t *buf, size_t n, enum mc_str_id id);

/* "&Sign in to Libre.fm…": mc_str_mnemonic(id) with one plain string argument for its {0},
 * in '&' form, into buf. The argument's own '_' and '&' show as they are. Returns buf. */
wchar_t *wstr_mnemonic_format(wchar_t *buf, size_t n, enum mc_str_id id, const char *arg);

/* "Rename…\tF2", a menu item with its shortcut, into buf. Returns buf. */
wchar_t *wstr_menu_item(wchar_t *buf, size_t n, enum mc_str_id text, enum mc_str_id shortcut);

/* "Next track (Ctrl+Right)", a button's tooltip with its shortcut, into buf. Returns buf. */
wchar_t *wstr_tooltip(wchar_t *buf, size_t n, enum mc_str_id text, enum mc_str_id shortcut);

#endif
