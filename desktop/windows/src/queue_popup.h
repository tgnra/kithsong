/* MOCK-UP. The play queue drop-down, under the toolbar's queue button:
 *
 *      ===O---------------  [shf] [que] [spk]      _ [] X
 *     +---------------------------------+
 *     | Queue                   [ Clear ] |
 *     |---------------------------------|
 *     | = Title                 4:12  x |
 *     |   Artist                        |
 *     | ...                             |
 *     +---------------------------------+
 *
 * A popup window as volume_popup.c is: a click elsewhere, Escape or another window taking
 * over closes it, and the main window keeps looking active meanwhile. Rows drag into another
 * order, and the x on a row or Clear takes them out.
 *
 * It isn't connected to anything yet: the tracks are made up, nothing here touches playback,
 * and opening it after Clear brings them back, to try it again. Mouse only, apart from
 * Escape, and UI Automation doesn't see it. Only one exists at a time.
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

#endif
