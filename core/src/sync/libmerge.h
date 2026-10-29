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

/* meta key holding the set-aside copy's file name (in the library folder) until the merge
 * has run. Never logged, like all of meta. */
#define MC_LIBMERGE_META "merge_source"
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
 * empties it with mc_library_reset_for_merge() and records the copy in meta
 * (MC_LIBMERGE_META). *backup_abs (may be NULL) gets the copy's absolute path, malloc'd. */
mc_status mc_libmerge_set_aside(mc_library *lib, char **backup_abs);

/* 1 if a set-aside copy is waiting to be merged in; *backup_abs (may be NULL) gets its
 * absolute path, malloc'd. */
int mc_libmerge_pending(mc_library *lib, char **backup_abs);

typedef void (*mc_libmerge_progress_fn)(void *user, const mc_merge_progress *progress);

/* Merges the pending copy into lib, which must hold the kept library by now. kept (sorted by
 * track uid, may be NULL) is the kept device's originals; without them tracks the kept
 * library has are attached rather than compared. progress (may be NULL) is called as it
 * goes. Fills *out (strings NULL; the caller sets backup_path and the scrobble fields) and
 * clears MC_LIBMERGE_META once done, even if some tracks failed; an error before that
 * (the copy can't be opened, a database error) leaves it pending for another try.
 * MC_ERR_NOT_FOUND if nothing is pending. */
mc_status mc_libmerge_run(mc_library *lib, const mc_kept_file *kept, int nkept,
                          mc_libmerge_progress_fn progress, void *user, mc_merge_result *out);

/* Sorts kept by track uid, for mc_libmerge_run(). */
void mc_libmerge_sort_kept(mc_kept_file *kept, int count);

#endif
