/* Radio: tracks of the library that sound like a seed track, chosen from what the audio
 * analysis found (musiccore/analysis.h). Nothing leaves the device. It comes as a mix, a list
 * of a fixed length, or a station, which goes on for as long as it's asked.
 *
 * Each pick is scored by how close it sounds to the seed and to the track before it, so the
 * mix holds its character and still moves smoothly. One of the best few is drawn at random,
 * so two mixes from one seed differ. A mix keeps artists apart and albums from repeating
 * while it can, leaves out continuous mixes, very short tracks and very long ones, and plays
 * down what was played lately. Only tracks with a file on this device and a result are
 * considered.
 *
 * To play a station, see mc_player_play_radio() in musiccore/playback.h. */
#ifndef MUSICCORE_RADIO_H
#define MUSICCORE_RADIO_H

#include <stdint.h>

#include "musiccore/analysis.h"
#include "musiccore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_radio_options {
  const char *seed_track; /* the track to start from; NULL: one drawn at random */
  int count;              /* tracks wanted, the seed included; a station doesn't look */
  uint32_t random;        /* the same value, library and time give the same mix */
  int64_t now_ms;         /* what "lately" is measured from; 0: now, at each pick of a station */
  /* Longer tracks aren't candidates, nor seeds: they stop the flow, and much of what's that
   * long isn't a song. 0: 8 minutes. Negative: no limit. A track of unknown length stays in. */
  int64_t max_track_ms;
} mc_radio_options;

typedef struct mc_radio_pick {
  char track_uid[MC_UID_STRLEN];
  double distance;  /* from the seed, as mc_analysis_distance(): 0 for the seed itself */
  const char *path; /* a station's: the file, absolute, while it's open. NULL in a mix */
} mc_radio_pick;

/* Fills out, room for opts->count, in playing order, seed first; *n receives how many, fewer
 * if the library runs out. MC_ERR_NOT_FOUND if the seed can't be used (no result, no file
 * here, too short or too long, or part of a continuous mix), or with no seed if no track can. */
mc_status mc_radio_make(mc_library *lib, const mc_radio_options *opts, mc_radio_pick *out, int *n);

/* Whether opts->seed_track can seed a mix or a station: MC_OK, or MC_ERR_NOT_FOUND as above.
 * It reads that one track, so a UI thread can ask, where opening a station would keep it
 * waiting. */
mc_status mc_radio_seed_check(mc_library *lib, const mc_radio_options *opts);

/* A station: a mix with no end. The candidates are read once, on opening. After that a station
 * never touches the library, which needn't stay open, and it can be asked from any thread, one
 * at a time. So it knows nothing of what changes later: it may pick a track that has since
 * gone, and it leaves out one added since. Whoever plays it skips what won't open.
 *
 * It goes through every candidate once, the nearest in sound first, so what's left drifts
 * further from the seed the longer it runs. With none left it starts another round, in which
 * everything may come back but the last half of the pool's worth of picks (64 at most), and it
 * plays down what it picked itself as it does what was played lately. An album stays out for
 * 20 picks rather than for good. With one track in the pool, that track comes round for ever. */
typedef struct mc_radio_station mc_radio_station;

/* Reads the candidates, which can take a second or more on a big library, so not on a UI
 * thread. cancel, which may be NULL, is asked as it goes: MC_ERR_IN_USE if it gave up.
 * MC_ERR_NOT_FOUND as mc_radio_make(). */
mc_status mc_radio_station_open(mc_library *lib, const mc_radio_options *opts, mc_analysis_cancel_fn cancel,
                                void *user, mc_radio_station **out);
/* Fills out with the next count picks, always that many; the very first is the seed. Every
 * candidate is compared for each one, so on a big library this too is for another thread. */
mc_status mc_radio_station_next(mc_radio_station *station, mc_radio_pick *out, int count);
void mc_radio_station_close(mc_radio_station *station);

#ifdef __cplusplus
}
#endif

#endif
