/* Pairing over the network: PAIR_HELLO, PAIR_READY or PAIR_REFUSE, the join rules, J-PAKE
 * and XXpsk3 (pair.c), PAIR_ACCEPT, and then the first session on the same connection.
 * See "Pairing" and "Join rules" in docs/sync-protocol.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "library/library_internal.h"
#include "sync/session.h"
#include "sync/sync_internal.h"

/* How long B keeps a code for its requester, and A waits for the user to type it. */
#define CODE_WAIT_MS (5 * 60 * 1000)
#define CONNECT_MS 10000

/* One side of a pairing, as PAIR_HELLO and PAIR_READY describe it. */
typedef struct side {
  char dev[MC_UID_STRLEN];
  char name[MC_SYNC_NAME_MAX];
  char kind[16];
  char host[MC_SYNC_NAME_MAX];
  char grp[MC_GROUP_HASH_STRLEN];
  char library_uid[MC_UID_STRLEN];
  char group_library_uid[MC_UID_STRLEN]; /* "" when not in a group or not known yet */
  int64_t tracks;
} side;

/* Where the pair ends up. */
typedef enum outcome {
  NEW_GROUP_A_LIBRARY, /* a new group syncing A's library, with A as its archive */
  NEW_GROUP_B_LIBRARY,
  A_JOINS_B,
  B_JOINS_A
} outcome;

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

/* The join rules. MC_PAIR_PAIRED if the pair is possible, with *out set. */
static mc_pair_reason decide(const side *a, const side *b, outcome *out) {
  int a_in = a->grp[0] != '\0', b_in = b->grp[0] != '\0';
  if (a_in && b_in) return strcmp(a->grp, b->grp) ? MC_PAIR_IN_GROUP : MC_PAIR_ALREADY_PAIRED;
  if (!a_in && !b_in) {
    /* Libraries with the same uid are one library (a copied folder), so either may hold it. */
    if (a->tracks > 0 && b->tracks > 0 && strcmp(a->library_uid, b->library_uid))
      return MC_PAIR_LIBRARY_MISMATCH;
    *out = a->tracks > 0 && b->tracks == 0 ? NEW_GROUP_A_LIBRARY : NEW_GROUP_B_LIBRARY;
    return MC_PAIR_PAIRED;
  }
  const side *joiner = a_in ? b : a, *member = a_in ? a : b;
  if (joiner->tracks > 0 && strcmp(joiner->library_uid, member->group_library_uid))
    return MC_PAIR_LIBRARY_MISMATCH;
  *out = a_in ? B_JOINS_A : A_JOINS_B;
  return MC_PAIR_PAIRED;
}

static const char *const k_reasons[] = {
  [MC_PAIR_PAIRED] = "paired",
  [MC_PAIR_WRONG_CODE] = "wrong_code",
  [MC_PAIR_IN_GROUP] = "in_group",
  [MC_PAIR_LIBRARY_MISMATCH] = "library_mismatch",
  [MC_PAIR_ALREADY_PAIRED] = "already_paired",
  [MC_PAIR_BUSY] = "busy",
  [MC_PAIR_CANCELLED] = "cancelled",
  [MC_PAIR_FAILED] = "failed",
};

static mc_pair_reason reason_parse(const char *s) {
  for (int i = 0; s && i < (int)(sizeof k_reasons / sizeof k_reasons[0]); i++)
    if (!strcmp(s, k_reasons[i])) return (mc_pair_reason)i;
  return MC_PAIR_FAILED;
}

static mc_status reason_status(mc_pair_reason r) {
  switch (r) {
  case MC_PAIR_PAIRED: return MC_OK;
  case MC_PAIR_IN_GROUP:
  case MC_PAIR_ALREADY_PAIRED: return MC_ERR_IN_GROUP;
  case MC_PAIR_LIBRARY_MISMATCH: return MC_ERR_LIBRARY_MISMATCH;
  case MC_PAIR_BUSY: return MC_ERR_LOCKED;
  case MC_PAIR_WRONG_CODE: return MC_ERR_FORMAT;
  default: return MC_ERR_NET;
  }
}

/* --- This device's side --- */

static void text_column(sqlite3 *db, const char *sql, const char *arg, char *out, size_t cap) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
  sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, cap, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
}

