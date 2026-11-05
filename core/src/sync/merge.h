/* Duplicate merging: the same artist, album or track imported on two devices becomes one
 * row, under the smaller uid. See "Duplicates" in docs/sync-protocol.md. Internal to core.
 *
 * Which uids are one is a function of the changelog alone: two uids merge once the
 * changelog shows them sharing a MusicBrainz id (artists by mb_artistid, albums by
 * mb_albumid, tracks by mb_releasetrackid) or a merge_name, which a row without an id logs
 * from what it's called (mc_merge_note_names()). "Shows" means
 * any value ever logged, not only the current winner, so the relation only grows as rows
 * arrive and every device reaches the same classes whatever order it gets them in. The
 * merge is applied locally and never logged; see rows.c for how the applier uses this. */
#ifndef MC_SYNC_MERGE_H
#define MC_SYNC_MERGE_H

#include <sqlite3.h>

#include "musiccore/musiccore.h"

typedef struct mc_library mc_library;

/* One uid merged away in a scan. */
typedef struct mc_merge_pair {
  char table[16]; /* artists, albums or tracks */
  char loser[MC_UID_STRLEN];
} mc_merge_pair;

/* 1 if rows of table can be merged: the merged entities, and recents, which is keyed by
 * their uids. */
int mc_merge_table(const char *table);

/* Logs merge_name for each artist, album and track with no MusicBrainz id, for whatever it
 * goes by now: an artist by name, an album by title and artist, a track on an album by those,
 * its disc and number, and its title, each as mc_name_key() has it. Whether a row has an id
 * is decided here and logged, not worked out by each device, or a row whose id arrives after
 * its name would merge on one device and not on another. Takes the write lock. Run before
 * sending rows; quick when no such row has changed since the last run. */
mc_status mc_merge_note_names(mc_library *lib);

/* Reads the changelog rows logged since the last scan (meta.merge_seq), records their keys
 * and merges the classes they join. *pairs (malloc'd, free with free()) lists each uid
 * merged away by this scan. Inside the caller's write transaction. */
mc_status mc_merge_scan(sqlite3 *db, mc_merge_pair **pairs, int *count);

/* The uid that uid now is: its class's canonical uid, or uid itself. *merged = 1 if it
 * differs, or if uid is canonical for others. */
mc_status mc_merge_canonical(sqlite3 *db, const char *uid, char out[MC_UID_STRLEN], int *merged);

/* After the canonical rows are materialized: moves every reference to p->loser over to its
 * canonical uid (or, if the canonical row doesn't exist, leaves them to ON DELETE), then
 * deletes the loser's row and its recents row. */
mc_status mc_merge_rewrite(sqlite3 *db, const mc_merge_pair *p);

/* Points one row's references at canonical uids, for rows that arrive naming a uid that has
 * been merged away. */
mc_status mc_merge_map_row(sqlite3 *db, const char *table, const char *uid);

/* For readers of other databases (sync profiles): the uid a library uid now is. out may be
 * uid. A NULL lib leaves it as it is. */
mc_status mc_merge_resolve(mc_library *lib, const char *uid, char out[MC_UID_STRLEN]);

/* mc_merge_resolve() for a long list of uids, which prepares its query once. lib may be NULL
 * as there. Between _open and _close, used on one thread. */
typedef struct mc_merge_resolver {
  sqlite3_stmt *stmt;
} mc_merge_resolver;

mc_status mc_merge_resolver_open(mc_library *lib, mc_merge_resolver *r);
mc_status mc_merge_resolver_get(mc_merge_resolver *r, const char *uid, char out[MC_UID_STRLEN]);
void mc_merge_resolver_close(mc_merge_resolver *r);

#endif
