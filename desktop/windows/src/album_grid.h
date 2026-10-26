/* The album grid: cover art with the album title and artist beneath, in as many columns as
 * fit.
 *
 * Built for large libraries:
 *  - virtualised: only the tiles in view are painted, into an off-screen buffer.
 *  - covers are decoded on a background thread with WIC, scaled down during decoding to
 *    exactly the tile size, visible tiles first and one screen ahead in each direction.
 *  - thumbnails far from the view are freed, so memory stays flat however many albums.
 *  - pixel scrolling, with the mouse wheel eased over a few frames, and a thin overlay
 *    scrollbar that appears while the mouse moves over the grid or it scrolls.
 *
 * Clicking an album (or moving to it with the arrow keys, Home, End, Page Up or Page Down)
 * selects it (an accent ring around the cover). The grid also draws the
 * content area's border and rounded top-left corner, which shows Mica outside it.
 */
#ifndef MUSICAPP_ALBUM_GRID_H
#define MUSICAPP_ALBUM_GRID_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Creates the grid as a child of parent. Fonts arrive through WM_SETFONT. Only one grid
 * exists at a time. */
HWND album_grid_create(HWND parent, int id);

/* Shows the albums of lib (NULL shows "Could not open the library"). The list is reloaded
 * the next time the grid paints, so calling this while it's hidden (e.g. after each item of
 * an import) costs nothing. Call it again whenever the library changes. */
void album_grid_set_library(mc_library *lib);

/* Sent to the parent as a WM_COMMAND notification code (posted) when the selected album
 * changes, by a click or because it left the library. */
#define AGN_SELCHANGE 1

/* The selected album: copies its uid and returns its title, or returns NULL if there's none. */
const wchar_t *album_grid_selection(char uid[MC_UID_STRLEN]);
/* The selected album's artist, or NULL. Valid until the grid next reloads. */
const wchar_t *album_grid_selection_artist(void);
/* The folder holding the selected album's cover, or NULL if it has none (or none is
 * selected). Valid until the grid next reloads. */
const wchar_t *album_grid_selection_cover(void);
/* The selected album's year, or 0 if it's unknown (or none is selected). */
int album_grid_selection_year(void);
/* Clears the selection, which notifies the parent (AGN_SELCHANGE) as a click would. */
void album_grid_unselect(void);

/* Sizes the tiles for a grid full_w wide, even while it's narrower, so a pane beside it
 * that comes and goes takes whole columns without resizing the tiles. Returns the width the
 * pane should take from the grid to be at least min_w: a whole number of columns, or 0 if
 * that would leave the grid none. Call before each layout. */
int album_grid_set_full_width(int full_w, int min_w);

/* Call after theme_update. */
void album_grid_theme_changed(void);

#endif
