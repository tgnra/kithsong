/* Renaming and deleting a playlist, from the track list's header menu:
 *
 *   +------------------------------+     +------------------------------+
 *   |       Rename Playlist        |     |      Delete "Driving"?       |
 *   |  [ Driving                ]  |     | Its tracks stay in the       |
 *   |                              |     | library.                     |
 *   |  [ Cancel ]   [ Rename ]     |     |  [ Cancel ]   [ Delete ]     |
 *   +------------------------------+     +------------------------------+
 *
 * Rename is the default (Enter in the entry), and stays insensitive while the name is
 * blank or unchanged. Delete is destructive; Escape cancels either.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog).
 */
#ifndef KITHSONG_PLAYLIST_DIALOGS_H
#define KITHSONG_PLAYLIST_DIALOGS_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* Each asks over parent (any widget in the main window) about the playlist uid, currently
 * called name, and calls changed once it's renamed or deleted. A failure is reported in a
 * dialog of its own, and changed isn't called. */
void playlist_rename_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                             void (*changed)(void *user), void *user);
void playlist_delete_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                             void (*changed)(void *user), void *user);

#endif
