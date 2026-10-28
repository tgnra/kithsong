/* Add to Playlist, a submenu of a track's right-click menu in the track list:
 *
 *   Add to Playlist  >  Driving
 *                       Mix
 *                       ---------------
 *                       New Playlist...
 *
 * Picking a playlist adds the track to its end. New Playlist... asks for a name
 * (playlist_dialogs.c), creates the playlist and adds the track to it.
 *
 * Add to Album Group is the same for an album, in the album grid's right-click menu.
 */
#ifndef MUSICAPP_ADD_TO_PLAYLIST_H
#define MUSICAPP_ADD_TO_PLAYLIST_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* The submenu: the playlists by name, with command ids first_id, first_id + 1, ..., then
 * New Playlist... as new_id. Free *lists with mc_playlist_list_free() once the menu is
 * done (it may be NULL), and the menu with DestroyMenu() (or its parent's). */
HMENU add_to_playlist_menu(mc_library *lib, UINT first_id, UINT new_id, mc_playlist_list **lists);

/* Adds track_uid to the end of the playlist playlist_uid, or for NULL asks for a new
 * playlist to add it to. Reports failures itself. Returns TRUE if the library changed. */
BOOL add_to_playlist(HWND owner, mc_library *lib, const char *track_uid, const char *playlist_uid);

/* The same for album groups: the submenu (free *groups with mc_album_group_list_free()),
 * and adding album_uid to the end of group_uid, or for NULL to a new group. */
HMENU add_to_album_group_menu(mc_library *lib, UINT first_id, UINT new_id, mc_album_group_list **groups);
BOOL add_to_album_group(HWND owner, mc_library *lib, const char *album_uid, const char *group_uid);

#endif
