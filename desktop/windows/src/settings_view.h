/* The Settings page, beside the sidebar. Its library and reset actions post to the main
 * window, which does the work: WM_APP_MOVE_LIBRARY (move_library.c), WM_APP_OPEN_LIBRARY, and
 * WM_APP_RESET, which marks the settings folder to be cleared on the next start
 * (mc_settings_request_reset()) and closes.
 *
 * This PC's name applies on Enter or loss of focus, through sync (sync_bridge_get()) and into
 * settings.json. Ask about Windows Firewall again appears once "Don't ask again" was ticked
 * (firewall_ask_again()). Scrobbling signs in with playback_scrobbler(), waiting on a thread
 * while the user approves in the browser; the account is the sync group's (scrobbler.h), the
 * checkbox this PC's own (settings.json).
 *
 * The controls are children of a window as tall as they reach, measured at each layout, and
 * scroll with scroller.h's overlay scrollbar. The main window has no dialog manager, so
 * settings_view_translate() gives the page Tab (scrolling the control it reaches into view),
 * the wheel and the scrolling keys.
 */
#ifndef KITHSONG_SETTINGS_VIEW_H
#define KITHSONG_SETTINGS_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"

#define WM_APP_OPEN_LIBRARY (WM_APP + 5)
#define WM_APP_MOVE_LIBRARY (WM_APP + 7)
#define WM_APP_RESET (WM_APP + 8)

void settings_view_create(HWND parent);
void settings_view_destroy(void);
/* Running from a checkout, the settings folder, which must outlive the view; else NULL. Before
 * settings_view_set_library_dir(), which works out from it what Reset Kithsong... says. */
void settings_view_set_source_run(const char *config_dir);
/* The library folder to show (UTF-8; NULL shows nothing). */
void settings_view_set_library_dir(const char *dir);
/* font is the main window's; the headings are made from it. */
void settings_view_set_font(HFONT font, UINT dpi);
void settings_view_show(BOOL show);
void settings_view_layout(int x, int y, int w, int h, UINT dpi);
/* Opens or closes the Testing section. */
void settings_view_open_testing(BOOL open);
/* Opens it and scrolls to it. The page must be showing. */
void settings_view_reveal_testing(void);
void settings_view_focus(void);
/* For the message loop, before dispatching: TRUE if msg was the page's. */
BOOL settings_view_translate(const MSG *msg);

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
