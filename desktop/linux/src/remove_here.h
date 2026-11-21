/* Remove from this computer and Sync to this computer (kithcore/sync_here.h): an album or
 * artist leaves this computer, or comes back, while it stays in the library and on the other
 * devices. Neither shows while sync is off, outside a group or on an archive, where Delete from
 * library is the only honest option. Playback stops first if a queued track goes, as the bar
 * would go on showing a file that's gone.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog). */
#ifndef KITHSONG_REMOVE_HERE_H
#define KITHSONG_REMOVE_HERE_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* What to do just before files go: before_remove (may be NULL) gets the plan, e.g. to stop
 * playback. */
void remove_here_init(void (*before_remove)(const mc_here_plan *plan));

/* Appends Remove from this computer... (remove_action) or Sync to this computer
 * (sync_action) for uid, an album or artist by kind, to menu; nothing if neither applies. */
void remove_here_append(GMenu *menu, mc_profile_item_kind kind, const char *uid, const char *remove_action,
                        const char *sync_action);

/* Remove from this computer: asks over parent (any widget in the main window), then removes
 * uid named name and calls changed. */
void remove_here_present(GtkWidget *parent, mc_profile_item_kind kind, const char *uid, const char *name,
                         void (*changed)(void *user), void *user);

/* Sync to this computer: undoes the removal and starts a sync, then calls changed; a failure
 * is reported over parent. */
void remove_here_restore(GtkWidget *parent, mc_profile_item_kind kind, const char *uid,
                         void (*changed)(void *user), void *user);

#endif
