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

#endif
