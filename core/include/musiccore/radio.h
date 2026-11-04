/* A radio mix: tracks of the library that sound like a seed track, chosen from what the
 * audio analysis found (musiccore/analysis.h). Nothing leaves the device.
 *
 * Each pick is scored by how close it sounds to the seed and to the track before it, so the
 * mix holds its character and still moves smoothly. One of the best few is drawn at random,
 * so two mixes from one seed differ. A mix keeps artists apart and albums from repeating
 * while it can, leaves out continuous mixes and very short tracks, and plays down what was
 * played lately. Only tracks with a file on this device and a result are considered. */
#ifndef MUSICCORE_RADIO_H
#define MUSICCORE_RADIO_H

#include <stdint.h>

#include "musiccore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_radio_options {
  const char *seed_track; /* the track to start from; NULL: one drawn at random */
  int count;              /* tracks wanted, the seed included */
  uint32_t random;        /* the same value, library and time give the same mix */
  int64_t now_ms;         /* what "lately" is measured from; 0: now */
} mc_radio_options;

typedef struct mc_radio_pick {
  char track_uid[MC_UID_STRLEN];
  double distance; /* from the seed, as mc_analysis_distance(): 0 for the seed itself */
} mc_radio_pick;

/* Fills out, room for opts->count, in playing order, seed first; *n receives how many, fewer
 * if the library runs out. MC_ERR_NOT_FOUND if the seed can't be used (no result, no file
 * here, or part of a continuous mix), or with no seed if no track can. */
mc_status mc_radio_make(mc_library *lib, const mc_radio_options *opts, mc_radio_pick *out, int *n);

#ifdef __cplusplus
}
#endif

#endif
