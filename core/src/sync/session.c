#include "sync/session.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "library/library_internal.h"
#include "musiccore/transcode.h"
#include "sync/archive.h"
#include "sync/rows.h"
#include "sync/transfer.h"

/* A device's free space only gets logged again when it moved by this much. */
#define FREE_BYTES_SLACK ((int64_t)64 << 20)
/* How long a refused peer gets to finish sending before we close. */
#define REFUSE_DRAIN_MS 2000

/* --- Messages --- */

/* Sends doc's root as a JSON control message and frees doc. */
static mc_status send_doc(mc_channel *ch, uint8_t type, yyjson_mut_doc *doc) {
  size_t len = 0;
  char *json = doc ? yyjson_mut_write(doc, 0, &len) : NULL;
  yyjson_mut_doc_free(doc);
  if (!json) return MC_ERR_NOMEM;
  mc_status st = len <= MC_CHANNEL_MAX_PAYLOAD ? mc_channel_send(ch, type, json, len)
                                               : MC_ERR_INVALID_ARG;
  free(json);
  return st;
}

static mc_status send_error(mc_channel *ch, const char *reason, const char *msg) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "reason", reason);
  yyjson_mut_obj_add_str(doc, root, "msg", msg);
  return send_doc(ch, MC_MSG_ERROR, doc);
}

void mc_session_refuse(mc_channel *ch, const char *reason, const char *msg) {
  if (send_error(ch, reason, msg) != MC_OK) return;
  /* Read what the peer sent meanwhile (its HELLO) before closing. Closing with unread
   * data makes TCP reset the connection, and a reset can discard the ERROR on the peer's
   * side before it reads it. */
  mc_channel_set_timeout(ch, REFUSE_DRAIN_MS);
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  for (int i = 0; i < 4 && mc_channel_recv(ch, &type, &payload, &len) == MC_OK; i++) {
  }
}

static yyjson_doc *parse(const unsigned char *payload, size_t len) {
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  if (doc && !yyjson_is_obj(yyjson_doc_get_root(doc))) {
    yyjson_doc_free(doc);
    return NULL;
  }
  return doc;
}

static const char *obj_str(yyjson_doc *doc, const char *key) {
  return yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), key));
}

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* Receives the next message. A peer's ERROR ends the session: "removed" means this
 * device has been removed from the group; "busy", that the peer has a session with us
 * already. */
static mc_status recv_msg(mc_session *s, uint8_t *type, const unsigned char **payload,
                          size_t *len) {
  mc_status st = mc_channel_recv(s->ch, type, payload, len);
  if (st != MC_OK || *type != MC_MSG_ERROR) return st;
  yyjson_doc *doc = parse(*payload, *len);
  const char *reason = doc ? obj_str(doc, "reason") : NULL;
  st = MC_ERR_FORMAT;
  if (reason && !strcmp(reason, "removed")) {
    s->we_removed = 1;
    st = MC_ERR_NOT_FOUND;
  } else if (reason && !strcmp(reason, "busy")) {
    st = MC_ERR_LOCKED;
  }
  fprintf(stderr, "musiccore: sync: peer ended the session: %s\n", reason ? reason : "?");
  yyjson_doc_free(doc);
  return st;
}

/* --- Handshake --- */

mc_status mc_session_handshake_initiate(mc_session *s, const unsigned char *peer_key) {
  mc_sync_db *sdb = s->conn->sdb;
  return mc_channel_handshake_kk(s->ch, mc_sync_db_private_key(sdb), mc_sync_db_public_key(sdb),
                                 (const unsigned char (*)[MC_NOISE_KEY_LEN])peer_key, 1, NULL,
                                 NULL);
}

