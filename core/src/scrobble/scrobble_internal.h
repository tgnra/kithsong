/* What sync needs of the scrobbler when it merges libraries (see "Merging libraries" in
 * docs/sync-protocol.md): whether this device's sync.db is signed in, and one attempt to
 * send its queued plays under that account before it is signed out. Internal to core. */
#ifndef MC_SCROBBLE_INTERNAL_H
#define MC_SCROBBLE_INTERNAL_H

#include <sqlite3.h>

#include "musiccore/musiccore.h"

/* 1 if db (a sync.db connection) holds a signed-in scrobbling account, with *username (may
 * be NULL) set to a malloc'd copy of its user name ("" if it has none); else 0. */
int mc_scrobble_signed_in(sqlite3 *db, char **username);

/* One attempt to send every play in db's scrobble_queue under db's account: batches until
 * the queue is empty or a call fails (no retries, no backoff). Plays sent are taken off the
 * queue. *sent and *left (either may be NULL) say how many went and how many are still
 * queued. MC_ERR_AUTH if not signed in; otherwise the status of the call that failed. */
mc_status mc_scrobble_flush_once(sqlite3 *db, int *sent, int *left);

#endif
