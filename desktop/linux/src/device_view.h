/* A device's page (the counterpart of desktop/windows/src/device_view.c): an
 * AdwPreferencesPage under an AdwBanner, in an AdwToastOverlay, for any member of the group,
 * this computer included (which has Sync now with every member and Leave group instead, and
 * outside a group only its address, connections and name). While a session with the device
 * runs, Sync now reads Cancel sync (mc_sync_cancel()). An archive's format, budget and
 * selection are fixed; only the device itself can stop being an archive. The format row's
 * size estimate is worked out on a thread as edits are made. Edits are collected and applied
 * together shortly after through the device's sync profile (get, change, set); a failure shows
 * as a toast and the page reloads. */
#ifndef KITHSONG_DEVICE_VIEW_H
#define KITHSONG_DEVICE_VIEW_H

#include <adwaita.h>

#include "sync_bridge.h"

/* What the page asks of the main window. */
typedef struct device_view_callbacks {
  void (*rename_self)(void *user, const char *name); /* this computer renamed: save it, refresh */
  void (*devices_changed)(void *user);               /* after rename/remove/leave: refresh the sidebar */
  void *user;
} device_view_callbacks;

/* The page. Only one exists at a time. callbacks is copied. */
GtkWidget *device_view_new(const device_view_callbacks *callbacks);

/* Shows device uid's page, loading its details, its profile and the library's lists. sync
 * and lib must stay valid until the next open, or until sync stops (open again with NULL,
 * which applies edits still waiting and waits for the size estimate's thread). */
void device_view_open(mc_sync *sync, mc_library *lib, const char *uid);
/* Reloads the device's details and profile, e.g. after the device list changed, keeping
 * edits not yet applied. */
void device_view_refresh(void);
/* Scrolls to What to sync and puts the focus on its first row, as Choose what to sync brought
 * the page up for it. Waits for the page to be shown, so call it just after opening it. */
void device_view_focus_what_to_sync(void);
/* Updates the status and progress from a sync event about this device; others are ignored. */
void device_view_on_sync_event(const sync_event *ev);

/* The status, problems and banner afresh, e.g. when the archive offer changed. */
void device_view_update(void);
/* Which device to update when device name's version can't sync with this one's; NULL when it
 * can. g_free it. */
char *device_view_version_text(const mc_sync_version *version, const char *name);

#endif