mc_status mc_session_handshake_respond(mc_session *s) {
  mc_sync_db *sdb = s->conn->sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  /* Candidates: every member with a key, the removed ones included, so a removed device
   * can be told; see below. */
  sqlite3_stmt *stmt;
  if (!mc_sync_db_group_uid(sdb)) return MC_ERR_NOT_FOUND;
  if (sqlite3_prepare_v2(db,
                         "SELECT uid, public_key, removed FROM devices"
                         " WHERE uid <> ?1 AND length(public_key) = ?2",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, MC_NOISE_KEY_LEN);
  typedef struct cand {
    char uid[MC_UID_STRLEN];
    int removed;
  } cand;
  unsigned char (*keys)[MC_NOISE_KEY_LEN] = NULL;
  cand *who = NULL;
  int n = 0, cap = 0;
  mc_status st = MC_OK;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    if (n == cap) {
      int grown = cap ? cap * 2 : 8;
      void *k = realloc(keys, (size_t)grown * MC_NOISE_KEY_LEN);
      if (k) keys = k;
      void *w = k ? realloc(who, (size_t)grown * sizeof *who) : NULL;
      if (!w) {
        st = MC_ERR_NOMEM;
        break;
      }
      who = w;
      cap = grown;
    }
    memcpy(keys[n], sqlite3_column_blob(stmt, 1), MC_NOISE_KEY_LEN);
    memcpy(who[n].uid, uid, MC_UID_STRLEN);
    who[n].removed = sqlite3_column_int(stmt, 2);
    n++;
  }
  sqlite3_finalize(stmt);
  if (st == MC_OK)
    st = mc_channel_handshake_kk(s->ch, mc_sync_db_private_key(sdb), mc_sync_db_public_key(sdb),
                                 (const unsigned char (*)[MC_NOISE_KEY_LEN])keys, (size_t)n,
                                 NULL, NULL);
  if (st == MC_OK) {
    const unsigned char *pk = mc_channel_peer_key(s->ch);
    int found = -1;
    for (int i = 0; i < n && found < 0; i++)
      if (!memcmp(keys[i], pk, MC_NOISE_KEY_LEN)) found = i;
    if (found < 0) {
      st = MC_ERR_NOT_FOUND;
    } else if (who[found].removed) {
      /* Authenticated, so it can be believed: the device then forgets the group. Nothing
       * else is exchanged with it. */
      mc_session_refuse(s->ch, "removed", "this device was removed from the group");
      st = MC_ERR_NOT_FOUND;
    } else {
      memcpy(s->peer_uid, who[found].uid, MC_UID_STRLEN);
    }
  }
  free(keys);
  free(who);
  return st;
}

/* --- This device's own rows --- */

