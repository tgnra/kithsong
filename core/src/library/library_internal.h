/* Internals shared between library.c, playlist.c, sync and tests. */
#ifndef MC_LIBRARY_INTERNAL_H
#define MC_LIBRARY_INTERNAL_H

#include <sqlite3.h>

#include "musiccore/library.h"
#include "util/changelog.h"
#include "util/hlc.h"

struct mc_library {
  sqlite3 *db;
  char *dir; /* the library folder, no trailing separator */
  mc_hlc hlc;
  char node_uid[MC_UID_STRLEN];
  char library_uid[MC_UID_STRLEN];
  /* Set while applying changes received from a peer, so triggers don't log them a second
   * time as local edits; sync inserts those changelog rows itself. */
  int applying_remote;
  mc_changelog_ctx log; /* points the trigger functions at hlc, node_uid, applying_remote */
  struct mc_search *search; /* search.c's index and its builder; NULL until first used */
};

static inline sqlite3 *mc_library_db(mc_library *lib) {
  return lib->db;
}

/* Every write to tracked tables goes through these. Several connections to one library
 * can exist in a process (the importer opens its own on its thread), each with its own
 * clock, so BEGIN IMMEDIATE takes the write lock and then catches the clock up to the
 * newest logged change. That keeps changelog hlcs strictly increasing per device. It also
 * rereads node_uid, in case another connection rotated it. */
mc_status mc_library_begin_write(mc_library *lib);

/* files.path is relative to the library folder and always uses '/', so the folder can be
 * moved, or mounted under another drive letter, without touching the database.
 * relpath returns NULL if abs isn't inside the library. Both return malloc'd strings. */
char *mc_library_relpath(const mc_library *lib, const char *abs);
char *mc_library_abspath(const mc_library *lib, const char *rel);
mc_status mc_library_commit(mc_library *lib);
void mc_library_rollback(mc_library *lib);

/* The columns mc_library_read_tracks() reads, then the joins they need; t is the track, al
 * its album, ar its artist and aar the album's artist. The file is the original if there
 * are transcodes too. A playlist's query adds its entry's uid as a last column. */
#define TRACK_COLS \
  "SELECT t.uid, t.title, t.track_no, t.disc_no, t.duration_ms," \
  " (SELECT f.path FROM files f WHERE f.track_uid = t.uid" \
  "  ORDER BY f.source_file_uid IS NOT NULL LIMIT 1)," \
  " coalesce(ar.name, aar.name), al.title, al.art_hash IS NOT NULL, coalesce(ar.uid, aar.uid)"
#define TRACK_JOINS \
  " LEFT JOIN artists ar ON ar.uid = t.artist_uid" \
  " LEFT JOIN albums al ON al.uid = t.album_uid" \
  " LEFT JOIN artists aar ON aar.uid = al.artist_uid"

/* The columns mc_library_read_albums() reads, the join they need and the album display order;
 * a is the album and ar its artist. The folder is wherever the album's files are: any one
 * file's directory will do. */
#define ALBUM_COLS \
  "SELECT a.uid, a.title, ar.name, a.year, a.kind = 'singles', a.art_hash IS NOT NULL," \
  " (SELECT f.path FROM tracks t JOIN files f ON f.track_uid = t.uid" \
  "  WHERE t.album_uid = a.uid LIMIT 1), ar.uid"
#define ALBUM_JOINS " LEFT JOIN artists ar ON ar.uid = a.artist_uid"
#define ALBUM_ORDER \
  "coalesce(ar.sort_name, ar.name) COLLATE NOCASE, a.kind = 'singles', a.year, a.title COLLATE NOCASE"

/* Step a prepared, bound TRACK_COLS or ALBUM_COLS query into a list. Both finalize stmt. */
mc_status mc_library_read_tracks(mc_library *lib, sqlite3_stmt *stmt, mc_track_list **out);
mc_status mc_library_read_albums(mc_library *lib, sqlite3_stmt *stmt, mc_album_list **out);

/* Whether saved smart list smart_uid holds something with a file here, into *out (see
 * MC_LIST_PLAYABLE). A rule that can't be evaluated holds nothing. In smartlist.c. */
mc_status mc_smart_has_playable(mc_library *lib, const char *smart_uid, int *out);

/* Any one file of album alias a, which finds the album's folder; originals first. */
#define MC_ALBUM_FILE(a)                                                             \
  "(SELECT f.path FROM tracks t2 JOIN files f ON f.track_uid = t2.uid"              \
  "  WHERE t2.album_uid = " a ".uid ORDER BY f.source_file_uid IS NOT NULL LIMIT 1)"

/* The albums saved smart list smart_uid holds now, for its cover art: a prepared, bound query
 * of (art_hash, MC_ALBUM_FILE) rows. A smart album group's come in the list's order, NULL
 * art_hash included; a smart playlist's are its tracks' albums with art, in no order. The
 * caller finalizes *stmt. MC_ERR_FORMAT if the rule can't be evaluated. In smartlist.c. */
mc_status mc_smart_prepare_art(mc_library *lib, const char *smart_uid, mc_smart_subject *subject,
                               sqlite3_stmt **stmt);

/* The tables sync replicates (k_tracked) and their logged columns. Sync works out the
 * order to apply them in from their foreign keys. */
const mc_changelog_table *mc_library_tracked(size_t *count);

/* meta values (not logged: meta never syncs). get: MC_ERR_NOT_FOUND if key isn't there.
 * set: value NULL deletes the key. */
mc_status mc_library_meta_get(mc_library *lib, const char *key, char *out, size_t cap);
mc_status mc_library_meta_set(mc_library *lib, const char *key, const char *value);

/* For merging libraries (sync/libmerge.c): empties the library in place without logging
 * anything. Every library row, files, aliases, merge_keys and the changelog go, the example
 * smart lists are seeded again as migration v10 does, and the library gets a new node uid.
 * library_uid and the rest of meta stay. Other connections stay valid (it's the same file,
 * and mc_library_begin_write() rereads the node uid). */
mc_status mc_library_reset_for_merge(mc_library *lib);

#endif
