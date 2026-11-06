/* Inside the analysis: what one decode feeds (loudness.c, timbre.c and rhythm.c behind it), the
 * feature BLOB, and what the scanner writes. Internal to core; white-box tests use it. */
#ifndef MC_ANALYSIS_INTERNAL_H
#define MC_ANALYSIS_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "kithcore/analysis.h"

/* What the timbre is worked out at, whatever the file's rate. 11 kHz of audio holds what tells
 * timbres apart, at half the frames of 44.1 kHz. */
#define MC_ANALYSIS_RATE 22050
#define MC_ANALYSIS_FRAME 1024
#define MC_ANALYSIS_HOP 512
#define MC_ANALYSIS_MELS 40

/* Layout byte, MFCC count, two reserved bytes, then floats; see docs/schema.md. */
#define MC_ANALYSIS_LAYOUT 2
#define MC_ANALYSIS_BLOB_SIZE \
  (4 + 4 * (3 * MC_ANALYSIS_MFCCS + 2 + MC_ANALYSIS_LAGS + 3 + MC_ANALYSIS_BANDS + MC_ANALYSIS_FLATNESS))

/* --- Loudness: at the file's own rate and channels --- */

typedef struct mc_loudness mc_loudness;

mc_loudness *mc_loudness_new(unsigned rate, unsigned channels);
void mc_loudness_free(mc_loudness *l);
void mc_loudness_feed(mc_loudness *l, const float *interleaved, size_t frames);
/* NaN for what's undefined, as in mc_analysis. */
void mc_loudness_finish(mc_loudness *l, double *lufs, double *lra, double *true_peak);

/* --- Timbre: mono, resampled to MC_ANALYSIS_RATE --- */

typedef struct mc_timbre mc_timbre;

mc_timbre *mc_timbre_new(unsigned rate);
void mc_timbre_free(mc_timbre *t);
void mc_timbre_feed(mc_timbre *t, const float *mono, size_t frames);
/* Fills out's fields from the frames: all but the loudness. */
void mc_timbre_finish(mc_timbre *t, mc_analysis *out);

/* The real FFT, for tests: power[0..n/2] of n real samples (n a power of two, at most
 * MC_ANALYSIS_FRAME). */
void mc_analysis_power_spectrum(const float *x, int n, float *power);

/* --- Rhythm: the onset strength of each frame, from timbre.c --- */

typedef struct mc_rhythm mc_rhythm;

mc_rhythm *mc_rhythm_new(void);
void mc_rhythm_free(mc_rhythm *r);
/* A frame that sounded: how much rose since the last, in dB. */
void mc_rhythm_onset(mc_rhythm *r, float strength);
/* A frame left out as silent. */
void mc_rhythm_silence(mc_rhythm *r);
/* Fills out's rhythm, pulse, onset_rate and bpm, which it expects zeroed. */
void mc_rhythm_finish(mc_rhythm *r, mc_analysis *out);

/* --- Both, from interleaved samples --- */

typedef struct mc_analyzer mc_analyzer;

/* NULL if out of memory, or for a rate or channel count no audio has. */
mc_analyzer *mc_analyzer_new(unsigned rate, unsigned channels);
void mc_analyzer_free(mc_analyzer *a);
/* Samples that aren't finite, as a float file can hold, count as silence. */
void mc_analyzer_feed(mc_analyzer *a, const float *interleaved, size_t frames);
void mc_analyzer_finish(mc_analyzer *a, mc_analysis *out);

/* --- Storing --- */

void mc_analysis_pack(const mc_analysis *a, unsigned char out[MC_ANALYSIS_BLOB_SIZE]);
/* Reads a BLOB into a's features; 0 if it's of a layout this build doesn't know. */
int mc_analysis_unpack(const void *blob, int len, mc_analysis *a);

/* Writes a track's result of source for file_rev (NULL for none), in a write transaction of its
 * own. A row per track and source, so a result from a transcode never replaces one from an
 * original, here or by sync. Nothing is written if the track is gone or its file was replaced
 * meanwhile, and that's MC_OK. */
mc_status mc_analysis_store(mc_library *lib, const char *track_uid, const char *file_rev,
                            mc_analysis_source source, const mc_analysis *a);

#endif
