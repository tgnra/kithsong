/* The Album groups page, in the library page's stack beside the album and playlist grids:
 * the groups' tiles, or a group's own page once one is opened. Its albums select as the album
 * grid's do, showing their tracks in the track list; an album played from there plays as part
 * of the group, going on into the albums after it (app_play_album()). Opening a group, and
 * adding, removing or moving its albums, put it first in the recents.
 *
 * Manual groups take albums dragged onto their tile and can be reordered by drag (drag.h).
 * Smart groups follow the manual ones and show the albums their rule picks; they have nothing
 * to remove, and a random one has a Reshuffle button.
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

/* Choose what to sync shows under "none of them have albums on this device" once this computer is
 * in a group: call when that may have changed. */
void album_groups_update_empty(void);

/* Opens the group with uid, records it in the recents and calls navigated. FALSE if there's
 * no such group. */
gboolean album_groups_open(app *a, const char *uid);
/* The open group's uid, or NULL while the groups show. */
const char *album_groups_open_group(void);
/* Whether the groups page shows group uid: not one empty here, as they're hidden. */
gboolean album_groups_shows(const char *uid);
/* The open group's album grid (album_grid.c), or NULL while the groups show. */
GtkWidget *album_groups_album_grid(void);

/* The menu for album in grid (an album grid), at x, y: Play next, Play last, Add to album group, and if in_group isn't NULL, Remove from album group (of in_group), then Edit... (edit_dialog.h), Remove from
 * this computer... or Sync to this computer where they apply (remove_here.h), and Delete from
 * library... (delete_dialog.h).
 * For album_grid_set_menu() callers. */
void album_groups_album_menu(app *a, GtkWidget *grid, AlbumItem *album, const char *in_group, double x, double y);

/* The same menu for showing elsewhere in the window, the Home page's tiles, outside any group:
 * the album uid, called title, or NULL with no library. g_object_unref() it. What it's for is
 * copied, so it can go while the menu is open. */
GMenuModel *album_groups_album_model(app *a, const char *uid, const char *title);
/* The same for group uid, called name: a group tile's right-click menu. */
GMenuModel *album_groups_group_model(const char *uid, const char *name, gboolean smart);

#endif