static mc_status exec_bind(sqlite3 *db, const char *sql, const char *const *texts, int n) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  for (int i = 0; i < n; i++) sqlite3_bind_text(stmt, i + 1, texts[i], -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

mc_status mc_session_refresh_self(mc_sync *sync, mc_sync_conn *c) {
  mc_sync_db *sdb = c->sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  if (!mc_sync_db_group_uid(sdb)) return MC_OK;
  const char *me = mc_sync_db_device_uid(sdb);

  /* What's logged now. */
  int have = 0, has_score = 0;
  int64_t score = 0, free_bytes = 0, on_power = -1, can_transcode = -1;
  char *old_hints = NULL;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT cpu_score, free_bytes, on_power, can_transcode, addresses"
                         " FROM capabilities WHERE device_uid = ?1",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, me, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    have = 1;
    has_score = sqlite3_column_type(stmt, 0) != SQLITE_NULL;
    score = sqlite3_column_int64(stmt, 0);
    free_bytes = sqlite3_column_int64(stmt, 1);
    if (sqlite3_column_type(stmt, 2) != SQLITE_NULL) on_power = sqlite3_column_int64(stmt, 2);
    if (sqlite3_column_type(stmt, 3) != SQLITE_NULL) can_transcode = sqlite3_column_int64(stmt, 3);
    const char *h = (const char *)sqlite3_column_text(stmt, 4);
    old_hints = h ? mc_strdup(h) : NULL;
  }
  sqlite3_finalize(stmt);

  /* What's true now. The benchmark runs once and is kept in this row. */
  int new_score = (int)score;
  if (!has_score && mc_transcode_benchmark(&new_score) != MC_OK) new_score = 0;
  int64_t new_free = 0;
  if (mc_free_space(mc_library_dir(c->lib), &new_free) != MC_OK) new_free = 0;
  /* Placeholders until the platforms report them: desktops are on mains and transcode. */
  int desktop = sync->kind == MC_DEVICE_DESKTOP;
  char hints[MC_SYNC_HINTS_MAX];
  mc_mutex_lock(&sync->lock);
  memcpy(hints, sync->hints, sizeof hints);
  mc_mutex_unlock(&sync->lock);

  int caps_changed = !have || !has_score || on_power != desktop || can_transcode != desktop ||
                     (old_hints ? strcmp(old_hints, hints) != 0 : hints[0] != '\0') ||
                     llabs(new_free - free_bytes) > FREE_BYTES_SLACK;
  free(old_hints);

  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  const char *kind = mc_sync_kind_str(sync->kind);
  char name[MC_SYNC_NAME_MAX];
  mc_mutex_lock(&sync->lock);
  memcpy(name, sync->name, sizeof name);
  mc_mutex_unlock(&sync->lock);
  const char *dev_args[] = {me, name, kind, sync->host};
  /* An upsert: the update triggers only log the columns that really changed. */
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO devices (uid, name, kind, host, public_key)"
                         " VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT (uid) DO UPDATE SET"
                         " name = excluded.name, kind = excluded.kind, host = excluded.host,"
                         " public_key = excluded.public_key",
                         -1, &stmt, NULL) != SQLITE_OK) {
    st = MC_ERR_DB;
  } else {
    for (int i = 0; i < 4; i++) sqlite3_bind_text(stmt, i + 1, dev_args[i], -1, SQLITE_STATIC);
    sqlite3_bind_blob(stmt, 5, mc_sync_db_public_key(sdb), MC_SYNC_KEY_LEN, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK && caps_changed) {
    if (sqlite3_prepare_v2(db,
                           "INSERT INTO capabilities (device_uid, cpu_score, free_bytes, on_power,"
                           " can_transcode, updated_ms, addresses)"
                           " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) ON CONFLICT (device_uid) DO"
                           " UPDATE SET cpu_score = excluded.cpu_score,"
                           " free_bytes = excluded.free_bytes, on_power = excluded.on_power,"
                           " can_transcode = excluded.can_transcode,"
                           " updated_ms = excluded.updated_ms, addresses = excluded.addresses",
                           -1, &stmt, NULL) != SQLITE_OK) {
      st = MC_ERR_DB;
    } else {
      sqlite3_bind_text(stmt, 1, me, -1, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 2, new_score);
      sqlite3_bind_int64(stmt, 3, new_free);
      sqlite3_bind_int(stmt, 4, desktop);
      sqlite3_bind_int(stmt, 5, desktop);
      sqlite3_bind_int64(stmt, 6, (sqlite3_int64)mc_wall_ms());
      if (hints[0])
        sqlite3_bind_text(stmt, 7, hints, -1, SQLITE_STATIC);
      else
        sqlite3_bind_null(stmt, 7);
      if (sqlite3_step(stmt) != SQLITE_DONE) st = MC_ERR_DB;
      sqlite3_finalize(stmt);
    }
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

/* --- HELLO --- */

typedef struct hello {
  char dev[MC_UID_STRLEN];
  char library_uid[MC_UID_STRLEN];
  char node_uid[MC_UID_STRLEN];
  int archive; /* -1: the HELLO doesn't say (an older version) */
} hello;

/* The library's uid as stored, in case another connection adopted a new one. */
static void library_uid_now(mc_library *lib, char out[MC_UID_STRLEN]) {
  snprintf(out, MC_UID_STRLEN, "%s", mc_library_uid(lib));
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT value FROM meta WHERE key = 'library_uid'",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    if (is_uid(v)) memcpy(out, v, MC_UID_STRLEN);
  }
  sqlite3_finalize(stmt);
}

static mc_status send_hello(mc_session *s, int archive) {
  mc_sync_db *sdb = s->conn->sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  char lib_uid[MC_UID_STRLEN];
  library_uid_now(s->conn->lib, lib_uid);

  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_int(doc, root, "v", MC_PROTOCOL_VERSION);
  yyjson_mut_obj_add_strcpy(doc, root, "dev", mc_sync_db_device_uid(sdb));
  yyjson_mut_obj_add_strcpy(doc, root, "library_uid", lib_uid);
  yyjson_mut_obj_add_strcpy(doc, root, "node_uid", mc_library_node_uid(s->conn->lib));
  if (s->user) yyjson_mut_obj_add_bool(doc, root, "user", true);
  yyjson_mut_obj_add_bool(doc, root, "covers", true);
  mc_mutex_lock(&s->sync->lock);
  s->files_plan = !s->sync->no_files_plan;
  mc_mutex_unlock(&s->sync->lock);
  if (s->files_plan) yyjson_mut_obj_add_bool(doc, root, "files_plan", true);
  yyjson_mut_obj_add_bool(doc, root, "archive", archive);
  yyjson_mut_val *caps = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, root, "caps", caps);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT cpu_score, free_bytes, on_power, can_transcode FROM capabilities"
                         " WHERE device_uid = ?1",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      yyjson_mut_obj_add_int(doc, caps, "cpu_score", sqlite3_column_int64(stmt, 0));
      yyjson_mut_obj_add_int(doc, caps, "free_bytes", sqlite3_column_int64(stmt, 1));
      yyjson_mut_obj_add_bool(doc, caps, "on_power", sqlite3_column_int(stmt, 2) != 0);
      yyjson_mut_obj_add_bool(doc, caps, "can_transcode", sqlite3_column_int(stmt, 3) != 0);
    }
    sqlite3_finalize(stmt);
  }
  return send_doc(s->ch, MC_MSG_HELLO, doc);
}

