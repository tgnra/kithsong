/* The radio stations a library offers, for the Home page's Radio row: one for each kind of music
 * the audio analysis finds in it, so two kinds make two stations. Worked out from the analysis
 * alone, on the device. How: "Radio stations" in docs/schema.md.
 *
 * A station keeps to its own kind (musiccore/radio.h). Every track belongs to one kind, the one
 * it sounds nearest, so stations don't overlap. A library that doesn't divide clearly gets one
 * station, and one with too little analysed gets none.
 *
 * Nothing is stored. What a library divides into depends only on the results, so it holds however
 * often it's asked and however much is played, until results arrive or tracks come and go. A
 * station's seed, which names it, stays all day and may move to another track of its kind the next
 * day. Its start track is drawn afresh each time the stations are listed. */
#ifndef KITHCORE_RADIO_STATIONS_H
#define KITHCORE_RADIO_STATIONS_H

#include <stdint.h>

#include "kithcore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MC_RADIO_STATION_MIN_TRACKS = 25, /* eligible tracks: fewer gets no stations */
  MC_RADIO_STATIONS_MAX = 8
};

typedef struct mc_radio_station_info {
  char seed_track[MC_UID_STRLEN];  /* for mc_radio_options.seed_track: what the station is */
  char start_track[MC_UID_STRLEN]; /* for mc_radio_options.start_track: where it starts this time */
  char artist_uid[MC_UID_STRLEN]; /* the seed's first artist; "" if unknown */
  char *name;       /* "Zedd Radio"; after the seed's title if it has no artist */
  char *sub;        /* the line beneath, which tells it from any station of the same name */
  char *seed_title; /* the seed track's */
  char *cover_dir;  /* the folder holding the seed's album's cover; NULL if there's none here */
} mc_radio_station_info;

typedef struct mc_radio_station_list {
  mc_radio_station_info *items; /* the most played first */
  int count;
} mc_radio_station_list;

/* The stations as of now_ms (0: now), which only decides the day. *out may hold none; free it
 * with mc_radio_station_list_free(). Reads every result, so it's for a background thread. Each
 * call draws new start tracks, so a frontend lists again after starting a station. */
mc_status mc_radio_stations_list(mc_library *lib, int64_t now_ms, mc_radio_station_list **out);
void mc_radio_station_list_free(mc_radio_station_list *list);

#ifdef __cplusplus
}
#endif

#endif
