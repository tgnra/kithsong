/* The string tables cmake/gen_strings.cmake writes (mc_strings_gen.c), read by i18n.c. */
#ifndef MC_I18N_TABLE_H
#define MC_I18N_TABLE_H

#include <stddef.h>

#include "mc_strings_gen.h"

/* CLDR plural categories, in the order of mc_str_entry.forms. */
enum { MC_PLURAL_ZERO, MC_PLURAL_ONE, MC_PLURAL_TWO, MC_PLURAL_FEW, MC_PLURAL_MANY, MC_PLURAL_OTHER,
       MC_PLURAL_COUNT };

typedef struct mc_str_entry {
  const char *text;     /* without the mnemonic marker; a plural's "other" form; NULL: untranslated */
  const char *mnemonic; /* with it, or NULL if the text has none */
  const char *forms[MC_PLURAL_COUNT]; /* a plural's forms, NULL where not given */
} mc_str_entry;

typedef struct mc_str_lang {
  const char *code; /* the file's name: "en", "fr", "pt-BR" */
  const mc_str_entry *entries; /* MC_STR_COUNT of them */
} mc_str_lang;

/* English first, then the translations. */
extern const mc_str_lang mc_str_langs[];
extern const int mc_str_lang_count;

#endif
