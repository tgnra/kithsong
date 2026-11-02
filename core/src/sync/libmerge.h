/* Merging libraries, the library side (see "Merging libraries" in docs/sync-protocol.md and
 * musiccore/sync.h). The device whose library isn't kept sets it aside (a copy next to it)
 * and empties it in place before it joins; once the first session has brought the kept
 * library, the copy's current state is merged in as an import would bring it in: tracks
 * through the import's duplicates check, then playlists, album groups, smart lists, play
 * counts and recents mapped onto the kept library's tracks, all as fresh edits. None of the
 * copy's changelog comes along. Internal to core; pairing.c and sync.c drive it. */
#ifndef MC_SYNC_LIBMERGE_H
#define MC_SYNC_LIBMERGE_H

#include <stdint.h>

#include "library/library_internal.h"
#include "musiccore/sync.h"
#include "sync/sync_db.h"

/* meta key holding the set-aside copy's file name (in the library folder) until the merge
 * has run. Never logged, like all of meta. */
#define MC_LIBMERGE_META "merge_source"
/* meta key set with MC_LIBMERGE_META: the merge waits until this device's user has
 * confirmed its profile (see "Confirming profiles" in docs/sync-protocol.md), so neither
 * library visibly changes before then. Cleared by mc_libmerge_release(). A copy set aside by
 * an older version has none, and is merged at once as it was then. Its value is the run that
 * set it aside (mc_libmerge_run_token()): found by a later run, the app stopped before Sync
 * was pressed, and the pairing is undone (see "Abandoning a merge" below). */
#define MC_LIBMERGE_WAIT_META "merge_wait"
/* meta keys set with it, for undoing it: the kept device's name, and this device's own name
 * before it paired. */
#define MC_LIBMERGE_WITH_META "merge_with"
#define MC_LIBMERGE_NAME_META "merge_name"
/* The copy's name: MC_LIBMERGE_PREFIX "YYYY-MM-DD" (UTC) ".db", with "-2" and so on before
 * ".db" if that's taken. */
#define MC_LIBMERGE_PREFIX "library-before-merge-"

/* Which library is kept: 1 if the first one (a), 0 if b. The larger by track count, then by
 * bytes of files held, then the smaller (older) library uid. Symmetric: swapping a and b
 * flips the answer, so both devices agree. */
int mc_libmerge_keeps_first(int64_t tracks_a, int64_t bytes_a, const char *library_a,
                            int64_t tracks_b, int64_t bytes_b, const char *library_b);

/* An original the kept library's device holds: what it sounds like, for the duplicates
 * check (format as files.format, kbps as files.bitrate_kbps, 0 if unknown). */
typedef struct mc_kept_file {
  char track[MC_UID_STRLEN];
  char format[16];
  int kbps;
} mc_kept_file;

/* Copies the library to a new file in its folder (VACUUM INTO; the copy has no WAL), then
 * empties it with mc_library_reset_for_merge() and, in the same transaction, records the
 * copy in meta (MC_LIBMERGE_META), waiting for the profile to be confirmed
 * (MC_LIBMERGE_WAIT_META), with kept_name and own_name (may be NULL) for undoing it.
 * *backup_abs (may be NULL) gets the copy's absolute path, malloc'd. The caller backs up
 * sync.db first (mc_libmerge_backup_sync()). */
mc_status mc_libmerge_set_aside(mc_library *lib, const char *kept_name, const char *own_name,
                                char **backup_abs);

/* 1 if a set-aside copy is waiting to be merged in; *backup_abs (may be NULL) gets its
 * absolute path, malloc'd. */
int mc_libmerge_pending(mc_library *lib, char **backup_abs);

/* 1 while a pending merge waits for this device's profile to be confirmed. */
int mc_libmerge_waiting(mc_library *lib);

/* This device's profile was confirmed: a pending merge may run after the next session that
 * syncs the library. Nothing to do (MC_OK) if none waits. */
mc_status mc_libmerge_release(mc_library *lib);

typedef void (*mc_libmerge_progress_fn)(void *user, const mc_merge_progress *progress);

/* Merges the pending copy into lib, which must hold the kept library by now. kept (sorted by
 * track uid, may be NULL) is the kept device's originals; without them tracks the kept
 * library has are attached rather than compared. progress (may be NULL) is called as it
 * goes. It doesn't look at MC_LIBMERGE_WAIT_META (the caller does). Fills *out (strings
 * NULL; the caller sets backup_path and the scrobble fields) and clears MC_LIBMERGE_META
 * (and MC_LIBMERGE_WAIT_META) once done, even if some tracks failed; an error before that
 * (the copy can't be opened, a database error) leaves it pending for another try.
 * MC_ERR_NOT_FOUND if nothing is pending. */
