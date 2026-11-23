/* The play queue's drop-down, under the toolbar's queue button: what's playing at the top, set
 * apart, then what the player has up next (playback.h), a row per entry, a track or a block (an
 * album or the like) with what's left of it. Rows drag into another order as whole blocks. A popup window as volume_popup.c is: a
 * click elsewhere, Escape or another window taking over closes it, and the main window keeps
 * looking active meanwhile. Only one exists at a time.
 */
#ifndef KITHSONG_QUEUE_POPUP_H
#define KITHSONG_QUEUE_POPUP_H

#include <windows.h>

/* Opens the popup under anchor, the queue button, at the window's dpi, or closes it if it's
 * open. anchor is redrawn when it opens and closes, as it shows which. */
void queue_popup_toggle(HWND anchor, UINT dpi);
BOOL queue_popup_is_open(void);
/* Closes it, if it's open, as for a theme or DPI change. */
void queue_popup_close(void);
/* The queue changed: shows it, if it's open. */
void queue_popup_update(void);

#endif
