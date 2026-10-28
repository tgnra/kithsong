/* Removing an album or artist from this device, and syncing it here again: the menus'
 * "Remove from this PC" (computer, phone) and "Sync to this PC".
 *
 * Removing edits this device's sync profile so it no longer holds the item, and removes the
 * item's files here at once rather than at the next sync. The library rows stay, so the item
 * is still listed (its tracks show as not on this device), and every other device keeps its
 * copies. The profile edit is an exclusion: profile_items rows of kind 'exclude_album' or
 * 'exclude_artist', which take the item's tracks out of the wanted set whatever else selects
 * them, "Sync everything" included (see "Profiles and what a device holds" in
 * docs/sync-protocol.md). In a selection, the item's own album or artist row goes too. Like
 * any profile edit it syncs to the group, and any member's editor keeps it (editors don't
 * see exclusions; mc_sync_get_profile() leaves them out and mc_sync_set_profile() keeps them).
 * It comes back with mc_sync_restore_here(), or by selecting the same album or artist under
 * What to sync, which drops its exclusion.
 *
 * Only for a device that keeps a part of the library: one in a group (where an archive holds
 * everything) and not an archive itself. With no group the music lives nowhere else, so
 * deleting it from the library is the only honest option.
 *
 * Originals imported on this device stay until an archive holds verified copies, as
 * reconcile keeps them: here that's when another archive's own report shows it verified
 * complete after the library's latest change (mc_sync_set_archive()'s test). They then go to
 * the Recycle Bin or trash; files sync brought are removed outright. A file that can't be
 * removed (open in another program, or playing on Windows) keeps its files row, and reconcile
 * removes it after a later session. If the profile was confirmed (Sync pressed) before the
 * edit, it is confirmed again after it: this is the device's own user's say-so, so the device
 * isn't paused for it. Tracks can't be removed one by one yet. */
#ifndef MUSICCORE_SYNC_HERE_H
#define MUSICCORE_SYNC_HERE_H

#include "musiccore/musiccore.h"
#include "musiccore/sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_here_state {
  MC_HERE_UNAVAILABLE, /* offer neither: not in a group, not the group's library, an
                        * archive, or (an album) its artist was removed from this device */
  MC_HERE_HELD,        /* offer Remove from this device */
  MC_HERE_REMOVED      /* removed from this device: offer Sync to this device */
} mc_here_state;

/* What the menus offer for uid, an album (MC_PROFILE_ALBUM) or artist (MC_PROFILE_ARTIST).
 * Cheap: a few lookups in sync.db. MC_ERR_INVALID_ARG for another kind. */
mc_status mc_sync_here_state(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                             mc_here_state *out);

typedef struct mc_here_plan mc_here_plan;

/* What removing uid (an album or artist) from this device would do, for the confirmation:
 * see mc_here_plan_files() and _kept(). MC_ERR_LOCKED unless its state is MC_HERE_HELD.
 * Free with mc_here_plan_free(). */
mc_status mc_sync_here_plan_new(mc_sync *sync, mc_profile_item_kind kind, const char *uid,
                                mc_here_plan **out);
void mc_here_plan_free(mc_here_plan *plan);
/* The files it removes from this device now. */
int mc_here_plan_files(const mc_here_plan *plan);
/* The originals imported here it keeps until an archive holds verified copies; they go
 * after a later session with an archive. */
int mc_here_plan_kept(const mc_here_plan *plan);
/* Whether it removes a file of track_uid, e.g. so a frontend can stop playing it first (on
 * Windows an open file can't be removed). */
int mc_here_plan_has(const mc_here_plan *plan, const char *track_uid);

typedef struct mc_here_result {
  int files_removed; /* removed, or moved to the trash (originals imported here) */
  int files_kept;    /* imported here and waiting for an archive, or couldn't be removed */
} mc_here_result;

/* Removes what plan names from this device: edits this device's profile, then removes the
 * files, as the library and profile are now. out may be NULL. MC_OK even if some files
 * stayed (files_kept); an error if the profile couldn't be written (then no file is
 * touched), MC_ERR_LOCKED if the state is no longer MC_HERE_HELD. Blocks while a session
 * fetches files. Members hear of the edit in a few seconds. */
mc_status mc_sync_remove_here(mc_sync *sync, const mc_here_plan *plan, mc_here_result *out);

/* Undoes a removal: drops uid's exclusion from this device's profile, and in a selection
 * adds it (an album or artist row). Its files arrive at the next sync. MC_ERR_LOCKED unless
 * its state is MC_HERE_REMOVED. Confirms the profile again as mc_sync_remove_here() does. */
mc_status mc_sync_restore_here(mc_sync *sync, mc_profile_item_kind kind, const char *uid);

#ifdef __cplusplus
}
#endif

#endif
