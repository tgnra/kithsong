/* Duplicate merging: the same artist, album or track imported on two devices becomes one
 * row, under the smaller uid. See "Duplicates" in docs/sync-protocol.md. Internal to core.
 *
 * Which uids are one is a function of the changelog alone: two uids merge once the
 * changelog shows them sharing a MusicBrainz id (artists by mb_artistid, albums by
 * mb_albumid, tracks by mb_releasetrackid). "Shows" means
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

#endif
