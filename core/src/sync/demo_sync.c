/* The demo group (kithcore/demo.h): the demo library's group of three, this computer the
 * archive, Alice's phone holding a selection as Opus and Alice's watch one album, written straight
 * into sync.db; and the phone shown part-way through a sync, with no peer and no network. */
#include "kithcore/demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/library.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "util/platform.h"

#define WATCH_UID "6a1b5c0e-77d2-4f3a-9b8e-1d0c2e4f5a62"

/* What the phone holds and what the watch does, as demo album indices (demo.c's order). */
static const int k_phone_albums[] = {5, 2, 1}; /* Copperline, Nightjar Season, Low Country Radio */
static const int k_watch_albums[] = {0};       /* Salt on the Sill */
#define PHONE_TRACKS 17                         /* 6 + 5 + 6 */
#define PHONE_HELD 6                            /* before this sync */
#define WATCH_TRACKS 6
#define PHONE_KBPS 160
#define WATCH_KBPS 96
/* Bytes of Opus at kbps for the demo's tracks, about 3¾ minutes each. */
#define OPUS_BYTES(tracks, kbps) ((int64_t)(tracks) * 225 * (kbps) * 125)

static mc_status exec_args(sqlite3 *db, const char *sql, const char *const *text, int ntext,
                           const int64_t *nums, int nnums) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  for (int i = 0; i < ntext; i++)
    if (text[i]) sqlite3_bind_text(stmt, i + 1, text[i], -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, i + 1);
  for (int i = 0; i < nnums; i++) sqlite3_bind_int64(stmt, ntext + i + 1, nums[i]);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

static mc_status add_device(sqlite3 *db, const char *uid, const char *name, const char *kind,
                            const unsigned char *key, const char *added_by) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "INSERT OR REPLACE INTO devices (uid, name, kind, public_key, added_by, removed)"
                         " VALUES (?1, ?2, ?3, ?4, ?5, 0)",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, kind, -1, SQLITE_STATIC);
  sqlite3_bind_blob(stmt, 4, key, MC_SYNC_KEY_LEN, SQLITE_STATIC);
  if (added_by) sqlite3_bind_text(stmt, 5, added_by, -1, SQLITE_STATIC);
  else sqlite3_bind_null(stmt, 5);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* A device's selection: the demo albums in which[], as profile items. */
static mc_status add_selection(sqlite3 *db, const char *device, const mc_album_list *albums,
                               const int *which, int n) {
  mc_status st = MC_OK;
  for (int k = 0; k < n && st == MC_OK; k++) {
    const char *title = mc_demo_album_title(which[k]);
    for (int i = 0; i < albums->count && st == MC_OK; i++) {
      if (!albums->items[i].title || strcmp(albums->items[i].title, title)) continue;
      char uid[MC_UID_STRLEN];
      if ((st = mc_uid_new(uid)) != MC_OK) break;
      const char *args[] = {uid, device, albums->items[i].uid};
      st = exec_args(db, "INSERT INTO profile_items (uid, device_uid, kind, ref_uid) VALUES (?1, ?2, 'album', ?3)",
                     args, 3, NULL, 0);
      break;
    }
  }
  return st;
}

static mc_status confirm(sqlite3 *db, const char *uid) {
  char fp[MC_PROFILE_FP_STRLEN];
  mc_status st = mc_profile_fingerprint(db, uid, fp);
  const char *args[] = {fp, uid};
  if (st == MC_OK) st = exec_args(db, "UPDATE devices SET confirmed_profile = ?1 WHERE uid = ?2", args, 2, NULL, 0);
  return st;
}

