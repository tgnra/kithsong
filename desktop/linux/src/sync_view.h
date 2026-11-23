/* The Sync page, under an AdwBanner in an AdwToastOverlay. A strip across the top says what
 * the last sync that changed anything did (mc_sync_activity_text()), or how the sessions in
 * progress are going, with Sync now (with every member online), which reads Cancel sync while
 * they run. Below it, side by side: this
 * computer's sync profile on the left, what to sync above and in what quality below, and on
 * the right the other members, each with what it syncs in a sentence
 * (mc_sync_profile_summary()) and its details and profile in a dialog (device_dialog.h), the
 * devices nearby that could pair, and Leave group. Outside a group the left side says so.
 *
 * An archive's format, budget and selection are fixed; only the device itself can stop being
 * an archive. The size estimate is worked out on a thread as edits are made. Edits are
 * collected and applied together shortly after through the profile (get, change, set); a
 * failure shows as a toast and the page reloads. */
#ifndef KITHSONG_SYNC_VIEW_H
#define KITHSONG_SYNC_VIEW_H

#include <adwaita.h>

#include "sync_bridge.h"

/* What the page asks of the main window. */
typedef struct sync_view_callbacks {
  void (*pair)(void *user, const char *uid);         /* a nearby device was picked: pair with it */
  void (*add_device)(void *user, GtkWidget *parent); /* + : pair with a device by its address */
  void (*changed)(void *user);                       /* left the group, removed a member: refresh */
  void *user;
} sync_view_callbacks;

/* The page. Only one exists at a time. callbacks is copied. */
GtkWidget *sync_view_new(const sync_view_callbacks *callbacks);

/* Shows sync, loading this computer's profile and the library's lists. sync and lib must stay
 * valid until the next open, or until sync stops (open again with NULL, which applies edits
 * still waiting, waits for the size estimate's thread and closes the device dialog). */
void sync_view_open(mc_sync *sync, mc_library *lib);
/* Reloads the devices and the profile while the page shows (it does as it comes into view),
 * keeping edits not yet applied, and the device dialog. */
void sync_view_refresh(void);
/* Opens member uid's dialog: FALSE if it isn't a member. */
gboolean sync_view_open_device(const char *uid);
/* Scrolls to What to sync and puts the focus on its first row, as Choose what to sync brought
 * the page up for it. Waits for the page to be shown, so call it just after showing it. */
void sync_view_focus_what_to_sync(void);
/* Updates the progress, the strip and the rows' spinners from a sync event. */
void sync_view_on_sync_event(const sync_event *ev);
/* The strip and banner afresh, e.g. when the archive offer changed. */
void sync_view_update(void);

#endif