static mc_status recv_hello(mc_session *s, hello *h) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_status st = recv_msg(s, &type, &payload, &len);
  if (st != MC_OK) return st;
  if (type != MC_MSG_HELLO) return MC_ERR_FORMAT;
  yyjson_doc *doc = parse(payload, len);
  if (!doc) return MC_ERR_FORMAT;
  const char *dev = obj_str(doc, "dev"), *lib = obj_str(doc, "library_uid"),
             *node = obj_str(doc, "node_uid");
  st = MC_ERR_FORMAT;
  /* The device must be the one the handshake authenticated. */
  if (is_uid(dev) && is_uid(lib) && is_uid(node) && !strcmp(dev, s->peer_uid)) {
    memcpy(h->dev, dev, MC_UID_STRLEN);
    memcpy(h->library_uid, lib, MC_UID_STRLEN);
    memcpy(h->node_uid, node, MC_UID_STRLEN);
    if (yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "user"))) s->user = 1;
    s->peer_covers = yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "covers"));
    if (!yyjson_get_bool(yyjson_obj_get(yyjson_doc_get_root(doc), "files_plan"))) s->files_plan = 0;
    yyjson_val *archive = yyjson_obj_get(yyjson_doc_get_root(doc), "archive");
    h->archive = yyjson_is_bool(archive) ? yyjson_get_bool(archive) : -1;
    st = MC_OK;
  }
  yyjson_doc_free(doc);
  return st;
}

/* --- Changelog exchange --- */

typedef struct exchange {
  mc_session *s;
  int db; /* MC_ROWS_GROUP or MC_ROWS_LIBRARY */
  const char *name;
  sqlite3 *handle;
  mc_rows_applier *applier;
  unsigned char *buf;
} exchange;

/* With the file counts too: they stay at their last values after the file phase, so the
 * closing MC_SYNC_PHASE_DONE carries the session's totals. */
static void progress(mc_session *s, mc_sync_phase phase) {
  mc_sync_progress p = {.device_uid = s->peer_uid, .phase = phase, .rows_sent = s->rows_sent,
                        .rows_received = s->rows_received, .files_done = s->files_done,
                        .files_total = s->files_total, .bytes_received = s->bytes_received,
                        .bytes_sent = s->bytes_sent, .files_bytes = s->files_bytes,
                        .sends_done = s->sends_done, .sends_total = s->sends_total,
                        .sends_bytes = s->sends_bytes};
  mc_sync_emit_progress(s->sync, &p);
}

static mc_status send_vv(exchange *x, int resend) {
  mc_origin_hlc *vv;
  int n;
  mc_status st = mc_changelog_version_vector(x->handle, &vv, &n);
  if (st != MC_OK) return st;
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) {
    free(vv);
    return MC_ERR_NOMEM;
  }
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "db", x->name);
  if (resend) yyjson_mut_obj_add_bool(doc, root, "resend", 1);
  yyjson_mut_val *obj = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, root, "vv", obj);
  for (int i = 0; i < n; i++)
    yyjson_mut_obj_add(obj, yyjson_mut_strcpy(doc, vv[i].origin_uid),
                       yyjson_mut_uint(doc, vv[i].hlc));
  free(vv);
  return send_doc(x->s->ch, MC_MSG_VV, doc);
}

