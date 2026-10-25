/* The album grid: cover art with the album title beneath, in as many columns as fit.
 *
 * Built for large libraries:
 *  - virtualised: only the tiles in view are painted, into an off-screen buffer.
 *  - covers are decoded on a background thread with WIC, scaled down during decoding to
 *    exactly the tile size, visible tiles first and one screen ahead in each direction.
 *  - thumbnails far from the view are freed, so memory stays flat however many albums.
 *  - pixel scrolling, with the mouse wheel eased over a few frames, and a thin overlay
 *    scrollbar that appears while the mouse moves over the grid or it scrolls.
 *
 * Clicking an album selects it (an accent ring around the cover). The grid also draws the
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

/* Call after theme_update. */
void album_grid_theme_changed(void);

#endif
