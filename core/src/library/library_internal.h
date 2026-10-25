/* Internals shared between library.c, playlist.c, sync and tests. */
#ifndef MC_LIBRARY_INTERNAL_H
#define MC_LIBRARY_INTERNAL_H

#include <sqlite3.h>

#include "musiccore/library.h"
#include "util/hlc.h"

struct mc_library {
  sqlite3 *db;
  char *dir; /* the library folder, no trailing separator */
  mc_hlc hlc;
  char node_uid[MC_UID_STRLEN];
  /* Set while applying changes received from a peer, so triggers don't log them a second
   * time as local edits; sync inserts those changelog rows itself. */
  int applying_remote;
};

static inline sqlite3 *mc_library_db(mc_library *lib) {
  return lib->db;
}

/* Every write to tracked tables goes through these. Several connections to one library
 * can exist in a process (the importer opens its own on its thread), each with its own
 * clock, so BEGIN IMMEDIATE takes the write lock and then catches the clock up to the
 * newest logged change. That keeps changelog hlcs strictly increasing per device. */
mc_status mc_library_begin_write(mc_library *lib);

/* files.path is relative to the library folder and always uses '/', so the folder can be
 * moved, or mounted under another drive letter, without touching the database.
 * relpath returns NULL if abs isn't inside the library. Both return malloc'd strings. */
char *mc_library_relpath(const mc_library *lib, const char *abs);
char *mc_library_abspath(const mc_library *lib, const char *rel);
mc_status mc_library_commit(mc_library *lib);
void mc_library_rollback(mc_library *lib);

#endif
