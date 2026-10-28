/* A device's page (the counterpart of desktop/windows/src/device_view.c), shown beside the
 * sidebar for any member of the group, this computer included:
 *
 *   +------------------------------------------------------------------+
 *   | [banner: Paused: its sync settings changed...      [Sync now]]   |
 *   |  [icon] Living room PC              [Sync now] [Remove from…]    |
 *   |         Online, synced 3 minutes ago                             |
 *   |         (or, syncing: [####----] Syncing library… 120 sent, …)   |
 *   |  | Files    1,204 of 1,210 files OK, 6 missing, 41.2 GB       |  |
 *   |  | Address  192.168.1.20, port 47823                        |  |
 *   |  | Connections  This computer can't connect…   [Check again] |  |
 *   |  Profile                                                         |
 *   |  | Name                                       [Living room ✓] |  |
 *   |  | Archive  Keeps the whole library as original files    (o) |  |
 *   |  | Format   About 28.1 GB for 5,120 tracks         [Auto   v] |  |
 *   |  | Limit storage to                    [ 32 ] GB         (o) |  |
 *   |  What to sync                                                    |
 *   |  | Sync everything                                       (o) |  |
 *   |  [Albums|Artists|Playlists|Album groups]                         |
 *   |  [ ] Artist – Album (1997)                                       |
 *   |  12 albums, 2 artists, 1 playlist selected                       |
 *   +------------------------------------------------------------------+
 *
 * An AdwPreferencesPage under an AdwBanner, in an AdwToastOverlay. This computer's own page
 * has Sync now (with every member) and Leave group… instead. An archive's format, budget and
 * selection are fixed, so they're insensitive; its name isn't. Making a device an archive
 * asks first, with the whole library's size; only the device itself can stop being one. The
 * format row's subtitle is the profile's size estimate, worked out on a thread as edits are
 * made: the size, what Auto picked, or by how much it doesn't fit (then nothing would sync).
 * Edits are collected and applied together shortly after, through the device's sync profile
 * (get, change, set); a failure shows as a toast, and the page reloads. The name applies
 * with its row's apply button or Enter. Outside a group this computer's page has only its
 * address, connections and name. */
#ifndef MUSICAPP_DEVICE_VIEW_H
#define MUSICAPP_DEVICE_VIEW_H

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
/* Updates the status and progress from a sync event about this device; others are ignored. */
void device_view_on_sync_event(const sync_event *ev);

/* What's known of whether this computer and device uid (named name) can connect to each
 * other: for this computer's own uid, whether other devices can reach it. warning_only:
 * NULL unless this computer can't be reached, or can't reach uid. g_free it. */
char *device_view_reach_text(mc_sync *sync, const char *uid, const char *name, gboolean warning_only);

#endif
