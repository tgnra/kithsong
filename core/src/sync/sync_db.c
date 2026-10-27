#include "sync/sync_db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ecp.h>
#include <mbedtls/platform_util.h>

#include "util/platform.h"

extern const unsigned char mc_sync_schema_v1_sql[];

/* Migration i takes the database from user_version i to i + 1. Append only. */
static const char *const k_migrations[] = {
  (const char *)mc_sync_schema_v1_sql,
  /* v2: reaching peers without mDNS. devices.host is the device's host name, set by the
   * device itself. capabilities.addresses is a JSON array of "addr:port" strings where the
   * device listens, its address hints. peer_contact is this device's alone and never
   * synced: the last address each peer was reached at, and when we last synced with it. */
  "ALTER TABLE devices ADD COLUMN host TEXT;"
  "ALTER TABLE capabilities ADD COLUMN addresses TEXT;"
  "CREATE TABLE peer_contact ("
  "  device_uid   TEXT PRIMARY KEY,"
  "  address      TEXT,"
  "  port         INTEGER,"
  "  last_sync_ms INTEGER"
  ") WITHOUT ROWID;",
  /* v3: names the group gives its devices. profiles.name is the device's friendly name,
   * editable from any member like the rest of the profile (an archive's too); NULL means the
   * device's own devices.name. */
  "ALTER TABLE profiles ADD COLUMN name TEXT;",
  /* v4: scrobbling. scrobble_account is the group's one account (keyed by
   * MC_SCROBBLE_ACCOUNT_UID, so a device that signed in before joining merges with the
   * group's by last writer wins), with the service it's on: NULLs are Last.fm and the
   * build's API key. scrobble_queue is this device's alone and never synced: its plays not
   * yet submitted, with the metadata resolved when they were played. */
  "CREATE TABLE scrobble_account ("
  "  uid         TEXT PRIMARY KEY,"
  "  api_url     TEXT,"
  "  auth_url    TEXT,"
  "  api_key     TEXT,"
  "  api_secret  TEXT,"
  "  session_key TEXT,"
  "  username    TEXT"
  ") WITHOUT ROWID;"
  "CREATE TABLE scrobble_queue ("
  "  seq          INTEGER PRIMARY KEY,"
  "  played_at_s  INTEGER NOT NULL,"
  "  artist       TEXT NOT NULL,"
  "  track        TEXT NOT NULL,"
  "  album        TEXT,"
  "  album_artist TEXT,"
  "  track_no     INTEGER,"
  "  duration_s   INTEGER,"
  "  mb_trackid   TEXT"
  ");",
};
#define MC_SYNC_SCHEMA_VERSION ((int)(sizeof k_migrations / sizeof k_migrations[0]))

/* Columns whose changes are logged for group sync. Keep in step with the latest schema.
 * `local` is this device's alone and never logged. */
static const char *const k_group_cols[] = {"library_uid", NULL};
static const char *const k_device_cols[] = {"name", "kind", "public_key", "node_uid", "added_by",
                                            "removed", "host", NULL};
static const char *const k_profile_cols[] = {"archive", "mode", "format", "bitrate_kbps",
                                             "budget_bytes", "budget_enabled", "name", NULL};
static const char *const k_item_cols[] = {"device_uid", "kind", "ref_uid", NULL};
static const char *const k_capability_cols[] = {"cpu_score", "free_bytes", "on_power",
                                                "can_transcode", "updated_ms", "addresses", NULL};
static const char *const k_status_cols[] = {"verified_ms", "expected", "ok", "missing", "corrupt",
                                            "bytes", NULL};
static const char *const k_scrobble_cols[] = {"api_url", "auth_url", "api_key", "api_secret",
                                              "session_key", "username", NULL};

