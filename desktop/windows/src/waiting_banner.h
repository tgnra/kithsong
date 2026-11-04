/* The strip across the top of the library pages (Albums, Playlists, Album Groups) while
 * tracks wait for a device this PC can't reach now (mc_sync_get_waiting()): "12 tracks are
 * waiting for Tania's desktop to come online." It's calm, not an error: an info icon and the
 * sentence on the accent's faint tint, as the import page's offline banner, with no button
 * and nothing to dismiss. It goes by itself once the tracks arrive. In high contrast it's an
 * outline in the text colour.
 *
 * It sits above the album grid, where the content area starts, so it draws the content
 * area's top border and rounded top-left corner itself, and the grid below it carries the
 * left border on (album_grid_set_below_banner()). Screen readers see it as a live region
 * named by its sentence (uia.h), read out when it appears or changes. */
#ifndef KITHSONG_WAITING_BANNER_H
#define KITHSONG_WAITING_BANNER_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Creates the strip as a hidden child of parent. */
HWND waiting_banner_create(HWND parent, int id);
/* Reads the waiting tracks from sync again (NULL: sync is off, so none). TRUE if the strip
 * came or went, for the parent to lay out again; it repaints itself otherwise. */
BOOL waiting_banner_update(mc_sync *sync);
/* While a stress test library is open it says so instead, and a click on it sends the parent
 * WM_COMMAND with BN_CLICKED and its id. TRUE if the strip came or went. */
BOOL waiting_banner_set_stress(BOOL stress);
/* Whether it has something to say, and so takes waiting_banner_height() on library pages. */
BOOL waiting_banner_shown(void);
/* Its height at the DPI it was last given: a line of text with its padding. */
int waiting_banner_height(void);
/* After a DPI or font change (the main window's message font). */
void waiting_banner_set_font(HFONT font, UINT dpi);
/* After theme_update. */
void waiting_banner_theme_changed(void);

#endif