static mc_status my_side(mc_sync *sync, mc_sync_conn *c, side *me) {
  memset(me, 0, sizeof *me);
  snprintf(me->dev, sizeof me->dev, "%s", sync->device_uid);
  mc_mutex_lock(&sync->lock);
  snprintf(me->name, sizeof me->name, "%s", sync->name);
  mc_mutex_unlock(&sync->lock);
  snprintf(me->kind, sizeof me->kind, "%s", mc_sync_kind_str(sync->kind));
  snprintf(me->host, sizeof me->host, "%s", sync->host);
  const char *group = mc_sync_db_group_uid(c->sdb);
  if (group) {
    mc_discovery_group_hash(group, me->grp);
    text_column(mc_sync_db_handle(c->sdb), "SELECT library_uid FROM sync_group WHERE uid = ?1",
                group, me->group_library_uid, sizeof me->group_library_uid);
  }
  text_column(mc_library_db(c->lib), "SELECT value FROM meta WHERE key = ?1", "library_uid",
              me->library_uid, sizeof me->library_uid);
  return mc_library_count(c->lib, MC_ENTITY_TRACKS, &me->tracks);
}

static mc_status send_side(mc_channel *ch, uint8_t type, const side *s) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "dev", s->dev);
  yyjson_mut_obj_add_str(doc, root, "name", s->name);
  yyjson_mut_obj_add_str(doc, root, "kind", s->kind);
  yyjson_mut_obj_add_str(doc, root, "host", s->host);
  yyjson_mut_obj_add_str(doc, root, "grp", s->grp);
  yyjson_mut_obj_add_str(doc, root, "library_uid", s->library_uid);
  yyjson_mut_obj_add_int(doc, root, "tracks", s->tracks);
  if (s->group_library_uid[0])
    yyjson_mut_obj_add_str(doc, root, "group_library_uid", s->group_library_uid);
  size_t len = 0;
  char *json = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!json) return MC_ERR_NOMEM;
  mc_status st = mc_channel_send_clear(ch, type, json, len);
  free(json);
  return st;
}

static void copy_field(yyjson_val *root, const char *key, char *out, size_t cap) {
  const char *v = yyjson_get_str(yyjson_obj_get(root, key));
  snprintf(out, cap, "%s", v ? v : "");
}

static mc_status parse_side(const unsigned char *payload, size_t len, side *s) {
  memset(s, 0, sizeof *s);
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  mc_status st = MC_ERR_FORMAT;
  if (yyjson_is_obj(root)) {
    copy_field(root, "dev", s->dev, sizeof s->dev);
    copy_field(root, "name", s->name, sizeof s->name);
    copy_field(root, "kind", s->kind, sizeof s->kind);
    copy_field(root, "host", s->host, sizeof s->host);
    copy_field(root, "grp", s->grp, sizeof s->grp);
    copy_field(root, "library_uid", s->library_uid, sizeof s->library_uid);
    copy_field(root, "group_library_uid", s->group_library_uid, sizeof s->group_library_uid);
    yyjson_val *t = yyjson_obj_get(root, "tracks");
    s->tracks = yyjson_is_int(t) ? yyjson_get_sint(t) : 0;
    if (is_uid(s->dev) && is_uid(s->library_uid) && (!s->grp[0] || strlen(s->grp) == 16))
      st = MC_OK;
  }
  yyjson_doc_free(doc);
  return st;
}

static void send_refuse(mc_channel *ch, mc_pair_reason reason, int attempts_left) {
  char json[96];
  int n = reason == MC_PAIR_WRONG_CODE
            ? snprintf(json, sizeof json, "{\"reason\":\"%s\",\"attempts_left\":%d}",
                       k_reasons[reason], attempts_left)
            : snprintf(json, sizeof json, "{\"reason\":\"%s\"}", k_reasons[reason]);
  mc_channel_send_clear(ch, MC_MSG_PAIR_REFUSE, json, (size_t)n);
}

static void parse_refuse(const unsigned char *payload, size_t len, mc_pair_reason *reason,
                         int *attempts_left) {
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  *reason = reason_parse(yyjson_get_str(yyjson_obj_get(root, "reason")));
  yyjson_val *a = yyjson_obj_get(root, "attempts_left");
  *attempts_left = yyjson_is_int(a) ? (int)yyjson_get_sint(a) : 0;
  yyjson_doc_free(doc);
}