static const mc_changelog_table k_tracked[] = {
  {"sync_group", "uid", k_group_cols},
  {"devices", "uid", k_device_cols},
  {"profiles", "device_uid", k_profile_cols},
  {"profile_items", "uid", k_item_cols},
  {"capabilities", "device_uid", k_capability_cols},
  {"sync_status", "device_uid", k_status_cols},
  {"scrobble_account", "uid", k_scrobble_cols},
};

/* --- Schema --- */

static mc_status exec(mc_sync_db *sdb, const char *sql) {
  return sqlite3_exec(sdb->db, sql, NULL, NULL, NULL) == SQLITE_OK ? MC_OK : MC_ERR_DB;
}

static int user_version(sqlite3 *db) {
  sqlite3_stmt *stmt;
  int version = -1;
  if (sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return version;
}

static mc_status migrate(mc_sync_db *sdb) {
  int version = user_version(sdb->db);
  if (version < 0) return MC_ERR_DB;
  if (version > MC_SYNC_SCHEMA_VERSION) return MC_ERR_FORMAT; /* written by a newer app */

  for (int v = version; v < MC_SYNC_SCHEMA_VERSION; v++) {
    char bump[64];
    snprintf(bump, sizeof bump, "PRAGMA user_version = %d", v + 1);
    if (exec(sdb, "BEGIN IMMEDIATE") != MC_OK) return MC_ERR_DB;
    if (exec(sdb, k_migrations[v]) != MC_OK || exec(sdb, bump) != MC_OK) {
      exec(sdb, "ROLLBACK");
      return MC_ERR_DB;
    }
    if (exec(sdb, "COMMIT") != MC_OK) return MC_ERR_DB;
  }
  return MC_OK;
}

/* --- Identity --- */

static int os_random(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  return mc_random_bytes(buf, len) == MC_OK ? 0 : MBEDTLS_ERR_ECP_RANDOM_FAILED;
}

/* A fresh X25519 key pair as raw little-endian bytes (RFC 7748). The OS generator is
 * already a CSPRNG, so it feeds mbedTLS directly rather than through a DRBG. */
static mc_status generate_keypair(unsigned char priv[MC_SYNC_KEY_LEN],
                                  unsigned char pub[MC_SYNC_KEY_LEN]) {
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  size_t priv_len = 0, pub_len = 0;
  int rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_CURVE25519, &kp, os_random, NULL);
  if (rc == 0) rc = mbedtls_ecp_write_key_ext(&kp, &priv_len, priv, MC_SYNC_KEY_LEN);
  if (rc == 0)
    rc = mbedtls_ecp_write_public_key(&kp, MBEDTLS_ECP_PF_UNCOMPRESSED, &pub_len, pub,
                                      MC_SYNC_KEY_LEN);
  mbedtls_ecp_keypair_free(&kp);
  return rc == 0 && priv_len == MC_SYNC_KEY_LEN && pub_len == MC_SYNC_KEY_LEN ? MC_OK : MC_ERR_IO;
}

/* Reads the `local` row: MC_ERR_NOT_FOUND if there is none yet. */
static mc_status read_local(mc_sync_db *sdb) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(sdb->db,
                         "SELECT device_uid, private_key, public_key FROM local WHERE id = 1", -1,
                         &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  mc_status st = MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const unsigned char *uid = sqlite3_column_text(stmt, 0);
    const void *priv = sqlite3_column_blob(stmt, 1);
    int priv_len = sqlite3_column_bytes(stmt, 1);
    const void *pub = sqlite3_column_blob(stmt, 2);
    int pub_len = sqlite3_column_bytes(stmt, 2);
    if (uid && strlen((const char *)uid) == MC_UID_STRLEN - 1 && priv &&
        priv_len == MC_SYNC_KEY_LEN && pub && pub_len == MC_SYNC_KEY_LEN) {
      memcpy(sdb->device_uid, uid, MC_UID_STRLEN);
      memcpy(sdb->private_key, priv, MC_SYNC_KEY_LEN);
      memcpy(sdb->public_key, pub, MC_SYNC_KEY_LEN);
      st = MC_OK;
    } else {
      st = MC_ERR_FORMAT;
    }
  } else if (rc == SQLITE_DONE) {
    st = MC_ERR_NOT_FOUND;
  }
  sqlite3_finalize(stmt);
  return st;
}

