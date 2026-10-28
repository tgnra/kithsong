/* The sidebar's Recents section, between the pages and Devices:
 *
 *   Recents
 *   [album]    Northern Lights
 *   [playlist] Road Trip
 *
 * The albums and playlists last played, on this computer or another device (recents are
 * library data and sync with it), newest first and at most ten; the section hides while
 * there are none. Picking one opens it on the Albums or Playlists page (app_open()). Playing
 * an album or playlist records it (app_play_album()). */
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
