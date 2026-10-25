/* Internals shared between library.c, playlist.c, sync and tests. */
#ifndef MC_LIBRARY_INTERNAL_H
#define MC_LIBRARY_INTERNAL_H

#include <sqlite3.h>

#include "musiccore/library.h"
#include "util/hlc.h"

struct mc_library {
  sqlite3 *db;
  mc_hlc hlc;
  char node_uid[MC_UID_STRLEN];
  /* Set while applying changes received from a peer, so triggers don't log them a second
   * time as local edits; sync inserts those changelog rows itself. */
  int applying_remote;
};

static inline sqlite3 *mc_library_db(mc_library *lib) {
  return lib->db;
}

#endif