/* Parses a VV payload for this database into a malloc'd vector. */
static mc_status parse_vv(exchange *x, const unsigned char *payload, size_t len,
                          mc_origin_hlc **out, int *count) {
  *out = NULL;
  *count = 0;
  yyjson_doc *doc = parse(payload, len);
  if (!doc) return MC_ERR_FORMAT;
  const char *db = obj_str(doc, "db");
  yyjson_val *vv = yyjson_obj_get(yyjson_doc_get_root(doc), "vv");
  mc_status st = MC_ERR_FORMAT;
  if (db && !strcmp(db, x->name) && yyjson_is_obj(vv)) {
    size_t n = yyjson_obj_size(vv);
    mc_origin_hlc *items = n ? calloc(n, sizeof *items) : NULL;
    st = n && !items ? MC_ERR_NOMEM : MC_OK;
    size_t idx, max;
    yyjson_val *key, *val;
    int k = 0;
    yyjson_obj_foreach(vv, idx, max, key, val) {
      if (st != MC_OK) break;
      const char *origin = yyjson_get_str(key);
      if (!is_uid(origin) || !yyjson_is_uint(val)) {
        st = MC_ERR_FORMAT;
        break;
      }
      memcpy(items[k].origin_uid, origin, MC_UID_STRLEN);
      items[k].hlc = yyjson_get_uint(val);
      k++;
    }
    if (st == MC_OK) {
      *out = items;
      *count = k;
    } else {
      free(items);
    }
  }
  yyjson_doc_free(doc);
  return st;
}

static mc_status recv_vv(exchange *x, mc_origin_hlc **vv, int *n) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_status st = recv_msg(x->s, &type, &payload, &len);
  if (st != MC_OK) return st;
  if (type != MC_MSG_VV) return MC_ERR_FORMAT;
  return parse_vv(x, payload, len, vv, n);
}

static mc_status flush_rows(exchange *x, mc_rows_writer *w) {
  if (!w->count) return MC_OK;
  unsigned count = w->count;
  size_t len = mc_rows_writer_finish(w);
  mc_status st = mc_channel_send(x->s->ch, MC_MSG_ROWS, x->buf, len);
  if (st == MC_OK) x->s->rows_sent += count;
  mc_rows_writer_init(w, x->buf, MC_CHANNEL_MAX_PAYLOAD, x->db);
  return st;
}

/* Streams every row the peer is missing, in hlc order. */
static mc_status send_rows(exchange *x, const mc_origin_hlc *peer_vv, int peer_n, int resend) {
  mc_rows_reader *rd;
  mc_status st = mc_rows_reader_open(x->handle, peer_vv, peer_n, resend, &rd);
  if (st != MC_OK) return st;
  mc_rows_writer w;
  mc_rows_writer_init(&w, x->buf, MC_CHANNEL_MAX_PAYLOAD, x->db);
  mc_row r;
  int got;
  while (st == MC_OK && (st = mc_rows_reader_next(rd, &r, &got)) == MC_OK && got) {
    int added = mc_rows_writer_add(&w, &r);
    if (added == 0) {
      if (!w.count) {
        fprintf(stderr, "musiccore: sync: a change too large to send was skipped\n");
        continue;
      }
      if ((st = flush_rows(x, &w)) != MC_OK) break;
      added = mc_rows_writer_add(&w, &r);
    }
    if (added < 0) fprintf(stderr, "musiccore: sync: skipped a malformed change\n");
  }
  if (st == MC_OK) st = flush_rows(x, &w);
  mc_rows_reader_close(rd);
  return st;
}

static mc_status send_rows_end(exchange *x, char digest[MC_DIGEST_STRLEN]) {
  mc_status st = mc_rows_vv_digest(x->handle, digest);
  if (st != MC_OK) return st;
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "db", x->name);
  yyjson_mut_obj_add_str(doc, root, "digest", digest);
  return send_doc(x->s->ch, MC_MSG_ROWS_END, doc);
}

static mc_status parse_rows_end(exchange *x, const unsigned char *payload, size_t len,
                                char digest[MC_DIGEST_STRLEN]) {
  yyjson_doc *doc = parse(payload, len);
  if (!doc) return MC_ERR_FORMAT;
  const char *db = obj_str(doc, "db"), *d = obj_str(doc, "digest");
  mc_status st = MC_ERR_FORMAT;
  if (db && !strcmp(db, x->name) && d && strlen(d) == MC_DIGEST_STRLEN - 1) {
    memcpy(digest, d, MC_DIGEST_STRLEN);
    st = MC_OK;
  }
  yyjson_doc_free(doc);
  return st;
}

