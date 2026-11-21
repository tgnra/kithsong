/* The strip across the top of the library pages while tracks wait for a device this PC can't
 * reach now (mc_sync_get_waiting()), and, above that in the same strip, problems a check left
 * for the user (mc_sync_get_problems()). Tracks missing for good get a Remove from Library
 * button, which sends the parent WM_COMMAND with WAITING_BANNER_REMOVE and the strip's id, and
 * any issue a Details button, which sends WAITING_BANNER_DETAILS (for issue_dialog.h). Both
 * go by themselves; there's nothing to dismiss.
 *
 * It sits where the content area starts, so it draws the content area's top border and rounded
 * top-left corner itself, and the grid below carries the left border on
 * (album_grid_set_below_banner()). Screen readers see it as a live region (uia.h). */
#ifndef KITHSONG_WAITING_BANNER_H
#define KITHSONG_WAITING_BANNER_H

#include <windows.h>

#include "kithcore/kithcore.h"

#define WAITING_BANNER_REMOVE 1  /* the notification code of the Remove from Library button */
#define WAITING_BANNER_DETAILS 2 /* and of Details */

/* Creates the strip as a hidden child of parent. */
HWND waiting_banner_create(HWND parent, int id);
/* Reads the waiting tracks and the problems from sync again (NULL: sync is off, so none). TRUE
 * if the strip's height changed, for the parent to lay out again; it repaints itself otherwise. */
BOOL waiting_banner_update(mc_sync *sync);
/* While a stress test library is open it says so instead, and a click on it sends the parent
 * WM_COMMAND with BN_CLICKED and its id. TRUE if the strip came or went. */
BOOL waiting_banner_set_stress(BOOL stress);
/* Whether it has something to say, and so takes waiting_banner_height() on library pages. */
BOOL waiting_banner_shown(void);
/* Its height at the DPI it was last given: a line of text with its padding, for each sentence. */
int waiting_banner_height(void);
/* After a DPI or font change (the main window's message font). */
void waiting_banner_set_font(HFONT font, UINT dpi);
/* After theme_update. */
void waiting_banner_theme_changed(void);

#endif
