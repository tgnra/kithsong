/* The Sync page, beside the sidebar. Across the top, after the notices: what sync last did
 * here (or how the sessions under way are going) and Sync now, which reads Cancel sync while
 * they run (mc_sync_now() and mc_sync_cancel() with every member). Below, two panels that
 * scroll on their own:
 *
 *   This PC     its sync profile: the archive switch, What to sync, then the format and the
 *               budget with the size estimate under them. An archive's format, budget and
 *               selection are fixed, so they're disabled; only it can stop being an archive.
 *   Devices     the other members, each with a sentence on what it syncs, which opens its
 *               details and profile, read-only, with Remove from group; the devices nearby
 *               that aren't paired, which a click asks to pair; Add device by address and
 *               Leave group.
 *
 * The size estimate is worked out on a thread as edits are made. Edits are collected and
 * applied together shortly after through this PC's sync profile (get, change, set); an error
 * shows on the page, which then reloads.
 *
 * Its windows are children of the main window, which it routes their messages to as if it
 * were their parent. Control ids are 400-469. device_view_theme_changed() is for what the
 * main window's theme_apply_controls() can't reach. */
#ifndef KITHSONG_DEVICE_VIEW_H
#define KITHSONG_DEVICE_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"
#include "sync_bridge.h"

/* What the page asks of the main window. */
typedef struct device_view_host {
  void (*devices_changed)(void);       /* after a remove or leaving: refresh the devices */
  void (*library_changed)(void);       /* lost tracks removed from its issues */
  void (*pair)(const char *uid);       /* a nearby device clicked: ask it to pair */
  void (*add_device)(void);            /* Add device by address */
} device_view_host;

/* host is copied. */
void device_view_create(HWND parent, const device_view_host *host);
void device_view_destroy(void);
/* font is the main window's; the headings are made from it. */
void device_view_set_font(HFONT font, UINT dpi);
void device_view_show(BOOL show);
void device_view_layout(int x, int y, int w, int h, UINT dpi);

/* Loads the devices, this PC's profile and the library's lists. sync and lib must stay valid
 * until the next open, or until sync stops (open again with NULL). */
void device_view_open(mc_sync *sync, mc_library *lib);
/* Reloads the devices and the profile, e.g. after the device list changed, keeping edits not
 * yet applied. */
void device_view_refresh(void);
/* Reads the devices again, not the profile, for their warnings and the issues' button: after
 * the problems or reachability changed. */
void device_view_issues_changed(void);
/* Puts the focus on What to Sync's first control, as Choose What to Sync brought the page up
 * for it, scrolling it into view. */
void device_view_focus_what_to_sync(void);
/* Puts the focus on the page's first control, as Tab does. */
void device_view_focus(void);
/* Updates the strip, and the devices' rows, from a sync event. */
void device_view_on_sync_event(const sync_event *ev);

void device_view_theme_changed(void);

/* For the message loop, before dispatching: TRUE if msg was the page's (Tab, or the wheel
 * over a panel, while it shows). */
BOOL device_view_translate(const MSG *msg);
/* Each returns TRUE if the message belonged to the device view. */
BOOL device_view_on_command(int id, int code);
BOOL device_view_on_notify(const NMHDR *nm, LRESULT *result);
BOOL device_view_draw_item(const DRAWITEMSTRUCT *dis);
/* For the parent's WM_CTLCOLORSTATIC, WM_CTLCOLOREDIT, WM_CTLCOLORBTN and
 * WM_CTLCOLORLISTBOX (msg): sets dc's colours and returns the background brush, or NULL if
 * wnd isn't one of the view's controls. */
HBRUSH device_view_ctlcolor(UINT msg, HWND wnd, HDC dc);

#endif
