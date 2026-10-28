/* The pairing dialogs (the counterpart of desktop/windows/src/pair_dialog.c). Pairing takes
 * two: the device asked to pair shows a code, and the device asking (the initiator) has the
 * user type it in. Each is an AdwDialog with its buttons in the header bar:
 *
 *   +--------------------------------------+   +--------------------------------------+
 *   | [Cancel]  Pair with Kitchen    [Pair]|   | [Cancel]   Pairing Request           |
 *   |                                      |   |                                      |
 *   |   Enter the code Kitchen shows.      |   |  To pair, enter this code on Laptop. |
 *   |             [ 123456 ]               |   |               123 456                |
 *   |          2 tries left.               |   |                                      |
 *   +--------------------------------------+   +--------------------------------------+
 *
 * Both can be open at once (this computer can be pairing each way). Each turns into a
 * status-page layout saying how its pairing ended, worded as on Windows and Android. Closing a
 * dialog before the end (Cancel, Escape) cancels the pairing. */
#ifndef KITHSONG_PAIR_DIALOG_H
#define KITHSONG_PAIR_DIALOG_H

#include <adwaita.h>

#include "sync_bridge.h"

/* Opens the enter-code dialog over parent once mc_sync_pair_request() (or _address)
 * succeeded. peer names the other device in the title; NULL or "" for one known only by
 * address, which address (NULL if not known) then gives. Six digits call
 * mc_sync_pair_enter(); Cancel calls mc_sync_pair_cancel(). */
void pair_dialog_begin_enter(GtkWidget *parent, mc_sync *sync, const char *peer, const char *address);
/* Opens the show-code dialog for a SYNC_EV_PAIR_CODE, or updates its code when it's already
 * open (a new code after too many wrong tries). */
void pair_dialog_show_code(GtkWidget *parent, mc_sync *sync, const sync_event *ev);
/* Shows a SYNC_EV_PAIR_RESULT in the dialog it belongs to (opening one to say why, if a
 * request was refused before any dialog showed). Returns TRUE if the devices paired, so the
 * caller refreshes and opens the device's page. */
gboolean pair_dialog_on_result(GtkWidget *parent, const sync_event *ev);
/* Closes both dialogs without cancelling: call before sync stops. */
void pair_dialog_close_all(void);

#endif
