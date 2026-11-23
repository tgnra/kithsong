/* A track's right-click menu, wherever the track shows: the track list's rows and the
 * now-playing strip's title and cover. Queue (greyed out with no file on this device),
 * and Add to Playlist; Go to album and Go to artist; for a
 * track in a playlist that's shown, Move Up, Move Down and Remove from Playlist; Edit... and
 * Delete from Library....
 *
 * The items name "track." actions (queue, add-to-playlist, go-to-album,
 * go-to-artist, edit, delete,
 * and move-up, move-down and remove in a playlist), which whoever shows the menu provides.
 * Go to album and Go to artist are hidden while their actions are disabled.
 */
#ifndef KITHSONG_TRACK_MENU_H
#define KITHSONG_TRACK_MENU_H

#include <gtk/gtk.h>

#include "kithcore/kithcore.h"

/* The menu, for a track in a shown playlist if in_playlist. The caller owns it. */
GMenuModel *track_menu_model(gboolean in_playlist);

/* Adds the queue action to group, which adds the track uid to the queue, and returns it for
 * track_menu_enable_queue(). uid is a buffer of the caller's, read when it's picked. */
GSimpleAction *track_menu_add_queue_action(GSimpleActionGroup *group, const char *uid);
/* A track with no file on this device can't be queued: here says if it has one. */
void track_menu_enable_queue(GSimpleAction *action, gboolean here);

/* Appends Queue for something the library holds, as an item the app handles (app.queue), for the
 * menus of albums, artists, playlists and album groups. name is what its row in the queue says. */
void track_menu_append_queue(GMenu *menu, mc_up_next_kind kind, const char *uid, const char *name);

/* A menu item that shows its key, as Windows' do. */
void track_menu_append_with_key(GMenu *menu, enum mc_str_id label, const char *action, const char *accel);

/* Whether album_uid has a page to go to: it isn't "", or a single (an album of one track),
 * which the album grid leaves out. */
gboolean track_menu_album_has_page(mc_library *lib, const char *album_uid);

#endif