/* --- Group changes --- */

static mc_status bind_exec(sqlite3 *db, const char *sql, const char *const *args, int n) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return MC_ERR_DB;
  for (int i = 0; i < n; i++)
    if (args[i])
      sqlite3_bind_text(stmt, i + 1, args[i], -1, SQLITE_STATIC);
    else
      sqlite3_bind_null(stmt, i + 1);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* A member's devices row, through the group changelog. Re-adding a removed device clears
 * its removed flag. added_by NULL leaves it as it is. */
static mc_status write_member(sqlite3 *db, const side *s, const unsigned char *key,
                              const char *added_by) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "INSERT INTO devices (uid, name, kind, host, public_key, added_by, removed)"
                         " VALUES (?1, ?2, ?3, ?4, ?5, ?6, 0) ON CONFLICT (uid) DO UPDATE SET"
                         " name = excluded.name, kind = excluded.kind, host = excluded.host,"
                         " public_key = excluded.public_key,"
                         " added_by = coalesce(excluded.added_by, devices.added_by), removed = 0",
                         -1, &stmt, NULL) != SQLITE_OK)
    return MC_ERR_DB;
  sqlite3_bind_text(stmt, 1, s->dev, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, s->name, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, s->kind, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, s->host, -1, SQLITE_STATIC);
  sqlite3_bind_blob(stmt, 5, key, MC_SYNC_KEY_LEN, SQLITE_STATIC);
  if (added_by)
    sqlite3_bind_text(stmt, 6, added_by, -1, SQLITE_STATIC);
  else
    sqlite3_bind_null(stmt, 6);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE ? MC_OK : MC_ERR_DB;
}

/* The group changes one device makes on a successful pairing, in one transaction:
 *  - group_uid: the group it is now in (local, not logged);
 *  - library_uid: the group's library, adopted first if this library is empty and has
 *    another uid;
 *  - archive (new groups only): the first archive, with sync_group and its profile;
 *  - the other device's row, and with a new group this device's own. */
static mc_status apply_pairing(mc_sync_conn *c, const char *group_uid, const char *library_uid,
                               const char *archive, const side *me, const side *other,
                               const unsigned char *other_key, const char *added_by) {
  char current[MC_UID_STRLEN];
  text_column(mc_library_db(c->lib), "SELECT value FROM meta WHERE key = ?1", "library_uid",
              current, sizeof current);
  mc_status st = MC_OK;
  if (strcmp(current, library_uid) && (st = mc_library_adopt_uid(c->lib, library_uid)) != MC_OK)
    return st;

  mc_sync_db *sdb = c->sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  if ((st = mc_sync_db_begin_write(sdb)) != MC_OK) return st;
  const char *g[] = {group_uid};
  st = bind_exec(db, "UPDATE local SET group_uid = ?1 WHERE id = 1", g, 1);
  if (st == MC_OK && archive) {
    const char *grp[] = {group_uid, library_uid};
    st = bind_exec(db, "INSERT OR IGNORE INTO sync_group (uid, library_uid) VALUES (?1, ?2)", grp,
                   2);
    const char *arch[] = {archive};
    if (st == MC_OK)
      st = bind_exec(db,
                     "INSERT INTO profiles (device_uid, archive, mode, format)"
                     " VALUES (?1, 1, 'all', 'original') ON CONFLICT (device_uid) DO UPDATE SET"
                     " archive = 1, mode = 'all', format = 'original'",
                     arch, 1);
    if (st == MC_OK) st = write_member(db, me, mc_sync_db_public_key(sdb), NULL);
  }
  if (st == MC_OK) st = write_member(db, other, other_key, added_by);
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

static mc_status send_accept(mc_channel *ch, const char *group_uid, const char *library_uid,
                             const char *archive) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  if (group_uid) {
    yyjson_mut_obj_add_str(doc, root, "group_uid", group_uid);
    yyjson_mut_obj_add_str(doc, root, "library_uid", library_uid);
    if (archive)
      yyjson_mut_obj_add_str(doc, root, "archive", archive);
    else
      yyjson_mut_obj_add_null(doc, root, "archive");
  }
  size_t len = 0;
  char *json = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!json) return MC_ERR_NOMEM;
  mc_status st = mc_channel_send(ch, MC_MSG_PAIR_ACCEPT, json, len);
  free(json);
  return st;
}

