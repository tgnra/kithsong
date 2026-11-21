/* Hooks into the import pipeline (tagger.c) for tests. */
#ifndef MC_TAGGER_TAGGER_INTERNAL_H
#define MC_TAGGER_TAGGER_INTERNAL_H

#include "tagger/import_db.h"

typedef mc_status (*mc_import_db_write_fn)(mc_library *lib, const mc_db_album *album,
                                           const mc_db_track *tracks, int count);

/* Imports save their rows through fn instead of mc_import_db_write (NULL: back to it), so a
 * test can see what they pass; fn usually calls mc_import_db_write itself. Set it while no
 * import is running. */
void mc_import_set_db_write(mc_import_db_write_fn fn);

/* A session that only holds items, for the demo import (kithcore/demo.h): no folders, no
 * library, no MusicBrainz, no writing. items are copied as they are (state, matches with
 * their tracks' files, confidence, outcome and the rest), and a thread of its own reports
 * MC_IMPORT_EV_SCANNED and MC_IMPORT_EV_ITEM_CHANGED for each, then nothing more: no
 * MC_IMPORT_EV_IDLE, so it stays part-way through. Requests (accept, search, ...) are taken and
 * never acted on. Freed with mc_import_free(). */
mc_status mc_import_start_fixed(const mc_import_item *items, int count, mc_import_fn callback,
                                void *user, mc_import **out);

#endif
