/* Language selection and fallback, against the small table in tests/strings/ (English,
 * French and Canadian French), built with the same generator and lookup code as the app. */
#include "musiccore/i18n.h"
#include "test.h"

static void test_select(void) {
  CHECK_STR(mc_strings_locale(), "en");
  CHECK_STR(mc_str(MC_STR_HELLO), "Hello");

  mc_strings_set_locale("fr");
  CHECK_STR(mc_strings_locale(), "fr");
  mc_strings_set_locale("FR");
  CHECK_STR(mc_strings_locale(), "fr");
  mc_strings_set_locale("fr-BE");
  CHECK_STR(mc_strings_locale(), "fr");
  mc_strings_set_locale("fr_FR.UTF-8");
  CHECK_STR(mc_strings_locale(), "fr");
  mc_strings_set_locale("fr-ca");
  CHECK_STR(mc_strings_locale(), "fr-CA");
  mc_strings_set_locale("fr-CA-x-private");
  CHECK_STR(mc_strings_locale(), "fr-CA");
  mc_strings_set_locale("de-DE");
  CHECK_STR(mc_strings_locale(), "en");
  mc_strings_set_locale("f");
  CHECK_STR(mc_strings_locale(), "en");
  mc_strings_set_locale("");
  CHECK_STR(mc_strings_locale(), "en");
}

static void test_fallback(void) {
  mc_strings_set_locale("fr-FR");
  CHECK_STR(mc_str(MC_STR_HELLO), "Bonjour");
  CHECK_STR(mc_str(MC_STR_ONLY_EN), "English only");
  CHECK_STR(mc_str(MC_STR_LINUX_LINUX_ONLY), "Linux");
  CHECK_STR(mc_str(MC_STR_SKIP), "Passer");
  CHECK_STR(mc_str_mnemonic(MC_STR_SKIP), "_Passer");

  /* fr-CA has only hello; the rest comes from fr, then English. */
  mc_strings_set_locale("fr-CA");
  CHECK_STR(mc_str(MC_STR_HELLO), "All\xC3\xB4");
  CHECK_STR(mc_str(MC_STR_GREET), "Bonjour, {0}");
  CHECK_STR(mc_str(MC_STR_ONLY_EN), "English only");
  CHECK_STR(mc_str_mnemonic(MC_STR_SKIP), "_Passer");

  mc_strings_set_locale("en");
  CHECK_STR(mc_str(MC_STR_HELLO), "Hello");
  CHECK_STR(mc_str_mnemonic(MC_STR_SKIP), "S_kip");
}

static void test_plurals(void) {
  mc_strings_set_locale("en");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 0), "{0:d} items");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 1), "{0:d} item");
  CHECK_STR(mc_str_plural(MC_STR_NO_ONE, 1), "{0:d} things"); /* only "other" */

  /* French's own rule: 0 is singular, whole millions are "many". */
  mc_strings_set_locale("fr");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 0), "{0:d} \xC3\xA9l\xC3\xA9ment");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 1), "{0:d} \xC3\xA9l\xC3\xA9ment");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 2), "{0:d} \xC3\xA9l\xC3\xA9ments");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 2000000), "{0:d} d'\xC3\xA9l\xC3\xA9ments");
  /* Untranslated: English's forms by English's rule, so 0 isn't singular. */
  CHECK_STR(mc_str_plural(MC_STR_NO_ONE, 0), "{0:d} things");

  /* fr-CA inherits fr's plural. */
  mc_strings_set_locale("fr-CA");
  CHECK_STR(mc_str_plural(MC_STR_ITEMS, 0), "{0:d} \xC3\xA9l\xC3\xA9ment");
  mc_strings_set_locale(NULL);
}

int main(void) {
  test_select();
  test_fallback();
  test_plurals();
  return TEST_RESULT();
}
