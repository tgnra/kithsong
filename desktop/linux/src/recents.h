/* The sidebar's Recents section: the albums and playlists last played and the artists and
 * album groups last opened, on any device (recents are library data and sync with it), newest
 * first and at most ten; the section hides while there are none. Smart lists are left out, as
 * they have no page to open on. A track dragged onto a manual playlist's row, or an album onto
 * a manual group's, is added to it (drag.h). */
#ifndef KITHSONG_RECENTS_H
#define KITHSONG_RECENTS_H

#include <adwaita.h>

#include "app.h"

/* The section, for the sidebar. Call once. */
GtkWidget *recents_new(app *a);
/* Reloads the section from a->lib, and the Home page with it: after a play, and whenever the
 * library changes (an import, a playlist edit, another library, or sync bringing opens or
 * removals from other devices). */
void recents_refresh(app *a);
/* Remove from Recents and Clear Recents, here or on a Home tile's menu. */
void recents_remove(app *a, const char *uid);
void recents_clear(app *a);

#endif
