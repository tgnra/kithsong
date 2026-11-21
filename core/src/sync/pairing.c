/* Pairing over the network: PAIR_HELLO, PAIR_READY or PAIR_REFUSE, the join rules, J-PAKE
 * and XXpsk3 (pair.c), PAIR_ACCEPT, and then the first session on the same connection.
 * See "Pairing" and "Join rules" in docs/sync-protocol.md. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/msg.h"
#include "sync/reach.h"
#include "sync/session.h"
#include "sync/issues.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"

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
  uint16_t port; /* where it listens: the other side may know it by no other means */
  mc_sync_version ver; /* as the other side judges it; this side's own is in sync */
  int compatible;      /* ver says the two can sync */
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

/* The join rules. MC_PAIR_PAIRED if the pair is possible, with *out set. Two libraries are
 * never mixed: a device that would join with music of another library is refused. */
static mc_pair_reason decide(const side *a, const side *b, outcome *out) {
  int a_in = a->grp[0] != '\0', b_in = b->grp[0] != '\0';
  if (a_in && b_in) return !strcmp(a->grp, b->grp) ? MC_PAIR_ALREADY_PAIRED : MC_PAIR_IN_GROUP;
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
  [MC_PAIR_VERSION] = "version",
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
  case MC_PAIR_VERSION: return MC_ERR_VERSION;
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
  me->port = sync->port;
  return mc_library_count(c->lib, MC_ENTITY_TRACKS, &me->tracks);
}

static mc_status send_side(mc_sync *sync, mc_channel *ch, uint8_t type, const side *s) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  if (!doc) return MC_ERR_NOMEM;
  yyjson_mut_val *root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);
  mc_version_self self;
  mc_sync_self_version(sync, &self);
  if (self.check) mc_version_write(doc, root, &self, 0); /* a test posing as 0.9.4 says none */
  yyjson_mut_obj_add_str(doc, root, "dev", s->dev);
  yyjson_mut_obj_add_str(doc, root, "name", s->name);
  yyjson_mut_obj_add_str(doc, root, "kind", s->kind);
  yyjson_mut_obj_add_str(doc, root, "host", s->host);
  yyjson_mut_obj_add_str(doc, root, "grp", s->grp);
  yyjson_mut_obj_add_str(doc, root, "library_uid", s->library_uid);
  yyjson_mut_obj_add_int(doc, root, "tracks", s->tracks);
  yyjson_mut_obj_add_uint(doc, root, "port", s->port);
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

/* Its versions are judged before anything else is checked: a version this one can't pair with
 * may send what it doesn't expect. */
static mc_status parse_side(mc_sync *sync, const unsigned char *payload, size_t len, side *s) {
  memset(s, 0, sizeof *s);
  s->compatible = 1; /* until it says otherwise */
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  mc_status st = MC_ERR_FORMAT;
  if (yyjson_is_obj(root)) {
    mc_version_self self;
    mc_sync_self_version(sync, &self);
    s->compatible = mc_version_read(&self, root, &s->ver);
    copy_field(root, "dev", s->dev, sizeof s->dev);
    copy_field(root, "name", s->name, sizeof s->name);
    copy_field(root, "kind", s->kind, sizeof s->kind);
    copy_field(root, "host", s->host, sizeof s->host);
    copy_field(root, "grp", s->grp, sizeof s->grp);
    copy_field(root, "library_uid", s->library_uid, sizeof s->library_uid);
    copy_field(root, "group_library_uid", s->group_library_uid, sizeof s->group_library_uid);
    yyjson_val *t = yyjson_obj_get(root, "tracks");
    s->tracks = yyjson_is_int(t) ? yyjson_get_sint(t) : 0;
    yyjson_val *p = yyjson_obj_get(root, "port");
    uint64_t port = yyjson_is_uint(p) ? yyjson_get_uint(p) : 0;
    s->port = port <= 65535 ? (uint16_t)port : 0;
    if (is_uid(s->dev) && is_uid(s->library_uid) && (!s->grp[0] || strlen(s->grp) == 16))
      st = MC_OK;
  }
  yyjson_doc_free(doc);
  return st;
}

