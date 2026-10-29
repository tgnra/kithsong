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

/* What the dialogs need of the app. Every member runs on the main thread. */
typedef struct pair_dialog_host {
  /* Shows a device's page (this computer's own uid for its own). */
  void (*show_device)(const char *uid);
  /* Create a New Library: asks over parent for an empty folder and switches this computer to
   * a new library there, as Settings' Open Another Library does (sync restarts, which closes
   * the pairing dialogs), then calls done(data, whether it switched). */
  void (*new_library)(GtkWidget *parent, void (*done)(gpointer data, gboolean switched), gpointer data);
  /* Asks the device to pair, as picking it under Unpaired Devices does: by uid, or by host and
   * port when discovery doesn't see it (yet). host may be NULL. */
  void (*pair_again)(const char *uid, const char *name, const char *host, guint16 port);
  /* TRUE the first time it's asked about key, which it records in settings.json. */
  gboolean (*first_time)(const char *key);
} pair_dialog_host;

/* The app's side (kept; NULL leaves out the buttons that need it). Call once. */
void pair_dialog_init(const pair_dialog_host *host);
/* Opens the enter-code dialog over parent once mc_sync_pair_request() (or _address)
 * succeeded. peer names the other device in the title; NULL or "" for one known only by
 * address, which address (NULL if not known) then gives. Six digits call
 * mc_sync_pair_enter(); Cancel calls mc_sync_pair_cancel(). */
void pair_dialog_begin_enter(GtkWidget *parent, mc_sync *sync, const char *peer, const char *address);
/* Where the enter-code dialog's pairing went (uid, or host and port; either may be NULL), so
 * Pair Again can ask the same device after a refusal that came before it said who it is. */
void pair_dialog_set_target(const char *uid, const char *host, guint16 port);
/* Opens the show-code dialog for a SYNC_EV_PAIR_CODE, or updates its code when it's already
 * open (a new code after too many wrong tries). */
void pair_dialog_show_code(GtkWidget *parent, mc_sync *sync, const sync_event *ev);
/* Shows a SYNC_EV_PAIR_RESULT in the dialog it belongs to (opening one to say why, if a
 * request was refused before any dialog showed). Returns TRUE if the devices paired, so the
 * caller refreshes the devices. The dialog then says nothing syncs yet, with Choose What to
 * Sync for the device that joined (and, once, on this computer if it joined, which device is
 * the archive). When this computer could have joined but has music of its own, it offers
 * Create a New Library, then to pair again. */
gboolean pair_dialog_on_result(GtkWidget *parent, const sync_event *ev);
/* Shows a SYNC_EV_PAIR_MERGE (both libraries have music, so pairing merges them) in the
 * dialog it belongs to, as on Windows. On this computer when its library is the one merged
 * (needs_confirm), the proposal says which library is kept, that this one is set aside as a
 * backup, and what goes with it (a Last.fm account, the paired devices), with Merge in the
 * header bar and Cancel, which call mc_sync_pair_merge_confirm(). The enter-code dialog
 * shows it in place of the code until answered (the code can be typed after); the show-code
 * dialog under the code, and Cancel there declines and closes it. On the computer whose
 * library is kept, it's a note under the code. The show-code dialog isn't open yet when it
 * arrives, so it's kept for it. */
void pair_dialog_on_merge(const sync_event *ev);
/* SYNC_EV_MERGE_PROGRESS and SYNC_EV_MERGE_DONE: the merge of this computer's old library,
 * after pairing, in a window of its own beside parent (not modal). Progress opens it (or
 * updates it); done turns it into a summary (opening it again if it was closed). */
void pair_dialog_on_merge_event(GtkWidget *parent, const sync_event *ev);
/* Closes the dialogs without cancelling: call before sync stops. */
void pair_dialog_close_all(void);

#endif
