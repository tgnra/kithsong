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
 * It keeps to the seed's kind of music: the library's groups are those its stations are made of
 * (musiccore/radio_stations.h), and only the seed's own group is drawn from, so a mix from a track
 * of one never strays into another. A library of one kind, or with too little analysed to say,
 * has the whole library to draw from.
 *
 * It also learns from skips, which it reads from the play log. A track skipped early on a
 * station didn't fit that station, and that's all it means: music someone owns isn't music they
 * dislike. So for a while that track and what sounds like it score a little worse, on that
 * station and those from seeds that sound like its seed, and nowhere else. A skip only ever
 * changes the order: no track is left out for it. "What skips teach a station" in docs/schema.md
 * has the rules. With no such skips, picks are what they'd be without any of this.
 *
 * To play a station, see mc_player_play_radio() in musiccore/playback.h. */
#ifndef MUSICCORE_RADIO_H
#define MUSICCORE_RADIO_H

#include <stdint.h>

#include "musiccore/analysis.h"
#include "musiccore/library.h"
#include "musiccore/play_log.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_radio_options {
  const char *seed_track; /* what the station is; NULL: a track drawn at random */
  int count;              /* tracks wanted, the seed included; a station doesn't look */
  uint32_t random;        /* the same value, library and time give the same mix */
  /* What "lately" is measured from; 0: now, at each pick of a station. Skips are faded from it
   * once, on opening, and a row after it doesn't count. */
  int64_t now_ms;
  /* Longer tracks aren't candidates, nor seeds: they stop the flow, and much of what's that
   * long isn't a song. 0: 8 minutes. Negative: no limit. A track of unknown length stays in. */
  int64_t max_track_ms;
  /* The first pick, and what the rest are measured from, for a station that starts somewhere
   * other than its seed; it has to be able to seed a mix itself. NULL: the seed. The seed still
   * decides the group, and it's the seed that skips are of, so what's learned stays with the
   * station wherever it starts. If the seed has gone from here, the start stands in for it. */
  const char *start_track;
} mc_radio_options;

typedef struct mc_radio_pick {
  char track_uid[MC_UID_STRLEN];
  double distance;  /* from the first pick, as mc_analysis_distance(): 0 for that track itself */
  const char *path; /* a station's: the file, absolute, while it's open. NULL in a mix */
} mc_radio_pick;

/* Fills out, room for opts->count, in playing order, first pick first; *n receives how many, fewer
 * if the group runs out. MC_ERR_NOT_FOUND if the seed can't be used (no result, no file
 * here, too short or too long, or part of a continuous mix), or with no seed if no track can. */
mc_status mc_radio_make(mc_library *lib, const mc_radio_options *opts, mc_radio_pick *out, int *n);

/* Whether opts->seed_track can seed a mix or a station: MC_OK, or MC_ERR_NOT_FOUND as above.
 * It reads that one track, so a UI thread can ask, where opening a station would keep it
 * waiting. */
mc_status mc_radio_seed_check(mc_library *lib, const mc_radio_options *opts);

/* A station: a mix with no end. The candidates are read once, on opening, as are the skips.
 * After that a station never touches the library, which needn't stay open, and it can be asked
 * from any thread, one at a time. So it knows nothing of what changes later, but for what it's
 * told with mc_radio_station_heard(): it may pick a track that has since gone, and it leaves
 * out one added since. Whoever plays it skips what won't open.
 *
 * It goes through every candidate of its group once, the nearest in sound first, so what's left drifts
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
/* Fills out with the next count picks, always that many; the very first is the seed, or
 * opts->start_track. Every
 * candidate is compared for each one, so on a big library this too is for another thread. */
mc_status mc_radio_station_next(mc_radio_station *station, mc_radio_pick *out, int count);
/* Tells a station how a play of it ended, so an early skip counts from the next pick and not
 * only the next time it's opened. Anything else is ignored: e can be any row, of this station
 * or not. A finished play doesn't offset a skip until then, which is rare enough in one sitting
 * that it isn't worth keeping each pairing for. One pass over the candidates, a good deal less
 * than a pick. The player does this for a station it plays. */
mc_status mc_radio_station_heard(mc_radio_station *station, const mc_play_event *e);
void mc_radio_station_close(mc_radio_station *station);

#ifdef __cplusplus
}
#endif

#endif
