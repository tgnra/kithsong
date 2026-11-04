/* What the Home page's rows hold, for every desktop's page to show. Everything that decides what
 * goes in a row is here, in one pick_*() function per row in home_picks.c, so the guesses from
 * track_plays can give way to the play event log without touching the pages.
 */
#ifndef MUSICCORE_HOME_PICKS_H
#define MUSICCORE_HOME_PICKS_H

#include "musiccore/i18n.h"
#include "musiccore/library.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_home_row_id {
  MC_HOME_ROW_RADIO,              /* mc_radio_stations_list(): none until enough is analysed */
  MC_HOME_ROW_RECENTS,            /* mc_library_list_recents(), as the sidebar's, stations too */
  MC_HOME_ROW_PLAYED_LATELY,      /* albums and playlists on repeat lately */
  MC_HOME_ROW_NOT_PLAYED_LATELY,  /* played before, but not lately */
  MC_HOME_ROW_RANDOM,             /* neither of those two, new each time the app starts */
  MC_HOME_ROW_COUNT
} mc_home_row_id;

enum { MC_HOME_ROW_MAX = 24 }; /* tiles in a row */

typedef struct mc_home_pick {
  mc_recent_kind kind;
  int station; /* a radio station: kind is MC_RECENT_ARTIST, and uid its seed's artist's, "" if
                * that's unknown. Several can share one, so seed_uid is what tells them apart */
  int smart;   /* a smart playlist or group */
  char uid[MC_UID_STRLEN];
  char seed_uid[MC_UID_STRLEN]; /* a station's seed track, to start its mix from; "" otherwise */
  char *title;
  char *sub;       /* the line beneath it */
  char *cover_dir; /* the folder holding its cover; NULL for none, and for playlists and
                    * groups, whose pictures are found by uid */
  char *seed_title; /* a station's seed track's title; NULL otherwise */
} mc_home_pick;

typedef struct mc_home_picks {
  mc_home_pick *rows[MC_HOME_ROW_COUNT];
  int counts[MC_HOME_ROW_COUNT];
  int has_music; /* the library has an album or playlist to show */
  int has_plays; /* something in it has been played */
} mc_home_picks;

/* Fills out from lib (NULL leaves every row empty). seed decides Random's selection and order,
 * so one seed keeps the same selection across reloads; Radio's stations don't depend on it, and
 * hold for the day. This lists every album, evaluates every smart list and reads every analysis
 * result, so it's for a background thread, on its own connection. */
void mc_home_picks_load(mc_library *lib, unsigned seed, mc_home_picks *out);
void mc_home_picks_free(mc_home_picks *picks);

/* The heading over row. */
enum mc_str_id mc_home_row_title(mc_home_row_id row);

#ifdef __cplusplus
}
#endif

#endif
