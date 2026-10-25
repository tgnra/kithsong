/* The import screen. While shown it replaces the sidebar, album grid and track pane:
 *
 *   before an import            during / after an import
 *   +----------------------+    +------------------------------------------+
 *   | [<- Back]            |    | Matching 12 of 40...               [Done] |
 *   |  .- - - - - - - - -. |    | ========-------------------------------- |
 *   |  : Drop a folder    : |    |                                          |
 *   |  '- - - - - - - - -' |    | NEEDS YOUR ATTENTION                     |
 *   |   [Choose Folder...]  |    |  Needs review  Low  Things We Lost  70%  |
 *   +----------------------+    | IMPORTED                                 |
 *                               |  Imported  Radiohead  OK Computer  100%  |
 *                               +------------------------------------------+
 *
 * Clicking an item opens the review dialog, where the user picks a match, searches again,
 * keeps the existing tags or skips it. Folders can be dropped anywhere on the window.
 */
#ifndef MUSICAPP_IMPORT_VIEW_H
#define MUSICAPP_IMPORT_VIEW_H

#include <windows.h>
#include <shellapi.h>

#include "musiccore/musiccore.h"

/* Posted to the main window: import events marshalled from the import thread, and a
 * notice that the library changed. */
#define WM_APP_IMPORT (WM_APP + 1)
#define WM_APP_LIBRARY_CHANGED (WM_APP + 2)
/* Posted to the main window to leave the import view. wParam 0 (Done) asks before stopping
 * a running import; IMPORT_CLOSE_CANCEL (Back, on the drop zone) doesn't ask. */
#define WM_APP_IMPORT_CLOSE (WM_APP + 3)
#define IMPORT_CLOSE_CANCEL 1

void import_view_create(HWND parent, mc_library *lib);
void import_view_destroy(void);
/* Switches to another library; ends any finished import session first. */
void import_view_set_library(mc_library *lib);
void import_view_set_font(HFONT font);
void import_view_show(BOOL show);
void import_view_layout(int x, int y, int w, int h, UINT dpi);

/* Starts importing the given UTF-8 paths. Returns FALSE if an import is still running. */
BOOL import_view_start(const char *const *paths, int count);
void import_view_browse(void);
/* Handles a WM_DROPFILES; returns TRUE if anything was imported. */
BOOL import_view_drop(HDROP drop);
/* Asks to stop a running import. Returns FALSE if the user wants to keep it running. */
BOOL import_view_confirm_close(void);
/* Stops any import without asking. Items already imported stay in the library. */
void import_view_cancel(void);

void import_view_on_event(WPARAM wp, LPARAM lp);
/* Returns TRUE if cmd belonged to the import view. */
BOOL import_view_on_command(int id);
BOOL import_view_on_notify(const NMHDR *nm);

#endif
