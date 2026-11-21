/* What keeps sync from working, and how to fix it: the issues of mc_sync_get_problems(), each
 * as mc_sync_issue_describe() words it, in a prompt dialog. Opened from the problems strip's
 * Details, a device's page and the finished line of a sync that left tracks missing. Tracks
 * missing for good get Remove from Library (delete_dialog_lost()); a lost archive gets Make
 * this computer the archive, which opens the takeover offer. Closing it dismisses the
 * notices it showed.
 *
 * The takeover offer (mc_sync_get_archive_offer()) pops up once by itself
 * (issue_dialog_check_offer()); afterwards the lost archive's page has a button for it. */
#ifndef KITHSONG_ISSUE_DIALOG_H
#define KITHSONG_ISSUE_DIALOG_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* Shows the issues about device_uid, or all of them for NULL, modal to owner. TRUE if the
 * library changed (lost tracks were removed). */
BOOL issue_dialog_show(HWND owner, mc_library *lib, mc_sync *sync, const char *device_uid);

/* The takeover offer for the lost archive device_uid, as a dialog whose button starts taking
 * over; says so if there's no offer any more. */
void issue_dialog_offer(HWND owner, mc_sync *sync, const char *device_uid);

/* After the problems change: shows the takeover offer if one has never been shown, unless a
 * dialog is open already. */
void issue_dialog_check_offer(HWND owner, mc_sync *sync);

/* Whether the problems hold an issue about device_uid; *offer: one of them offers to take
 * over its role here. offer may be NULL. */
BOOL issue_dialog_has_issues(mc_sync *sync, const char *device_uid, BOOL *offer);

#endif