/* Every PAIR_REFUSE carries this device's versions, so that one without them is known to come
 * from a release before versions. "version" also says who this device is (me), so that the
 * other can say which to update even when it reached this one by address. */
static void send_refuse(mc_sync *sync, mc_channel *ch, mc_pair_reason reason, int attempts_left,
                        const side *me) {
  yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
  yyjson_mut_val *root = doc ? yyjson_mut_obj(doc) : NULL;
  if (!root) {
    yyjson_mut_doc_free(doc);
    return;
  }
  yyjson_mut_doc_set_root(doc, root);
  yyjson_mut_obj_add_str(doc, root, "reason", k_reasons[reason]);
  if (reason == MC_PAIR_WRONG_CODE) yyjson_mut_obj_add_int(doc, root, "attempts_left", attempts_left);
  mc_version_self self;
  mc_sync_self_version(sync, &self);
  if (self.check) mc_version_write(doc, root, &self, 0);
  if (me) {
    yyjson_mut_obj_add_str(doc, root, "dev", me->dev);
    yyjson_mut_obj_add_str(doc, root, "name", me->name);
  }
  size_t len = 0;
  char *json = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (json) mc_channel_send_clear(ch, MC_MSG_PAIR_REFUSE, json, len);
  free(json);
}

/* who, for B's answer to PAIR_HELLO (else NULL): B's dev, name and versions as judged here. A
 * refusal there without versions comes from a release before them, which refused by its join
 * rules: whatever it said, it's the version that stands in the way, and it gets "version". */
