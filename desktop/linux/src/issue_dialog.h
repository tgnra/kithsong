/* Sync problems in words (the counterpart of desktop/windows/src/issue_dialog.c): one dialog
 * listing mc_sync_get_problems()'s issues, each with its title, what happened and how to fix
 * it (mc_sync_issue_describe()), opened from the problems banner, a device's page and the
 * finished bar; and the offer to make this computer the archive in place of a lost one
 * (mc_sync_get_archive_offer()), shown once by itself and then from the lost device's page.
 * Closing the problems dialog forgets the notices (mc_sync_dismiss_notices()). Everything runs
 * on the main thread; the offer is read on a thread of its own, as reading it reads every
 * track. */
#ifndef KITHSONG_ISSUE_DIALOG_H
#define KITHSONG_ISSUE_DIALOG_H

#include "app.h"

/* Sync started for a's library (devices_start()). */
void issue_dialog_start(app *a);
/* Before sync stops: closes the dialogs and waits for the offer's read. */
void issue_dialog_stop(void);

/* Opens the dialog over parent (NULL: the window) with every issue, or only device_uid's. */
void issue_dialog_present(GtkWidget *parent, const char *device_uid);
/* The problems changed (SYNC_EV_WAITING): the open dialog shows them afresh, and the offer is
 * read again shortly, popping up if it never has. */
void issue_dialog_refresh(void);
/* How many issues concern device_uid (all of them for NULL), and the first one's title. */
int issue_dialog_count(const char *device_uid, char *title, size_t size);

/* The lost archive on offer as of the last read, or NULL. Valid until the next refresh. */
const mc_sync_archive_offer *issue_dialog_offer(void);
/* Shows the offer as a dialog over parent (NULL: the window), if there is one. */
void issue_dialog_present_offer(GtkWidget *parent);

#endif
