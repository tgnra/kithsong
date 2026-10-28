/* Creating, renaming and deleting a playlist. Modal dialogs in the style of the import's
 * review dialog (themed standard controls, dark title bar in dark mode):
 *
 *   +- New Playlist ---------------+     +- Delete Playlist ------------+
 *   | Name                         |     | Delete "Driving"?            |
 *   | [ Driving                 ]  |     | Its tracks stay in the       |
 *   |                              |     | library.                     |
 *   |         [ Create ] [ Cancel ]|     |         [ Delete ] [ Cancel ]|
 *   +------------------------------+     +------------------------------+
 *
 * Rename is the same dialog as New, with the current name filled in. Create and Rename are
 * the default button (Enter), and stay disabled while the name is blank or, renaming,
 * unchanged. Delete's default is Cancel. Escape cancels any of them. A failure is reported
 * in a message box of its own.
 *
 * Album groups have the same three, with their own titles and Delete's note ("Its albums
 * stay in the library.").
 */
#ifndef MUSICAPP_PLAYLIST_DIALOGS_H
#define MUSICAPP_PLAYLIST_DIALOGS_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Each returns TRUE if the library changed. owner is the main window. */
BOOL playlist_create_dialog(HWND owner, mc_library *lib, char out_uid[MC_UID_STRLEN]);
BOOL playlist_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);
BOOL playlist_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);

BOOL album_group_create_dialog(HWND owner, mc_library *lib, char out_uid[MC_UID_STRLEN]);
BOOL album_group_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);
BOOL album_group_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);

#endif
