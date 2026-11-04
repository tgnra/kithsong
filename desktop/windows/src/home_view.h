/* The Home page, beside the sidebar, custom-drawn: titled rows of covers, one under another,
 * each scrolling sideways on its own.
 *
 *   Radio
 *   +------+ +------+ +------+ +------+ +---
 *   | art  | | art  | | art  | | art  | |       <- scrolls sideways
 *   +------+ +------+ +------+ +------+ +---
 *   Recents
 *   +------+ (  o   ) +------+ +------+ +---
 *   ...
 *
 * What's in each row is core's business (musiccore/home_picks.h); a row with nothing in it
 * isn't shown. A library with no music shows a welcome and the Import button (HVN_IMPORT), and
 * one that hasn't been played yet says that the page fills up as it is.
 *
 * The wheel scrolls the page, wherever the mouse is, so it never gets stuck on a row on the way
 * down. Shift and the wheel, or a trackpad's or tilting wheel's sideways scroll
 * (WM_MOUSEHWHEEL), scroll the row under the mouse; so does dragging it, which carries on a
 * little when let go, and the arrows at its ends, which show while the mouse is over it. Left
 * and Right move the focus along a row, Up and Down between rows, and Page Up, Page Down, Home
 * and End scroll the page.
 *
 * Clicking a tile (or Enter on it) opens it, HVN_OPEN. Radio's stations are placeholders, so
 * nothing happens yet.
 *
 * Covers are decoded on a background thread, as the album grid's, those in view first, then
 * those a screen along or down, and only ever those. The rows are capped (MC_HOME_ROW_MAX), so
 * what's decoded stays small however big the library is, and it's kept until the page reloads.
 *
 * The page reloads the next time it paints after home_view_refresh(), on that thread too, as
 * reading the rows lists every album and evaluates the smart lists; what's showing stays until
 * it's done. Random's selection comes from a seed picked once, when the view is created, so it
 * holds until the app next starts. Screen readers don't see the tiles yet.
 */
#ifndef KITHSONG_HOME_VIEW_H
#define KITHSONG_HOME_VIEW_H

#include <windows.h>

#include "musiccore/musiccore.h"

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

/* The tile last clicked: its uid (valid until the view next reloads), and in *kind what it is. */
const char *home_view_clicked(mc_recent_kind *kind);

#endif
