/* The kinds of music a library holds, worked out from the audio analysis alone: what a library's
 * stations are (kithcore/radio_stations.h), and what keeps each one to its own music
 * (kithcore/radio.h). "Radio stations" in docs/schema.md has the reasoning.
 *
 * Nothing is stored. The answer depends only on the results, so working it out again gives the
 * same groups, on any day and however much is played. It's quick next to reading the results. */
#ifndef KITHCORE_RADIO_GROUPS_H
#define KITHCORE_RADIO_GROUPS_H

#include "kithcore/analysis.h"
#include "kithcore/radio_stations.h"

/* On the library's scales, about as far as a track's nearest few are in a varied library: in a real
 * one of 1,026 tracks the third nearest was 0.45 away at the median and the eighth 0.48. */
#define MC_RADIO_NEARBY 0.52

typedef struct mc_radio_groups {
  int count;               /* how many kinds; 0 with nothing to divide */
  mc_analysis_stats stats; /* the library's scales, which all of this goes by */
  int centre[MC_RADIO_STATIONS_MAX];    /* the most central track of each, by index */
  mc_analysis centres[MC_RADIO_STATIONS_MAX];
  double reach[MC_RADIO_STATIONS_MAX];  /* how far from its centre a track is still typical */
  int tracks[MC_RADIO_STATIONS_MAX];    /* how many tracks each has */
  double silhouette[MC_RADIO_STATIONS_MAX + 1]; /* how clearly each count divided it, for tuning */
} mc_radio_groups;

/* Divides count results, each of them a track that can be in a mix. hashes, parallel to them,
 * must not change from day to day: they choose what's compared. stats are the results' own. */
mc_status mc_radio_groups_find(const mc_analysis *results, const unsigned *hashes, int count,
                               const mc_analysis_stats *stats, mc_radio_groups *out);

/* Reads the library's results and divides them, so not for a UI thread. Too few to have stations
 * leaves out->count 0. cancel may be NULL: MC_ERR_IN_USE if it gave up. It's in stations.c, with
 * what reads the results. */
mc_status mc_radio_groups_load(mc_library *lib, mc_analysis_cancel_fn cancel, void *user, mc_radio_groups *out);

/* The group a result belongs to: the nearest centre, so each track has exactly one, the first of
 * two as near. It needn't be one of the results the groups came from. -1 if there are none. */
int mc_radio_group_of(const mc_radio_groups *groups, const mc_analysis *result);

/* Whether a result of that group is plainly of it, and so fit to start its station or to name it:
 * not a track that only just came down on this side, nor one out on its own. */
int mc_radio_group_typical(const mc_radio_groups *groups, const mc_analysis *result, int group);

#endif
