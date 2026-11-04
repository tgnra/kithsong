/* What the Home page's rows hold (home_view.c draws them). Everything that decides what goes
 * in a row is here, in one pick_*() function per row in home_picks.c, so the guesses from
 * track_plays can give way to the play event log without touching the page.
 */
#ifndef KITHSONG_HOME_PICKS_H
#define KITHSONG_HOME_PICKS_H

#include <windows.h>

#include "musiccore/musiccore.h"

typedef enum home_row_id {
  HOME_ROW_RADIO,              /* placeholders: stations named after the artists played most */
  HOME_ROW_RECENTS,            /* mc_library_list_recents(), as the sidebar's */
  HOME_ROW_PLAYED_LATELY,      /* albums and playlists on repeat lately */
  HOME_ROW_NOT_PLAYED_LATELY,  /* played before, but not lately */
  HOME_ROW_RANDOM,             /* neither of those two, new each time the app starts */
  HOME_ROW_COUNT
} home_row_id;

enum { HOME_ROW_MAX = 24 }; /* tiles in a row */

typedef struct home_pick {
  mc_recent_kind kind;
  BOOL station; /* a radio station: kind is MC_RECENT_ARTIST, and uid the artist's */
  BOOL smart;   /* a smart playlist or group */
  char uid[MC_UID_STRLEN];
  wchar_t *title;
  wchar_t *sub;       /* the line beneath it */
  wchar_t *cover_dir; /* the folder holding its cover; NULL for none, and for playlists and
                       * groups, whose pictures are found by uid */
} home_pick;

typedef struct home_picks {
  home_pick *rows[HOME_ROW_COUNT];
  int counts[HOME_ROW_COUNT];
  BOOL has_music; /* the library has an album or playlist to show */
  BOOL has_plays; /* something in it has been played */
} home_picks;

/* Fills out from lib (NULL leaves every row empty). seed decides Random's selection and order,
 * so one seed keeps the same selection across reloads. This lists every album and evaluates
 * every smart list, so it's for a background thread, on its own connection. */
void home_picks_load(mc_library *lib, unsigned seed, home_picks *out);
void home_picks_free(home_picks *picks);

#endif
