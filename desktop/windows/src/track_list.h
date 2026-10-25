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

/* The playing track's file (UTF-8), or NULL. It stays playing while other albums are shown. */
const char *track_list_playing_path(void);

#endif