static void parse_refuse(mc_sync *sync, const unsigned char *payload, size_t len,
                         mc_pair_reason *reason, int *attempts_left, side *who) {
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  mc_version_self self;
  mc_sync_self_version(sync, &self);
  *reason = reason_parse(yyjson_get_str(yyjson_obj_get(root, "reason")));
  /* A build that doesn't know the reason (as releases up to 0.9.4) takes it for a failure. */
  if (*reason == MC_PAIR_VERSION && !self.check) *reason = MC_PAIR_FAILED;
  if (who && self.check && !yyjson_obj_get(root, "v")) *reason = MC_PAIR_VERSION;
  yyjson_val *a = yyjson_obj_get(root, "attempts_left");
  *attempts_left = yyjson_is_int(a) ? (int)yyjson_get_sint(a) : 0;
  if (*reason == MC_PAIR_VERSION && who) {
    if (mc_version_read(&self, root, &who->ver))
      /* Refused by a rule this build doesn't know: the newer one knows best. */
      who->ver.outdated = who->ver.protocol > self.protocol ? MC_SYNC_SELF_OUTDATED
                                                            : MC_SYNC_PEER_OUTDATED;
    const char *dev = yyjson_get_str(yyjson_obj_get(root, "dev"));
    if (is_uid(dev) && !who->dev[0]) snprintf(who->dev, sizeof who->dev, "%s", dev);
    if (!who->name[0]) copy_field(root, "name", who->name, sizeof who->name);
  }
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
 *  - library_uid: the group's library, adopted first if this device is joining (adopt)
 *    and its library is empty and has another uid. The device whose group it is keeps
 *    whatever library it has open: it may have another one open (MC_SYNC_LIBRARY_OTHER),
 *    with music, and only group data changes here;
 *  - archive (new groups only, on the device whose group it is): the first archive, with
 *    sync_group, and this device's own row;
 *  - this device's profile: the archive's if it is the archive of a new group, which only
 *    it may write; else, on the device that joins (adopt), not chosen yet, with a budget
 *    from its free space (mc_profile_prefill());
 *  - the other device's row. */
static mc_status apply_pairing(mc_sync *sync, mc_sync_conn *c, const char *group_uid, const char *library_uid,
                               const char *archive, const side *me, const side *other,
                               const unsigned char *other_key, const char *added_by,
                               int adopt) {
  int self_archive = archive && !strcmp(archive, me->dev);
  char current[MC_UID_STRLEN];
  text_column(mc_library_db(c->lib), "SELECT value FROM meta WHERE key = ?1", "library_uid",
              current, sizeof current);
  mc_status st = MC_OK;
  if (adopt && strcmp(current, library_uid) &&
      (st = mc_library_adopt_uid(c->lib, library_uid)) != MC_OK)
    return st;

  mc_sync_db *sdb = c->sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  int64_t budget[1] = {0};
  if ((st = mc_sync_db_begin_write(sdb)) != MC_OK) return st;
  const char *g[] = {group_uid};
  st = bind_exec(db, "UPDATE local SET group_uid = ?1 WHERE id = 1", g, 1);
  if (st == MC_OK && archive && added_by) {
    const char *grp[] = {group_uid, library_uid};
    st = bind_exec(db, "INSERT OR IGNORE INTO sync_group (uid, library_uid) VALUES (?1, ?2)", grp,
                   2);
    if (st == MC_OK && added_by) st = write_member(db, me, mc_sync_db_public_key(sdb), NULL);
  }
  if (st == MC_OK && self_archive) {
    const char *arch[] = {me->dev};
    st = bind_exec(db,
                   "INSERT INTO profiles (device_uid, archive, mode, format)"
                   " VALUES (?1, 1, 'all', 'original') ON CONFLICT (device_uid) DO UPDATE SET"
                   " archive = 1, mode = 'all', format = 'original'",
                   arch, 1);
    if (st == MC_OK) st = bind_exec(db, "UPDATE local SET profile_chosen = 1 WHERE id = 1", NULL, 0);
  } else if (st == MC_OK && adopt) {
    st = mc_profile_prefill(sdb, mc_sync_free_space(c->lib), budget);
  }
  if (st == MC_OK) st = write_member(db, other, other_key, added_by);
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  if (st == MC_OK && budget[0] > 0)
    mc_sync_add_notice(sync, MC_ISSUE_NOTICE_BUDGET, mc_sync_db_device_uid(sdb), budget[0]);
  return st;
}

/* The shown names (profile name, else device name) of the group's current archives, a void
 * demotion included, by name, as a JSON array in doc; NULL on error. */
static yyjson_mut_val *archive_names(yyjson_mut_doc *doc, sqlite3 *db) {
  char void_uid[MC_UID_STRLEN] = "";
  if (mc_archive_void(db, void_uid) != MC_OK) return NULL;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db,
                         "SELECT coalesce(nullif(p.name, ''), d.name) AS shown FROM devices d"
                         " JOIN profiles p ON p.device_uid = d.uid"
                         " WHERE d.removed = 0 AND (p.archive = 1 OR d.uid = ?1)"
                         " ORDER BY shown COLLATE NOCASE, d.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    return NULL;
  sqlite3_bind_text(stmt, 1, void_uid, -1, SQLITE_STATIC);
  yyjson_mut_val *arr = yyjson_mut_arr(doc);
  while (arr && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *name = (const char *)sqlite3_column_text(stmt, 0);
    yyjson_mut_arr_add_strcpy(doc, arr, name ? name : "");
  }
  sqlite3_finalize(stmt);
  return arr;
}

/* group_uid NULL sends B's empty PAIR_ACCEPT. db, when not NULL, is the group the other
 * device joins: its archives' names go with it. */
static mc_status send_accept(mc_channel *ch, const char *group_uid, const char *library_uid,
                             const char *archive, sqlite3 *db) {
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
    yyjson_mut_val *names = db ? archive_names(doc, db) : NULL;
    if (names) yyjson_mut_obj_add_val(doc, root, "archive_names", names);
  }
  size_t len = 0;
  char *json = yyjson_mut_write(doc, 0, &len);
  yyjson_mut_doc_free(doc);
  if (!json) return MC_ERR_NOMEM;
  mc_status st = mc_channel_send(ch, MC_MSG_PAIR_ACCEPT, json, len);
  free(json);
  return st;
}

/* What the device that joined learns of its group's archives. */
typedef struct joined_archives {
  char first[MC_SYNC_NAME_MAX]; /* "" if not known */
  int count;
} joined_archives;

/* PAIR_ACCEPT's group, or "" for B's empty one ("send me yours"). arch (may be NULL) gets
 * the archives' names, which a member sends to the device joining its group (older versions
 * don't). */
