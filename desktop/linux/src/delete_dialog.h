/* Deleting albums and tracks from the library (kithcore/delete.h), after asking. Afterwards a
 * dialog says if any track stayed because its file couldn't go to the Trash (open elsewhere,
 * or no trash Kithsong can use or make on its file system). Restoring is mc_delete_restore(),
 * which main.c runs at startup and whenever the window becomes active.
 *
 * Needs libadwaita 1.5 (AdwAlertDialog).
 */
#ifndef KITHSONG_DELETE_DIALOG_H
#define KITHSONG_DELETE_DIALOG_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* What to do just before files go: before_delete (may be NULL) gets the plan, e.g. to stop
 * playing a track in it, which would otherwise play on from its trashed file. Call once at
 * startup. */
void delete_dialog_init(void (*before_delete)(const mc_delete_plan *plan));

/* Asks over parent (any widget in the main window) about deleting uid (an album or track,
 * by kind) named name, with deleted.db in config_dir, and calls changed once anything was
 * deleted. A failure is reported in a dialog of its own. */
void delete_dialog_present(GtkWidget *parent, mc_library *lib, const char *config_dir, mc_delete_kind kind,
                           const char *uid, const char *name, void (*changed)(void *user), void *user);

/* Asks over parent about removing the tracks that are lost as of now, on every device, and
 * calls changed once any went. */
void delete_dialog_lost(GtkWidget *parent, mc_library *lib, void (*changed)(void *user), void *user);

#endif
