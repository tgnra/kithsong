/* Artist credits: splitting a credited string such as "A feat. B" or "A & B" into its
 * artists. See docs/tagging-rules.md, "Artist credits". */
#ifndef MC_TAGGER_CREDIT_H
#define MC_TAGGER_CREDIT_H

#include "musiccore/musiccore.h"

typedef struct mc_credit_split_part {
  char *name;
  char *join; /* the separator after it as written (" feat. ", ", "), NULL after the last */
} mc_credit_split_part;

typedef struct mc_credit_split {
  mc_credit_split_part *parts;
  int count;
} mc_credit_split;

/* Whether name is an artist known to be one artist (in the library, by MusicBrainz id or
 * exact name): 1 yes, 0 no. */
typedef int (*mc_known_artist_fn)(void *user, const char *name);

/* Splits a credit at its separators. "feat."-style separators (feat., ft., featuring, also
 * "(feat. B)" at the end) always split. Ambiguous ones (&, ",", and, x, vs., with, /, ;) split
 * only where every resulting part is known and the joined string is not; known may be NULL,
 * and then they never split. With nothing to split, out has one part, the whole credit.
 * Release with mc_credit_split_clear(). */
mc_status mc_credit_split_text(const char *credit, mc_known_artist_fn known, void *user,
                               mc_credit_split *out);

void mc_credit_split_clear(mc_credit_split *s);

#endif
