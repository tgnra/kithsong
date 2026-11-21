/* The volume drop-down, under the toolbar's speaker button: a mute button and a slider. A
 * popup window: a click elsewhere, Escape or another window taking over closes it, and the main
 * window keeps looking active meanwhile.
 *
 * It shows playback_volume() and playback_muted(), so whatever changes them calls
 * volume_popup_update(). Only one exists at a time.
 */
#ifndef KITHSONG_VOLUME_POPUP_H
#define KITHSONG_VOLUME_POPUP_H

#include <windows.h>

/* What the user picked. Neither changes playback itself, so main.c can keep the volume in
 * the settings as it does the keyboard's. */
typedef struct volume_popup_host {
  void (*set_volume)(int volume); /* 0 to 100 */
  void (*set_muted)(BOOL muted);
} volume_popup_host;

/* Opens the popup under anchor, the speaker button, at the window's dpi, or closes it if
 * it's open. anchor is redrawn when it opens and closes, as it shows which (see
 * volume_popup_is_open()). */
void volume_popup_toggle(HWND anchor, UINT dpi, const volume_popup_host *host);
BOOL volume_popup_is_open(void);
/* Closes it, if it's open, as for a theme or DPI change. */
void volume_popup_close(void);
/* After the volume or muting changed, from here or anywhere else. */
void volume_popup_update(void);

/* The speaker for the volume and muting, in Segoe Fluent Icons: Mute, or Volume 0 (no
 * waves) to 3 by thirds. For the toolbar's button too. */
wchar_t volume_popup_glyph(void);

#endif
