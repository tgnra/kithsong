/* Deleting albums, artists and tracks from the library (musiccore/delete.h): Delete from
 * library... on the album tiles' menu (album_groups.c), the artist page's and the track
 * menus (track_list.c, artist_view.c). Asks first:
 *
 *   +---------------------------------------------+
 *   |            Delete "OK Computer"?             |
 *   | 13 files will be moved to the Trash and      |
 *   | removed from the library on all your         |
 *   | devices. Restore them from the Trash to      |
 *   | bring them back.                             |
 *   |       [ Cancel ]        [ Delete ]           |
 *   +---------------------------------------------+
 *
 * Delete is destructive, Cancel the default. Afterwards a dialog says if any track stayed
 * because its file couldn't go to the Trash (it's open elsewhere, or its file system has no
 * trash Kithsong can use or make). Restoring is mc_delete_restore(), which main.c runs at startup and whenever the
 * window becomes active.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog).
 */
#ifndef KITHSONG_DELETE_DIALOG_H
#define KITHSONG_DELETE_DIALOG_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* Asks over parent (any widget in the main window) about deleting uid (an album, artist or
 * track, by kind) named name, with deleted.db in config_dir, and calls changed once anything was
 * deleted. A failure is reported in a dialog of its own. */
void delete_dialog_present(GtkWidget *parent, mc_library *lib, const char *config_dir, mc_delete_kind kind,
                           const char *uid, const char *name, void (*changed)(void *user), void *user);

#endif
