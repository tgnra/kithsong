/* UI text in the user's language. Every user-visible string lives in strings/<lang>.json
 * (see strings/README.md); the build generates enum mc_str_id from it, one id per string:
 * MC_STR_<KEY> for the shared section (e.g. MC_STR_CANCEL), MC_STR_<SECTION>_<KEY> for the
 * platform sections (MC_STR_LINUX_NOT_AVAILABLE_YET, MC_STR_WINDOWS_COL_YEAR).
 *
 * All text is UTF-8 and static: never free it. A string a language doesn't translate comes
 * out in English. */
#ifndef MUSICCORE_I18N_H
#define MUSICCORE_I18N_H

#include <stddef.h>

#include "mc_strings_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Picks the language from a BCP 47 tag ("fr-FR") or a POSIX locale ("fr_FR.UTF-8"): the
 * full tag if there is a table for it, then shorter ones ("fr"), then English. NULL, "",
 * "C" and "POSIX" mean English, which is also what you get before the first call. Call
 * it at startup, before other threads look up strings; strings already looked up stay in
 * the old language. */
void mc_strings_set_locale(const char *locale);

/* The language in use, e.g. "en" or "fr". */
const char *mc_strings_locale(void);

/* The string, without its mnemonic marker ("S_kip" -> "Skip"). For a plural, its "other"
 * form. An id out of range gives "". */
const char *mc_str(enum mc_str_id id);

/* The string with its mnemonic, the letter after a single '_' ("S_kip"; "__" is a literal
 * underscore), as GTK's *_with_mnemonic and use-underline take it. The Windows frontend
 * turns it into '&' form. The same as mc_str() for strings without one. */
const char *mc_str_mnemonic(enum mc_str_id id);

/* The plural form for count n in the current language (CLDR categories: zero, one, two,
 * few, many, other), falling back to "other". The count still has to be formatted in:
 * mc_format(buf, sizeof buf, mc_str_plural(MC_STR_TRACKS, n), MC_D(n)). */
const char *mc_str_plural(enum mc_str_id id, long n);

/* --- Formatting ---
 *
 * Placeholders are {0}, {1}, ... (zero-based, in any order, any number of times); {0:d}
 * says the argument is a number, for translators and Android, but the argument's own kind
 * decides how it's written. {{ and }} are literal braces. A placeholder with no argument
 * is copied through as is, so a mistake shows instead of crashing.
 *
 * Arguments are tagged: MC_S("text") for a string (NULL writes nothing), MC_D(42) for an
 * integer. Pre-format anything else (sizes, times) into a string first:
 *
 *   char buf[256];
 *   mc_format(buf, sizeof buf, mc_str(MC_STR_IMPORT_MATCH_YEAR), MC_S(artist), MC_S(album),
 *             MC_D(year));
 *
 * Like snprintf: writes at most size bytes including the NUL (never splitting a UTF-8
 * character), always terminates when size > 0, and returns the full length it wanted, so
 * a return >= size means it was cut short. */

typedef enum mc_arg_kind { MC_ARG_STR, MC_ARG_INT } mc_arg_kind;

typedef struct mc_arg {
  mc_arg_kind kind;
  const char *s;
  long long i;
} mc_arg;

#define MC_S(x) ((mc_arg){MC_ARG_STR, (x), 0})
#define MC_D(x) ((mc_arg){MC_ARG_INT, NULL, (long long)(x)})

size_t mc_format_args(char *buf, size_t size, const char *fmt, const mc_arg *args, int nargs);

/* As mc_format_args, into a new string to free with mc_free(); NULL if out of memory. */
char *mc_format_alloc_args(const char *fmt, const mc_arg *args, int nargs);

/* mc_format(buf, size, fmt, MC_S(..), MC_D(..), ...) and mc_format_alloc(fmt, args...):
 * the same with the arguments listed in place. They need at least one argument; with none,
 * use the _args forms with NULL, 0. */
#define MC_ARGS_(...) ((const mc_arg[]){__VA_ARGS__})
#define MC_NARGS_(...) ((int)(sizeof MC_ARGS_(__VA_ARGS__) / sizeof(mc_arg)))
#define mc_format(buf, size, fmt, ...) \
  mc_format_args((buf), (size), (fmt), MC_ARGS_(__VA_ARGS__), MC_NARGS_(__VA_ARGS__))
#define mc_format_alloc(fmt, ...) \
  mc_format_alloc_args((fmt), MC_ARGS_(__VA_ARGS__), MC_NARGS_(__VA_ARGS__))

#ifdef __cplusplus
}
#endif

#endif
