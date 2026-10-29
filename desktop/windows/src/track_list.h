/* The track list: the selected album's or playlist's tracks, in a pane beside the grid.
 *
 *   +------+  OK Computer          [...] [x]   <- [...], the playlist menu, for playlists only
 *   | art  |  Radiohead
 *   +------+  1997 · 12 tracks · 53 min
 *   [     |> Play     ] [    >< Shuffle    ]
 *   ----------------------------------------
 *    1   Airbag                        4:44
 *   >)   Paranoid Android              6:23   <- playing, in the accent colour
 *
 * Play and Shuffle play the whole list, in order or shuffled (TLN_PLAY_ALL, TLN_SHUFFLE_ALL);
 * they're greyed out when nothing in it can be played here. From the keyboard, Up from the
 * first track (or Tab) moves the focus to Play, Left and Right between Play and Shuffle,
 * Enter or Space presses one, and Down (or Tab) goes back to the tracks. Track numbers and durations are
 * secondary text; in a playlist the numbers count its places. Tracks with no file on this
 * device are greyed out. Clicking selects a track;
 * double-clicking or Enter plays it. Right-click (or the menu key) opens a menu for the
 * track: Add to Playlist (add_to_playlist.c) and Delete from Library, and in a playlist also
 * Move Up, Move Down and Remove from Playlist. A playlist's tracks can be dragged into another order too, or moved
 * with Alt+Up and Alt+Down; Delete removes one. Any track can be dragged onto a playlist
 * outside the list (drag.h: a recent in the sidebar, a tile on the Playlists page), which
 * asks for it to be added (TLN_ADD_TO_PLAYLIST). The [...] button (or F2, or the menu key
 * with no track selected) offers Rename... and Delete Playlist.... The close button or
 * Escape asks the parent to close the pane (TLN_CLOSE).
 *
 * Screen readers see the header's text and buttons and a list of the tracks (uia.h).
 */
#ifndef KITHSONG_TRACK_LIST_H
#define KITHSONG_TRACK_LIST_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Sent to the parent as WM_COMMAND notification codes. TLN_PLAY: the user plays a track
 * (see track_list_play_index). The others ask the parent to change the library; the list
 * changes nothing itself. The parent does it, then reloads the list with track_list_show.
 * track_list_last_request has the details. */
#define TLN_PLAY 1
#define TLN_ADD_TO_PLAYLIST 2 /* track_uid, to the end of playlist_uid ("" = a new playlist); here */
#define TLN_REMOVE 3          /* entry_uid, out of the shown playlist */
#define TLN_MOVE 4            /* entry_uid, to after after_uid ("" = to the start) */
#define TLN_RENAME_PLAYLIST 5 /* the shown playlist */
#define TLN_DELETE_PLAYLIST 6
/* Sent likewise when the user closes the pane, with its close button or Escape. */
#define TLN_CLOSE 7
/* And for the header's Play and Shuffle buttons: play every track, from the first in order
 * (shuffle off), or shuffled (shuffle on). */
#define TLN_PLAY_ALL 8
#define TLN_SHUFFLE_ALL 9
/* The track menu's Delete from Library...: track_uid, out of the library and to the Recycle
 * Bin (delete_dialog.h). */
#define TLN_DELETE_TRACK 10
/* A random smart playlist's Reshuffle button: give the shown one a new order. */
#define TLN_RESHUFFLE 11
/* A smart playlist's menu's Edit Rules...: open the shown one in the editor (smart_editor.h).
 * Its Rename... (or F2) and Delete... send TLN_RENAME_PLAYLIST and TLN_DELETE_PLAYLIST, as a
 * playlist's do. */
#define TLN_EDIT_SMART 12

typedef struct track_list_request {
  char track_uid[MC_UID_STRLEN];
  char entry_uid[MC_UID_STRLEN];
  char after_uid[MC_UID_STRLEN];
  char playlist_uid[MC_UID_STRLEN];
  BOOL here; /* track_uid has a file on this PC */
} track_list_request;

/* The request the last notification was for. */
const track_list_request *track_list_last_request(void);

/* Creates the list as a child of parent. Only one exists at a time. */
HWND track_list_create(HWND parent, int id);

/* Rebuilds fonts and metrics. Call after creating it, and on DPI or theme changes. */
void track_list_update(UINT dpi);

/* Shows an album's tracks, or a playlist's, or nothing for a NULL uid. Showing the same
 * one again reloads it (after an import, say), keeping the selection and scroll position. */
void track_list_show(mc_library *lib, const char *uid, const wchar_t *title, BOOL playlist);
/* Shows the tracks credited to an artist, numbered in order, each with its album beside it,
 * and no header or close button: on the artist page, which has its own. Escape still sends
 * TLN_CLOSE. Showing the same one again reloads it, as track_list_show does. */
void track_list_show_artist(mc_library *lib, const char *artist_uid);
/* Shows smart playlist uid's tracks, as its rule picks them now, with "Smart playlist" for
 * the artist and the rule as a sentence across the pane under the cover. Nothing can be
 * moved or removed; the menu's button offers Edit Rules..., Rename... and Delete..., and in
 * random order (random) a Reshuffle button sits beside it (TLN_RESHUFFLE). */
void track_list_show_smart(mc_library *lib, const char *uid, const wchar_t *title, BOOL random);
/* The header's details for the album shown: its artist (NULL shows "Unknown Artist"), year
 * (0 if unknown) and the folder holding its cover (NULL if there's none). Call after
 * track_list_show. A playlist's header shows "Playlist" for the artist, and cover is its
 * picture file (see mc_playlist_art()) rather than a folder. */
void track_list_set_details(const wchar_t *artist, int year, const wchar_t *cover);

/* The shown tracks, in order, for the play queue. Valid until the list next changes. */
typedef struct track_list_item {
  const char *uid;
  const wchar_t *title;
  const wchar_t *artist, *album; /* NULL if unknown */
  const wchar_t *cover_dir;      /* the album folder, or NULL if it has no cover */
  int duration_ms;               /* 0 if unknown */
  const char *path;              /* UTF-8; NULL if the track has no file on this device */
} track_list_item;
int track_list_count(void);
track_list_item track_list_item_at(int i);
/* The track the last TLN_PLAY was for. */
int track_list_play_index(void);

/* Selects the shown track with uid and scrolls it into view, if it's there. */
void track_list_select(const char *uid);

/* Marks the playing track, by uid, or none for NULL. It stays marked while other albums are
 * shown, and when its album is shown again. */
void track_list_set_playing(const char *uid);

#endif
