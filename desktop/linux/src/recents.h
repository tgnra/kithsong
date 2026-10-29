/* The sidebar's Recents section, between the pages and Devices:
 *
 *   Recents
 *   [album]    Northern Lights
 *   [playlist] Road Trip
 *   [artist]   Low
 *
 * The albums and playlists last played and the artists and album groups last opened, on this
 * computer or another device (recents are library data and sync with it), newest first and
 * at most ten; the section hides while there are none. Picking one opens it on its page
 * (app_open(), app_open_artist(), app_open_album_group()). Playing an album or playlist
 * records it (app_play_album()), as opening an artist's or group's page does. Smart lists
 * are left out until there's a page for them. A row's menu (right-click, a long press, the
 * Menu key or Shift+F10) has Remove from Recents and Clear Recents, which sync too. A track
 * dragged onto a manual playlist's row, or an album onto a manual group's, is added to it
 * (drag.h). */
#ifndef KITHSONG_RECENTS_H
#define KITHSONG_RECENTS_H

#include <adwaita.h>

#include "app.h"

/* The section, for the sidebar. Call once. */
GtkWidget *recents_new(app *a);
/* Reloads the section from a->lib: after a play, and whenever the library changes (an
 * import, a playlist edit, another library, or sync bringing opens or removals from other
 * devices). */
void recents_refresh(app *a);

#endif
