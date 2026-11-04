/* The Home page: titled rows of covers, one under another, each scrolling sideways on its own.
 *
 *   Radio
 *   +------+ +------+ +------+ +------+ +---
 *   | art  | | art  | | art  | | art  | |       <- scrolls sideways
 *   +------+ +------+ +------+ +------+ +---
 *   Recents
 *   +------+ (  o   ) +------+ +------+ +---
 *   ...
 *
 * What's in each row is core's business (musiccore/home_picks.h); a row with nothing in it isn't
 * shown. A library with no music shows a welcome and the Import button, and one that hasn't been
 * played yet says that the page fills up as it is. The Windows page is the same, drawn by hand.
 *
 * The wheel scrolls the page, wherever the pointer is, so it never gets stuck on a row on the
 * way down. Shift and the wheel, or a trackpad's or tilting wheel's sideways scroll, scroll the
 * row under the pointer; so does dragging it, which carries on a little when let go, and the
 * arrows at its ends, which show while the pointer is over it. A drag claims the press from
 * the tile under it, so dragging never opens anything.
 *
 * Tab reaches one tile, the last one the keys were on. From there Left and Right move along a
 * row, Up and Down between rows, and Page Up, Page Down, Home and End scroll the page; the
 * viewports keep the focused tile in view. Space plays or pauses, as in the album grids.
 *
 * Clicking a tile (or Enter on it) opens it, or if it's one of Radio's stations, plays it.
 *
 * The rows are read on a worker thread with its own library connection, as doing so lists every
 * album and evaluates the smart lists, and what's showing stays until it's done. Reloading
 * updates the tiles where they are, so the scroll positions and the focus stay. Random's
 * selection comes from a seed picked once, when the view is made, so it holds until the app
 * next starts. The rows are capped (MC_HOME_ROW_MAX), so every tile can be a real widget, and
 * its cover is asked for as soon as it's there, the top rows first; cover.h keeps them.
 */
#ifndef KITHSONG_HOME_VIEW_H
#define KITHSONG_HOME_VIEW_H

#include "app.h"

typedef struct home_view_callbacks {
  /* A tile was opened. artist is the line under an album, for finding a single. */
  void (*open)(void *user, mc_recent_kind kind, const char *uid, const char *artist);
  void (*import)(void *user); /* the welcome's Import */
  void *user;
} home_view_callbacks;

/* The page. Call once. */
GtkWidget *home_view_new(app *a, const home_view_callbacks *callbacks);

/* What's played or opened has changed, or the library has, or the page is about to show: it
 * reads it all again, now if it's showing and otherwise when it next shows. */
void home_view_refresh(void);

/* Whether widget, the focus, is one of the tiles. */
gboolean home_view_is_tile(GtkWidget *widget);

/* The scrolled window of the rows, for the performance log. */
GtkWidget *home_view_scrolled(void);

#endif
