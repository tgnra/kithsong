/* What the checks left for the user: mc_sync_get_problems() in musiccore/sync.h, and "Verification"
 * in docs/sync-protocol.md. Internal to core.
 *
 * Removing a lost track deletes it on every device, so what counts as lost is deliberately
 * narrow. A track is lost only when all of these hold, and any doubt keeps it off the list:
 *  - this device fetches files (mc_waiting_fetching()) and is an archive, so it wants them all;
 *  - it holds nothing of the track: no file that satisfies it, and no other file on disk that
 *    a check hasn't found corrupt, so a lossy copy or a file that merely changed saves it;
 *  - every other current member has a peer_lacks row for it, from a session in the last
 *    MC_SYNC_LOST_ASKED_DAYS, that says it has no copy of any kind.
 * A row only exists if the member was asked while this device was missing the track, so a
 * member that never answered, or an offline one, keeps the track safe. */
#ifndef MC_SYNC_PROBLEMS_H
#define MC_SYNC_PROBLEMS_H

#include "musiccore/library.h"
#include "musiccore/sync.h"
#include "sync/sync_db.h"

/* Works them out from sdb and lib; *out is never NULL on MC_OK. Reads every track on an archive
 * (its plan), so not for a UI thread. Free with mc_sync_problems_free(). */
mc_status mc_problems_compute(mc_sync_db *sdb, mc_library *lib, mc_sync_problems **out);

/* Whether p has track_uid as lost. */
int mc_problems_is_lost(const mc_sync_problems *p, const char *track_uid);

/* Whether a and b say the same (either may be NULL, which is the same as empty). */
int mc_problems_equal(const mc_sync_problems *a, const mc_sync_problems *b);

/* A deep copy of p (NULL: an empty one), or NULL if out of memory. */
mc_sync_problems *mc_problems_copy(const mc_sync_problems *p);

#endif