/* PAIR_ACCEPT's group, or "" for B's empty one ("send me yours"). */
static mc_status parse_accept(const unsigned char *payload, size_t len,
                              char group_uid[MC_UID_STRLEN], char library_uid[MC_UID_STRLEN]) {
  group_uid[0] = library_uid[0] = '\0';
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  mc_status st = MC_ERR_FORMAT;
  if (yyjson_is_obj(root)) {
    copy_field(root, "group_uid", group_uid, MC_UID_STRLEN);
    copy_field(root, "library_uid", library_uid, MC_UID_STRLEN);
    if (!group_uid[0] || (is_uid(group_uid) && is_uid(library_uid))) st = MC_OK;
  }
  yyjson_doc_free(doc);
  return st;
}

static mc_status recv_accept(mc_channel *ch, char group_uid[MC_UID_STRLEN],
                             char library_uid[MC_UID_STRLEN]) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_status st = mc_channel_recv(ch, &type, &payload, &len);
  if (st == MC_OK && type != MC_MSG_PAIR_ACCEPT) st = MC_ERR_FORMAT;
  if (st == MC_OK) st = parse_accept(payload, len, group_uid, library_uid);
  return st;
}

/* After PAIR_ACCEPT: the connection becomes the first session. */
static void first_session(mc_sync *sync, mc_sync_conn *c, mc_channel *ch, const char *peer,
                          const char *address, uint16_t port) {
  mc_session s;
  memset(&s, 0, sizeof s);
  s.sync = sync;
  s.conn = c;
  s.ch = ch;
  snprintf(s.peer_uid, sizeof s.peer_uid, "%s", peer);
  int initiator = mc_channel_is_initiator(ch);
  int lower = initiator ? strcmp(sync->device_uid, peer) < 0 : strcmp(peer, sync->device_uid) < 0;
  if (!mc_sync_claim(sync, peer, lower, ch)) return; /* a session is already running */
  mc_status st = mc_session_run(&s);
  /* Now that the group's rows are here: this device's name becomes its profile name, unless
   * the group already has one for it (it was a member before). The peer hears of it next
   * time. */
  if (st == MC_OK && !s.we_removed && mc_sync_seed_profile_name(sync, c)) s.follow_up = 1;
  mc_sync_result r = {peer, st, s.library_synced, st == MC_OK && s.converged, s.rows_sent,
                      s.rows_received};
  mc_sync_release(sync, peer);
  if (s.we_removed) mc_sync_forget_group(sync, c);
  mc_sync_session_ended(sync, c, peer, &r, s.rows_new, s.follow_up, initiator ? address : NULL, port);
}

static void result(mc_sync *sync, const side *peer, int initiator, mc_pair_reason reason,
                   int attempts_left, mc_status st) {
  mc_pair_result r;
  r.device_uid = peer ? peer->dev : "";
  r.name = peer ? peer->name : "";
  r.initiator = initiator;
  r.reason = reason;
  r.attempts_left = attempts_left;
  r.status = st;
  mc_sync_emit_pair_result(sync, &r);
}

/* --- A: asking to pair --- */

/* Waits for mc_sync_pair_enter(). 0 if cancelled, stopped or timed out. */
static int wait_for_code(mc_sync *sync, char code[MC_PAIR_CODE_DIGITS + 1]) {
  uint64_t deadline = mc_mono_ms() + CODE_WAIT_MS;
  mc_mutex_lock(&sync->lock);
  while (!sync->out_has_code && !sync->out_cancel && !sync->stopping && mc_mono_ms() < deadline)
    mc_cond_wait_ms(&sync->cond, &sync->lock, 500);
  int ok = sync->out_has_code && !sync->out_cancel && !sync->stopping;
  if (ok) memcpy(code, sync->out_code, MC_PAIR_CODE_DIGITS + 1);
  mc_mutex_unlock(&sync->lock);
  return ok;
}

