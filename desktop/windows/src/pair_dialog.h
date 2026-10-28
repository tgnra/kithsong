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
#ifndef KITHSONG_PAIR_DIALOG_H
#define KITHSONG_PAIR_DIALOG_H

#include <windows.h>

#include "musiccore/musiccore.h"
#include "sync_bridge.h"

#define IDC_PAIR_CODE 1201   /* the code: an edit (enter-code) or a label (show-code) */
#define IDC_PAIR_STATUS 1202 /* tries left, a new code, how the pairing ended */

/* What the dialogs need of the app. Every member runs on the UI thread. */
typedef struct pair_dialog_host {
  /* Shows a device's page (this PC's own uid for its own), as clicking it in the sidebar. */
  void (*show_device)(const char *uid);
  /* Create a New Library: asks for an empty folder and switches this PC to a new library
   * there, as Settings' Open Another Library does (sync restarts, which closes the pairing
   * dialogs). TRUE once it has. */
  BOOL (*new_library)(void);
  /* Asks the device to pair, as clicking it under Nearby does: by uid, or by host and port
   * when discovery doesn't see it (yet). host may be NULL. */
  void (*pair_again)(const char *uid, const wchar_t *name, const char *host, uint16_t port);
  /* TRUE the first time it's asked about key, which it records in settings.json. */
  BOOL (*first_time)(const char *key);
} pair_dialog_host;

/* The window the dialogs belong to, disabled while one is open, and the app's side (kept;
 * may be NULL, which leaves out the buttons that need it). Call once. */
void pair_dialog_init(HWND owner, const pair_dialog_host *host);
/* Opens the enter-code dialog once mc_sync_pair_request() (or _address) succeeded. peer
 * names the other device in the title; NULL or "" for one known only by address, which
 * address (NULL if not known) then gives. Six digits call mc_sync_pair_enter(); Cancel
 * calls mc_sync_pair_cancel(). */
void pair_dialog_begin_enter(mc_sync *sync, const wchar_t *peer, const wchar_t *address);
/* Where the enter-code dialog's pairing went (uid, or host and port; either may be NULL), so
 * Pair Again can ask the same device after a refusal that came before it said who it is. */
void pair_dialog_set_target(const char *uid, const char *host, uint16_t port);
/* Opens the show-code dialog for a SYNC_EV_PAIR_CODE, or updates its code when it's already
 * open (a new code after too many wrong tries), and flashes the taskbar button. */
void pair_dialog_show_code(mc_sync *sync, const sync_event *ev);
/* Shows a SYNC_EV_PAIR_RESULT in the dialog it belongs to. Returns TRUE if the devices
 * paired, so the caller refreshes the devices. The dialog then says nothing syncs yet, with
 * Choose What to Sync for the device that joined (and, once, on this PC if it joined, which
 * device is the archive). When this PC could have joined but has music of its own, it offers
 * Create a New Library, then to pair again. */
BOOL pair_dialog_on_result(const sync_event *ev);
/* For the message loop: IsDialogMessage for the open dialogs. TRUE if msg was handled. */
BOOL pair_dialog_translate(MSG *msg);
/* Closes both dialogs without cancelling (call before sync stops) and re-enables the owner. */
void pair_dialog_close_all(void);

#endif
