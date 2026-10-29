/* Remove from this computer and Sync to this computer, on the album tiles' menu
 * (album_groups.c) and the artist page's menu (artist_view.c), through musiccore/sync_here.h:
 * an album or artist leaves this computer, or comes back, while it stays in the library and
 * on the other devices. Neither shows while sync is off, outside a group or on an archive,
 * where Delete from library is the only honest option. Asks first:
 *
 *   +---------------------------------------------+
 *   |    Remove "OK Computer" from this computer?  |
 *   | Its 13 files are removed here. It stays in   |
 *   | the library and on your other devices. To    |
 *   | get it back, choose Sync to this computer    |
 *   | from its menu.                               |
 *   |       [ Cancel ]        [ Remove ]           |
 *   +---------------------------------------------+
 *
 * Remove is destructive, Cancel the default. Files open elsewhere can still be removed on
 * Linux, so playback isn't stopped first; a file that couldn't be removed is reported.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog), as delete_dialog.c does. */
#ifndef KITHSONG_REMOVE_HERE_H
#define KITHSONG_REMOVE_HERE_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* Appends Remove from this computer... (remove_action) or Sync to this computer
 * (sync_action) for uid, an album or artist by kind, to menu; nothing if neither applies. */
void remove_here_append(GMenu *menu, mc_profile_item_kind kind, const char *uid, const char *remove_action,
                        const char *sync_action);

/* Remove from this computer: asks over parent (any widget in the main window), then removes
 * uid named name and calls changed. */
void remove_here_present(GtkWidget *parent, mc_profile_item_kind kind, const char *uid, const char *name,
                         void (*changed)(void *user), void *user);

/* Sync to this computer: undoes the removal and starts a sync, then calls changed; a failure
 * is reported over parent. */
void remove_here_restore(GtkWidget *parent, mc_profile_item_kind kind, const char *uid,
                         void (*changed)(void *user), void *user);

#endif