static int out_cancelled(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  int c = sync->out_cancel || sync->stopping;
  mc_mutex_unlock(&sync->lock);
  return c;
}

void mc_pairing_initiate(void *arg) {
  mc_pair_target *t = arg;
  mc_sync *sync = t->sync;
  mc_sync_conn c;
  side me, peer;
  memset(&peer, 0, sizeof peer);
  mc_pair_reason reason = MC_PAIR_FAILED;
  int attempts_left = 0;
  mc_status st = mc_sync_conn_open(sync, &c);
  mc_channel *ch = NULL;

  while (st == MC_OK) {
    if ((st = my_side(sync, &c, &me)) != MC_OK) break;
    if ((st = mc_channel_connect(t->host, t->port, MC_MODE_PAIRING, CONNECT_MS, &ch)) != MC_OK)
      break;
    mc_sync_track_pairing(sync, ch, 0);
    if ((st = send_side(ch, MC_MSG_PAIR_HELLO, &me)) != MC_OK) break;

    uint8_t type;
    const unsigned char *payload;
    size_t len;
    if ((st = mc_channel_recv_clear(ch, &type, &payload, &len)) != MC_OK) break;
    if (type == MC_MSG_PAIR_REFUSE) {
      parse_refuse(payload, len, &reason, &attempts_left);
      st = reason_status(reason);
      break;
    }
    side ready;
    if (type != MC_MSG_PAIR_READY || (st = parse_side(payload, len, &ready)) != MC_OK) {
      st = MC_ERR_FORMAT;
      break;
    }
    if (!strcmp(ready.dev, me.dev) || (t->device_uid[0] && strcmp(ready.dev, t->device_uid))) {
      st = MC_ERR_NOT_FOUND; /* not the device we meant */
      break;
    }
    peer = ready;
    outcome out;
    reason = decide(&me, &peer, &out);
    if (reason != MC_PAIR_PAIRED) {
      st = reason_status(reason);
      break;
    }

    char code[MC_PAIR_CODE_DIGITS + 1];
    if (!wait_for_code(sync, code)) {
      reason = MC_PAIR_CANCELLED;
      send_refuse(ch, MC_PAIR_CANCELLED, 0); /* so B stops showing the code */
      st = MC_ERR_NET;
      break;
    }
    unsigned char peer_key[MC_SYNC_KEY_LEN];
    reason = MC_PAIR_FAILED;
    st = mc_pair_initiate(ch, code, mc_sync_db_private_key(c.sdb), mc_sync_db_public_key(c.sdb),
                          peer_key);
    int clear = 0;
    if (st == MC_OK) st = mc_channel_recv_after_pair(ch, &type, &payload, &len, &clear);
    if (st == MC_OK && clear) {
      parse_refuse(payload, len, &reason, &attempts_left);
      st = reason_status(reason);
      if (reason != MC_PAIR_WRONG_CODE) break;
      /* Wrong code: say so, and wait for another on a new connection. */
      result(sync, &peer, 1, reason, attempts_left, st);
      mc_mutex_lock(&sync->lock);
      sync->out_has_code = 0;
      mc_mutex_unlock(&sync->lock);
      mc_sync_untrack(sync, ch);
      mc_channel_close(ch);
      ch = NULL;
      st = MC_OK;
      reason = MC_PAIR_FAILED;
      if (out_cancelled(sync)) {
        reason = MC_PAIR_CANCELLED;
        st = MC_ERR_NET;
      }
      continue;
    }
    if (st == MC_OK && type != MC_MSG_PAIR_ACCEPT) st = MC_ERR_FORMAT;
    char group_uid[MC_UID_STRLEN], library_uid[MC_UID_STRLEN];
    if (st == MC_OK) st = parse_accept(payload, len, group_uid, library_uid);
    if (st != MC_OK) break;

    if (out == B_JOINS_A) {
      /* Our group: B waits for it. */
      if (group_uid[0] || !is_uid(me.group_library_uid)) {
        st = MC_ERR_FORMAT;
        break;
      }
      const char *group = mc_sync_db_group_uid(c.sdb);
      snprintf(group_uid, sizeof group_uid, "%s", group ? group : "");
      st = apply_pairing(&c, group_uid, me.group_library_uid, NULL, &me, &peer, peer_key,
                         me.dev);
      if (st == MC_OK) st = send_accept(ch, group_uid, me.group_library_uid, NULL);
    } else {
      if (!group_uid[0]) {
        st = MC_ERR_FORMAT;
        break;
      }
      st = apply_pairing(&c, group_uid, library_uid, NULL, &me, &peer, peer_key, NULL);
    }
    if (st != MC_OK) {
      reason = st == MC_ERR_LIBRARY_MISMATCH ? MC_PAIR_LIBRARY_MISMATCH : MC_PAIR_FAILED;
      break;
    }
    reason = MC_PAIR_PAIRED;
    mc_sync_group_changed(sync, &c);
    result(sync, &peer, 1, MC_PAIR_PAIRED, 0, MC_OK);
    first_session(sync, &c, ch, peer.dev, t->host, t->port);
    break;
  }

  if (reason != MC_PAIR_PAIRED) {
    if (out_cancelled(sync)) reason = MC_PAIR_CANCELLED;
    result(sync, peer.dev[0] ? &peer : NULL, 1, reason, attempts_left, st);
  }
  if (ch) {
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
  }
  mc_sync_conn_close(&c);
  mc_mutex_lock(&sync->lock);
  sync->out_active = 0;
  sync->out_has_code = 0;
  sync->out_cancel = 0;
  mc_mutex_unlock(&sync->lock);
  free(t);
}

