/* Tracks waiting for a device: what this device's profile wants and it doesn't hold in the
 * right form, that only members it can't reach now could give it (mc_sync_get_waiting() in
 * musiccore/sync.h, "Waiting for a device" in docs/sync-protocol.md). Internal to core.
 *
 * Nothing records which member holds which file (files is local), so who could give a track
 * is worked out from what is recorded:
 *  - each other current member's plan (plan.h), from its profile: a member whose plan has
 *    the track as an original holds the original; one whose plan has it as Opus at the very
 *    bitrate this device wants holds a copy it can send as it is. A transcode at another
 *    bitrate is no source (a transcode is never encoded again; see "Lossy copies"). A member
 *    that never confirmed a profile (just paired) holds nothing yet, so it counts only as
 *    below;
 *  - the member that imported the track's current original: the origin of the newest
 *    tracks.file_rev change in the library changelog (a better file swapped in), else of the
 *    track's first change, mapped to a member through devices.node_uid. It keeps that
 *    original until an archive holds a verified copy, and the archive is a holder then.
 * Neither counts for a track the member said it hasn't in its last session with this device
 * (peer_lacks, from HAVE_RESP): one short of its own profile isn't named for what it hasn't
 * fetched yet. It may have the track since, but then it's named after the next session.
 * A track counts as waiting when it has a holder other than this device and none of them is
 * reachable. A track no member is known to hold is left out: it isn't waiting for anyone. */
#ifndef MC_SYNC_WAITING_H
#define MC_SYNC_WAITING_H

#include "musiccore/library.h"
#include "musiccore/sync.h"
#include "sync/sync_db.h"

/* Whether this device can reach device_uid now, in the sense the caller decides (sync.c:
 * the last session attempt with it). */
typedef int (*mc_waiting_reach_fn)(void *user, const char *device_uid);

/* Works out the waiting tracks from sdb and lib, asking reachable() about each other member.
 * *out is empty (never NULL on MC_OK) outside a group, when the open library isn't the
 * group's, while this device's profile isn't confirmed or a merged library is still to come
 * in (nothing is fetched then), and when this device's plan is over budget. Free with
 * mc_sync_waiting_free(). */
mc_status mc_waiting_compute(mc_sync_db *sdb, mc_library *lib, mc_waiting_reach_fn reachable,
                             void *user, mc_sync_waiting **out);

/* Whether a and b say the same (either may be NULL, which is the same as empty). */
int mc_waiting_equal(const mc_sync_waiting *a, const mc_sync_waiting *b);

/* A deep copy of w (NULL: an empty one), or NULL if out of memory. */
mc_sync_waiting *mc_waiting_copy(const mc_sync_waiting *w);

#endif
