/* The play queue's drop-down, under the toolbar's queue button:
 *
 *      ===O---------------  [shf] [que] [spk]      _ [] X
 *     +-----------------------------------+
 *     | Queue                   [ Clear ] |
 *     |-----------------------------------|
 *     | = Title                  4:12   x |
 *     |   Artist                          |
 *     | ...                               |
 *     +-----------------------------------+
 *
 * A popup window as volume_popup.c is: a click elsewhere, Escape or another window taking
 * over closes it, and the main window keeps looking active meanwhile.
 *
 * It shows what the player has up next (playback.h), a row for each entry: a track, or an album
 * or the like as a block with what's left of it, and keeps up as that changes. Rows drag into
 * another order, as whole blocks, and the x on a row takes it out; Clear empties the queue and
 * leaves playback alone. The keyboard does the same (on_key()), and UI Automation sees it all.
 * Only one exists at a time.
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
