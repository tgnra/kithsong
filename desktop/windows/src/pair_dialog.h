/* The pairing dialogs. Pairing takes two: the device asked to pair shows a code, and the
 * device asking (the initiator) has the user type it in:
 *
 *   +- Pair with Living room PC -------- X -+   +- Pairing request --------------- X -+
 *   | Enter the code Living room PC shows.  |   | Kitchen laptop wants to pair. Type |
 *   |                                       |   | this code on it:                   |
 *   | [      ]                              |   |                                    |
 *   | 2 tries left                          |   |              123 456               |
 *   |                     [Pair] [Cancel]   |   |                           [Cancel] |
 *   +---------------------------------------+   +------------------------------------+
 *
 * Both are modeless, with the main window disabled while one is open, and both can be open
 * at once (this PC can be pairing each way). Each shows how its pairing ended, worded as on
 * Android. The code and the status line have fixed ids, so test scripts can drive the
 * dialogs. The templates are built in memory, as in add_device.c, so they need no
 * resources. */
#ifndef MUSICAPP_PAIR_DIALOG_H
#define MUSICAPP_PAIR_DIALOG_H

#include <windows.h>

#include "musiccore/musiccore.h"
#include "sync_bridge.h"

#define IDC_PAIR_CODE 1201   /* the code: an edit (enter-code) or a label (show-code) */
#define IDC_PAIR_STATUS 1202 /* tries left, a new code, how the pairing ended */

/* The window the dialogs belong to, disabled while one is open. Call once. */
void pair_dialog_init(HWND owner);
/* Opens the enter-code dialog once mc_sync_pair_request() (or _address) succeeded. peer
 * names the other device in the title; NULL or "" for one known only by address. Six
 * digits call mc_sync_pair_enter(); Cancel calls mc_sync_pair_cancel(). */
void pair_dialog_begin_enter(mc_sync *sync, const wchar_t *peer);
/* Opens the show-code dialog for a SYNC_EV_PAIR_CODE, or updates its code when it's already
 * open (a new code after too many wrong tries), and flashes the taskbar button. */
void pair_dialog_show_code(mc_sync *sync, const sync_event *ev);
/* Shows a SYNC_EV_PAIR_RESULT in the dialog it belongs to. Returns TRUE if the devices
 * paired, so the caller refreshes and opens the device's page. */
BOOL pair_dialog_on_result(const sync_event *ev);
/* For the message loop: IsDialogMessage for the open dialogs. TRUE if msg was handled. */
BOOL pair_dialog_translate(MSG *msg);
/* Closes both dialogs without cancelling (call before sync stops) and re-enables the owner. */
void pair_dialog_close_all(void);

#endif