mc_status mc_libmerge_run(mc_library *lib, const mc_kept_file *kept, int nkept,
                          mc_libmerge_progress_fn progress, void *user, mc_merge_result *out);

/* Sorts kept by track uid, for mc_libmerge_run(). */
void mc_libmerge_sort_kept(mc_kept_file *kept, int count);

/* --- Abandoning a merge (librestore.c) ---
 * While the merge still waits for Sync now, the pairing can be undone: the app was
 * restarted, or this device left or was removed from the kept group. The device then goes
 * back to exactly where it was before it paired: its library from the set-aside copy, and
 * its sync.db (its old group, if any, its scrobbling account and queue) from a copy taken
 * just before preparing. If the kept group still holds it as a member, it leaves that group
 * as mc_sync_leave_group() would. Nothing on disk but the databases changed while the merge
 * waited (its file phase did nothing), so no file needs putting back. See "Abandoning a
 * merge" in docs/sync-protocol.md.
 *
 * Each step is one transaction, or a folder renamed into place once it's whole, and keys in
 * meta say what's left, so a restore that stops halfway carries on at the next open:
 *  1. the library (mc_libmerge_restore()), which leaves MC_LIBMERGE_RESTORE_SYNC_META;
 *  2. sync.db (mc_libmerge_restore_sync()), at the next mc_sync_new() or straight away,
 *     which first copies the kept group's sync.db aside as the leave notice to send, then
 *     clears that key;
 *  3. the leave notice, sent to one member of the kept group once sync runs (sync.c).
 * The set-aside copy of the library stays, as after a merge. */

/* In the restored library until a frontend has said so (mc_sync_take_merge_restored()): the
 * kept device's name, "" if not known. */
#define MC_LIBMERGE_RESTORED_META "merge_restored"
/* In the restored library until sync.db has been restored too: "1", and this device's name
 * from before the pairing ("" if not known). */
#define MC_LIBMERGE_RESTORE_SYNC_META "merge_restore_sync"
#define MC_LIBMERGE_RESTORED_NAME_META "merge_restored_name"
/* Folders in the sync config dir, each holding a sync.db: the copy from before the pairing,
 * until the merge has run or been undone; the kept group's, with this device marked removed,
 * until a member has been told. "<name>.part" while one is being written. */
#define MC_LIBMERGE_SYNC_BACKUP_DIR "sync-before-merge"
#define MC_LIBMERGE_LEAVE_DIR "sync-leave-notice"

/* This run of the app (a fresh uid the first time it's asked for, the same after). */
const char *mc_libmerge_run_token(void);
/* Test hook: a new run, as if the app had been restarted. */
void mc_libmerge_test_new_run(void);

/* Copies sync.db to MC_LIBMERGE_SYNC_BACKUP_DIR in config_dir, replacing any copy there. */
mc_status mc_libmerge_backup_sync(mc_sync_db *sdb, const char *config_dir);
/* Puts sync.db back from that copy in place (the caller is still preparing: nothing else
 * has changed) and removes it. MC_ERR_NOT_FOUND if there's none. */
mc_status mc_libmerge_unprepare_sync(mc_sync_db *sdb, const char *config_dir);
/* Removes the copy, once the merge has run. */
void mc_libmerge_drop_sync_backup(const char *config_dir);
/* Leaving the kept group while the merge waits: if the group's scrobbling account is still
 * the one this device brought into it (only this device was signed in, so pairing made its
 * account the group's), it's signed out in sdb, logged, so the group hears of it with the
 * leave and is signed out as it was before. Nothing to do (MC_OK) otherwise. A leave notice
 * (step 3) does the same. */
mc_status mc_libmerge_sign_out_brought(mc_library *lib, mc_sync_db *sdb, const char *config_dir);

/* Step 1: puts back the set-aside copy (brought up to this version's schema, the copy itself
 * left as it is) in place of the library's contents, in one transaction, unlogged, if a merge
 * waits (MC_LIBMERGE_WAIT_META). Every table, meta included, is as the copy has it, plus
 * MC_LIBMERGE_RESTORED_META and step 2's keys. MC_ERR_NOT_FOUND if no merge waits, or its
 * copy is gone (it then stays pending). */
mc_status mc_libmerge_restore(mc_library *lib);
/* Step 1 at library open, if the waiting merge was set aside by an earlier run. */
mc_status mc_libmerge_restore_on_open(mc_library *lib);
/* Step 2, if step 1 left it to do (MC_ERR_NOT_FOUND otherwise, after removing a sync.db copy
 * that no waiting merge needs any more). own_name (cap bytes) gets this device's name from
 * before the pairing, "" if not known; *leave (may be NULL) is 1 if a leave notice waits to
 * be sent. */
mc_status mc_libmerge_restore_sync(mc_library *lib, mc_sync_db *sdb, const char *config_dir,
                                   char *own_name, size_t cap, int *leave);

#endif
