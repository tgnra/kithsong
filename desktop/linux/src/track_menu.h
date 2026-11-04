/* A track's right-click menu, wherever the track shows: the track list's rows and the
 * now-playing strip's title and cover. Start radio and Add to Playlist; Go to album and Go to artist; for a
 * track in a playlist that's shown, Move Up, Move Down and Remove from Playlist; Edit... and
 * Delete from Library....
 *
 * The items name "track." actions (start-radio, add-to-playlist, go-to-album, go-to-artist, edit, delete,
 * and move-up, move-down and remove in a playlist), which whoever shows the menu provides.
 * Go to album and Go to artist are hidden while their actions are disabled.
 */
#ifndef KITHSONG_TRACK_MENU_H
#define KITHSONG_TRACK_MENU_H

#include <gtk/gtk.h>

#include "musiccore/musiccore.h"

/* The menu, for a track in a shown playlist if in_playlist. The caller owns it. */
GMenuModel *track_menu_model(gboolean in_playlist);

/* A menu item that shows its key, as Windows' do. */
void track_menu_append_with_key(GMenu *menu, enum mc_str_id label, const char *action, const char *accel);

/* Whether album_uid has a page to go to: it isn't "", or a single (an album of one track),
 * which the album grid leaves out. */
gboolean track_menu_album_has_page(mc_library *lib, const char *album_uid);

#endif
