/* A device's page, beside the sidebar, for any member of the group, this PC included (which
 * has Sync now with every member and Leave group instead). While a session with the device
 * runs, Sync now reads Cancel sync (mc_sync_cancel()). An archive's format, budget and
 * selection are fixed, so they're disabled; only the device itself can stop being an archive.
 * The size estimate under the format is worked out on a thread as edits are made. Edits are
 * collected and applied together shortly after through the device's sync profile (get, change,
 * set); an error shows on the page, which then reloads.
 *
 * Like the settings page, its controls are loose children of the main window, which routes
 * messages to it. Control ids are 400-449. device_view_theme_changed() is for what the main
 * window's theme_apply_controls() can't reach. */
#ifndef KITHSONG_DEVICE_VIEW_H
#define KITHSONG_DEVICE_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"
#include "sync_bridge.h"

/* What the page asks of the main window. */
typedef struct device_view_host {
  void (*rename_self)(const char *utf8_name); /* this PC renamed: update settings.json, refresh */
  void (*devices_changed)(void);              /* after rename/remove/leave: refresh sidebar */
  void (*library_changed)(void);              /* lost tracks removed from its issues */
} device_view_host;

/* host is copied. */
void device_view_create(HWND parent, const device_view_host *host);
void device_view_destroy(void);
/* font is the main window's; the headings are made from it. */
void device_view_set_font(HFONT font, UINT dpi);
void device_view_show(BOOL show);
void device_view_layout(int x, int y, int w, int h, UINT dpi);

/* Shows device uid's page, loading its details, its profile and the library's lists. sync
 * and lib must stay valid until the next open, or until sync stops (open again with NULL). */
void device_view_open(mc_sync *sync, mc_library *lib, const char *uid);
/* Reloads the device's details and profile, e.g. after the device list changed, keeping
 * edits not yet applied. */
void device_view_refresh(void);
/* Reads the device's details again, not its profile, for its warnings and its issues'
 * buttons: after the problems or reachability changed. */
void device_view_issues_changed(void);
/* Puts the focus on What to Sync's first control, as Choose What to Sync brought the page up
 * for it. It's always in view: the page doesn't scroll. */
void device_view_focus_what_to_sync(void);
/* Updates the status and progress lines from a sync event about this device; others are
 * ignored. */
void device_view_on_sync_event(const sync_event *ev);

void device_view_theme_changed(void);

/* Each returns TRUE if the message belonged to the device view. */
BOOL device_view_on_command(int id, int code);
BOOL device_view_on_notify(const NMHDR *nm, LRESULT *result);
BOOL device_view_draw_item(const DRAWITEMSTRUCT *dis);
/* For the parent's WM_CTLCOLORSTATIC, WM_CTLCOLOREDIT, WM_CTLCOLORBTN and
 * WM_CTLCOLORLISTBOX (msg): sets dc's colours and returns the background brush, or NULL if
 * wnd isn't one of the view's controls. */
HBRUSH device_view_ctlcolor(UINT msg, HWND wnd, HDC dc);

#endif
