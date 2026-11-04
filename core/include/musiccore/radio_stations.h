/* The radio stations a library offers: a few tracks that sound unlike each other, each one the
 * seed of a mix (musiccore/radio.h), for the Home page's Radio row. Worked out from the audio
 * analysis alone, on the device. How the seeds are chosen: "Radio stations" in docs/schema.md.
 *
 * How many there are depends on the library. mc_radio_station_limit() says what its size allows;
 * a library whose tracks all sound alike gets fewer than that, as there are only ever as many
 * stations as sounds to tell apart. One with too little analysed gets none.
 *
 * Nothing is stored. The same library gives the same stations all day, however often it's asked
 * and however much is played in the meantime, and the next day they may move to other tracks.
 * Results arriving or tracks coming and going can change them sooner. */
#ifndef MUSICCORE_RADIO_STATIONS_H
#define MUSICCORE_RADIO_STATIONS_H

#include <stdint.h>

#include "musiccore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
  MC_RADIO_STATION_MIN_TRACKS = 25, /* eligible tracks: fewer gets no stations */
  MC_RADIO_STATIONS_MAX = 8
};

typedef struct mc_radio_station {
  char seed_track[MC_UID_STRLEN]; /* for mc_radio_options.seed_track */
  char artist_uid[MC_UID_STRLEN]; /* the seed's first artist; "" if unknown */
  char *name;       /* "Zedd Radio"; after the seed's title if it has no artist */
  char *sub;        /* the line beneath, which tells it from any station of the same name */
  char *seed_title; /* the seed track's */
  char *cover_dir;  /* the folder holding the seed's album's cover; NULL if there's none here */
} mc_radio_station;

typedef struct mc_radio_station_list {
  mc_radio_station *items; /* the most played first */
  int count;
} mc_radio_station_list;

/* The most stations a library of that many eligible tracks may have: one from
 * MC_RADIO_STATION_MIN_TRACKS, and another each time that doubles, so MC_RADIO_STATIONS_MAX
 * from 3,200 tracks. */
int mc_radio_station_limit(int tracks);

/* The stations as of now_ms (0: now), which only decides the day. *out may hold none; free it
 * with mc_radio_station_list_free(). Reads every result, so it's for a background thread. */
mc_status mc_radio_stations_list(mc_library *lib, int64_t now_ms, mc_radio_station_list **out);
void mc_radio_station_list_free(mc_radio_station_list *list);

#ifdef __cplusplus
}
#endif

#endif
