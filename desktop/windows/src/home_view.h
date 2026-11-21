/* The Home page, beside the sidebar, custom-drawn: titled rows of covers, each scrolling
 * sideways on its own. What's in each row is the core's business (kithcore/home_picks.h).
 *
 * The wheel always scrolls the page, so it never gets stuck on a row on the way down; Shift
 * and the wheel, WM_MOUSEHWHEEL, dragging and the end arrows scroll the row under the mouse.
 *
 * Covers are decoded on a background thread, those in view first, then those a screen along
 * or down, and only ever those. Rows are capped (MC_HOME_ROW_MAX), so what's decoded stays
 * small however big the library is; it's kept until the page reloads. The page reloads on that
 * thread the next time it paints after home_view_refresh(), as reading the rows lists every
 * album and evaluates the smart lists; what's showing stays until it's done. Random's seed is
 * picked once, when the view is created, so its picks hold until the app next starts. Screen
 * readers see each row as a group of buttons (uia.h).
 */
#ifndef KITHSONG_HOME_VIEW_H
#define KITHSONG_HOME_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* Creates the view as a child of parent, hidden. */
HWND home_view_create(HWND parent, int id);

/* Shows lib (NULL says it couldn't be opened). Call again whenever the library changes. */
void home_view_set_library(mc_library *lib);
/* What's played or opened has changed, or the page is about to show: reads it all again. */
void home_view_refresh(void);
/* Where playlists' pictures are cached (see album_grid_set_playlist_art_dir()). */
void home_view_set_playlist_art_dir(const char *dir);

/* Call when the font or DPI changes, and after theme_update. */
void home_view_update(UINT dpi, HFONT font);

/* Sent to the parent as a WM_COMMAND notification code (posted) when a tile is clicked, or
 * Enter pressed on one, to open it; home_view_clicked() says which. */
#define HVN_OPEN 1
/* Sent the same way when the welcome's Import is pressed, for the parent to show that page. */
#define HVN_IMPORT 2
/* Sent the same way when a tile's menu changed the library, for the parent to reload the views. */
#define HVN_LIBRARY_CHANGED 3
/* Sent the same way from a Recents tile's menu: Remove from Recents (home_view_clicked() says
 * which) and Clear Recents. */
#define HVN_REMOVE_RECENT 4
#define HVN_CLEAR_RECENTS 5

/* The tile last clicked: its uid (valid until the view next reloads), and in *kind what it is. */
const char *home_view_clicked(mc_recent_kind *kind);

#endif