static mc_status write_group(mc_sync_db *sdb, mc_library *lib, const mc_album_list *albums,
                             const char *device_name) {
  sqlite3 *db = mc_sync_db_handle(sdb);
  const char *me = mc_sync_db_device_uid(sdb);
  char group[MC_UID_STRLEN];
  mc_status st = mc_uid_new(group);
  if (st != MC_OK) return st;
  /* Made-up keys: nothing ever connects as these devices. */
  unsigned char phone_key[MC_SYNC_KEY_LEN], watch_key[MC_SYNC_KEY_LEN];
  for (int i = 0; i < MC_SYNC_KEY_LEN; i++) {
    phone_key[i] = (unsigned char)(0x5A ^ (i * 37));
    watch_key[i] = (unsigned char)(0xA5 ^ (i * 53));
  }
  int64_t now = (int64_t)mc_wall_ms();
  if ((st = mc_sync_db_begin_write(sdb)) != MC_OK) return st;
  const char *g[] = {group, mc_library_uid(lib)};
  st = exec_args(db, "UPDATE local SET group_uid = ?1 WHERE id = 1", g, 1, NULL, 0);
  if (st == MC_OK) st = exec_args(db, "INSERT INTO sync_group (uid, library_uid) VALUES (?1, ?2)", g, 2, NULL, 0);
  if (st == MC_OK) st = add_device(db, me, device_name ? device_name : "Alice's laptop", "desktop",
                                   mc_sync_db_public_key(sdb), NULL);
  if (st == MC_OK) st = add_device(db, MC_DEMO_PHONE_UID, MC_DEMO_PHONE_NAME, "phone", phone_key, me);
  if (st == MC_OK) st = add_device(db, WATCH_UID, MC_DEMO_WATCH_NAME, "watch", watch_key, MC_DEMO_PHONE_UID);
  /* This computer keeps everything; the phone and the watch a selection, as Opus. */
  const char *mine[] = {me};
  if (st == MC_OK)
    st = exec_args(db, "INSERT INTO profiles (device_uid, archive, mode, format) VALUES (?1, 1, 'all', 'original')",
                   mine, 1, NULL, 0);
  const char *phone[] = {MC_DEMO_PHONE_UID}, *watch[] = {WATCH_UID};
  const int64_t phone_kbps[] = {PHONE_KBPS}, watch_kbps[] = {WATCH_KBPS};
  if (st == MC_OK)
    st = exec_args(db,
                   "INSERT INTO profiles (device_uid, archive, mode, format, bitrate_kbps)"
                   " VALUES (?1, 0, 'selected', 'opus', ?2)",
                   phone, 1, phone_kbps, 1);
  if (st == MC_OK)
    st = exec_args(db,
                   "INSERT INTO profiles (device_uid, archive, mode, format, bitrate_kbps)"
                   " VALUES (?1, 0, 'selected', 'opus', ?2)",
                   watch, 1, watch_kbps, 1);
  if (st == MC_OK) st = add_selection(db, MC_DEMO_PHONE_UID, albums, k_phone_albums, 3);
  if (st == MC_OK) st = add_selection(db, WATCH_UID, albums, k_watch_albums, 1);
  /* Room on each, and what each said it held when it last checked: the phone before this
   * sync, with most of its selection still to come. */
  const int64_t phone_caps[] = {41LL << 30, now - 4 * 60000};
  const int64_t watch_caps[] = {5LL << 30, now - 50 * 60000};
  if (st == MC_OK)
    st = exec_args(db, "INSERT INTO capabilities (device_uid, free_bytes, on_power, can_transcode, updated_ms)"
                       " VALUES (?1, ?2, 1, 0, ?3)", phone, 1, phone_caps, 2);
  if (st == MC_OK)
    st = exec_args(db, "INSERT INTO capabilities (device_uid, free_bytes, on_power, can_transcode, updated_ms)"
                       " VALUES (?1, ?2, 0, 0, ?3)", watch, 1, watch_caps, 2);
  const int64_t phone_status[] = {now - 4 * 60000, PHONE_TRACKS, PHONE_HELD, PHONE_TRACKS - PHONE_HELD,
                                  OPUS_BYTES(PHONE_HELD, PHONE_KBPS)};
  const int64_t watch_status[] = {now - 50 * 60000, WATCH_TRACKS, WATCH_TRACKS, 0, OPUS_BYTES(WATCH_TRACKS, WATCH_KBPS)};
  const char *status_sql = "INSERT INTO sync_status (device_uid, verified_ms, expected, ok, missing, corrupt, bytes)"
                           " VALUES (?1, ?2, ?3, ?4, ?5, 0, ?6)";
  if (st == MC_OK) st = exec_args(db, status_sql, phone, 1, phone_status, 5);
  if (st == MC_OK) st = exec_args(db, status_sql, watch, 1, watch_status, 5);
  /* Last synced a few minutes ago, which also holds back automatic sessions with them for an
   * hour (MC_SYNC_AUTO_INTERVAL_MS); no address, so nothing would be tried anyway. */
  const int64_t phone_seen[] = {now - 4 * 60000}, watch_seen[] = {now - 50 * 60000};
  if (st == MC_OK)
    st = exec_args(db, "INSERT OR REPLACE INTO peer_contact (device_uid, last_sync_ms) VALUES (?1, ?2)", phone, 1,
                   phone_seen, 1);
  if (st == MC_OK)
    st = exec_args(db, "INSERT OR REPLACE INTO peer_contact (device_uid, last_sync_ms) VALUES (?1, ?2)", watch, 1,
                   watch_seen, 1);
  /* Each confirmed as it is, so nothing reads as paused. */
  if (st == MC_OK) st = confirm(db, me);
  if (st == MC_OK) st = confirm(db, MC_DEMO_PHONE_UID);
  if (st == MC_OK) st = confirm(db, WATCH_UID);
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

mc_status mc_demo_sync_group(const char *library_dir, const char *config_dir, const char *device_name) {
  if (!library_dir || !config_dir) return MC_ERR_INVALID_ARG;
  mc_library *lib = NULL;
  mc_status st = mc_library_open(library_dir, &lib);
  if (st != MC_OK) return st;
  mc_sync_db *sdb = NULL;
  mc_album_list *albums = NULL;
  if ((st = mc_sync_db_open(config_dir, &sdb)) == MC_OK && !mc_sync_db_group_uid(sdb) &&
      (st = mc_library_list_albums(lib, &albums)) == MC_OK)
    st = write_group(sdb, lib, albums, device_name);
  mc_album_list_free(albums);
  mc_sync_db_close(sdb);
  mc_library_close(lib);
  return st;
}

mc_status mc_demo_sync_show(mc_sync *sync) {
  if (!sync) return MC_ERR_INVALID_ARG;
  /* No automatic sessions or reach checks from here on: the phone isn't there to answer. It
   * reads as online, as a device a session reached does. */
  mc_mutex_lock(&sync->lock);
  sync->manual_only = 1;
  mc_sync_peer *p = NULL;
  for (int i = 0; i < sync->npeers && !p; i++)
    if (!strcmp(sync->peers[i].uid, MC_DEMO_PHONE_UID)) p = &sync->peers[i];
  if (!p) {
    mc_sync_peer *grown = realloc(sync->peers, (size_t)(sync->npeers + 1) * sizeof *grown);
    if (grown) {
      sync->peers = grown;
      p = &sync->peers[sync->npeers++];
      memset(p, 0, sizeof *p);
      memcpy(p->uid, MC_DEMO_PHONE_UID, MC_UID_STRLEN);
    }
  }
  if (p) {
    p->reached = 1;
    p->member = 1;
  }
  mc_mutex_unlock(&sync->lock);
  if (!p) return MC_ERR_NOMEM;

  if (sync->ev.on_peer) {
    mc_mutex_lock(&sync->cb_lock);
    sync->ev.on_peer(sync->user, MC_DEMO_PHONE_UID, 1, 1);
    mc_mutex_unlock(&sync->cb_lock);
  }
  /* The phone fetching its selection from here: 7 of the 11 it lacked sent so far. */
  const int sends_total = PHONE_TRACKS - PHONE_HELD, sends_done = 7;
  mc_sync_progress progress = {
    .device_uid = MC_DEMO_PHONE_UID,
    .phase = MC_SYNC_PHASE_FILES,
    .rows_sent = 412,
    .rows_received = 38,
    .bytes_sent = OPUS_BYTES(sends_done, PHONE_KBPS) + 1234567,
    .sends_done = sends_done,
    .sends_total = sends_total,
    .sends_bytes = OPUS_BYTES(sends_total, PHONE_KBPS),
    .speed = 3400000,
    .eta_seconds = 30,
    .user = 1,
    .connected = 1,
  };
  mc_sync_emit_progress(sync, &progress);
  return MC_OK;
}