static mc_status parse_accept(const unsigned char *payload, size_t len,
                              char group_uid[MC_UID_STRLEN], char library_uid[MC_UID_STRLEN],
                              joined_archives *arch) {
  group_uid[0] = library_uid[0] = '\0';
  yyjson_doc *doc = yyjson_read((const char *)payload, len, 0);
  yyjson_val *root = yyjson_doc_get_root(doc);
  mc_status st = MC_ERR_FORMAT;
  if (yyjson_is_obj(root)) {
    copy_field(root, "group_uid", group_uid, MC_UID_STRLEN);
    copy_field(root, "library_uid", library_uid, MC_UID_STRLEN);
    if (!group_uid[0] || (is_uid(group_uid) && is_uid(library_uid))) st = MC_OK;
    yyjson_val *names = yyjson_obj_get(root, "archive_names");
    if (arch && yyjson_is_arr(names) && yyjson_arr_size(names) > 0) {
      const char *first = yyjson_get_str(yyjson_arr_get_first(names));
      snprintf(arch->first, sizeof arch->first, "%s", first ? first : "");
      arch->count = (int)yyjson_arr_size(names);
    }
  }
  yyjson_doc_free(doc);
  return st;
}

static mc_status recv_accept(mc_channel *ch, char group_uid[MC_UID_STRLEN],
                             char library_uid[MC_UID_STRLEN], joined_archives *arch) {
  uint8_t type;
  const unsigned char *payload;
  size_t len;
  mc_status st = mc_channel_recv(ch, &type, &payload, &len);
  if (st == MC_OK && type != MC_MSG_PAIR_ACCEPT) st = MC_ERR_FORMAT;
  if (st == MC_OK) st = parse_accept(payload, len, group_uid, library_uid, arch);
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
  s.asked = 1; /* a user on each side is looking at the pairing */
  int initiator = mc_channel_is_initiator(ch);
  int lower = initiator ? strcmp(sync->device_uid, peer) < 0 : strcmp(peer, sync->device_uid) < 0;
  if (!mc_sync_claim(sync, peer, lower, ch)) return; /* a session is already running */
  mc_status st = mc_session_run(&s);
  /* Now that the group's rows are here: this device's name becomes its profile name, unless
   * the group already has one for it (it was a member before). The peer hears of it next
   * time. */
  if (st == MC_OK && !s.we_removed && mc_sync_seed_profile_name(sync, c)) s.follow_up = 1;
  mc_sync_result r = {.device_uid = peer, .status = st, .library_synced = s.library_synced,
                      .converged = st == MC_OK && s.converged, .rows_sent = s.rows_sent,
                      .rows_received = s.rows_received, .user = 1, .connected = 1};
  mc_transfer_fill_result(&s, &r);
  r.cancelled = mc_sync_release(sync, peer) && st != MC_OK;
  if (s.we_removed) mc_sync_forget_group(sync, c);
  mc_sync_session_ended(sync, c, peer, &r, s.rows_new, s.follow_up, initiator ? address : NULL, port);
}

/* host and port: where a device that asked to pair can be found again. */
static void result_at(mc_sync *sync, const side *peer, int initiator, mc_pair_reason reason,
                      int attempts_left, mc_status st, const char *host, uint16_t port) {
  mc_pair_result r;
  memset(&r, 0, sizeof r);
  if (reason == MC_PAIR_VERSION && peer) {
    r.version = peer->ver;
    mc_sync_note_version(sync, peer->dev, &peer->ver);
  }
  r.device_uid = peer ? peer->dev : "";
  r.name = peer ? peer->name : "";
  r.initiator = initiator;
  r.reason = reason;
  r.attempts_left = attempts_left;
  r.status = st;
  r.joined = 0;
  r.archive_name = "";
  r.archives = 0;
  r.host = host;
  r.port = port;
  mc_sync_emit_pair_result(sync, &r);
}

static void result(mc_sync *sync, const side *peer, int initiator, mc_pair_reason reason,
                   int attempts_left, mc_status st) {
  result_at(sync, peer, initiator, reason, attempts_left, st, "", 0);
}

/* MC_PAIR_PAIRED. arch is NULL on the device that didn't join, else what it knows of the
 * group's archives. */