/* Applies ROWS until ROWS_END, whose digest it returns. */
static mc_status recv_rows(exchange *x, mc_sync_phase phase, char digest[MC_DIGEST_STRLEN]) {
  for (;;) {
    uint8_t type;
    const unsigned char *payload;
    size_t len;
    mc_status st = recv_msg(x->s, &type, &payload, &len);
    if (st != MC_OK) return st;
    if (type == MC_MSG_ROWS_END) return parse_rows_end(x, payload, len, digest);
    if (type != MC_MSG_ROWS) return MC_ERR_FORMAT;
    int db, n, logged = 0;
    mc_row *rows;
    if ((st = mc_rows_decode(payload, len, &db, &rows, &n)) != MC_OK) return st;
    if (db == x->db)
      st = mc_rows_apply(x->applier, rows, n, &logged);
    else
      st = MC_ERR_FORMAT;
    free(rows);
    if (st != MC_OK) return st;
    x->s->rows_received += n;
    x->s->rows_new += logged;
    progress(x->s, phase);
  }
}

static mc_status receive_only_end(exchange *x, char digest[MC_DIGEST_STRLEN]) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_status st = recv_msg(x->s, &type, &payload, &len);
  if (st != MC_OK) return st;
  if (type != MC_MSG_ROWS_END) return MC_ERR_FORMAT;
  return parse_rows_end(x, payload, len, digest);
}

/* One database, per "Session" steps 2 to 5. The initiator streams first. The responder
 * applies that, then streams its own rows; its ROWS_END digest is final. The initiator
 * applies those and answers with a second ROWS_END carrying its final digest. Both then
 * compare the same two digests, and on a mismatch run one more round in which origins
 * whose newest hlc differs are sent again from 0. first_vv: the responder's already
 * received VV payload, or NULL. */
static mc_status exchange_db(mc_session *s, int which, const unsigned char *first_vv,
                             size_t first_len) {
  exchange x;
  memset(&x, 0, sizeof x);
  x.s = s;
  x.db = which;
  x.name = which == MC_ROWS_GROUP ? "group" : "library";
  mc_rows_target tg;
  if (which == MC_ROWS_GROUP)
    mc_rows_target_group(s->conn->sdb, &tg);
  else
    mc_rows_target_library(s->conn->lib, &tg);
  x.handle = tg.db;
  mc_sync_phase phase = which == MC_ROWS_GROUP ? MC_SYNC_PHASE_GROUP : MC_SYNC_PHASE_LIBRARY;
  progress(s, phase);
  mc_status st = mc_rows_applier_new(&tg, &x.applier);
  if (st != MC_OK) return st;
  x.buf = malloc(MC_CHANNEL_MAX_PAYLOAD);
  if (!x.buf) st = MC_ERR_NOMEM;
  int initiator = mc_channel_is_initiator(s->ch);
  int converged = 0;

  for (int round = 0; st == MC_OK && round < 2 && !converged; round++) {
    mc_origin_hlc *peer = NULL;
    int peer_n = 0;
    char mine[MC_DIGEST_STRLEN], theirs[MC_DIGEST_STRLEN];
    if (initiator) {
      st = send_vv(&x, round);
      if (st == MC_OK) st = recv_vv(&x, &peer, &peer_n);
      if (st == MC_OK) st = send_rows(&x, peer, peer_n, round);
      if (st == MC_OK) st = send_rows_end(&x, mine); /* provisional */
      if (st == MC_OK) st = recv_rows(&x, phase, theirs);
      if (st == MC_OK) st = send_rows_end(&x, mine);
    } else {
      if (round == 0 && first_vv)
        st = parse_vv(&x, first_vv, first_len, &peer, &peer_n);
      else
        st = recv_vv(&x, &peer, &peer_n);
      if (st == MC_OK) st = send_vv(&x, round);
      if (st == MC_OK) st = recv_rows(&x, phase, theirs); /* their provisional digest */
      if (st == MC_OK) st = send_rows(&x, peer, peer_n, round);
      if (st == MC_OK) st = send_rows_end(&x, mine);
      if (st == MC_OK) st = receive_only_end(&x, theirs);
    }
    free(peer);
    if (st != MC_OK) break;
    converged = !strcmp(mine, theirs);
    if (!converged)
      fprintf(stderr, "musiccore: sync: %s data differs from %s after round %d%s\n", x.name,
              s->peer_uid, round + 1, round ? "" : "; sending differing origins again");
  }
  if (st == MC_OK && !converged) s->converged = 0;
  free(x.buf);
  mc_rows_applier_free(x.applier);
  return st;
}

