/* The sidebar's Devices section and everything sync does for the main window (the Linux
 * counterpart of the sync parts of desktop/windows/src/main.c and sidebar.c):
 *
 *   DEVICES                          [+]
 *   [computer] Study PC
 *              This computer
 *   [phone]    Pixel                 (spinner while syncing)
 *   [computer] Living room PC
 *              Offline
 *   UNPAIRED DEVICES (1)
 *   [computer] Kitchen laptop
 *
 * Sync (sync_bridge.c) runs while a library is open and stops before it closes, so switching
 * library restarts it. The section lists the group's devices (this computer first, then the
 * online ones, then the offline ones) and the nearby ones, refreshed shortly after sync
 * events and every 10 seconds (a rename elsewhere fires no event). Picking a paired device
 * shows its page (device_view.c) in the main stack's "device" page; picking a nearby one
 * asks it to pair (pair_dialog.c), as does the + (add_device.c, by address). */
#ifndef MUSICAPP_DEVICES_H
#define MUSICAPP_DEVICES_H

#include <adwaita.h>

#include "app.h"

/* The section, for the sidebar. Also adds the device page to a->pages as "device". Call
 * once, before devices_start(). */
GtkWidget *devices_new(app *a);
/* Starts sync for a->lib (stopping any running first) and refreshes the section. */
void devices_start(app *a);
/* Stops sync: call before a->lib closes. Closes the pairing dialogs and lets the device page
 * go of sync first. */
void devices_stop(app *a);
/* A page other than a device's was picked in the sidebar: no device is selected now. */
void devices_unselect(void);

/* The running mc_sync, or NULL; for Settings. */
mc_sync *devices_sync(void);
/* Sync's state for Settings: "Sync is off (reason)." if it's off, else NULL. g_free it. */
char *devices_sync_off_text(void);
/* This computer was renamed (in Settings or on its page): saves the name for next time and
 * refreshes the devices. */
void devices_renamed(app *a, const char *name);
/* The group changed (left it, say): refreshes the devices now. */
void devices_refresh(app *a);

#endif
