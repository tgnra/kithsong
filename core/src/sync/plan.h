/* What a device should hold: its wanted set, worked out from its sync profile (see
 * "Profiles and what a device holds" in docs/sync-protocol.md). Internal to core.
 *
 * plan.c computes it; transfer.c (the file phase) and reconcile use it. Both databases are
 * read, never written: sdb for the profile, lib for tracks, albums, playlists and files. */
#ifndef MC_SYNC_PLAN_H
#define MC_SYNC_PLAN_H

#include <stdint.h>

#include "musiccore/library.h"
#include "musiccore/transcode.h"
#include "sync/sync_db.h"

typedef enum mc_plan_status {
  MC_PLAN_OK,
  MC_PLAN_OVER_BUDGET, /* even 64 kbps doesn't fit: nothing should sync */
  MC_PLAN_NO_PROFILE   /* the device has no profiles row yet */
} mc_plan_status;

/* One wanted track, in the format the device should hold it in. */
typedef struct mc_plan_track {
  char track_uid[MC_UID_STRLEN];
  mc_transcode_opts format; /* MC_FORMAT_ORIGINAL, or Opus at a bitrate */
  int64_t est_bytes;        /* exact for originals, estimated for Opus */
} mc_plan_track;

typedef struct mc_plan {
  mc_plan_status status;
  int archive;        /* the device is an archive: everything, originals, never prune */
  int kbps;           /* the chosen Opus bitrate, 0 for originals */
  int64_t total_bytes;
  int64_t shortfall_bytes; /* MC_PLAN_OVER_BUDGET: how much too big at 64 kbps */
  mc_plan_track *tracks;   /* sorted by track_uid */
  int count;
} mc_plan;

/* Works out device_uid's wanted set. free_bytes is that device's free space (from its
 * capabilities row, or measured locally for this device); budget mode uses
 * min(budget_bytes, free_bytes + bytes it already holds of the set). Free with
 * mc_plan_free(). Selections naming uids the library doesn't have are skipped. */
mc_status mc_plan_compute(mc_sync_db *sdb, mc_library *lib, const char *device_uid,
                          int64_t free_bytes, mc_plan **out);
void mc_plan_free(mc_plan *plan);

/* The entry for track_uid, or NULL if the track isn't wanted. Binary search. */
const mc_plan_track *mc_plan_find(const mc_plan *plan, const char *track_uid);

#endif
