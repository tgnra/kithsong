/* Folding text for comparison: lowercase, without accents, "&" as "and". Shared by the
 * importer's string distance (tagger/match.c) and search. */
#ifndef MC_UTIL_TEXT_H
#define MC_UTIL_TEXT_H

#include <stdint.h>

/* What mc_text_fold() made of a code point when it isn't letters. */
#define MC_FOLD_DROP 0       /* nothing, and the word goes on: ' . combining accents */
#define MC_FOLD_SPACE (-1)   /* a word break: spaces */
#define MC_FOLD_PUNCT (-2)   /* a word break inside a run: / - ( ) and other punctuation */

/* Decodes the code point at *s and moves *s past it. Lenient: a broken sequence yields
 * whatever bits it has rather than failing. */
uint32_t mc_utf8_next(const unsigned char **s);

/* The comparable form of c, written to out: up to 3 code points ("&" -> "and"), returning
 * how many, or one of MC_FOLD_DROP, MC_FOLD_SPACE, MC_FOLD_PUNCT. Folds case and strips
 * accents for Latin-1, Latin Extended-A, Greek and Cyrillic capitals; other letters
 * (e.g. CJK) are kept as they are. */
int mc_text_fold(uint32_t c, uint32_t out[3]);

/* What two names must share to be the same name, for merging duplicates: name folded as
 * above, its punctuation gone, its words one space apart, and a leading "The" gone. malloc'd,
 * or NULL if out of memory or nothing is left of it ("!!!"), which matches nothing. */
char *mc_name_key(const char *name);

#endif
