/* Deleting tracks and albums from the library, and taking them back when their files come
 * back out of the Recycle Bin or trash. An artist isn't deleted as such: it goes when its
 * last track does.
 *
 * Deleting moves each track's files on this device (the original, and any copies or
 * transcodes sync brought) to the Recycle Bin or trash with mc_trash(), then removes its
 * rows: the track, its plays and its places in playlists, and an album or artist that's
 * left with no tracks (with the album's places in album groups). An album folder left with
 * only its cover loses the cover to the trash too, and the folder goes. Like any change it's
 * logged, so sync deletes the track on every device: their copies are removed and originals
 * imported there go to their own trash. A track one of whose files can't be trashed (it's in
 * use, or there's no trash, as on Android) stays, with that file.
 *
 * Where there's no trash (Android), mc_delete_run_permanently() does the same but removes
 * the files outright and records nothing, so there's nothing to restore; the frontend says
 * so before it deletes. Sync passes it on the same way.
 *
 * What's needed to take a track back is kept outside the library, in deleted.db in the
 * config dir (next to settings.json and sync.db): for each trashed file, where it was, its
 * size, modification time and hash (when known), and its rows as JSON (the track and its
 * album, artists, plays, playlist places and album group places, the file's own row). So
 * library.db holds only the library, and nothing about deleted tracks travels with it.
 *
 * mc_delete_restore() looks for those files at their old paths. One that's back, the same
 * size and either the same modification time or the same hash, gets its rows back with
 * their old uids: the track reappears with its play counts, added date, playlists and
 * album groups (those that still exist), and sync brings it back on the other devices too.
 * It's cheap (one stat per deleted file), so frontends call it at startup and whenever the
 * window is activated, as the user may have just restored files from the Recycle Bin; sync
 * calls it too before it sweeps unknown files out of the library folder. Records older than
 * MC_DELETE_KEEP_DAYS are dropped, the files most likely gone from the trash by then.
 *
 * Sync and deletes: a delete is newer than every edit seen before it, so it wins over them
 * on every device and a track doesn't come back by itself. An edit to the track's own
 * fields made on another device before it heard of the delete, but later, still wins (as
 * with any row) and brings the track's row back without its files here; plays and playlist
 * places made there don't, as they belong to the deleted track. */
#ifndef MUSICCORE_DELETE_H
#define MUSICCORE_DELETE_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;

/* How long a deleted file's record waits to be restored. */
#define MC_DELETE_KEEP_DAYS 90

typedef enum mc_delete_kind {
  MC_DELETE_TRACKS,  /* the tracks named */
  MC_DELETE_ALBUMS  /* every track of the albums named */
} mc_delete_kind;

typedef struct mc_delete_plan mc_delete_plan;

/* What deleting uids (count of them, of kind) would delete, for the confirmation: see
 * mc_delete_plan_tracks() and _files(). Uids that don't exist are skipped. Free with
 * mc_delete_plan_free(). */
mc_status mc_delete_plan_new(mc_library *lib, mc_delete_kind kind, const char *const *uids,
                             int count, mc_delete_plan **out);
void mc_delete_plan_free(mc_delete_plan *plan);
/* The tracks it deletes. */
int mc_delete_plan_tracks(const mc_delete_plan *plan);
/* The files it moves to the trash: the tracks' files on this device, and the covers of
 * album folders it leaves empty. Tracks with no file here only lose their rows. */
int mc_delete_plan_files(const mc_delete_plan *plan);
/* Whether it deletes track_uid, e.g. so a frontend can stop playing it first (a file that's
 * open can't go to the Recycle Bin). */
int mc_delete_plan_has(const mc_delete_plan *plan, const char *track_uid);

typedef struct mc_delete_result {
  int tracks_deleted;
  int files_trashed; /* or removed, by mc_delete_run_permanently() */
  int tracks_kept;   /* a file couldn't go to the trash (or be removed), so the track stayed with it */
} mc_delete_result;

/* Deletes what plan holds, as the library is now, recording what's needed to restore it in
 * config_dir/deleted.db. out may be NULL. MC_OK even if some tracks were kept (see
 * tracks_kept); an error only if the library couldn't be written, in which case files
 * already trashed are still recorded, so restoring them brings them back. */
mc_status mc_delete_run(mc_library *lib, const char *config_dir, const mc_delete_plan *plan,
                        mc_delete_result *out);

/* mc_delete_run() for a device with no trash (Android): the files, and the covers of album
 * folders it leaves empty, are removed for good rather than trashed, and nothing is
 * recorded, so mc_delete_restore() can't bring them back. The same results and errors; a
 * file that can't be removed keeps its track. */
mc_status mc_delete_run_permanently(mc_library *lib, const mc_delete_plan *plan,
                                    mc_delete_result *out);

/* Brings back deleted tracks whose files are back where they were (see above), and drops
 * records older than MC_DELETE_KEEP_DAYS. *restored (may be NULL) is how many files came
 * back: if it's not 0, the library changed. *tracks (may be NULL) is how many tracks came
 * back with them, e.g. for a "Restored N tracks" message: fewer when a track had several
 * files here (an original and a copy), or one of its files came back earlier. A missing
 * deleted.db is not an error. */
mc_status mc_delete_restore(mc_library *lib, const char *config_dir, int *restored, int *tracks);

#ifdef __cplusplus
}
#endif

#endif
