/* Start radio, on the menus of tracks, albums and artists. A menu works out the seed as it
 * opens, greyed out if there isn't one, for instance while nothing's been analysed, and if the
 * item is chosen passes that seed to playback_play_radio(). */
#ifndef KITHSONG_RADIO_MENU_H
#define KITHSONG_RADIO_MENU_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Each appends Start radio as cmd for the track, album or artist uid, and fills seed, which has
 * room for MC_UID_STRLEN, "" if it's greyed out. They read only uid's own tracks, so they're
 * fine for a menu on the UI thread. lib may be NULL. */
void radio_menu_append_track(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed);
void radio_menu_append_album(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed);
void radio_menu_append_artist(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed);
/* The same for a station, from its seed track: it's already one, so the item says Play. */
void radio_menu_append_station(HMENU menu, UINT cmd, mc_library *lib, const char *uid, char *seed);

#endif