static void paired(mc_sync *sync, const side *peer, int initiator, const joined_archives *arch) {
  mc_pair_result r;
  memset(&r, 0, sizeof r);
  r.device_uid = peer->dev;
  r.name = peer->name;
  r.initiator = initiator;
  r.reason = MC_PAIR_PAIRED;
  r.status = MC_OK;
  r.joined = arch != NULL;
  r.archive_name = arch ? arch->first : "";
  r.archives = arch ? arch->count : 0;
  r.host = "";
  mc_sync_emit_pair_result(sync, &r);
}

/* A new group syncing the other device's library: it's the archive. */
static joined_archives peer_is_archive(const side *peer) {
  joined_archives a;
  snprintf(a.first, sizeof a.first, "%s", peer->name);
  a.count = 1;
  return a;
}

/* --- A: asking to pair --- */

/* Waits for mc_sync_pair_enter(). 1 when the code is in, 0 if cancelled, stopped or timed
 * out, -2 if B said something or went: nothing but a refusal comes from it before the
 * code. */
static int wait_for_code(mc_sync *sync, mc_channel *ch, char code[MC_PAIR_CODE_DIGITS + 1]) {
  uint64_t deadline = mc_mono_ms() + CODE_WAIT_MS;
  int heard = 0;
  mc_mutex_lock(&sync->lock);
  while (!sync->out_has_code && !sync->out_cancel && !sync->stopping &&
         mc_mono_ms() < deadline && !(heard = mc_channel_readable(ch)))
    mc_cond_wait_ms(&sync->cond, &sync->lock, 500);
  int ok = sync->out_cancel || sync->stopping ? 0
           : heard                            ? -2
           : sync->out_has_code;
  if (ok > 0) memcpy(code, sync->out_code, MC_PAIR_CODE_DIGITS + 1);
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
    if ((st = send_side(sync, ch, MC_MSG_PAIR_HELLO, &me)) != MC_OK) break;

    uint8_t type;
    const unsigned char *payload;
    size_t len;
    if ((st = mc_channel_recv_clear(ch, &type, &payload, &len)) != MC_OK) break;
    if (type == MC_MSG_PAIR_REFUSE) {
      parse_refuse(sync, payload, len, &reason, &attempts_left, &peer);
      if (reason == MC_PAIR_VERSION && !peer.dev[0])
        snprintf(peer.dev, sizeof peer.dev, "%s", t->device_uid); /* a release that didn't say */
      st = reason_status(reason);
      break;
    }
    side ready;
    st = type == MC_MSG_PAIR_READY ? parse_side(sync, payload, len, &ready) : MC_ERR_FORMAT;
    if (type == MC_MSG_PAIR_READY && !ready.compatible) {
      /* B accepted, so it doesn't check versions: a release from before them. It waits for
       * JPAKE_1, takes the refusal for A giving up, and stops showing its code. */
      peer = ready;
      reason = MC_PAIR_VERSION;
      st = MC_ERR_VERSION;
      send_refuse(sync, ch, MC_PAIR_VERSION, 0, &me);
      break;
    }
    if (st != MC_OK) {
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
    int got = wait_for_code(sync, ch, code);
    if (got == -2) {
      /* B gave up: there's no code to wait for. */
      reason = MC_PAIR_CANCELLED;
      st = mc_channel_recv_clear(ch, &type, &payload, &len);
      if (st == MC_OK && type == MC_MSG_PAIR_REFUSE)
        parse_refuse(sync, payload, len, &reason, &attempts_left, NULL);
      st = reason == MC_PAIR_CANCELLED ? MC_ERR_NET : reason_status(reason);
      break;
    }
    if (got <= 0) {
      reason = MC_PAIR_CANCELLED;
      send_refuse(sync, ch, MC_PAIR_CANCELLED, 0, NULL); /* so B stops showing the code */
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
      parse_refuse(sync, payload, len, &reason, &attempts_left, NULL);
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
    joined_archives arch = {"", 0};
    if (st == MC_OK) st = parse_accept(payload, len, group_uid, library_uid, &arch);
    if (st != MC_OK) break;

    if (out == B_JOINS_A) {
      /* Our group: B waits for it. */
      if (group_uid[0] || !is_uid(me.group_library_uid)) {
        st = MC_ERR_FORMAT;
        break;
      }
      const char *group = mc_sync_db_group_uid(c.sdb);
      snprintf(group_uid, sizeof group_uid, "%s", group ? group : "");
      st = apply_pairing(sync, &c, group_uid, me.group_library_uid, NULL, &me, &peer, peer_key,
                         me.dev, 0);
      if (st == MC_OK)
        st = send_accept(ch, group_uid, me.group_library_uid, NULL, mc_sync_db_handle(c.sdb));
    } else {
      if (!group_uid[0]) {
        st = MC_ERR_FORMAT;
        break;
      }
      st = apply_pairing(sync, &c, group_uid, library_uid, out == NEW_GROUP_A_LIBRARY ? me.dev : NULL,
                         &me, &peer, peer_key, NULL, 1);
    }
    if (st != MC_OK) {
      reason = st == MC_ERR_LIBRARY_MISMATCH ? MC_PAIR_LIBRARY_MISMATCH : MC_PAIR_FAILED;
      break;
    }
    reason = MC_PAIR_PAIRED;
    mc_sync_group_changed(sync, &c);
    /* A joined B's group, or a new group syncing B's library. */
    if (out == NEW_GROUP_B_LIBRARY) arch = peer_is_archive(&peer);
    paired(sync, &peer, 1, out == A_JOINS_B || out == NEW_GROUP_B_LIBRARY ? &arch : NULL);
    first_session(sync, &c, ch, peer.dev, t->host, t->port);
    break;
  }

  if (reason != MC_PAIR_PAIRED) {
    if (out_cancelled(sync)) reason = MC_PAIR_CANCELLED;
    result_at(sync, peer.dev[0] || reason == MC_PAIR_VERSION ? &peer : NULL, 1, reason,
              attempts_left, st, "", 0);
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
  char from[MC_SYNC_ADDR_MAX];
  mc_reach_host_of(mc_channel_peer_addr(ch), from);
  if (st != MC_OK || type != MC_MSG_PAIR_HELLO) goto untrack;
  st = parse_side(sync, payload, len, &peer);
  if (!peer.compatible) {
    /* Before the code, the join rules and anything else: nothing is shown or written. A
     * release from before versions takes the refusal for a failure. */
    if (my_side(sync, c, &me) != MC_OK) snprintf(me.dev, sizeof me.dev, "%s", sync->device_uid);
    send_refuse(sync, ch, MC_PAIR_VERSION, 0, &me);
    result_at(sync, &peer, 0, MC_PAIR_VERSION, 0, MC_ERR_VERSION, from, peer.port);
    goto untrack;
  }
  if (st != MC_OK || !strcmp(peer.dev, sync->device_uid)) goto untrack;

  /* One requester at a time; it may come back after a wrong code. */
  mc_mutex_lock(&sync->lock);
  /* A group of one being forgotten: decide as the device it is afterwards. */
  while (sync->dissolving) mc_cond_wait_ms(&sync->cond, &sync->lock, 100);
  uint64_t now = mc_mono_ms();
  if (sync->in_active && !sync->in_conn && now >= sync->in_expires_ms) sync->in_active = 0;
  int busy = sync->in_conn || (sync->in_active && strcmp(sync->in_requester, peer.dev));
  if (!busy) sync->in_conn = 1;
  mc_mutex_unlock(&sync->lock);
  if (busy) {
    send_refuse(sync, ch, MC_PAIR_BUSY, 0, NULL);
    goto untrack;
  }

  outcome out = NEW_GROUP_B_LIBRARY;
  mc_pair_reason reason = MC_PAIR_FAILED;
  if ((st = my_side(sync, c, &me)) == MC_OK) reason = decide(&peer, &me, &out);
  if (reason != MC_PAIR_PAIRED) {
    send_refuse(sync, ch, reason, 0, NULL);
    result_at(sync, &peer, 0, reason, 0, reason_status(reason), from, peer.port);
    mc_mutex_lock(&sync->lock);
    sync->in_conn = 0;
    mc_mutex_unlock(&sync->lock);
    goto untrack;
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

  if ((st = send_side(sync, ch, MC_MSG_PAIR_READY, &me)) != MC_OK) goto done;
  mc_sync_track_pairing(sync, ch, 1);
  /* The user may take a while to type the code. */
  mc_channel_set_timeout(ch, CODE_WAIT_MS);
  unsigned char peer_key[MC_SYNC_KEY_LEN];
  int failures = sync->in_r.failures, new_code = 0;
  st = mc_pair_respond(&sync->in_r, ch, mc_sync_db_private_key(c->sdb),
                       mc_sync_db_public_key(c->sdb), peer_key, &new_code);
  mc_channel_set_timeout(ch, mc_channel_default_timeout());

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
      send_refuse(sync, ch, MC_PAIR_WRONG_CODE, attempts, NULL);
      result(sync, &peer, 0, MC_PAIR_WRONG_CODE, attempts, MC_ERR_FORMAT);
      goto untrack;
    }
    /* A gave up (or cancelled, or the connection broke) before guessing. */
    result(sync, &peer, 0, MC_PAIR_CANCELLED, 0, st);
    goto done;
  }

  /* Paired. Settle the group, then carry on as a session. */
  char group_uid[MC_UID_STRLEN], library_uid[MC_UID_STRLEN];
  joined_archives arch = {"", 0};
  if (out == B_JOINS_A) {
    st = send_accept(ch, NULL, NULL, NULL, NULL);
    if (st == MC_OK) st = recv_accept(ch, group_uid, library_uid, &arch);
    if (st == MC_OK && !group_uid[0]) st = MC_ERR_FORMAT;
    if (st == MC_OK)
      st = apply_pairing(sync, c, group_uid, library_uid, NULL, &me, &peer, peer_key, NULL, 1);
  } else if (out == A_JOINS_B) {
    const char *group = mc_sync_db_group_uid(c->sdb);
    snprintf(group_uid, sizeof group_uid, "%s", group ? group : "");
    st = is_uid(me.group_library_uid) ? MC_OK : MC_ERR_NOT_FOUND;
    if (st == MC_OK)
      st = apply_pairing(sync, c, group_uid, me.group_library_uid, NULL, &me, &peer, peer_key, me.dev,
                         0);
    if (st == MC_OK)
      st = send_accept(ch, group_uid, me.group_library_uid, NULL, mc_sync_db_handle(c->sdb));
  } else {
    const side *holder = out == NEW_GROUP_A_LIBRARY ? &peer : &me;
    snprintf(library_uid, sizeof library_uid, "%s", holder->library_uid);
    st = mc_uid_new(group_uid);
    if (st == MC_OK)
      st = apply_pairing(sync, c, group_uid, library_uid, holder->dev, &me, &peer, peer_key, me.dev, 1);
    if (st == MC_OK) st = send_accept(ch, group_uid, library_uid, holder->dev, NULL);
  }
  if (st != MC_OK) {
    result_at(sync, &peer, 0,
              st == MC_ERR_LIBRARY_MISMATCH ? MC_PAIR_LIBRARY_MISMATCH : MC_PAIR_FAILED, 0, st,
              from, peer.port);
    goto done;
  }
  mc_mutex_lock(&sync->lock);
  sync->in_active = sync->in_conn = 0;
  mc_pair_responder_wipe(&sync->in_r);
  mc_mutex_unlock(&sync->lock);
  mc_sync_group_changed(sync, c);
  /* B joined A's group, or a new group syncing A's library. */
  if (out == NEW_GROUP_A_LIBRARY) arch = peer_is_archive(&peer);
  paired(sync, &peer, 0, out == B_JOINS_A || out == NEW_GROUP_A_LIBRARY ? &arch : NULL);
  first_session(sync, c, ch, peer.dev, NULL, 0);
  goto untrack;

done:
  mc_mutex_lock(&sync->lock);
  sync->in_active = sync->in_conn = 0;
  mc_pair_responder_wipe(&sync->in_r);
  mc_mutex_unlock(&sync->lock);
untrack:
  mc_sync_untrack(sync, ch);
  mc_channel_close(ch);
}
