/* Making, renaming and deleting an album group, as playlist_dialogs.c does for playlists:
 *
 *   +------------------------------+     +------------------------------+
 *   |       New album group        |     |      Delete "Ambient"?       |
 *   |  [ Ambient                ]  |     | Its albums stay in the       |
 *   |                              |     | library.                     |
 *   |  [ Cancel ]   [ Create ]     |     |  [ Cancel ]   [ Delete ]     |
 *   +------------------------------+     +------------------------------+
 *
 * Create and Rename are the default (Enter in the entry), insensitive while the name is
 * blank (or for Rename, unchanged). Delete is destructive; Escape cancels any of them.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog).
 */
#ifndef KITHSONG_ALBUM_GROUP_DIALOGS_H
#define KITHSONG_ALBUM_GROUP_DIALOGS_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* Each asks over parent (any widget in the main window) and calls changed once the library
 * has changed. A failure is reported in a dialog of its own.
 *
 * Create makes a group and, if album_uid isn't NULL, adds that album to it (Add to album
 * group's New album group...). changed is called if the group was made, even if adding the
 * album then failed. */
void album_group_create_present(GtkWidget *parent, mc_library *lib, const char *album_uid,
                                void (*changed)(void *user), void *user);
/* Create with no album (the Album groups page's +), calling created with the new group's
 * uid once it's made. */
void album_group_new_present(GtkWidget *parent, mc_library *lib, void (*created)(void *user, const char *uid),
                             void *user);
/* For group uid, currently called name. */
void album_group_rename_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                                void (*changed)(void *user), void *user);
void album_group_delete_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                                void (*changed)(void *user), void *user);

#endif
