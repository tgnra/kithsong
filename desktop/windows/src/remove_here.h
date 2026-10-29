/* Remove from this PC and Sync to this PC, on the album and artist menus
 * (musiccore/sync_here.h): an album or artist leaves this PC, or comes back, while it stays
 * in the library and on the other devices. Neither shows on an archive or outside a group,
 * where Delete from library is the only honest option. Asks first:
 *
 *   +- Remove from this PC -----------------------+
 *   | Remove "OK Computer" from this PC?          |
 *   | Its 13 files are removed here. It stays in  |
 *   | the library and on your other devices.      |
 *   | To get it back, choose Sync to this PC from |
 *   | its menu.                                   |
 *   |                       [ Remove ] [ Cancel ] |
 *   +---------------------------------------------+
 *
 * Cancel is the default. Playback stops first if it would play one of the files (an open
 * file can't be removed), as for a delete. */
#ifndef KITHSONG_REMOVE_HERE_H
#define KITHSONG_REMOVE_HERE_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* before_remove (may be NULL) gets the plan just before files go, e.g. to stop playing a
 * track in it. Call once at startup. */
void remove_here_init(void (*before_remove)(const mc_here_plan *plan));

/* Appends Remove from this PC... (as cmd_remove) or Sync to this PC (as cmd_sync) for uid,
 * an album or artist by kind, e.g. just before Delete from library...; nothing if neither
 * applies (sync is off, this PC is an archive or in no group). */
void remove_here_append_menu(HMENU menu, mc_profile_item_kind kind, const char *uid, UINT cmd_remove,
                             UINT cmd_sync);

/* Does cmd if it's cmd_remove (asking first) or cmd_sync, for uid named name. owner is the
 * main window. Returns TRUE if the library's files or this PC's profile changed, so the
 * views should reload. */
BOOL remove_here_command(HWND owner, UINT cmd, UINT cmd_remove, UINT cmd_sync, mc_profile_item_kind kind,
                         const char *uid, const wchar_t *name);

#endif
