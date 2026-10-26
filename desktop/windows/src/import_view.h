/* The Import page, beside the sidebar:
 *
 *   before an import            during / after an import
 *   +----------------------+    +------------------------------------------+
 *   |  .- - - - - - - - -. |    | Matching 12 of 40...               [Done] |
 *   |  : Drop folders     : |    | ========-------------------------------- |
 *   |  '- - - - - - - - -' |    |                                          |
 *   |  [Choose Folders...]  |    | NEEDS YOUR ATTENTION                     |
 *   +----------------------+    |  Needs review  Low  Things We Lost  70%  |
 *                               | IMPORTED                                 |
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
/* Posted to the main window when Done is clicked; import_view_confirm_close ends the session. */
#define WM_APP_IMPORT_CLOSE (WM_APP + 3)
/* Sent to the main window when an import session starts (wParam TRUE) or ends (FALSE). A
 * session lasts from the start of an import until Done. */
#define WM_APP_IMPORT_SESSION (WM_APP + 4)

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
/* Asks to stop a running import, then ends the session. Returns FALSE if the user wants to
 * keep it running. Items already imported stay in the library. */
BOOL import_view_confirm_close(void);

void import_view_on_event(WPARAM wp, LPARAM lp);
/* Returns TRUE if cmd belonged to the import view. */
BOOL import_view_on_command(int id);
BOOL import_view_on_notify(const NMHDR *nm);

#endif