/* First open: this device gets its uid and key pair. INSERT OR IGNORE and a reread, so if
 * two connections race, both end up with whichever row landed first. */
static mc_status load_local(mc_sync_db *sdb) {
  mc_status st = read_local(sdb);
  if (st != MC_ERR_NOT_FOUND) return st;

  char uid[MC_UID_STRLEN];
  unsigned char priv[MC_SYNC_KEY_LEN], pub[MC_SYNC_KEY_LEN];
  if ((st = mc_uid_new(uid)) != MC_OK) return st;
  if ((st = generate_keypair(priv, pub)) != MC_OK) return st;

  sqlite3_stmt *stmt;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(sdb->db,
                         "INSERT OR IGNORE INTO local (id, device_uid, private_key, public_key)"
                         " VALUES (1, ?1, ?2, ?3)",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 2, priv, MC_SYNC_KEY_LEN, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 3, pub, MC_SYNC_KEY_LEN, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  mbedtls_platform_zeroize(priv, sizeof priv);
  return st == MC_OK ? read_local(sdb) : st;
}

/* --- API --- */

mc_status mc_sync_db_open(const char *config_dir, mc_sync_db **out) {
  if (!config_dir || !*config_dir || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;

  mc_sync_db *sdb = calloc(1, sizeof *sdb);
  if (!sdb) return MC_ERR_NOMEM;
  mc_hlc_init(&sdb->hlc, mc_wall_ms, 0);
  sdb->log.hlc = &sdb->hlc;
  sdb->log.node_uid = sdb->device_uid;
  sdb->log.applying_remote = &sdb->applying_remote;

  size_t len = strlen(config_dir);
  while (len > 1 && (config_dir[len - 1] == '/' || config_dir[len - 1] == '\\')) len--;
  char *dir = malloc(len + 1);
  char *db_path = malloc(len + sizeof MC_SYNC_DB_NAME + 1);
  mc_status st = MC_ERR_NOMEM;
  if (!dir || !db_path) goto fail;
  memcpy(dir, config_dir, len);
  dir[len] = '\0';
  snprintf(db_path, len + sizeof MC_SYNC_DB_NAME + 1, "%s%c" MC_SYNC_DB_NAME, dir, MC_PATH_SEP);

  if ((st = mc_mkdirs(dir)) != MC_OK) goto fail;
  st = MC_ERR_DB;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_open_v2(db_path, &sdb->db, flags, NULL) != SQLITE_OK) goto fail;

  sqlite3_busy_timeout(sdb->db, 5000);
  if ((st = exec(sdb, "PRAGMA journal_mode = WAL;"
                      "PRAGMA synchronous = NORMAL;"
                      "PRAGMA trusted_schema = OFF;")) != MC_OK)
    goto fail;
  if ((st = mc_changelog_register(sdb->db, &sdb->log)) != MC_OK) goto fail;
  if ((st = migrate(sdb)) != MC_OK) goto fail;
  if ((st = mc_changelog_install_triggers(sdb->db, k_tracked,
                                          sizeof k_tracked / sizeof k_tracked[0])) != MC_OK)
    goto fail;
  if ((st = load_local(sdb)) != MC_OK) goto fail;
  mc_hlc_init(&sdb->hlc, mc_wall_ms, mc_changelog_max_hlc(sdb->db));

  free(dir);
  free(db_path);
  *out = sdb;
  return MC_OK;

fail:
  if (sdb->db) fprintf(stderr, "musiccore: sync.db open failed: %s\n", sqlite3_errmsg(sdb->db));
  free(dir);
  free(db_path);
  mc_sync_db_close(sdb);
  return st;
}

void mc_sync_db_close(mc_sync_db *sdb) {
  if (!sdb) return;
  sqlite3_close_v2(sdb->db);
  mbedtls_platform_zeroize(sdb->private_key, sizeof sdb->private_key);
  free(sdb);
}

int mc_sync_db_schema_version(const mc_sync_db *sdb) {
  return user_version(sdb->db);
}

const char *mc_sync_db_device_uid(const mc_sync_db *sdb) {
  return sdb->device_uid;
}

const unsigned char *mc_sync_db_public_key(const mc_sync_db *sdb) {
  return sdb->public_key;
}

const unsigned char *mc_sync_db_private_key(const mc_sync_db *sdb) {
  return sdb->private_key;
}

const char *mc_sync_db_group_uid(mc_sync_db *sdb) {
  sqlite3_stmt *stmt;
  sdb->group_uid[0] = '\0';
  if (sqlite3_prepare_v2(sdb->db, "SELECT group_uid FROM local WHERE id = 1", -1, &stmt, NULL) !=
      SQLITE_OK)
    return NULL;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const unsigned char *v = sqlite3_column_text(stmt, 0);
    if (v && strlen((const char *)v) == MC_UID_STRLEN - 1) memcpy(sdb->group_uid, v, MC_UID_STRLEN);
  }
  sqlite3_finalize(stmt);
  return sdb->group_uid[0] ? sdb->group_uid : NULL;
}

