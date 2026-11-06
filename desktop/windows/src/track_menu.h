/* A track's right-click menu, wherever the track shows: the track list's rows and the
 * now-playing strip's title and cover.
 *
 *   Add to Playlist  >   (add_to_playlist.h)
 *   -------------------
 *   Play Next            greyed out if the track has no file here
 *   Play Last
 *   Start radio          greyed out if the track can't seed a station
 *   -------------------
 *   Go to Album          where there's a page to go to
 *   Go to Artist
 *   -------------------
 *   Move Up              for a track in a playlist that's shown
 *   Move Down
 *   -------------------
 *   Remove from Playlist
 *   -------------------
 *   Edit...
 *   Delete from Library...
 *
 * It only asks: the caller acts on what was picked, but for Play Next and Play Last, which it
 * carries out, as they're the same wherever the menu is. The menu's message loop runs other
 * messages, so what the caller shows may have changed by the time this returns.
 */
#ifndef KITHSONG_TRACK_MENU_H
#define KITHSONG_TRACK_MENU_H

#include <windows.h>

#include "kithcore/kithcore.h"

typedef enum track_menu_pick {
  TRACK_MENU_NOTHING,
  TRACK_MENU_ADD_TO_PLAYLIST, /* the playlist in playlist_uid */
  TRACK_MENU_NEW_PLAYLIST,
  TRACK_MENU_START_RADIO,
  TRACK_MENU_MOVE_UP,
  TRACK_MENU_MOVE_DOWN,
  TRACK_MENU_REMOVE,
  TRACK_MENU_GO_TO_ALBUM,
  TRACK_MENU_GO_TO_ARTIST,
  TRACK_MENU_EDIT,
  TRACK_MENU_DELETE,
} track_menu_pick;

typedef struct track_menu_options {
  mc_library *lib;
  const char *track_uid; /* the track, for Start radio and the queue */
  BOOL go_to_album, go_to_artist;
  BOOL in_playlist; /* offers Move Up, Move Down and Remove from Playlist */
  BOOL can_move_up, can_move_down;
} track_menu_options;

/* Shows the menu at pt (screen coordinates) and returns what was picked. */
track_menu_pick track_menu_show(HWND owner, POINT pt, const track_menu_options *options,
                                char playlist_uid[MC_UID_STRLEN]);

/* Whether album_uid has a page to go to: it isn't "", or a single (an album of one track),
 * which the album grid leaves out. */
BOOL track_menu_album_has_page(mc_library *lib, const char *album_uid);

#endif
