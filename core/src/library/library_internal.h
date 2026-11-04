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
  struct mc_refresh *refresh; /* the artist refresh this handle started (tagger/refresh.h) */
};

/* mc_library_open() without starting background work of its own (the artist refresh after
 * upgrading): for connections the core opens for itself, on an import's or sync's thread,
 * which come and go while the frontend's handle stays open. */
mc_status mc_library_open_internal(const char *dir, mc_library **out);
/* mc_library_open_internal() that never undoes an abandoned pairing (sync/libmerge.h): for
 * the copy of the library from before it, which undoing it opens to bring up to date. */
mc_status mc_library_open_plain(const char *dir, mc_library **out);

/* Rereads node_uid and library_uid from meta into this handle, after the library's contents
 * were replaced (sync/libmerge.h). */
mc_status mc_library_reload_ids(mc_library *lib);

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

/* Artist credits (docs/schema.md). A track or album credited "A feat. B" shows its
 * artist_credit, else its first artist's name; a track with no artist of its own shows its
 * album's. With t, ar, al and aar as in TRACK_COLS: */
#define TRACK_ARTIST_NAME "coalesce(t.artist_credit, ar.name, al.artist_credit, aar.name)"

/* The albums, and the tracks, credited to artist p (an SQL parameter or column): artist_uid
 * is the first artist, and credit rows past position 0 the others. Row 0 repeats artist_uid
 * and isn't read, so a peer of an older version that changes artist_uid alone is followed.
 * A track with no artist of its own is credited as its album is. Queries of uids. */
#define CREDITED_ALBUMS(p) \
  "SELECT uid FROM albums WHERE artist_uid = " p \
  " UNION SELECT album_uid FROM album_artists WHERE artist_uid = " p " AND position > 0"
#define CREDITED_TRACKS(p) \
  "SELECT uid FROM tracks WHERE artist_uid = " p \
  " UNION SELECT track_uid FROM track_artists WHERE artist_uid = " p " AND position > 0" \
  " UNION SELECT t0.uid FROM tracks t0 WHERE t0.artist_uid IS NULL" \
  "  AND t0.album_uid IN (" CREDITED_ALBUMS(p) ")"

/* The columns mc_library_read_tracks() reads, then the joins they need; t is the track, al
 * its album, ar its artist and aar the album's artist. The file is the original if there
 * are transcodes too. A playlist's query adds its entry's uid as a last column. */
#define TRACK_COLS \
  "SELECT t.uid, t.title, t.track_no, t.disc_no, t.duration_ms," \
  " (SELECT f.path FROM files f WHERE f.track_uid = t.uid" \
  "  ORDER BY f.source_file_uid IS NOT NULL LIMIT 1)," \
  " " TRACK_ARTIST_NAME ", al.title, al.art_hash IS NOT NULL, coalesce(ar.uid, aar.uid)," \
  " coalesce(t.year, al.year), al.uid"
#define TRACK_JOINS \
  " LEFT JOIN artists ar ON ar.uid = t.artist_uid" \
  " LEFT JOIN albums al ON al.uid = t.album_uid" \
  " LEFT JOIN artists aar ON aar.uid = al.artist_uid"

/* A track alone on its album from this long up is taken for a continuous mix. 30 minutes, so
 * it clears what's long but is still one piece: the prog epics (Supper's Ready 23, Echoes 23,
 * most of the rest under 27) and the single movements of a symphony (Mahler's longest about
 * 30), while a DJ set or radio mix is rarely under it. The two mistakes aren't equal: a track
 * taken for a mix leaves smart playlists without a word, one missed just stays a single until
 * it's ticked. A podcast episode does pass, and is better found as an album anyway. Worked
 * out from synced rows on each device, so a new value here needs no migration. */
#define MC_MIX_MIN_MS 1800000
#define MC_SQL_NUM_(n) #n
#define MC_SQL_NUM(n) MC_SQL_NUM_(n)

/* Whether album alias a is a continuous mix: albums.mix is the user's choice, 1 or -1, and 0
 * leaves it to the album's one track being long. See "Album flags" in docs/schema.md. */
#define MC_ALBUM_IS_MIX(a)                                                                      \
  "(" a ".mix = 1 OR (" a ".mix = 0 AND coalesce((SELECT count(*) = 1 AND max(st.duration_ms) >= " \
  MC_SQL_NUM(MC_MIX_MIN_MS) " FROM tracks st WHERE st.album_uid = " a ".uid), 0)))"

/* The same for track t on album al, as in TRACK_JOINS: never NULL, also with no album. Only a
 * long track costs a count, as every track of a smart list asks. */
#define TRACK_IN_MIX                                                                  \
  "coalesce(al.mix = 1 OR (al.mix = 0 AND t.duration_ms >= " MC_SQL_NUM(MC_MIX_MIN_MS) \
  " AND (SELECT count(*) FROM tracks st WHERE st.album_uid = al.uid) = 1), 0)"

/* The flag two albums being made one end up with: either's yes, else either's no. */
#define MC_MIX_EITHER(a, b) "CASE WHEN " a " = 1 OR " b " = 1 THEN 1 ELSE min(" a ", " b ") END"

/* Whether album a has exactly one track in the library and isn't a mix: a single. Album
 * listings leave those out, and mc_library_list_artist_singles() lists their tracks instead. */
#define ALBUM_IS_SINGLE \
  "((SELECT count(*) FROM tracks st WHERE st.album_uid = a.uid) = 1 AND NOT " MC_ALBUM_IS_MIX("a") ")"

/* The columns mc_library_read_albums() reads, the join they need and the album display order;
 * a is the album and ar its artist. The folder is wherever the album's files are: any one
 * file's directory will do. */
#define ALBUM_COLS \
  "SELECT a.uid, a.title, coalesce(a.artist_credit, ar.name), a.year, " ALBUM_IS_SINGLE \
  ", a.art_hash IS NOT NULL," \
  " (SELECT f.path FROM tracks t JOIN files f ON f.track_uid = t.uid" \
  "  WHERE t.album_uid = a.uid LIMIT 1), ar.uid"
#define ALBUM_JOINS " LEFT JOIN artists ar ON ar.uid = a.artist_uid"
#define ALBUM_ORDER \
  "coalesce(ar.sort_name, ar.name) COLLATE NOCASE, a.year, a.title COLLATE NOCASE"

/* Files the tracks with no album that where says (an SQL condition on t, with ?1 for arg if
 * it's not NULL) as albums of one track, of the track's title, artists and year, for them to
 * be mixes on: a mix is an album's flag. Rows only: moving the files is the caller's. Inside a
 * write transaction; logged, like any edit. In album_flags.c. */
mc_status mc_mix_file_tracks(mc_library *lib, const char *where, const char *arg);

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
 * library_uid and the rest of meta stay. meta (nmeta key/value pairs, values not NULL; may
 * be NULL with 0) is written in the same transaction. Other connections stay valid (it's the
 * same file, and mc_library_begin_write() rereads the node uid). */
mc_status mc_library_reset_for_merge(mc_library *lib, const char *const *meta, int nmeta);

#endif
