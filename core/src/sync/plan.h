/* What a device should hold: its wanted set, worked out from its sync profile, less its
 * exclusions (see
 * "Profiles and what a device holds" in docs/sync-protocol.md). Internal to core.
 *
 * plan.c computes it; transfer.c (the file phase) and reconcile use it. Both databases are
 * read, never written: sdb for the profile, lib for tracks, albums, playlists and files. */
#ifndef MC_SYNC_PLAN_H
#define MC_SYNC_PLAN_H

#include <stdint.h>

#include "musiccore/library.h"
#include "musiccore/sync.h"
#include "musiccore/transcode.h"
#include "sync/sync_db.h"

typedef enum mc_plan_status {
  MC_PLAN_OK,
  MC_PLAN_OVER_BUDGET, /* even 64 kbps doesn't fit: nothing should sync */
  MC_PLAN_NO_PROFILE   /* the device has no profiles row yet: nothing syncs until it has */
} mc_plan_status;

/* One wanted track, in the format the device should hold it in. */
typedef struct mc_plan_track {
  char track_uid[MC_UID_STRLEN];
  mc_transcode_opts format; /* MC_FORMAT_ORIGINAL, or Opus at a bitrate */
  int64_t est_bytes;        /* exact for originals known here, else estimated */
  int duration_ms;
} mc_plan_track;

/* The ladder budget mode walks: originals, then each of mc_opus_bitrates, best first. */
#define MC_PLAN_STEPS (1 + MC_OPUS_BITRATE_COUNT)

typedef struct mc_plan {
  mc_plan_status status;
  int archive;        /* the device is an archive: everything, originals, never prune */
  int kbps;           /* the chosen Opus bitrate, 0 for originals */
  int wanted;         /* tracks in the wanted set, whatever the status */
  int64_t total_bytes; /* the set at kbps; MC_PLAN_OVER_BUDGET: at 64 kbps */
  int64_t shortfall_bytes; /* MC_PLAN_OVER_BUDGET: how much too big at 64 kbps */
  int64_t limit_bytes;     /* with a budget, what the set had to fit in; else -1 */
  /* With MC_PLAN_SIZES: the set's size at each step of the ladder (index 0 originals, then
   * mc_opus_bitrates' order), whatever the profile's own format. */
  int64_t step_bytes[MC_PLAN_STEPS];
  mc_plan_track *tracks;   /* sorted by track_uid; empty unless MC_PLAN_OK */
  int count;
} mc_plan;

/* Works out device_uid's wanted set from its stored profile. free_bytes is that device's
 * free space (from its capabilities row, or measured locally for this device), negative if
 * unknown; budget mode uses min(budget_bytes, free_bytes + bytes it already holds of the
 * set). files is local, so the bytes held are only known for this device; for another one
 * that term is 0. Originals' sizes come from this device's files too (an original is the
 * same file everywhere), and are estimated at MC_PLAN_ORIGINAL_KBPS where this device has
 * none. The device is an archive if sync/archive.h says so. Free with mc_plan_free().
 * Selections naming uids the library doesn't have are skipped. */
mc_status mc_plan_compute(mc_sync_db *sdb, mc_library *lib, const char *device_uid,
                          int64_t free_bytes, mc_plan **out);

/* Flags for mc_plan_compute_profile(). */
#define MC_PLAN_SIZES 1 /* fill step_bytes */

/* As mc_plan_compute(), for a profile that isn't stored (the profile editor's estimate):
 * profile's mode, format, budget and items stand in for the stored ones, which need not
 * exist, so the status is never MC_PLAN_NO_PROFILE. The device is an archive if it is one
 * or profile->archive is set. A NULL profile reads the stored one, as mc_plan_compute(). */
mc_status mc_plan_compute_profile(mc_sync_db *sdb, mc_library *lib, const char *device_uid,
                                  const mc_sync_profile *profile, int64_t free_bytes, int flags,
                                  mc_plan **out);
void mc_plan_free(mc_plan *plan);

/* profile_items.kind for kind ("album", ...); NULL for a value out of range. */
const char *mc_profile_kind_str(mc_profile_item_kind kind);

/* profile_items.kind of an exclusion (musiccore/sync_here.h): the album's or artist's tracks
 * are not wanted, whatever else selects them (but on an archive). Not an mc_profile_item_kind:
 * editors neither see nor drop them. */
#define MC_PLAN_EXCLUDE_ALBUM "exclude_album"
#define MC_PLAN_EXCLUDE_ARTIST "exclude_artist"
/* The kind an exclusion kind takes out ("album" or "artist"), or NULL if kind isn't one. */
const char *mc_plan_excluded_kind(const char *kind);

/* The bitrate an original of unknown size is estimated at, roughly a FLAC of a CD. */
#define MC_PLAN_ORIGINAL_KBPS 1000

/* The entry for track_uid, or NULL if the track isn't wanted. Binary search. */
const mc_plan_track *mc_plan_find(const mc_plan *plan, const char *track_uid);

/* --- Files and formats ---
 * A files row is one of three kinds: an original imported on this device (no source), an
 * original received from a peer (source_peer_uid only), or a transcode received from a peer
 * (source_file_uid too: the original's file uid on the device that encoded it). */

typedef enum mc_file_kind {
  MC_FILE_IMPORTED,
  MC_FILE_RECEIVED,
  MC_FILE_TRANSCODE
} mc_file_kind;

mc_file_kind mc_plan_file_kind(const char *source_file_uid, const char *source_peer_uid);

/* Whether a file of this format ("flac", "mp3", ...) is lossy. bitrate_kbps (0 if unknown)
 * is unused: every format imported says by its extension alone. */
int mc_plan_is_lossy(const char *format, int bitrate_kbps);

/* "A lossy original that is already smaller than the Opus estimate is sent as-is": whether
 * an original of this format and size stands in for Opus at kbps. */
int mc_plan_keep_lossy(const char *format, int bitrate_kbps, int64_t size_bytes, int duration_ms,
                       int kbps);

/* Whether a file the device holds is the track in the format t wants. */
int mc_plan_file_satisfies(const mc_plan_track *t, mc_file_kind kind, const char *format,
                           int bitrate_kbps, int64_t size_bytes);

#endif
