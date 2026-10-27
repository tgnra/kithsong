/* Archives and the void-demotion rule. See archive.h and "Archives" in
 * docs/sync-protocol.md.
 *
 * Only the device itself may clear its archive flag, and only while another archive is
 * verified complete (profile.c), so a single device can't leave the group without one. Two
 * devices can still do it at once, each relying on the other: once their rows meet, no
 * stored flag is left. The rule then voids the oldest of the demotions in effect. Every
 * device reads it from the same changelog rows, so they agree on the device without talking
 * about it, and that device writes the flag again so the stored state catches up. */
#include "sync/archive.h"

#include <stdio.h>
#include <string.h>

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* Current members whose stored flag is set; -1 on error. */
static int stored_archives(sqlite3 *db) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT count(*) FROM profiles p JOIN devices d ON d.uid = p.device_uid"
                         " WHERE p.archive <> 0 AND d.removed = 0",
                         -1, &stmt, NULL) != SQLITE_OK)
    return -1;
  int n = sqlite3_step(stmt) == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return n;
}

mc_status mc_archive_void(sqlite3 *db, char out[MC_UID_STRLEN]) {
  out[0] = '\0';
  int n = stored_archives(db);
  if (n < 0) return MC_ERR_DB;
  if (n > 0) return MC_OK;
  /* Each current member's winning archive change (no later one for its row), where it is a
   * 0 that follows a 1: the demotions in effect. The oldest by (hlc, origin_uid) is void. A
   * 0 with no 1 before it (the defaults a logged insert writes) is no demotion. */
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(
          db,
          "SELECT c.row_uid FROM changelog c JOIN devices d ON d.uid = c.row_uid AND d.removed = 0"
          " WHERE c.entity = 'profiles' AND c.field = 'archive' AND c.value = 0"
          " AND NOT EXISTS (SELECT 1 FROM changelog w WHERE w.entity = 'profiles'"
          "  AND w.row_uid = c.row_uid AND w.field = 'archive'"
          "  AND (w.hlc > c.hlc OR (w.hlc = c.hlc AND w.origin_uid > c.origin_uid)))"
          " AND EXISTS (SELECT 1 FROM changelog p WHERE p.entity = 'profiles'"
          "  AND p.row_uid = c.row_uid AND p.field = 'archive' AND p.value <> 0"
          "  AND (p.hlc < c.hlc OR (p.hlc = c.hlc AND p.origin_uid < c.origin_uid)))"
          " ORDER BY c.hlc, c.origin_uid LIMIT 1",
          -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (is_uid(uid)) memcpy(out, uid, MC_UID_STRLEN);
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

int mc_archive_is(sqlite3 *db, const char *uid) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT archive FROM profiles WHERE device_uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return -1;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  int stored = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0) != 0;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) return -1;
  if (stored) return 1;
  char v[MC_UID_STRLEN];
  if (mc_archive_void(db, v) != MC_OK) return -1;
  return v[0] && !strcmp(v, uid);
}

int mc_archive_count(sqlite3 *db) {
  int n = stored_archives(db);
  if (n != 0) return n;
  char v[MC_UID_STRLEN];
  if (mc_archive_void(db, v) != MC_OK) return -1;
  return v[0] ? 1 : 0;
}

mc_status mc_archive_restore(mc_sync_db *sdb, int *wrote) {
  if (wrote) *wrote = 0;
  if (!mc_sync_db_group_uid(sdb)) return MC_OK;
  char v[MC_UID_STRLEN];
  const char *me = mc_sync_db_device_uid(sdb);
  /* Almost always some member is an archive: answered without a write. */
  mc_status st = mc_archive_void(mc_sync_db_handle(sdb), v);
  if (st != MC_OK || strcmp(v, me) != 0) return st;
  if ((st = mc_sync_db_begin_write(sdb)) != MC_OK) return st;
  /* Decided again inside the write, so rows a session applies meanwhile can't change it. */
  st = mc_archive_void(mc_sync_db_handle(sdb), v);
  int mine = st == MC_OK && !strcmp(v, me);
  if (mine) {
    /* The row exists: the demotion was applied to it. Only the flag is logged. */
    sqlite3_stmt *stmt;
    st = MC_ERR_DB;
    if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                           "UPDATE profiles SET archive = 1 WHERE device_uid = ?1", -1, &stmt,
                           NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, me, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
      sqlite3_finalize(stmt);
    }
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) {
    mc_sync_db_rollback(sdb);
    return st;
  }
  if (mine) {
    fprintf(stderr, "musiccore: sync: no archive left in the group; this device is one again\n");
    if (wrote) *wrote = 1;
  }
  return MC_OK;
}