/* --- B: showing the code --- */

void mc_pairing_respond(mc_sync *sync, mc_sync_conn *c, mc_channel *ch) {
  side me, peer;
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_status st = mc_channel_recv_clear(ch, &type, &payload, &len);
  if (st != MC_OK || type != MC_MSG_PAIR_HELLO || parse_side(payload, len, &peer) != MC_OK ||
      !strcmp(peer.dev, sync->device_uid))
    goto close;

  /* One requester at a time; it may come back after a wrong code. */
  mc_mutex_lock(&sync->lock);
  uint64_t now = mc_mono_ms();
  if (sync->in_active && !sync->in_conn && now >= sync->in_expires_ms) sync->in_active = 0;
  int busy = sync->in_conn || (sync->in_active && strcmp(sync->in_requester, peer.dev));
  if (!busy) sync->in_conn = 1;
  mc_mutex_unlock(&sync->lock);
  if (busy) {
    send_refuse(ch, MC_PAIR_BUSY, 0);
    goto close;
  }

  outcome out = NEW_GROUP_B_LIBRARY;
  mc_pair_reason reason = MC_PAIR_FAILED;
  if ((st = my_side(sync, c, &me)) == MC_OK) reason = decide(&peer, &me, &out);
  if (reason != MC_PAIR_PAIRED) {
    send_refuse(ch, reason, 0);
    result(sync, &peer, 0, reason, 0, reason_status(reason));
    mc_mutex_lock(&sync->lock);
    sync->in_conn = 0;
    mc_mutex_unlock(&sync->lock);
    goto close;
  }

  mc_mutex_lock(&sync->lock);
  int fresh = !sync->in_active;
  if (fresh) {
    st = mc_pair_responder_init(&sync->in_r);
    sync->in_active = st == MC_OK;
    snprintf(sync->in_requester, sizeof sync->in_requester, "%s", peer.dev);
    snprintf(sync->in_requester_name, sizeof sync->in_requester_name, "%s", peer.name);
  }
  sync->in_cancel = 0;
  sync->in_expires_ms = now + CODE_WAIT_MS;
  char code[MC_PAIR_CODE_DIGITS + 1];
  memcpy(code, sync->in_r.code, sizeof code);
  int attempts = MC_PAIR_MAX_FAILURES - sync->in_r.failures;
  mc_mutex_unlock(&sync->lock);
  if (st != MC_OK) goto done;
  if (fresh) mc_sync_emit_pair_code(sync, code, peer.name, attempts);

  if ((st = send_side(ch, MC_MSG_PAIR_READY, &me)) != MC_OK) goto done;
  mc_sync_track_pairing(sync, ch, 1);
  /* The user may take a while to type the code. */
  mc_channel_set_timeout(ch, CODE_WAIT_MS);
  unsigned char peer_key[MC_SYNC_KEY_LEN];
  int failures = sync->in_r.failures, new_code = 0;
  st = mc_pair_respond(&sync->in_r, ch, mc_sync_db_private_key(c->sdb),
                       mc_sync_db_public_key(c->sdb), peer_key, &new_code);
  mc_channel_set_timeout(ch, MC_CHANNEL_TIMEOUT_MS);

  mc_mutex_lock(&sync->lock);
  int cancelled = sync->in_cancel || sync->stopping;
  int counted = new_code || sync->in_r.failures != failures;
  memcpy(code, sync->in_r.code, sizeof code);
  attempts = MC_PAIR_MAX_FAILURES - sync->in_r.failures;
  mc_mutex_unlock(&sync->lock);

  if (st != MC_OK) {
    if (counted && !cancelled) {
      /* A guessed wrong. Tell it in the clear: its keys don't match ours. A may come
       * straight back, so this connection stops counting first. */
      mc_mutex_lock(&sync->lock);
      sync->in_conn = 0;
      mc_mutex_unlock(&sync->lock);
      if (new_code) mc_sync_emit_pair_code(sync, code, peer.name, attempts);
      send_refuse(ch, MC_PAIR_WRONG_CODE, attempts);
      result(sync, &peer, 0, MC_PAIR_WRONG_CODE, attempts, MC_ERR_FORMAT);
      goto untrack;
    }
    /* A gave up (or cancelled, or the connection broke) before guessing. */
    result(sync, &peer, 0, MC_PAIR_CANCELLED, 0, st);
    goto done;
  }

  /* Paired. Settle the group, then carry on as a session. */
  char group_uid[MC_UID_STRLEN], library_uid[MC_UID_STRLEN];
  if (out == B_JOINS_A) {
    st = send_accept(ch, NULL, NULL, NULL);
    if (st == MC_OK) st = recv_accept(ch, group_uid, library_uid);
    if (st == MC_OK && !group_uid[0]) st = MC_ERR_FORMAT;
    if (st == MC_OK)
      st = apply_pairing(c, group_uid, library_uid, NULL, &me, &peer, peer_key, NULL);
  } else if (out == A_JOINS_B) {
    const char *group = mc_sync_db_group_uid(c->sdb);
    snprintf(group_uid, sizeof group_uid, "%s", group ? group : "");
    st = is_uid(me.group_library_uid) ? MC_OK : MC_ERR_NOT_FOUND;
    if (st == MC_OK)
      st = apply_pairing(c, group_uid, me.group_library_uid, NULL, &me, &peer, peer_key, me.dev);
    if (st == MC_OK) st = send_accept(ch, group_uid, me.group_library_uid, NULL);
  } else {
    const side *holder = out == NEW_GROUP_A_LIBRARY ? &peer : &me;
    snprintf(library_uid, sizeof library_uid, "%s", holder->library_uid);
    st = mc_uid_new(group_uid);
    if (st == MC_OK)
      st = apply_pairing(c, group_uid, library_uid, holder->dev, &me, &peer, peer_key, me.dev);
    if (st == MC_OK) st = send_accept(ch, group_uid, library_uid, holder->dev);
  }
  if (st != MC_OK) {
    result(sync, &peer, 0,
           st == MC_ERR_LIBRARY_MISMATCH ? MC_PAIR_LIBRARY_MISMATCH : MC_PAIR_FAILED, 0, st);
    goto done;
  }
  mc_mutex_lock(&sync->lock);
  sync->in_active = sync->in_conn = 0;
  mc_pair_responder_wipe(&sync->in_r);
  mc_mutex_unlock(&sync->lock);
  mc_sync_group_changed(sync, c);
  result(sync, &peer, 0, MC_PAIR_PAIRED, 0, MC_OK);
  first_session(sync, c, ch, peer.dev, NULL, 0);
  goto untrack;

done:
  mc_mutex_lock(&sync->lock);
  sync->in_active = sync->in_conn = 0;
  mc_pair_responder_wipe(&sync->in_r);
  mc_mutex_unlock(&sync->lock);
untrack:
  mc_sync_untrack(sync, ch);
close:
  mc_channel_close(ch);
}