/* --- Library identity --- */

static int text_query(sqlite3 *db, const char *sql, const char *arg, char *out, size_t cap) {
  sqlite3_stmt *stmt;
  int found = 0;
  if (out && cap) out[0] = '\0';
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
  if (arg) sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    found = 1;
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    if (out && cap) snprintf(out, cap, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
  return found;
}

/* Step 3: whether this device's library node uid is someone else's too (e.g. the library
 * folder was copied from another member), and if so, a fresh one. The device that recorded
 * the uid first keeps it. If both claim it or neither has recorded it, the higher device
 * uid gives it up. Then this device's devices row records the uid it uses. */
static mc_status check_node_uid(mc_session *s, const hello *peer) {
  sqlite3 *db = mc_sync_db_handle(s->conn->sdb);
  mc_library *lib = s->conn->lib;
  const char *me = mc_sync_db_device_uid(s->conn->sdb);
  char node[MC_UID_STRLEN];
  snprintf(node, sizeof node, "%s", mc_library_node_uid(lib));

  int others = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM devices WHERE node_uid = ?1 AND uid <> ?2", -1,
                         &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, node, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, me, -1, SQLITE_STATIC);
    others = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
  }
  char recorded[MC_UID_STRLEN];
  text_query(db, "SELECT node_uid FROM devices WHERE uid = ?1", me, recorded, sizeof recorded);
  int mine_recorded = !strcmp(recorded, node);
  int rotate = 0;
  if (others && !mine_recorded) {
    rotate = 1; /* another member has it on record */
  } else if (!strcmp(peer->node_uid, node)) {
    char theirs[MC_UID_STRLEN];
    text_query(db, "SELECT node_uid FROM devices WHERE uid = ?1", peer->dev, theirs,
               sizeof theirs);
    int peer_recorded = !strcmp(theirs, node);
    rotate = mine_recorded == peer_recorded ? strcmp(me, peer->dev) > 0 : !mine_recorded;
  } else if (others) {
    /* Both this device and another have it on record: the higher uid gives it up. */
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM devices WHERE node_uid = ?1 AND uid < ?2", -1,
                           &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, node, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, me, -1, SQLITE_STATIC);
      rotate = sqlite3_step(stmt) == SQLITE_ROW;
      sqlite3_finalize(stmt);
    }
  }
  mc_status st = MC_OK;
  if (rotate) {
    fprintf(stderr, "musiccore: sync: library node uid %s is another device's; rotating\n", node);
    if ((st = mc_library_rotate_node_uid(lib)) != MC_OK) return st;
    snprintf(node, sizeof node, "%s", mc_library_node_uid(lib));
  }
  if (strcmp(recorded, node) != 0) {
    if ((st = mc_sync_db_begin_write(s->conn->sdb)) != MC_OK) return st;
    const char *args[] = {node, me};
    st = exec_bind(db, "UPDATE devices SET node_uid = ?1 WHERE uid = ?2", args, 2);
    s->follow_up = 1; /* after the group exchange: the peer hears of it next time */
    if (st == MC_OK) st = mc_sync_db_commit(s->conn->sdb);
    if (st != MC_OK) mc_sync_db_rollback(s->conn->sdb);
  }
  return st;
}

/* --- Session --- */

/* Step 3's condition: both libraries are the group's. */
static int library_matches(mc_session *s, const hello *peer) {
  mc_sync_db *sdb = s->conn->sdb;
  const char *group = mc_sync_db_group_uid(sdb);
  if (!group) return 0;
  char group_lib[MC_UID_STRLEN], mine[MC_UID_STRLEN];
  if (!text_query(mc_sync_db_handle(sdb), "SELECT library_uid FROM sync_group WHERE uid = ?1",
                  group, group_lib, sizeof group_lib) ||
      !is_uid(group_lib))
    return 0;
  library_uid_now(s->conn->lib, mine);
  return !strcmp(mine, group_lib) && !strcmp(peer->library_uid, group_lib);
}

static int removed_here(mc_session *s) {
  char v[8];
  return text_query(mc_sync_db_handle(s->conn->sdb), "SELECT removed FROM devices WHERE uid = ?1",
                    mc_sync_db_device_uid(s->conn->sdb), v, sizeof v) &&
         strcmp(v, "0") != 0 && v[0];
}

