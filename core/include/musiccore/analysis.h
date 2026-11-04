/* Local audio analysis: a fingerprint of how each track sounds, worked out on this device
 * from its audio alone, for telling which tracks sound alike, and its loudness.
 *
 * One decode of a file gives its timbre (the mean and variance of 13 MFCCs and of the spectral
 * centroid, from a mono mix at a fixed internal rate, so the file's own rate doesn't matter)
 * and its energy (integrated loudness, loudness range and true peak, per ITU-R BS.1770 and
 * EBU R 128, over the whole track). Nothing here changes playback.
 *
 * Results live in track_analysis and sync like any row, so a device that doesn't scan has them
 * too. How they are stored and how the distance is scaled: "Audio analysis" in docs/schema.md. */
#ifndef MUSICCORE_ANALYSIS_H
#define MUSICCORE_ANALYSIS_H

#include "musiccore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Which algorithm made a result. A change that alters results raises it: the scanner then does
 * every track again, and distances only compare results of the same version. */
#define MC_ANALYSIS_VERSION 1
#define MC_ANALYSIS_MFCCS 13
/* What mc_analysis_distance() compares; see "Audio analysis" in docs/schema.md. */
#define MC_ANALYSIS_DIMS 29

/* What a result was made from. A lower value is the better source. */
typedef enum mc_analysis_source {
  MC_ANALYSIS_ORIGINAL = 0,
  MC_ANALYSIS_LOSSY_COPY = 1 /* a transcode received from a peer */
} mc_analysis_source;

typedef struct mc_analysis {
  int version;
  mc_analysis_source source;
  /* In dB, from 40 mel bands; coefficient 0 is the level and the rest don't depend on it. Over
   * frames that aren't silent. */
  float mfcc_mean[MC_ANALYSIS_MFCCS];
  float mfcc_var[MC_ANALYSIS_MFCCS];
  float centroid_mean; /* Hz: brightness */
  float centroid_var;  /* Hz squared */
  /* NaN when undefined: a silent track has no loudness, and one under 3 s no range. */
  double lufs;      /* integrated loudness, LUFS */
  double lra;       /* loudness range, LU */
  double true_peak; /* dBTP, the highest over all channels */
  double seconds;   /* of audio analysed; 0 for a result read from the library */
} mc_analysis;

/* Called between pieces of a file: non-zero gives up. */
typedef int (*mc_analysis_cancel_fn)(void *user);

/* Decodes path once and analyses all of it; cancel may be NULL. MC_ERR_FORMAT if it's no audio
 * this build can decode, MC_ERR_IO if it can't be opened, MC_ERR_IN_USE if cancelled. */
mc_status mc_analysis_file(const char *path, mc_analysis_cancel_fn cancel, void *user,
                           mc_analysis *out);

/* A track's best result: of the file the track has now over one of a file since replaced, then
 * the better source, then the newest version. MC_ERR_NOT_FOUND if it has none. */
mc_status mc_analysis_get(mc_library *lib, const char *track_uid, mc_analysis *out);

/* --- Comparing --- */

/* How the features spread over a library: what a difference in each is measured against. */
typedef struct mc_analysis_stats {
  int count; /* results they were worked out from */
  float scale[MC_ANALYSIS_DIMS];
} mc_analysis_stats;

/* From count results of this version; others are left out. With few, the scales lean on fixed
 * ones, so three tracks aren't told apart by noise in their spread. */
void mc_analysis_stats_from(const mc_analysis *results, int count, mc_analysis_stats *out);
/* From the library's results, one per track. */
mc_status mc_analysis_stats_load(mc_library *lib, mc_analysis_stats *out);

/* How differently two tracks sound: 0 for the same features, about 1 for two tracks picked at
 * random from the library stats came from (NULL: the fixed scales). -1 if a and b aren't both
 * of this version's results, whose features can't be compared. */
double mc_analysis_distance(const mc_analysis *a, const mc_analysis *b,
                            const mc_analysis_stats *stats);

/* --- Scanning ---
 *
 * The scanner analyses the tracks of one library that have a file here and no result yet, one
 * at a time, on a low-priority thread of its own with a connection of its own. It has no state
 * but the results: stopped at any point, it goes on from there next time. It stands aside while
 * a sync session or an import runs. Android doesn't run one, for the battery: results reach it
 * by sync. */
typedef struct mc_analysis_scanner mc_analysis_scanner;

/* Starts scanning the library in dir, after a short wait so a starting app isn't held up. */
mc_status mc_analysis_scanner_start(const char *dir, mc_analysis_scanner **out);
/* Stops it and waits for its thread, which stops within a moment, whatever the file: a track
 * half done is done again next time. NULL is fine. Before the library's files move. */
void mc_analysis_scanner_stop(mc_analysis_scanner *s);

#ifdef __cplusplus
}
#endif

#endif
