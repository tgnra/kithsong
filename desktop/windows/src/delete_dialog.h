/* Deleting albums, artists and tracks from the library (musiccore/delete.h), from their
 * right-click menus: the album grid's tiles, the artist list's rows, the track list's
 * tracks. Asks first, in the playlist dialogs' Delete dialog:
 *
 *   +- Delete from Library ----------------------+
 *   | Delete "OK Computer"?                       |
 *   | 13 files will be moved to the Recycle Bin   |
 *   | and removed from the library on all your    |
 *   | devices. Restore them from the Recycle Bin  |
 *   | to bring them back.                         |
 *   |                       [ Delete ] [ Cancel ] |
 *   +---------------------------------------------+
 *
 * Cancel is the default. Afterwards a message box says if any track stayed because its file
 * couldn't go to the Recycle Bin. Restoring is mc_delete_restore(), which main.c runs at
 * startup and whenever the window is activated.
 */
#ifndef KITHSONG_DELETE_DIALOG_H
#define KITHSONG_DELETE_DIALOG_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Where deleted.db goes (the config dir, UTF-8; copied), and what to do just before files
 * go: before_delete (may be NULL) gets the plan, e.g. to stop playing a track in it, whose
 * open file couldn't be moved. Call once at startup. */
void delete_dialog_init(const char *config_dir, void (*before_delete)(const mc_delete_plan *plan));

/* Asks, then deletes uid (an album, artist or track, by kind) named name. owner is the main
 * window. Returns TRUE if the library changed. */
BOOL delete_dialog(HWND owner, mc_library *lib, mc_delete_kind kind, const char *uid, const wchar_t *name);

#endif
