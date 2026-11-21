/* The Home page: titled rows of covers, each scrolling sideways on its own (the Windows page is
 * the same, drawn by hand). What's in each row is the core's business (kithcore/home_picks.h).
 *
 * The wheel always scrolls the page, so it never gets stuck on a row on the way down; Shift and
 * the wheel, sideways scrolling, dragging and the end arrows scroll the row under the pointer.
 * A drag claims the press from the tile under it, so dragging never opens anything. Tab reaches
 * one tile, the last one the keys were on; the arrow keys move from there.
 *
 * The rows are read on a worker thread with its own library connection, as that lists every
 * album and evaluates the smart lists; what's showing stays until it's done. Reloading updates
 * tiles in place, so scroll positions and focus stay. Random's seed is picked once, when the
 * view is made, so its picks hold until the app next starts. Rows are capped (MC_HOME_ROW_MAX),
 * so every tile can be a real widget; cover.h keeps the covers.
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

/* The menu a playlist has on its tile, for showing elsewhere: Search's results. g_object_unref() it.
 * Its items are "home." actions, home_view_actions(), which whatever shows it has to insert. What
 * it's for is copied, so it can go while the menu is open. */
GMenuModel *home_view_playlist_menu(const char *uid, const char *title, gboolean smart);
GActionGroup *home_view_actions(void);

/* The scrolled window of the rows, for the performance log. */
GtkWidget *home_view_scrolled(void);

#endif
