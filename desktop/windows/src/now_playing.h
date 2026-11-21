/* The now-playing strip in the toolbar, after the playback buttons: cover, track and seekbar.
 *
 * Drawn with alpha, as it sits over Mica. Everywhere but the bar lets clicks through to the
 * title bar, so the window can still be dragged by it. That includes right-clicks, so main.c
 * opens the track's menu from the title bar's right-clicks, using now_playing_on_track().
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

/* The cover file in the folder shown was replaced. */
void now_playing_art_changed(void);

/* Where playback is, in ms. Ignored while the user drags the thumb. */
void now_playing_set_position(int ms);
int now_playing_position(void);

/* Whether pt (screen coordinates) is on the track's cover or its line of text. Never with
 * nothing playing, nor on text that a flash has taken the place of. */
BOOL now_playing_on_track(POINT pt);
/* Where the track's menu opens from the keyboard (screen coordinates): under the cover. */
POINT now_playing_menu_point(void);

/* Shows text in the track's place for a moment, e.g. "Volume 40%" after a shortcut. */
void now_playing_flash(const wchar_t *text);
/* The same for a sentence, held long enough to read (e.g. that a pairing was undone). */
void now_playing_notice(const wchar_t *text);

#endif
