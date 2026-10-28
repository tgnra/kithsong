/* A device's page, beside the sidebar, for any member of the group, this PC included:
 *
 *   +------------------------------------------------------------------+
 *   | [icon] Living room PC          [Sync now] [Remove from group...] |
 *   | Online, synced 3 minutes ago                                     |
 *   | 1,204 of 1,210 files OK, 6 missing, 41.2 GB                      |
 *   | [####----] Syncing library... 120 sent, 340 received             |
 *   | Name   [Living room PC          ]                                |
 *   | Format [Auto          v]   [x] Limit storage to [32] GB          |
 *   |        About 28.1 GB for 5,120 tracks, as Opus 160 kbps          |
 *   | What to sync                                                     |
 *   | [x] Sync everything                                              |
 *   | ( Albums | Artists | Playlists | Album groups )                  |
 *   | [list with checkboxes, hidden while Sync everything]             |
 *   | 12 albums, 2 artists, 1 playlist selected                        |
 *   +------------------------------------------------------------------+
 *
 * This PC's own page has Sync now (with every member) and Leave group... instead. An
 * archive's format, budget and selection are fixed, so they're disabled; its name isn't.
 * Under the format is the profile's size estimate, worked out on a thread as edits are made:
 * the size, what Auto picked, or by how much it doesn't fit (then nothing would sync).
 * Edits are collected and applied together shortly after, through the device's sync profile
 * (get, change, set); an error shows on the page, which then reloads. The name applies on
 * Enter or when it loses the focus.
 *
 * Like the settings page, its controls are loose children of the main window, which routes
 * messages to it. Control ids are 400-449. */
#ifndef KITHSONG_DEVICE_VIEW_H
#define KITHSONG_DEVICE_VIEW_H

#include <windows.h>

#include "musiccore/musiccore.h"
#include "sync_bridge.h"

/* What the page asks of the main window. */
typedef struct device_view_host {
  void (*rename_self)(const char *utf8_name); /* this PC renamed: update settings.json, refresh */
  void (*devices_changed)(void);              /* after rename/remove/leave: refresh sidebar */
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
/* Updates the status and progress lines from a sync event about this device; others are
 * ignored. */
void device_view_on_sync_event(const sync_event *ev);

/* Each returns TRUE if the message belonged to the device view. */
BOOL device_view_on_command(int id, int code);
BOOL device_view_on_notify(const NMHDR *nm, LRESULT *result);
BOOL device_view_draw_item(const DRAWITEMSTRUCT *dis);
/* For the parent's WM_CTLCOLORSTATIC, WM_CTLCOLOREDIT, WM_CTLCOLORBTN and
 * WM_CTLCOLORLISTBOX (msg): sets dc's colours and returns the background brush, or NULL if
 * wnd isn't one of the view's controls. */
HBRUSH device_view_ctlcolor(UINT msg, HWND wnd, HDC dc);

#endif
