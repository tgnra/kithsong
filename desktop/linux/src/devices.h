/* Everything sync does for the main window (the Linux counterpart of the sync parts of
 * desktop/windows/src/main.c and sidebar.c): the Sync page (sync_view.h), pairing, and the
 * sidebar's Sync row. Sync (sync_bridge.c) runs while a library is open and stops before it
 * closes, so switching library restarts it. The devices refresh shortly after sync events and
 * every 10 seconds, as a rename elsewhere fires no event. A warning triangle on the Sync row
 * marks something wrong with this computer, its tooltip saying what (mc_sync_device_warning():
 * a hold on its side, reach, too little space to transcode; outside a group, whether other
 * devices can reach it). A change of network has sync look for the devices again
 * (mc_sync_network_changed()). */
#ifndef KITHSONG_DEVICES_H
#define KITHSONG_DEVICES_H

#include <adwaita.h>

#include "app.h"

/* Adds the Sync page to a->pages as "sync". Call once, before devices_start(), with
 * a->sync_warning built. */
void devices_init(app *a);
/* Starts sync for a->lib (stopping any running first) and refreshes the devices. */
void devices_start(app *a);
/* Stops sync: call before a->lib closes. Closes the pairing dialogs and lets the Sync page go
 * of sync first. */
void devices_stop(app *a);

/* The running mc_sync, or NULL; for Settings. */
mc_sync *devices_sync(void);
/* Sync's state for Settings: "Sync is off (reason)." if it's off, else NULL. g_free it. */
char *devices_sync_off_text(void);
/* This computer was renamed in Settings: saves the name for next time and refreshes the
 * devices. */
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
/* Shows the Sync page, and member uid's details over it unless uid is NULL (--page sync).
 * FALSE if it isn't a member (yet). */
gboolean devices_open(const char *uid);
/* Shows the Sync page at What to sync, while this computer is in a group. */
void devices_open_self(void);

#endif
