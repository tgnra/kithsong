/* Internals of library/delete.c shared with tests. */
#ifndef MC_DELETE_INTERNAL_H
#define MC_DELETE_INTERNAL_H

#include "kithcore/kithcore.h"

/* How deleted files are trashed: mc_trash, or a stand-in for tests. */
extern mc_status (*mc_delete_trash)(const char *path);
/* How a permanent delete removes them: mc_remove, or a stand-in for tests. */
extern mc_status (*mc_delete_remove)(const char *path);

typedef struct mc_library mc_library;
typedef struct sqlite3 sqlite3;

/* --- A peer's delete (sync/transfer.c) ---
 * Originals sync trashes after another device's delete get a record as a delete here does, so
 * they can be restored rather than swept as unknown. By then the rows have gone, so the session
 * catches them first with the SQL function this registers on db, mc_delete_record(track_uid,
 * file_uid), which gives what mc_delete_save_record() stores as rows. */
mc_status mc_delete_register(sqlite3 *db);
/* Opens the records of config_dir for saving in one go; NULL if it can't. */
sqlite3 *mc_delete_records_open(const char *config_dir);
/* A file that went to the trash: abs where it was, rel as files.path, its size, modification
 * time and hash (may be NULL). */
mc_status mc_delete_save_record(sqlite3 *store, mc_library *lib, const char *abs, const char *rel,
                                int64_t size, int64_t mtime, const char *sha, const char *rows);
void mc_delete_records_close(sqlite3 *store);

/* --- The recycle bin (kithcore/delete.h), for sync (sync/here.c and session.c) --- */

/* An album or track a taken-back entry named: the device keeps it off itself. */
typedef struct mc_bin_item {
  mc_delete_kind kind; /* MC_DELETE_ALBUMS: an album uid; else a track uid */
  char uid[MC_UID_STRLEN];
} mc_bin_item;

typedef struct mc_bin_taken {
  mc_bin_item *items;
  int count;
  int entries; /* entries taken back: the one asked for, and those its rows needed */
  int tracks;
} mc_bin_taken;

/* Takes entry id back out of the bin, in one write: deletes the changelog rows its delete
 * logged, so no other device ever hears of it, and puts its rows back unlogged (they're as
 * the changelog has them). A later entry whose delete took a row this one needs (the album
 * of a track, gone with its last track) comes back with it. out lists the albums and tracks
 * of every entry taken, for the caller to take off this device's profile; free it with
 * mc_bin_taken_free(). MC_ERR_NOT_FOUND if there's no such entry, e.g. a sync sealed it. */
mc_status mc_delete_bin_take(mc_library *lib, int64_t id, mc_bin_taken *out);
void mc_bin_taken_free(mc_bin_taken *t);

/* Before a sync sends the library's rows: empties the bin, in a write of its own, as its
 * deletes are about to reach another device (or may have). With allow 0, a bin that holds
 * anything is left as it is and MC_ERR_LOCKED returned: this device's user hasn't asked for
 * that sync. *pending (may be NULL) is how many entries it held. */
mc_status mc_delete_bin_seal(mc_library *lib, int allow, int *pending);

#endif
