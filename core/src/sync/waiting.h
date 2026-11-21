/* Tracks waiting for a device (mc_sync_get_waiting() in kithcore/sync.h, "Waiting for a
 * device" in docs/sync-protocol.md). Internal to core.
 *
 * The files table is local, so holders are inferred:
 *  - each other current member's plan (plan.h): the track as an original, or as Opus at
 *    exactly the bitrate wanted (a transcode is never encoded again). A member with no
 *    sync_status (no file phase yet) holds nothing yet;
 *  - the member that imported the track's current original: the origin of the newest
 *    tracks.file_rev change, else of the track's first change, mapped through
 *    devices.node_uid. It keeps the original until an archive holds a verified copy, and the
 *    archive is a holder then.
 * Neither counts if the member said in its last session that it lacks the track (peer_lacks).
 * An original is a source of an Opus copy only where it may be converted (capabilities say
 * so, or say nothing). A track is waiting when it has a source other than this device and
 * none is reachable. */
#ifndef MC_SYNC_WAITING_H
#define MC_SYNC_WAITING_H

#include "kithcore/library.h"
#include "kithcore/sync.h"
#include "sync/issues.h"
#include "sync/sync_db.h"

/* Whether this device can reach device_uid now, in the sense the caller decides (sync.c:
 * the last session attempt with it). */
typedef int (*mc_waiting_reach_fn)(void *user, const char *device_uid);

/* Works out the waiting tracks from sdb and lib, asking reachable() about each other member,
 * and with them the outlook (may be NULL; see mc_sync_get_outlook()). *out is empty (never
 * NULL on MC_OK) outside a group, when the open library isn't the group's, while this
 * device's profile isn't chosen yet (nothing is fetched then), and when this device's plan
 * is over budget. Free with mc_sync_waiting_free(). extra (may be NULL): the wanted tracks
 * that aren't waiting for a reason sync can't fix by itself (issues.h); free with
 * mc_waiting_extra_free(). */
mc_status mc_waiting_compute(mc_sync_db *sdb, mc_library *lib, mc_waiting_reach_fn reachable,
                             void *user, mc_sync_waiting **out, mc_sync_outlook *outlook,
                             mc_waiting_extra *extra);

/* Whether this device fetches files now: in a group, the group's library and its profile
 * chosen. */
int mc_waiting_fetching(mc_sync_db *sdb, mc_library *lib);
/* Whether this device is in a group and has the group's library open. */
int mc_waiting_library_is_groups(mc_sync_db *sdb, mc_library *lib);

/* Whether a and b say the same (either may be NULL, which is the same as empty). */
int mc_waiting_equal(const mc_sync_waiting *a, const mc_sync_waiting *b);

/* A deep copy of w (NULL: an empty one), or NULL if out of memory. */
mc_sync_waiting *mc_waiting_copy(const mc_sync_waiting *w);

#endif
