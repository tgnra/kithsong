/* The Album groups page, in the library page's stack beside the album and playlist grids:
 * the groups' tiles (their pictures and album counts), or a group's own page once one is
 * opened:
 *
 *   (<-)  Ambient                                   (...)
 *   +------+ +------+ +------+
 *   | art  | | art  | | art  |      <- the group's albums, in its order
 *   +------+ +------+ +------+
 *
 * A click (or Enter) on a group opens it, which puts it first in the recents; Back, Alt+Left
 * or the mouse's back button goes back to the groups. Its albums select as the album grid's
 * do, showing their tracks in the track list. The header's menu (...) and a group tile's
 * right-click menu rename or delete it.
 *
 * Right-clicking an album, here or in the album grid, gives Add to album group (each group,
 * then New album group...), and inside a group Remove from album group, as on Windows.
 * Smart album groups aren't listed (see TODO: smart lists).
 *
 * Only one exists at a time. */
#ifndef KITHSONG_ALBUM_GROUPS_H
#define KITHSONG_ALBUM_GROUPS_H

#include "album_grid.h"
#include "app.h"

/* The page. album_changed is called when the user picks another album in a group (as
 * album_grid_new()'s changed), navigated when it opens a group or goes back, so the track
 * list can follow. Call album_groups_reload() to fill it. */
GtkWidget *album_groups_new(app *a, void (*album_changed)(void *user), void (*navigated)(void *user));

/* Lists the groups, and the open group's albums, again; an open group that's gone goes back
 * to the groups. Returns the number of groups. */
int album_groups_reload(app *a);

/* Opens the group with uid, records it in the recents and calls navigated. FALSE if there's
 * no such group. */
gboolean album_groups_open(app *a, const char *uid);
/* The open group's uid, or NULL while the groups show. */
const char *album_groups_open_group(void);
/* The open group's album grid (album_grid.c), or NULL while the groups show. */
GtkWidget *album_groups_album_grid(void);

/* The menu for album in grid (an album grid), at x, y: Add to album group, and if in_group
 * isn't NULL, Remove from album group (of in_group). For album_grid_set_menu() callers. */
void album_groups_album_menu(app *a, GtkWidget *grid, AlbumItem *album, const char *in_group, double x, double y);

#endif
