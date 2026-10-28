/* Which devices are archives, counting a void demotion (see "Archives" in
 * docs/sync-protocol.md). Internal to core.
 *
 * profiles.archive is the stored flag. When no current member has it, the demotion with the
 * lowest (hlc, origin_uid) is void and its device still counts as an archive: these helpers
 * are what every reader of "is it an archive" goes through. The rule is a function of the
 * changelog alone, so every device that holds the same rows reaches the same answer. */
#ifndef MC_SYNC_ARCHIVE_H
#define MC_SYNC_ARCHIVE_H

#include <sqlite3.h>

#include "musiccore/library.h"
#include "musiccore/musiccore.h"
#include "musiccore/sync.h"
#include "sync/sync_db.h"

/* The device whose demotion is void into out, or "" if some current member is an archive
 * or no demotion qualifies. A demotion is the winning archive change of a current member's
 * profile when it sets 0 after an earlier change set 1. */
mc_status mc_archive_void(sqlite3 *db, char out[MC_UID_STRLEN]);

/* Whether uid is an archive: its stored flag, or its demotion is void. 1, 0, or -1 on a
 * database error. */
int mc_archive_is(sqlite3 *db, const char *uid);

/* How many current (non-removed) members are archives, a void one included; -1 on error. */
int mc_archive_count(sqlite3 *db);

/* From profile.c: the wall time (Unix ms) of the library's latest change, 0 if it has none,
 * -1 on error; and whether a current member other than uid is an archive whose sync_status
 * shows it verified complete (nothing missing or corrupt, checked after latest_ms): 1, 0 or
 * -1. Clearing an archive flag, and removing originals imported here, rely on the latter. */
int64_t mc_profile_library_latest_ms(mc_library *lib);
int mc_profile_other_archive_verified(sqlite3 *db, const char *uid, int64_t latest_ms);

/* Deletes device_uid's profile_items rows of kind (a profile_items.kind, an exclusion's too)
 * that resolve to item->ref_uid, every copy; item->kind says how a ref_uid resolves (albums
 * and artists follow merges). Inside the caller's sync.db write. */
mc_status mc_profile_delete_rows(sqlite3 *db, mc_library *lib, const char *device_uid,
                                 const char *kind, const mc_profile_item *item);

/* "It writes archive = 1 again": if this device's own demotion is void, logs archive = 1
 * on its profile in a write of its own. *wrote (may be NULL) says whether it did. */
mc_status mc_archive_restore(mc_sync_db *sdb, int *wrote);

#endif
