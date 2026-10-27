/* The app's own string table (English only so far) and mc_format. Translations and the
 * fallback between languages are tested against a small table in test_strings_lang.c. */
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "test.h"

/* CHECK_STR for a char array, which is never NULL. */
#define CHECK_BUF(buf, s) CHECK(strcmp((buf), (s)) == 0)

static void test_lookup(void) {
  CHECK_STR(mc_strings_locale(), "en");
  CHECK_STR(mc_str(MC_STR_CANCEL), "Cancel");
  CHECK_STR(mc_str(MC_STR_LINUX_NOTHING_PLAYING), "Nothing playing");
  CHECK_STR(mc_str(MC_STR_WINDOWS_COL_YEAR), "Year");
  CHECK_STR(mc_str(MC_STR_ANDROID_BACK), "Back");
  CHECK_STR(mc_str(MC_STR_COUNT), "");
  CHECK_STR(mc_str((enum mc_str_id)-1), "");

  /* Mnemonics: stripped by mc_str, kept by mc_str_mnemonic. */
  CHECK_STR(mc_str(MC_STR_IMPORT_SKIP), "Skip");
  CHECK_STR(mc_str_mnemonic(MC_STR_IMPORT_SKIP), "S_kip");
  CHECK_STR(mc_str_mnemonic(MC_STR_CANCEL), "Cancel");

  /* Core's own text comes from the table too. */
  CHECK_STR(mc_status_str(MC_ERR_NOMEM), "out of memory");
  CHECK_STR(mc_status_str((mc_status)-999), "unknown error");
}

static void test_locale(void) {
  /* No translations yet: everything falls back to English. */
  mc_strings_set_locale("xx-YY");
  CHECK_STR(mc_strings_locale(), "en");
  CHECK_STR(mc_str(MC_STR_CANCEL), "Cancel");
  mc_strings_set_locale(NULL);
  CHECK_STR(mc_strings_locale(), "en");
  mc_strings_set_locale("C");
  CHECK_STR(mc_strings_locale(), "en");
  mc_strings_set_locale("en_US.UTF-8");
  CHECK_STR(mc_strings_locale(), "en");
}

static void test_plurals(void) {
  CHECK_STR(mc_str_plural(MC_STR_TRACKS, 1), "{0:d} track");
  CHECK_STR(mc_str_plural(MC_STR_TRACKS, 0), "{0:d} tracks");
  CHECK_STR(mc_str_plural(MC_STR_TRACKS, 2), "{0:d} tracks");
  CHECK_STR(mc_str_plural(MC_STR_TRACKS, 11), "{0:d} tracks");
  CHECK_STR(mc_str_plural(MC_STR_TRACKS, -1), "{0:d} track");
  CHECK_STR(mc_str(MC_STR_TRACKS), "{0:d} tracks"); /* a plural's "other" */
  CHECK_STR(mc_str_plural(MC_STR_CANCEL, 1), "Cancel"); /* not a plural */

  char buf[64];
  long n = 1;
  mc_format(buf, sizeof buf, mc_str_plural(MC_STR_TRACKS, n), MC_D(n));
  CHECK_BUF(buf, "1 track");
}

static void test_format(void) {
  char buf[64];
  CHECK(mc_format(buf, sizeof buf, "{1}, then {0}", MC_S("b"), MC_S("a")) == 9);
  CHECK_BUF(buf, "a, then b");
  mc_format(buf, sizeof buf, "{0}{0}{0}", MC_S("ab"));
  CHECK_BUF(buf, "ababab");
  mc_format(buf, sizeof buf, "{0:d} of {1:d}", MC_D(3), MC_D(-12));
  CHECK_BUF(buf, "3 of -12");
  mc_format(buf, sizeof buf, "{0}", MC_D(LLONG_MIN));
  CHECK_BUF(buf, "-9223372036854775808");
  mc_format(buf, sizeof buf, "{0:d}", MC_D(0));
  CHECK_BUF(buf, "0");

  /* Escapes, and what isn't a placeholder. */
  mc_format(buf, sizeof buf, "{{0}} is {0}", MC_S("x"));
  CHECK_BUF(buf, "{0} is x");
  mc_format(buf, sizeof buf, "{{{0}}}", MC_S("x"));
  CHECK_BUF(buf, "{x}");
  mc_format(buf, sizeof buf, "{x} {0 {} } {", MC_S("x"));
  CHECK_BUF(buf, "{x} {0 {} } {");
  mc_format(buf, sizeof buf, "{0} {1} {0:s}", MC_S("x"));
  CHECK_BUF(buf, "x {1} {0:s}"); /* missing argument, unknown type: copied through */
  mc_format(buf, sizeof buf, "[{0}]", MC_S(NULL));
  CHECK_BUF(buf, "[]");
  CHECK(mc_format_args(buf, sizeof buf, "{0} plain", NULL, 0) == 9);
  CHECK_BUF(buf, "{0} plain");
  CHECK(mc_format_args(buf, sizeof buf, NULL, NULL, 0) == 0);
  CHECK_BUF(buf, "");

  /* A real one. */
  mc_format(buf, sizeof buf, mc_str(MC_STR_IMPORT_MATCH_YEAR), MC_S("Radiohead"), MC_S("OK Computer"),
            MC_D(1997));
  CHECK_BUF(buf, "Radiohead \xE2\x80\x93 OK Computer (1997)");
}

static void test_truncation(void) {
  char buf[8];
  memset(buf, 'z', sizeof buf);
  CHECK(mc_format(buf, sizeof buf, "{0} and {1}", MC_S("this"), MC_S("that")) == 13);
  CHECK_BUF(buf, "this an");
  CHECK(mc_format(buf, 1, "{0}", MC_S("abc")) == 3);
  CHECK_BUF(buf, "");
  CHECK(mc_format(NULL, 0, "{0}", MC_S("abc")) == 3);
  /* A UTF-8 character is written whole or not at all: "a…" needs 5 bytes with the NUL. */
  CHECK(mc_format(buf, 4, "a{0}", MC_S("\xE2\x80\xA6")) == 4);
  CHECK_BUF(buf, "a");
  CHECK(mc_format(buf, 5, "a{0}", MC_S("\xE2\x80\xA6")) == 4);
  CHECK_BUF(buf, "a\xE2\x80\xA6");
  /* Nothing more after the first character that doesn't fit, even a shorter one. */
  CHECK(mc_format(buf, 4, "ab\xE2\x80\xA6" "c", MC_S("")) == 6);
  CHECK_BUF(buf, "ab");

  char *s = mc_format_alloc("{0}: {1:d} tracks", MC_S("Long playlist name"), MC_D(1234));
  CHECK_STR(s, "Long playlist name: 1234 tracks");
  mc_free(s);
}

int main(void) {
  test_lookup();
  test_locale();
  test_plurals();
  test_format();
  test_truncation();
  return TEST_RESULT();
}