mc_status mc_sync_db_begin_write(mc_sync_db *sdb) {
  return mc_changelog_begin_write(sdb->db, &sdb->hlc);
}

mc_status mc_sync_db_commit(mc_sync_db *sdb) {
  return exec(sdb, "COMMIT");
}

void mc_sync_db_rollback(mc_sync_db *sdb) {
  exec(sdb, "ROLLBACK");
}

mc_status mc_sync_db_version_vector(mc_sync_db *sdb, mc_origin_hlc **out, int *count) {
  if (!sdb) return MC_ERR_INVALID_ARG;
  return mc_changelog_version_vector(sdb->db, out, count);
}

const mc_changelog_table *mc_sync_db_tracked(size_t *count) {
  *count = sizeof k_tracked / sizeof k_tracked[0];
  return k_tracked;
}

mc_status mc_sync_db_clear_group(mc_sync_db *sdb) {
  if (!sdb) return MC_ERR_INVALID_ARG;
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  /* Not logged: this is forgetting the group, not a change to send anyone. */
  sdb->applying_remote = 1;
  char sql[256];
  for (size_t i = 0; i < sizeof k_tracked / sizeof k_tracked[0] && st == MC_OK; i++) {
    /* This device's own capabilities stay: they hold the cached benchmark. The scrobbling
     * account stays too: leaving a group doesn't sign you out. */
    if (!strcmp(k_tracked[i].table, "scrobble_account")) continue;
    if (!strcmp(k_tracked[i].table, "capabilities"))
      snprintf(sql, sizeof sql, "DELETE FROM capabilities WHERE device_uid <> '%s'",
               sdb->device_uid);
    else
      snprintf(sql, sizeof sql, "DELETE FROM %s", k_tracked[i].table);
    st = exec(sdb, sql);
  }
  if (st == MC_OK) {
    snprintf(sql, sizeof sql,
             "DELETE FROM changelog WHERE NOT (entity = 'capabilities' AND row_uid = '%s'"
             " AND origin_uid = '%s') AND entity <> 'scrobble_account'",
             sdb->device_uid, sdb->device_uid);
    st = exec(sdb, sql);
  }
  if (st == MC_OK) st = exec(sdb, "DELETE FROM peer_contact");
  if (st == MC_OK) st = exec(sdb, "UPDATE local SET group_uid = NULL WHERE id = 1");
  sdb->applying_remote = 0;
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  if (st == MC_OK) sdb->group_uid[0] = '\0';
  return st;
}
