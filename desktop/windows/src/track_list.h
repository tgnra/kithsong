/* The track list: the selected album's tracks, in a pane beside the album grid.
 *
 *   Album Title
 *   12 tracks · 47 min
 *   ----------------------------------
 *    1   Airbag                   4:44
 *   >)   Paranoid Android         6:23   <- playing, in the accent colour
 *
 * Track numbers and durations are secondary text. Clicking selects a track; double-clicking
 * or Enter plays it. Right-click (or the menu key) opens a menu for the track.
 */
#ifndef MUSICAPP_TRACK_LIST_H
#define MUSICAPP_TRACK_LIST_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Sent to the parent as a WM_COMMAND notification code when the user plays a track. */
#define TLN_PLAY 1

/* Creates the list as a child of parent. Only one exists at a time. */
HWND track_list_create(HWND parent, int id);

/* Rebuilds fonts and metrics. Call after creating it, and on DPI or theme changes. */
void track_list_update(UINT dpi);

/* Shows an album's tracks, or nothing for a NULL album_uid. Showing the same album again
 * reloads it (after an import, say), keeping the selection and scroll position. */
void track_list_show(mc_library *lib, const char *album_uid, const wchar_t *title);

/* The shown album's tracks, in order, for the play queue. Valid until the list next changes. */
typedef struct track_list_item {
  const char *uid;
  const wchar_t *title;
  int duration_ms; /* 0 if unknown */
  const char *path; /* UTF-8; NULL if the track has no file on this device */
} track_list_item;
int track_list_count(void);
track_list_item track_list_item_at(int i);
/* The track the last TLN_PLAY was for. */
int track_list_play_index(void);

/* Marks the playing track, by uid, or none for NULL. It stays marked while other albums are
 * shown, and when its album is shown again. */
void track_list_set_playing(const char *uid);

#endif
