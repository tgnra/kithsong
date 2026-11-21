/* The sidebar's Devices section and everything sync does for the main window (the Linux
 * counterpart of the sync parts of desktop/windows/src/main.c and sidebar.c). Sync
 * (sync_bridge.c) runs while a library is open and stops before it closes, so switching
 * library restarts it. The list (this computer first, then online, then offline, then nearby
 * devices) refreshes shortly after sync events and every 10 seconds, as a rename elsewhere
 * fires no event. A warning triangle marks a device with something wrong, its tooltip saying
 * what (mc_sync_device_warning(): its version, a hold on its side, reach, an archive away
 * for long, too little space to transcode). A change of network has sync look for the devices
 * again (mc_sync_network_changed()). */
#ifndef KITHSONG_DEVICES_H
#define KITHSONG_DEVICES_H

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

/* This computer's place in sync, as of the last refresh, for the empty pages (which
 * app_update_empty_pages() updates whenever it changes). */
typedef enum devices_state {
  DEVICES_SYNC_OFF, /* sync isn't running */
  DEVICES_ALONE,    /* running, and this computer isn't in a group */
  DEVICES_GROUPED,  /* this computer is in a group */
} devices_state;
devices_state devices_sync_state(void);
/* Shows group member uid's page, selected in the sidebar, as picking it there does
 * (--page sync). FALSE if it isn't listed (yet). */
gboolean devices_open(const char *uid);
/* Shows this computer's page, where What to sync is, while it's in a group. */
void devices_open_self(void);

#endif
