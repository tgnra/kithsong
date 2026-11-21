/* Removing an album or artist from this device, and syncing it here again: the menus'
 * "Remove from this PC" and "Sync to this PC".
 *
 * Removing adds an exclusion to this device's sync profile (profile_items of kind
 * 'exclude_album' or 'exclude_artist', which win over any selection, "Sync everything"
 * included, except a ticked playlist) and removes the item's files here at once. The rows
 * stay, so the item is still listed, and other devices keep their copies. Exclusions sync
 * with the profile; mc_sync_get_profile() lists them apart (mc_sync_profile.removed) and
 * mc_sync_set_profile() keeps them. Ticking the item again under What to sync, or
 * mc_sync_restore_here(), drops it; Select all leaves it. See "Profiles and what a device
 * holds" in docs/sync-protocol.md.
 *
 * Only for a device in a group that isn't an archive: with no group the music lives nowhere
 * else, so deleting it is the only honest option.
 *
 * Originals imported here stay until an archive reports holding verified copies (as
 * reconcile requires); then they go to the trash. Files sync brought are removed outright.
 * A file that can't be removed keeps its files row, for reconcile to remove later. Single
 * tracks get an exclusion ('exclude_track') only when taken back out of the recycle bin
 * (below). */
#ifndef KITHCORE_SYNC_HERE_H
#define KITHCORE_SYNC_HERE_H

#include "kithcore/kithcore.h"
#include "kithcore/sync.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_here_state {
  MC_HERE_UNAVAILABLE, /* offer none: not in a group, not the group's library, or an archive */
  MC_HERE_HELD,        /* offer Remove from this device */
  MC_HERE_REMOVED,     /* removed from this device, or (an album) with its artist: offer Sync
                        * to this device */
  MC_HERE_NOT_INCLUDED /* this device's selection doesn't have it: offer Add to this device */
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
/* The tracks of it with a file here that stay, because a ticked playlist has them. */
int mc_here_plan_staying(const mc_here_plan *plan);
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

/* Undoes a removal, or adds to a selection: drops uid's exclusion from this device's profile,
 * and in a selection adds it (an album or artist row). An album removed with its artist comes
 * back alone, the artist staying removed. Its files arrive at the next sync. MC_ERR_LOCKED
 * unless its state is MC_HERE_REMOVED or MC_HERE_NOT_INCLUDED. */
mc_status mc_sync_restore_here(mc_sync *sync, mc_profile_item_kind kind, const char *uid);

/* --- The recycle bin (kithcore/delete.h) ---
 * On a device with no trash, a delete from the library can be taken back until the next sync
 * sends it. Taken back, it becomes a removal from this device: the files stay gone here, and
 * the album or track stays in the library and on the other devices. */

/* Whether deletes here go to the recycle bin (mc_delete_run_to_bin()): as for Remove from
 * this device, the device keeps part of the library (it's in a group, the group's library is
 * open, and it isn't an archive), so there's something to take a delete back to. Cheap. */
int mc_sync_bin_offered(mc_sync *sync);

/* mc_delete_bin_count() of sync's library: while it isn't 0, this device starts no sync by
 * itself and refuses those other devices start, so a frontend says that its user must press
 * Sync, which makes the deletes for good. */
int mc_sync_bin_count(mc_sync *sync);

/* Takes entry id (mc_bin_entry.id) back out of the recycle bin: its delete never reaches
 * another device, its rows are back in the library, and this device's profile excludes what
 * it deleted (exclude_album for an album's delete, exclude_track for a track's; the latter is
 * an exclusion kind of its own, which only this does), so its files don't come back. Entries
 * whose deletes took rows this one needs (a later delete of the same album's last track took
 * the album) are taken back with it; *entries (may be NULL) is how many were. In a selection
 * the album's own row goes too, as with mc_sync_remove_here(). MC_ERR_NOT_FOUND if the entry is gone (a sync sent it, or it was taken
 * back already), MC_ERR_LOCKED while a sync is sending the library's rows (it then empties
 * the bin) or if mc_sync_bin_offered() says no. The library is written before the profile:
 * should the profile write fail, the entry is back in the library and syncs here again. */
mc_status mc_sync_bin_restore(mc_sync *sync, int64_t id, int *entries);

#ifdef __cplusplus
}
#endif

#endif
