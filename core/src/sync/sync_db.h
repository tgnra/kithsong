/* sync.db: this device's sync identity and the group's replicated state (devices, profiles,
 * capabilities, status), with its own changelog and clock. Kept apart from library.db, which
 * holds no sync data. Internal to core; see docs/sync-protocol.md. */
#ifndef MC_SYNC_DB_H
#define MC_SYNC_DB_H

#include <sqlite3.h>

#include "musiccore/musiccore.h"
#include "util/changelog.h"
#include "util/hlc.h"

/* In the per-device config dir, next to settings.json. */
#define MC_SYNC_DB_NAME "sync.db"

/* The uid of the one scrobble_account row. The same on every device, so accounts signed in
 * on two of them are one row, merged by last writer wins. */
#define MC_SCROBBLE_ACCOUNT_UID "00000000-0000-0000-0000-000000000000"

/* Raw X25519 key length, public and private. */
#define MC_SYNC_KEY_LEN 32

typedef struct mc_sync_db {
  sqlite3 *db;
  mc_hlc hlc;
  /* This device. device_uid is also the origin of every change logged here. */
  char device_uid[MC_UID_STRLEN];
  unsigned char private_key[MC_SYNC_KEY_LEN];
  unsigned char public_key[MC_SYNC_KEY_LEN];
  char group_uid[MC_UID_STRLEN]; /* "" while not in a group */
  /* Set while applying changes received from a peer, so triggers don't log them a second
   * time as local edits; sync inserts those changelog rows itself. */
  int applying_remote;
  mc_changelog_ctx log; /* points the trigger functions at hlc, device_uid, applying_remote */
} mc_sync_db;

/* Opens <config_dir>/sync.db, creating the folder and database if needed, and migrates it.
 * The first open gives this device its uid and X25519 key pair. */
mc_status mc_sync_db_open(const char *config_dir, mc_sync_db **out);
void mc_sync_db_close(mc_sync_db *sdb);

static inline sqlite3 *mc_sync_db_handle(mc_sync_db *sdb) {
  return sdb->db;
}

/* PRAGMA user_version. */
int mc_sync_db_schema_version(const mc_sync_db *sdb);

const char *mc_sync_db_device_uid(const mc_sync_db *sdb);
/* MC_SYNC_KEY_LEN bytes each. */
const unsigned char *mc_sync_db_public_key(const mc_sync_db *sdb);
const unsigned char *mc_sync_db_private_key(const mc_sync_db *sdb);

/* The group this device belongs to, or NULL if none. Rereads local.group_uid, so it's
 * current even if another connection changed it. Valid until the next call. */
const char *mc_sync_db_group_uid(mc_sync_db *sdb);

/* Every write to replicated tables goes through these, as with mc_library_begin_write(). */
mc_status mc_sync_db_begin_write(mc_sync_db *sdb);
mc_status mc_sync_db_commit(mc_sync_db *sdb);
void mc_sync_db_rollback(mc_sync_db *sdb);

/* The group changelog's version vector; see mc_changelog_version_vector(). */
mc_status mc_sync_db_version_vector(mc_sync_db *sdb, mc_origin_hlc **out, int *count);

/* The replicated tables and their logged columns. */
const mc_changelog_table *mc_sync_db_tracked(size_t *count);

/* Forgets the group, after leaving it or being removed: clears local.group_uid and deletes
 * every replicated row and the group changelog without logging it, so nothing of the old
 * group reaches the next one. Only this device's capabilities row (its cached benchmark)
 * and its own changes to it are kept, and the scrobbling account with its changes: leaving
 * doesn't sign the device out, and the account goes with it to the next group. */
mc_status mc_sync_db_clear_group(mc_sync_db *sdb);

/* Merging libraries, on the device whose library isn't kept (sync/pairing.c). */
/* Signs this device out without telling anyone: the scrobbling account, its changes and the
 * plays still queued are deleted unlogged, so the kept group's account arrives whole in the
 * next session instead of merging field by field with this one. *dropped (may be NULL): how
 * many queued plays went with it. */
mc_status mc_sync_db_forget_scrobble(mc_sync_db *sdb, int *dropped);
/* Logs every field of the scrobbling account again, stamped now, so that it wins over any
 * older change the kept group has to the account (e.g. a sign-out) when they meet. */
mc_status mc_sync_db_restamp_scrobble(mc_sync_db *sdb);

#endif
