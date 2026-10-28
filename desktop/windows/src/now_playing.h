/* The now-playing strip in the toolbar, after the playback buttons:
 *
 *   +----+         Paranoid Android — Radiohead
 *   |art |  1:02  ======O---------------------------  -5:21
 *   +----+
 *
 * The cover sits in a rounded square, the track (title in bold, artist dimmed) above the
 * seekbar, with the time so far on its left and the time left on its right. Clicking or dragging the bar (or the
 * arrow keys, Home and End once it has focus) moves the position. With nothing playing the
 * bar is greyed out and has no thumb.
 *
 * Drawn with alpha, as it sits over Mica. Everywhere but the bar lets clicks through to the
 * title bar, so the window can still be dragged by it.
 */
#ifndef KITHSONG_NOW_PLAYING_H
#define KITHSONG_NOW_PLAYING_H

#include <windows.h>

/* Posted to the parent as a WM_COMMAND notification code when the user moves the position;
 * now_playing_position() has the new one. */
#define NPN_SEEK 1

/* Creates the strip as a child of parent. Only one exists at a time. */
HWND now_playing_create(HWND parent, int id);

/* Rebuilds fonts, metrics and the cover. Call after creating it, and on DPI or theme changes. */
void now_playing_update(UINT dpi);

/* Shows a track: its title, artist (may be NULL), length (0 if unknown) and the folder
 * holding its album's cover (NULL if there's none). A NULL title shows nothing playing. The
 * position goes back to 0. */
void now_playing_show(const wchar_t *title, const wchar_t *artist, int duration_ms, const wchar_t *cover_dir);

/* Where playback is, in ms. Ignored while the user drags the thumb. */
void now_playing_set_position(int ms);
int now_playing_position(void);

#endif
