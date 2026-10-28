/* The Settings page, beside the sidebar:
 *
 *   +--------------------------------------------------+
 *   | Settings                                         |
 *   |                                                  |
 *   | Library location                                 |
 *   | C:\Users\me\Music                                |
 *   | [Move Library...] [Open Another Library...]      |
 *   | Opening another library switches to ...          |
 *   |                                                  |
 *   | Sync                                             |
 *   | This PC's name                                   |
 *   | [Kitchen laptop                ]                 |
 *   | This PC's address                                |
 *   | 192.168.1.20:47823, [fe80::1]:47823              |
 *   | [Leave Group...]                                 |
 *   | Leaving the group stops syncing ...              |
 *   |                                                  |
 *   | Scrobbling                                       |
 *   | Signed in as tania                               |
 *   | [Sign Out]                                       |
 *   | [x] Scrobble what I play on this device          |
 *   | Signing in opens Last.fm in your browser ...     |
 *   +--------------------------------------------------+
 *
 * Move Library posts WM_APP_MOVE_LIBRARY to the main window, which moves it (move_library.c).
 * Open Another Library posts WM_APP_OPEN_LIBRARY to the main window, which asks for the
 * folder.
 *
 * The name applies on Enter or when the box loses the focus: through sync, which shows it
 * to other devices, and into settings.json for next time. Leave Group is enabled while this
 * PC is in a group. The sync shown is sync_bridge_get()'s.
 *
 * Scrobbling signs in with playback_scrobbler(): Sign In opens Last.fm's page in the browser
 * and waits, on a thread, for the user to approve it there. The account is the sync group's
 * (see scrobbler.h); the checkbox is this PC's own, kept in settings.json.
 */
#ifndef KITHSONG_SETTINGS_VIEW_H
#define KITHSONG_SETTINGS_VIEW_H

#include <windows.h>

#include "musiccore/musiccore.h"

#define WM_APP_OPEN_LIBRARY (WM_APP + 5)
#define WM_APP_MOVE_LIBRARY (WM_APP + 7)

void settings_view_create(HWND parent);
void settings_view_destroy(void);
/* The library folder to show (UTF-8; NULL shows nothing). */
void settings_view_set_library_dir(const char *dir);
/* font is the main window's; the headings are made from it. */
void settings_view_set_font(HFONT font, UINT dpi);
void settings_view_show(BOOL show);
void settings_view_layout(int x, int y, int w, int h, UINT dpi);

/* Where the Sync section saves this PC's name, and what it calls after renaming it or
 * leaving the group (to refresh the devices). Call once, after creating it. */
void settings_view_init_sync(mc_settings *settings, void (*devices_changed)(void));
/* Reloads the Sync section: the name (unless it's being edited), the addresses and whether
 * this PC is in a group. Call when sync starts or stops and when the devices change. */
void settings_view_update_sync(void);

/* Returns TRUE if the command (a WM_COMMAND's control id and notification code) belonged to
 * the settings view. */
BOOL settings_view_on_command(int id, int code);
/* For the parent's WM_CTLCOLORSTATIC and WM_CTLCOLORBTN: sets dc's colours and returns the
 * background brush, or NULL if wnd isn't one of the view's labels or its checkbox. */
HBRUSH settings_view_ctlcolor(HWND wnd, HDC dc);

#endif