mc_status mc_session_run(mc_session *s) {
  if (!s || !s->ch || !s->conn || !is_uid(s->peer_uid)) return MC_ERR_INVALID_ARG;
  s->converged = 1;
  mc_session_refresh_self(s->sync, s->conn); /* best effort: stale capabilities are fine */
  mc_transfer_write_status(s->sync, s->conn);
  /* A void demotion of ours whose re-write failed last time (best effort, as above). */
  mc_archive_restore(s->conn->sdb, NULL);

  /* Step 6's order: an archive facing a non-archive receives first, so an original imported
   * on the other device reaches it before the other's turn, which can then swap that
   * original for its profile's format in the same session. Taken from what each HELLO says,
   * so both sides agree even if their group data differs. */
  mc_sync_db *sdb = s->conn->sdb;
  int archive = mc_archive_is(mc_sync_db_handle(sdb), mc_sync_db_device_uid(sdb)) > 0;
  hello peer;
  mc_status st = send_hello(s, archive);
  if (st == MC_OK) st = recv_hello(s, &peer);
  if (st == MC_OK) st = exchange_db(s, MC_ROWS_GROUP, NULL, 0);
  if (st != MC_OK) return st;
  s->receive_first = peer.archive >= 0 && peer.archive != archive ? archive
                                                                  : mc_channel_is_initiator(s->ch);

  /* "Archives": with the merged group data, if no member is an archive any more and the
   * void demotion is ours, write archive = 1 again. That comes after the exchange, so the
   * peer hears of it in the next session; every device reads the void demotion from the
   * same rows meanwhile, so all of them count us as an archive anyway. */
  int restored = 0;
  if (mc_archive_restore(s->conn->sdb, &restored) == MC_OK && restored) s->follow_up = 1;

  /* "Confirming profiles": a press of Sync at either end confirms this device's profile as
   * the group exchange left it. Written after the exchange, so the peer hears of it next
   * time, as above. */
  int confirmed = 0;
  if (s->user && mc_profile_confirm(s->conn->sdb, &confirmed) == MC_OK && confirmed)
    s->follow_up = 1;

  if (removed_here(s)) s->we_removed = 1;
  int lib = !s->we_removed && library_matches(s, &peer);
  if (lib) check_node_uid(s, &peer);

  uint8_t type;
  const unsigned char *payload;
  size_t len;
  if (mc_channel_is_initiator(s->ch)) {
    if (lib) {
      mc_transfer_begin(s);
      st = exchange_db(s, MC_ROWS_LIBRARY, NULL, 0);
      s->library_synced = st == MC_OK;
      if (st == MC_OK) st = mc_transfer_run(s); /* step 6 */
    }
    if (st == MC_OK) st = mc_channel_send(s->ch, MC_MSG_BYE, "{}", 2);
    if (st == MC_OK) st = recv_msg(s, &type, &payload, &len);
    if (st == MC_OK && type != MC_MSG_BYE) st = MC_ERR_FORMAT;
  } else {
    st = recv_msg(s, &type, &payload, &len);
    if (st == MC_OK && type == MC_MSG_VV) {
      if (!lib) {
        /* We disagree about the library: our group data says it isn't shared. */
        mc_session_refuse(s->ch, "library", "not the group's library here");
        return MC_ERR_LIBRARY_MISMATCH;
      }
      /* The payload lives in the channel's buffer; copy it before the next receive. */
      unsigned char *vv = malloc(len ? len : 1);
      if (!vv) return MC_ERR_NOMEM;
      memcpy(vv, payload, len);
      mc_transfer_begin(s);
      st = exchange_db(s, MC_ROWS_LIBRARY, vv, len);
      free(vv);
      s->library_synced = st == MC_OK;
      if (st == MC_OK) st = mc_transfer_run(s); /* step 6 */
      if (st == MC_OK) st = recv_msg(s, &type, &payload, &len);
    }
    if (st == MC_OK && type != MC_MSG_BYE) st = MC_ERR_FORMAT;
    if (st == MC_OK) st = mc_channel_send(s->ch, MC_MSG_BYE, "{}", 2);
  }
  /* Reconcile and the fast check, once the peer is done with our files. */
  mc_transfer_end(s, st == MC_OK);
  if (st == MC_OK && removed_here(s)) s->we_removed = 1;
  progress(s, MC_SYNC_PHASE_DONE);
  return st;
}
