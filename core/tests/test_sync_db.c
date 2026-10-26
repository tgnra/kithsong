#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ecp.h>

#include "musiccore/musiccore.h"
#include "sync/sync_db.h"
#include "test.h"
#include "util/platform.h"

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int changelog_count(sqlite3 *db, const char *where) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE %s", where);
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static void remove_db(const char *dir) {
  const char *const names[] = {MC_SYNC_DB_NAME, MC_SYNC_DB_NAME "-wal", MC_SYNC_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

static int os_random(void *ctx, unsigned char *buf, size_t len) {
  (void)ctx;
  return mc_random_bytes(buf, len) == MC_OK ? 0 : -1;
}

/* The public key is X25519(private, 9): recompute it independently of how it was made. */
static int keys_match(const unsigned char *priv, const unsigned char *pub) {
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  unsigned char derived[MC_SYNC_KEY_LEN];
  size_t len = 0;
  int rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_CURVE25519, &kp, priv, MC_SYNC_KEY_LEN);
  if (rc == 0) rc = mbedtls_ecp_keypair_calc_public(&kp, os_random, NULL);
  if (rc == 0)
    rc = mbedtls_ecp_write_public_key(&kp, MBEDTLS_ECP_PF_UNCOMPRESSED, &len, derived,
                                      sizeof derived);
  mbedtls_ecp_keypair_free(&kp);
  return rc == 0 && len == MC_SYNC_KEY_LEN && memcmp(derived, pub, MC_SYNC_KEY_LEN) == 0;
}

static int all_zero(const unsigned char *p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return 0;
  return 1;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  char path[1024], path2[1024];
  snprintf(path, sizeof path, "%s/test_sync_db.d", scratch);
  snprintf(path2, sizeof path2, "%s/test_sync_db_other.d", scratch);
  remove_db(path);
  remove_db(path2);

  /* First open gives the device its uid and key pair. */
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(path, &sdb) == MC_OK);
  if (!sdb) return TEST_RESULT();
  CHECK(mc_sync_db_schema_version(sdb) == 2);
  char device[MC_UID_STRLEN];
  unsigned char priv[MC_SYNC_KEY_LEN], pub[MC_SYNC_KEY_LEN];
  strcpy(device, mc_sync_db_device_uid(sdb));
  memcpy(priv, mc_sync_db_private_key(sdb), sizeof priv);
  memcpy(pub, mc_sync_db_public_key(sdb), sizeof pub);
  CHECK(strlen(device) == MC_UID_STRLEN - 1);
  CHECK(!all_zero(priv, sizeof priv) && !all_zero(pub, sizeof pub));
  CHECK(keys_match(priv, pub));
  CHECK(mc_sync_db_group_uid(sdb) == NULL);

  sqlite3 *db = mc_sync_db_handle(sdb);
  /* `local` is this device's alone: creating it and joining a group aren't logged. */
  CHECK(changelog_count(db, "1") == 0);
  CHECK(exec(db, "UPDATE local SET group_uid = '0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e'") ==
        SQLITE_OK);
  CHECK_STR(mc_sync_db_group_uid(sdb), "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e");
  CHECK(changelog_count(db, "1") == 0);

  /* Replicated tables are logged field by field, with this device as origin, keyed by uid
   * or, for per-device tables, by device_uid. NULLs are skipped; defaults are logged. */
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  CHECK(exec(db, "INSERT INTO sync_group (uid, library_uid) VALUES ('g1', 'lib1')") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO devices (uid, name, kind, public_key) VALUES"
                 " ('d1', 'Desk', 'desktop', x'0102')") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO profiles (device_uid, archive) VALUES ('d1', 1)") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO profile_items (uid, device_uid, kind, ref_uid) VALUES"
                 " ('i1', 'd1', 'album', 'al1')") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO capabilities (device_uid, cpu_score) VALUES ('d1', 40)") ==
        SQLITE_OK);
  CHECK(exec(db, "INSERT INTO sync_status (device_uid, expected, ok) VALUES ('d1', 3, 2)") ==
        SQLITE_OK);
  CHECK(mc_sync_db_commit(sdb) == MC_OK);

  CHECK(changelog_count(db, "entity = 'sync_group' AND row_uid = 'g1' AND field = 'library_uid'"
                            " AND value = 'lib1'") == 1);
  CHECK(changelog_count(db, "entity = 'devices' AND row_uid = 'd1'") == 4); /* + removed = 0 */
  CHECK(changelog_count(db, "entity = 'devices' AND field = 'public_key' AND value = x'0102'") ==
        1);
  CHECK(changelog_count(db, "entity = 'devices' AND field = 'node_uid'") == 0);
  /* archive, and the defaults for mode, format, budget_enabled */
  CHECK(changelog_count(db, "entity = 'profiles' AND row_uid = 'd1'") == 4);
  CHECK(changelog_count(db, "entity = 'profiles' AND field = 'mode' AND value = 'all'") == 1);
  CHECK(changelog_count(db, "entity = 'profile_items' AND row_uid = 'i1'") == 3);
  CHECK(changelog_count(db, "entity = 'capabilities' AND row_uid = 'd1' AND value = 40") == 1);
  CHECK(changelog_count(db, "entity = 'sync_status' AND row_uid = 'd1'") == 2);
  CHECK(changelog_count(db, "origin_uid <> mc_node_uid()") == 0);
  CHECK(changelog_count(db, "origin_uid = (SELECT device_uid FROM local)") == 15);

  /* Unchanged values aren't logged; changed ones are, once; deletes leave a tombstone. */
  CHECK(exec(db, "UPDATE devices SET name = 'Desk' WHERE uid = 'd1'") == SQLITE_OK);
  CHECK(changelog_count(db, "entity = 'devices' AND field = 'name'") == 1);
  CHECK(exec(db, "UPDATE profiles SET mode = 'selected' WHERE device_uid = 'd1'") == SQLITE_OK);
  CHECK(changelog_count(db, "entity = 'profiles' AND field = 'mode' AND value = 'selected'") == 1);
  CHECK(exec(db, "DELETE FROM profile_items WHERE uid = 'i1'") == SQLITE_OK);
  CHECK(changelog_count(db, "row_uid = 'i1' AND field = '_deleted' AND value = 1") == 1);
  CHECK(exec(db, "DELETE FROM capabilities WHERE device_uid = 'd1'") == SQLITE_OK);
  CHECK(changelog_count(db, "entity = 'capabilities' AND row_uid = 'd1' AND field = '_deleted'") ==
        1);

  /* Timestamps are strictly increasing. */
  CHECK(changelog_count(db, "hlc <= (SELECT max(hlc) FROM changelog c2 WHERE c2.seq < changelog.seq)") == 0);

  /* Changes being applied from a peer are not re-logged by the triggers. */
  int before = changelog_count(db, "1");
  sdb->applying_remote = 1;
  CHECK(exec(db, "INSERT INTO devices (uid, name) VALUES ('d2', 'Phone')") == SQLITE_OK);
  CHECK(exec(db, "UPDATE profiles SET archive = 0 WHERE device_uid = 'd1'") == SQLITE_OK);
  CHECK(exec(db, "DELETE FROM sync_status WHERE device_uid = 'd1'") == SQLITE_OK);
  sdb->applying_remote = 0;
  CHECK(changelog_count(db, "1") == before);

  /* The version vector is the newest hlc per origin, here this device and a peer whose rows
   * sync inserted itself. A later local write's clock catches up past what it has seen. */
  uint64_t ahead = (mc_wall_ms() + 3600 * 1000) << MC_HLC_COUNTER_BITS;
  char sql[512];
  snprintf(sql, sizeof sql,
           "INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value) VALUES"
           " (5, 'ffffffff-ffff-7fff-bfff-ffffffffffff', 'devices', 'd2', 'name', 'Phone'),"
           " (%llu, 'ffffffff-ffff-7fff-bfff-ffffffffffff', 'devices', 'd2', 'kind', 'phone')",
           (unsigned long long)ahead);
  CHECK(exec(db, sql) == SQLITE_OK);
  sqlite3_stmt *stmt;
  uint64_t local_max = 0;
  if (sqlite3_prepare_v2(db, "SELECT max(hlc) FROM changelog WHERE origin_uid = mc_node_uid()", -1,
                         &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) local_max = (uint64_t)sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  mc_origin_hlc *vv = NULL;
  int count = -1;
  CHECK(mc_sync_db_version_vector(sdb, &vv, &count) == MC_OK && count == 2);
  if (vv && count == 2) {
    CHECK(strcmp(vv[0].origin_uid, device) == 0); /* ordered by origin; the peer's sorts last */
    CHECK(vv[0].hlc == local_max && local_max > 0);
    CHECK_STR(vv[1].origin_uid, "ffffffff-ffff-7fff-bfff-ffffffffffff");
    CHECK(vv[1].hlc == ahead);
  }
  free(vv);

  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  CHECK(exec(db, "UPDATE devices SET name = 'Desktop' WHERE uid = 'd1'") == SQLITE_OK);
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  snprintf(sql, sizeof sql, "field = 'name' AND value = 'Desktop' AND hlc > %llu",
           (unsigned long long)ahead);
  CHECK(changelog_count(db, sql) == 1);

  mc_sync_db_close(sdb);

  /* Reopening keeps the identity and the group. */
  sdb = NULL;
  CHECK(mc_sync_db_open(path, &sdb) == MC_OK);
  if (sdb) {
    CHECK(strcmp(mc_sync_db_device_uid(sdb), device) == 0);
    CHECK(memcmp(mc_sync_db_private_key(sdb), priv, sizeof priv) == 0);
    CHECK(memcmp(mc_sync_db_public_key(sdb), pub, sizeof pub) == 0);
    CHECK_STR(mc_sync_db_group_uid(sdb), "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e");
    CHECK(mc_sync_db_schema_version(sdb) == 2);
    mc_sync_db_close(sdb);
  }

  /* Another device (config dir) is someone else, and starts with an empty vector. */
  sdb = NULL;
  CHECK(mc_sync_db_open(path2, &sdb) == MC_OK);
  if (sdb) {
    CHECK(strcmp(mc_sync_db_device_uid(sdb), device) != 0);
    CHECK(memcmp(mc_sync_db_public_key(sdb), pub, sizeof pub) != 0);
    CHECK(keys_match(mc_sync_db_private_key(sdb), mc_sync_db_public_key(sdb)));
    vv = NULL;
    count = -1;
    CHECK(mc_sync_db_version_vector(sdb, &vv, &count) == MC_OK && count == 0 && vv == NULL);
    mc_sync_db_close(sdb);
  }

  return TEST_RESULT();
}
