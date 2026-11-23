/* A group member's details and sync profile, read-only, in a dialog over the Sync page
 * (sync_view.h): whether it's online and when it last synced, its problems, its own report of
 * its files, its address, whether this computer and it can reach each other, and what it
 * syncs in what format, which only the device itself can change. Remove from group takes it
 * out of the group, but for an archive, which the core refuses. One at a time; it follows the
 * device as sync events come in, and closes if it leaves the group. */
#ifndef KITHSONG_DEVICE_DIALOG_H
#define KITHSONG_DEVICE_DIALOG_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* Shows member uid's dialog over parent: FALSE if it isn't a member. sync must stay valid
 * until device_dialog_close(). Once Remove from group has removed it, calls removed(user). */
gboolean device_dialog_present(GtkWidget *parent, mc_sync *sync, const char *uid, void (*removed)(void *user),
                               void *user);
/* Reads the device afresh, if the dialog is open. */
void device_dialog_refresh(void);
/* Closes it: before sync stops. */
void device_dialog_close(void);

#endif
