/* The mc_sync object: the listener, the scheduler, discovery, pairing and the public API.
 * Sessions are in session.c, pairing's protocol in pairing.c, rows in rows.c. See
 * docs/sync-protocol.md.
 *
 * Scheduling. Each group member gets a session when discovery reports it online, every
 * PERIOD_MS, and CHANGE_DELAY_MS after a local change (polled from the changelogs, or
 * mc_sync_notify_changed()). The lower device uid of a pair is the preferred initiator: on
 * the online and periodic triggers the higher one waits HIGHER_GRACE_MS and only initiates
 * if it hasn't been contacted by then (the lower one may be behind a firewall). A change
 * only one side knows about is sent by that side at once. Two sessions between one pair
 * never run together: see mc_sync_claim(). Peers are tried at their discovered address,
 * then their address hints, then the address that last worked. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "discovery/local_addr.h"
#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/rows.h"
#include "sync/session.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"

#define PERIOD_MS (5 * 60 * 1000)
#define CHANGE_DELAY_MS 3000
#define HIGHER_GRACE_MS 30000
#define POLL_MS 2000
#define HINTS_MS 60000
#define BACKOFF_MIN_MS 10000
#define CONNECT_MS 3000
#define ACCEPT_WAIT_MS 1000
#define CLAIM_WAIT_MS 5000
#define MAX_ADDRS 8
/* How long a press of Sync waits for a session to carry it: long enough for a member that
 * only it can reach out from to call back, not so long that a press confirms edits made
 * long after it. */
#define USER_WINDOW_MS (PERIOD_MS + HIGHER_GRACE_MS)

/* --- Small helpers --- */

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

const char *mc_sync_kind_str(mc_device_kind kind) {
  switch (kind) {
  case MC_DEVICE_PHONE: return "phone";
  case MC_DEVICE_WATCH: return "watch";
  default: return "desktop";
  }
}

mc_device_kind mc_sync_kind_parse(const char *s) {
  if (s && !strcmp(s, "phone")) return MC_DEVICE_PHONE;
  if (s && !strcmp(s, "watch")) return MC_DEVICE_WATCH;
  return MC_DEVICE_DESKTOP;
}

static char *dup_or_null(const char *s) {
  return s && *s ? mc_strdup(s) : NULL;
}

/* Length of s cut to at most max bytes without splitting a UTF-8 sequence. */
static size_t utf8_cut(const char *s, size_t max) {
  size_t n = strlen(s);
  if (n <= max) return n;
  n = max;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return n;
}

static int is_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

/* name with the white space around it trimmed, cut to MC_SYNC_DEVICE_NAME_MAX bytes at a
 * character boundary; "" if nothing is left. */
static void clean_name(const char *name, char out[MC_SYNC_NAME_MAX]) {
  const char *s = name ? name : "";
  while (is_space(*s)) s++;
  size_t n = utf8_cut(s, MC_SYNC_DEVICE_NAME_MAX);
  while (n > 0 && is_space(s[n - 1])) n--;
  memcpy(out, s, n);
  out[n] = '\0';
}

mc_status mc_sync_conn_open(mc_sync *sync, mc_sync_conn *c) {
  memset(c, 0, sizeof *c);
  mc_status st = mc_sync_db_open(sync->config_dir, &c->sdb);
  if (st == MC_OK) st = mc_library_open(sync->lib_dir, &c->lib);
  if (st != MC_OK) mc_sync_conn_close(c);
  return st;
}

void mc_sync_conn_close(mc_sync_conn *c) {
  mc_library_close(c->lib);
  mc_sync_db_close(c->sdb);
  c->lib = NULL;
  c->sdb = NULL;
}

/* Under lock. */
static mc_sync_peer *find_peer(mc_sync *sync, const char *uid, int create) {
  for (int i = 0; i < sync->npeers; i++)
    if (!strcmp(sync->peers[i].uid, uid)) return &sync->peers[i];
  if (!create || !is_uid(uid)) return NULL;
  mc_sync_peer *grown = realloc(sync->peers, (size_t)(sync->npeers + 1) * sizeof *grown);
  if (!grown) return NULL;
  sync->peers = grown;
  mc_sync_peer *p = &sync->peers[sync->npeers++];
  memset(p, 0, sizeof *p);
  memcpy(p->uid, uid, MC_UID_STRLEN);
  return p;
}

int mc_sync_peer_online(mc_sync *sync, const char *uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  int online = p && (p->discovered || p->reached);
  mc_mutex_unlock(&sync->lock);
  return online;
}

uint64_t mc_sync_peer_swap_prepared(mc_sync *sync, const char *uid, uint64_t sig) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 1);
  uint64_t old = 0;
  if (p) {
    old = p->prepared_sig;
    p->prepared_sig = sig;
  }
  mc_mutex_unlock(&sync->lock);
  return old;
}

int mc_sync_take_user(mc_sync *sync, const char *peer_uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 0);
  int user = p && p->user_ms && mc_mono_ms() < p->user_ms + USER_WINDOW_MS;
  if (p) p->user_ms = 0;
  mc_mutex_unlock(&sync->lock);
  return user;
}

/* --- Callbacks --- */

static void emit_peer(mc_sync *sync, const char *uid, int online, int member) {
  if (!sync->ev.on_peer) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_peer(sync->user, uid, online, member);
  mc_mutex_unlock(&sync->cb_lock);
}

void mc_sync_emit_pair_code(mc_sync *sync, const char *code, const char *name, int attempts) {
  if (!sync->ev.on_pair_code) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_pair_code(sync->user, code, name, attempts);
  mc_mutex_unlock(&sync->cb_lock);
}

void mc_sync_emit_pair_result(mc_sync *sync, const mc_pair_result *r) {
  if (!sync->ev.on_pair_result) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_pair_result(sync->user, r);
  mc_mutex_unlock(&sync->cb_lock);
}

void mc_sync_emit_progress(mc_sync *sync, const mc_sync_progress *p) {
  if (!sync->ev.on_progress) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_progress(sync->user, p);
  mc_mutex_unlock(&sync->cb_lock);
}

static void emit_done(mc_sync *sync, const mc_sync_result *r) {
  if (!sync->ev.on_sync_done) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_sync_done(sync->user, r);
  mc_mutex_unlock(&sync->cb_lock);
}

/* --- Workers and channels --- */

typedef struct spawn_arg {
  mc_sync *sync;
  mc_sync_worker *w;
  void (*fn)(void *);
  void *arg;
} spawn_arg;

static void worker_main(void *p) {
  spawn_arg a = *(spawn_arg *)p;
  free(p);
  a.fn(a.arg);
  mc_mutex_lock(&a.sync->lock);
  a.w->done = 1;
  mc_cond_broadcast(&a.sync->cond);
  mc_mutex_unlock(&a.sync->lock);
}

mc_status mc_sync_spawn(mc_sync *sync, void (*fn)(void *), void *arg) {
  mc_sync_worker *w = calloc(1, sizeof *w);
  spawn_arg *a = malloc(sizeof *a);
  if (!w || !a) {
    free(w);
    free(a);
    return MC_ERR_NOMEM;
  }
  a->sync = sync;
  a->w = w;
  a->fn = fn;
  a->arg = arg;
  mc_mutex_lock(&sync->lock);
  mc_status st = sync->stopping ? MC_ERR_INVALID_ARG : mc_thread_start(&w->thread, worker_main, a);
  if (st == MC_OK) {
    w->next = sync->workers;
    sync->workers = w;
  }
  mc_mutex_unlock(&sync->lock);
  if (st != MC_OK) {
    free(w);
    free(a);
  }
  return st;
}

/* Joins finished workers, or all of them. */
static void reap(mc_sync *sync, int all) {
  for (;;) {
    mc_mutex_lock(&sync->lock);
    mc_sync_worker **pp = &sync->workers, *w = NULL;
    while (*pp && !w) {
      if ((*pp)->done || all) {
        w = *pp;
        *pp = w->next;
      } else {
        pp = &(*pp)->next;
      }
    }
    mc_mutex_unlock(&sync->lock);
    if (!w) return;
    mc_thread_join(&w->thread);
    free(w);
  }
}

static void track(mc_sync *sync, mc_channel *ch, int role) {
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->nchans; i++) {
    if (sync->chans[i].ch != ch) continue;
    sync->chans[i].role = role; /* e.g. a served connection that turned out to be pairing */
    mc_mutex_unlock(&sync->lock);
    return;
  }
  if (sync->nchans == sync->chans_cap) {
    int grown = sync->chans_cap ? sync->chans_cap * 2 : 8;
    struct mc_sync_tracked *more = realloc(sync->chans, (size_t)grown * sizeof *more);
    if (more) {
      sync->chans = more;
      sync->chans_cap = grown;
    }
  }
  if (sync->nchans < sync->chans_cap) {
    sync->chans[sync->nchans].ch = ch;
    sync->chans[sync->nchans].role = role;
    sync->nchans++;
  }
  if (sync->stopping) mc_channel_abort(ch);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_track(mc_sync *sync, mc_channel *ch) {
  track(sync, ch, 0);
}

void mc_sync_track_pairing(mc_sync *sync, mc_channel *ch, int incoming) {
  track(sync, ch, incoming ? 2 : 1);
}

void mc_sync_untrack(mc_sync *sync, mc_channel *ch) {
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->nchans; i++) {
    if (sync->chans[i].ch != ch) continue;
    sync->chans[i] = sync->chans[--sync->nchans];
    break;
  }
  mc_mutex_unlock(&sync->lock);
}

/* Under lock. role -1: all of them. */
static void abort_channels(mc_sync *sync, int role) {
  for (int i = 0; i < sync->nchans; i++)
    if (role < 0 || sync->chans[i].role == role) mc_channel_abort(sync->chans[i].ch);
}

/* --- One session per peer --- */

int mc_sync_claim(mc_sync *sync, const char *peer_uid, int lower, mc_channel *ch) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 1);
  int ok = 0;
  if (p && p->busy && lower && !p->busy_lower) {
    /* Both sides called each other: the session the lower uid started wins. */
    p->abort = 1;
    if (p->busy_ch) mc_channel_abort(p->busy_ch);
    uint64_t deadline = mc_mono_ms() + CLAIM_WAIT_MS;
    while ((p = find_peer(sync, peer_uid, 0)) && p->busy && !sync->stopping &&
           mc_mono_ms() < deadline)
      mc_cond_wait_ms(&sync->cond, &sync->lock, 200);
  }
  if (p && !p->busy && !sync->stopping) {
    p->busy = 1;
    p->busy_lower = lower;
    p->busy_ch = ch;
    p->abort = 0;
    ok = 1;
  }
  mc_mutex_unlock(&sync->lock);
  return ok;
}

void mc_sync_release(mc_sync *sync, const char *peer_uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 0);
  if (p) {
    p->busy = 0;
    p->busy_ch = NULL;
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

/* --- Names --- */

/* This device's profile name, cleaned; "" if it has none or isn't in a group. */
static void own_profile_name(mc_sync_conn *c, char out[MC_SYNC_NAME_MAX]) {
  out[0] = '\0';
  if (!mc_sync_db_group_uid(c->sdb)) return;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                         "SELECT name FROM profiles WHERE device_uid = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(c->sdb), -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) clean_name((const char *)sqlite3_column_text(stmt, 0), out);
  sqlite3_finalize(stmt);
}

mc_status mc_sync_write_profile_name(mc_sync_conn *c, const char *name, int only_if_unset,
                                     int *wrote) {
  if (wrote) *wrote = 0;
  mc_sync_db *sdb = c->sdb;
  sqlite3 *db = mc_sync_db_handle(sdb);
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  /* An upsert that sets only the name: on an existing row only that column is logged. */
  sqlite3_stmt *stmt;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(db,
                         only_if_unset
                           ? "INSERT INTO profiles (device_uid, name) VALUES (?1, ?2)"
                             " ON CONFLICT (device_uid) DO UPDATE SET name = excluded.name"
                             " WHERE coalesce(profiles.name, '') = ''"
                           : "INSERT INTO profiles (device_uid, name) VALUES (?1, ?2)"
                             " ON CONFLICT (device_uid) DO UPDATE SET name = excluded.name"
                             " WHERE profiles.name IS NOT excluded.name",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(sdb), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  int changes = st == MC_OK ? sqlite3_changes(db) : 0;
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  if (st == MC_OK && wrote) *wrote = changes > 0;
  return st;
}

/* This device is now called name: announced at once, written to its devices row (in a
 * group), and passed on to the members. c is the caller's connection (the API's under
 * api_lock). Takes lock, so not under it. */
static mc_status adopt_name(mc_sync *sync, mc_sync_conn *c, const char *name) {
  mc_mutex_lock(&sync->lock);
  if (strcmp(sync->name, name) != 0) {
    snprintf(sync->name, sizeof sync->name, "%s", name);
    if (sync->disc) mc_discovery_set_name(sync->disc, name);
  }
  mc_mutex_unlock(&sync->lock);

  mc_status st = MC_OK;
  if (mc_sync_db_group_uid(c->sdb) && (st = mc_sync_db_begin_write(c->sdb)) == MC_OK) {
    sqlite3_stmt *stmt;
    st = MC_ERR_DB;
    if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                           "UPDATE devices SET name = ?1 WHERE uid = ?2 AND name IS NOT ?1", -1,
                           &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
      sqlite3_bind_text(stmt, 2, mc_sync_db_device_uid(c->sdb), -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
      sqlite3_finalize(stmt);
    }
    if (st == MC_OK) st = mc_sync_db_commit(c->sdb);
    if (st != MC_OK) mc_sync_db_rollback(c->sdb);
  }
  mc_sync_notify_changed(sync);
  return st;
}

void mc_sync_take_group_name(mc_sync *sync, mc_sync_conn *c) {
  char name[MC_SYNC_NAME_MAX];
  own_profile_name(c, name);
  if (!name[0]) return;
  mc_mutex_lock(&sync->lock);
  int same = !strcmp(name, sync->name);
  mc_mutex_unlock(&sync->lock);
  if (!same) adopt_name(sync, c, name);
}

int mc_sync_seed_profile_name(mc_sync *sync, mc_sync_conn *c) {
  if (!mc_sync_db_group_uid(c->sdb)) return 0;
  char name[MC_SYNC_NAME_MAX];
  mc_mutex_lock(&sync->lock);
  memcpy(name, sync->name, sizeof name);
  mc_mutex_unlock(&sync->lock);
  int wrote = 0;
  if (mc_sync_write_profile_name(c, name, 1, &wrote) != MC_OK) return 0;
  return wrote;
}

/* --- Group state --- */

void mc_sync_group_changed(mc_sync *sync, mc_sync_conn *c) {
  char hash[MC_GROUP_HASH_STRLEN] = "";
  const char *group = mc_sync_db_group_uid(c->sdb);
  if (group) mc_discovery_group_hash(group, hash);
  mc_mutex_lock(&sync->lock);
  memcpy(sync->group_hash, hash, sizeof hash);
  if (sync->disc) mc_discovery_set_group(sync->disc, hash);
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_forget_group(mc_sync *sync, mc_sync_conn *c) {
  fprintf(stderr, "musiccore: sync: this device was removed from its group\n");
  mc_sync_db_clear_group(c->sdb);
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->npeers; i++) sync->peers[i].member = 0;
  mc_mutex_unlock(&sync->lock);
  mc_sync_group_changed(sync, c);
}

/* Under lock: every member but skip is due soon, from either side. */
static void trigger_members(mc_sync *sync, const char *skip, mc_sync_trigger why, uint64_t at) {
  for (int i = 0; i < sync->npeers; i++) {
    mc_sync_peer *p = &sync->peers[i];
    if (!p->member || (skip && !strcmp(p->uid, skip))) continue;
    p->trigger = why;
    p->trigger_ms = mc_mono_ms();
    p->due_ms = at;
  }
  mc_cond_broadcast(&sync->cond);
}


void mc_sync_session_ended(mc_sync *sync, mc_sync_conn *c, const char *peer_uid,
                           const mc_sync_result *result, int64_t rows_new, int follow_up,
                           const char *address, uint16_t port) {
  if (result->status == MC_OK && c->sdb) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                           "INSERT INTO peer_contact (device_uid, address, port, last_sync_ms)"
                           " VALUES (?1, ?2, ?3, ?4) ON CONFLICT (device_uid) DO UPDATE SET"
                           " address = coalesce(excluded.address, peer_contact.address),"
                           " port = coalesce(excluded.port, peer_contact.port),"
                           " last_sync_ms = excluded.last_sync_ms",
                           -1, &stmt, NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, peer_uid, -1, SQLITE_STATIC);
      if (address) {
        sqlite3_bind_text(stmt, 2, address, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, port);
      }
      sqlite3_bind_int64(stmt, 4, (sqlite3_int64)mc_wall_ms());
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    /* Before on_sync_done, so a frontend that asks then sees the new name. */
    mc_sync_take_group_name(sync, c);
  }
  uint64_t now = mc_mono_ms();
  int changed = 0, online = 0, member = 0;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 1);
  if (p) {
    int was = p->discovered || p->reached;
    p->contact_ms = now;
    if (result->status == MC_OK) {
      p->reached = 1;
      p->seen_ms = now;
      p->backoff_ms = 0;
      p->due_ms = now + PERIOD_MS;
      p->trigger = MC_TRIGGER_PERIODIC;
      p->trigger_ms = p->due_ms;
      if (address && !p->discovered) {
        snprintf(p->address, sizeof p->address, "%s", address);
        p->port = port;
      }
    } else if (result->status != MC_ERR_LOCKED) {
      p->reached = 0;
      p->backoff_ms = p->backoff_ms ? p->backoff_ms * 2 : BACKOFF_MIN_MS;
      if (p->backoff_ms > PERIOD_MS) p->backoff_ms = PERIOD_MS;
      p->due_ms = now + p->backoff_ms;
    }
    online = p->discovered || p->reached;
    member = p->member;
    changed = online != was;
  }
  /* What we just learnt, the others may lack too. The changelog poll would see it as a
   * local change, so it is told to skip it. */
  sync->absorb = 1;
  if (rows_new > 0) trigger_members(sync, peer_uid, MC_TRIGGER_CHANGED, now + CHANGE_DELAY_MS);
  if (follow_up && p && result->status == MC_OK) {
    p->trigger = MC_TRIGGER_CHANGED;
    p->trigger_ms = now;
    p->due_ms = now + CHANGE_DELAY_MS;
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  if (changed) emit_peer(sync, peer_uid, online, member);
  emit_done(sync, result);
}

/* --- Address hints --- */

static void compute_hints(mc_sync *sync, char out[MC_SYNC_HINTS_MAX]) {
  mc_mutex_lock(&sync->lock);
  uint16_t port = sync->port;
  int overridden = sync->hint_override[0] != '\0';
  if (overridden) memcpy(out, sync->hint_override, MC_SYNC_HINTS_MAX);
  mc_mutex_unlock(&sync->lock);
  if (overridden) return;
  char addrs[MAX_ADDRS][MC_LOCAL_ADDR_STRLEN];
  int n = port ? mc_local_addresses(addrs, MAX_ADDRS) : 0;
  size_t len = (size_t)snprintf(out, MC_SYNC_HINTS_MAX, "[");
  for (int i = 0; i < n && len < MC_SYNC_HINTS_MAX - 64; i++) {
    int v6 = strchr(addrs[i], ':') != NULL;
    len += (size_t)snprintf(out + len, MC_SYNC_HINTS_MAX - len, "%s\"%s%s%s:%u\"", i ? "," : "",
                            v6 ? "[" : "", addrs[i], v6 ? "]" : "", (unsigned)port);
  }
  snprintf(out + len, MC_SYNC_HINTS_MAX - len, "]");
  if (n == 0) out[0] = '\0';
}

/* Splits "1.2.3.4:5" or "[::1]:5". */
static int split_address(const char *s, char host[MC_SYNC_ADDR_MAX], uint16_t *port) {
  const char *colon;
  size_t n;
  if (s[0] == '[') {
    const char *end = strchr(s, ']');
    if (!end || end[1] != ':') return 0;
    n = (size_t)(end - s - 1);
    s++;
    colon = end + 1;
  } else {
    colon = strrchr(s, ':');
    if (!colon) return 0;
    n = (size_t)(colon - s);
  }
  if (!n || n >= MC_SYNC_ADDR_MAX) return 0;
  long v = strtol(colon + 1, NULL, 10);
  if (v <= 0 || v > 65535) return 0;
  memcpy(host, s, n);
  host[n] = '\0';
  *port = (uint16_t)v;
  return 1;
}

/* --- Initiating sessions --- */

typedef struct session_job {
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  char host[MAX_ADDRS][MC_SYNC_ADDR_MAX];
  uint16_t port[MAX_ADDRS];
  int n;
} session_job;

static void job_add(session_job *j, const char *host, uint16_t port) {
  if (!host || !*host || !port || j->n == MAX_ADDRS) return;
  for (int i = 0; i < j->n; i++)
    if (!strcmp(j->host[i], host) && j->port[i] == port) return;
  snprintf(j->host[j->n], MC_SYNC_ADDR_MAX, "%s", host);
  j->port[j->n++] = port;
}

static void job_add_hints(session_job *j, const char *json) {
  yyjson_doc *doc = json ? yyjson_read(json, strlen(json), 0) : NULL;
  yyjson_val *arr = yyjson_doc_get_root(doc), *v;
  size_t idx, max;
  if (yyjson_is_arr(arr)) {
    yyjson_arr_foreach(arr, idx, max, v) {
      char host[MC_SYNC_ADDR_MAX];
      uint16_t port;
      const char *s = yyjson_get_str(v);
      if (s && split_address(s, host, &port)) job_add(j, host, port);
    }
  }
  yyjson_doc_free(doc);
}

/* Where to reach a member: its discovered address, its hints, and where it last worked.
 * Reads the database, so not under lock; the discovered address is passed in. */
static void job_addresses(mc_sync_conn *c, session_job *j, const char *disc_host,
                          uint16_t disc_port) {
  job_add(j, disc_host, disc_port);
  sqlite3 *db = mc_sync_db_handle(c->sdb);
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, "SELECT addresses FROM capabilities WHERE device_uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, j->uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
      job_add_hints(j, (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
  }
  if (sqlite3_prepare_v2(db, "SELECT address, port FROM peer_contact WHERE device_uid = ?1", -1,
                         &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, j->uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW)
      job_add(j, (const char *)sqlite3_column_text(stmt, 0), (uint16_t)sqlite3_column_int(stmt, 1));
    sqlite3_finalize(stmt);
  }
}

static int stopping(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  int s = sync->stopping;
  mc_mutex_unlock(&sync->lock);
  return s;
}

/* Connects to a member and runs a session as the initiator. */
static mc_status initiate(mc_sync *sync, mc_sync_conn *c, session_job *j, int quiet) {
  unsigned char key[MC_SYNC_KEY_LEN];
  int have_key = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                         "SELECT public_key FROM devices WHERE uid = ?1 AND removed = 0"
                         " AND length(public_key) = ?2",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, j->uid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MC_SYNC_KEY_LEN);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      memcpy(key, sqlite3_column_blob(stmt, 0), MC_SYNC_KEY_LEN);
      have_key = 1;
    }
    sqlite3_finalize(stmt);
  }
  if (!have_key) return MC_ERR_NOT_FOUND;

  mc_sync_progress prog = {.device_uid = j->uid, .phase = MC_SYNC_PHASE_CONNECTING};
  mc_sync_emit_progress(sync, &prog);
  mc_channel *ch = NULL;
  mc_status st = MC_ERR_NET;
  int at = -1;
  for (int i = 0; i < j->n && !ch && !stopping(sync); i++) {
    if (mc_channel_connect(j->host[i], j->port[i], MC_MODE_SESSION, CONNECT_MS, &ch) == MC_OK)
      at = i;
  }
  mc_session s;
  memset(&s, 0, sizeof s);
  s.sync = sync;
  s.conn = c;
  s.ch = ch;
  memcpy(s.peer_uid, j->uid, MC_UID_STRLEN);
  int claimed = 0;
  if (ch) {
    mc_sync_track(sync, ch);
    claimed = mc_sync_claim(sync, j->uid, strcmp(sync->device_uid, j->uid) < 0, ch);
    if (claimed) s.user = mc_sync_take_user(sync, j->uid);
    st = claimed ? MC_OK : MC_ERR_LOCKED;
    if (st == MC_OK) st = mc_session_handshake_initiate(&s, key);
    if (st == MC_OK) st = mc_session_run(&s);
    if (claimed) mc_sync_release(sync, j->uid);
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
  }
  if (s.we_removed) mc_sync_forget_group(sync, c);
  if (!claimed && quiet) {
    /* Unreachable, or someone else is syncing with it: nothing to report, but the
     * CONNECTING above said a session began, so say it's over. */
    prog.phase = MC_SYNC_PHASE_DONE;
    mc_sync_emit_progress(sync, &prog);
    return st;
  }
  mc_sync_result r = {.device_uid = j->uid, .status = st, .library_synced = s.library_synced,
                      .converged = st == MC_OK && s.converged, .rows_sent = s.rows_sent,
                      .rows_received = s.rows_received};
  mc_transfer_fill_result(&s, &r);
  mc_sync_session_ended(sync, c, j->uid, &r, s.rows_new, s.follow_up, at >= 0 ? j->host[at] : NULL,
                        at >= 0 ? j->port[at] : 0);
  return st;
}

static void session_worker(void *arg) {
  session_job *j = arg;
  mc_sync_conn c;
  if (mc_sync_conn_open(j->sync, &c) == MC_OK) {
    job_addresses(&c, j, NULL, 0);
    initiate(j->sync, &c, j, 1);
    mc_sync_conn_close(&c);
  }
  free(j);
}

/* --- Serving connections --- */

typedef struct serve_arg {
  mc_sync *sync;
  mc_channel *ch;
} serve_arg;

static void serve_worker(void *arg) {
  serve_arg a = *(serve_arg *)arg;
  free(arg);
  mc_sync *sync = a.sync;
  mc_channel *ch = a.ch;
  /* Tracked from the start, so stopping cuts short a peer that never sends a thing. */
  mc_sync_track(sync, ch);
  mc_sync_conn c;
  if (mc_channel_read_preamble(ch) != MC_OK || mc_sync_conn_open(sync, &c) != MC_OK) {
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
    return;
  }
  if (mc_channel_mode(ch) == MC_MODE_PAIRING) {
    mc_pairing_respond(sync, &c, ch);
    mc_sync_conn_close(&c);
    return;
  }
  mc_session s;
  memset(&s, 0, sizeof s);
  s.sync = sync;
  s.conn = &c;
  s.ch = ch;
  mc_status st = mc_session_handshake_respond(&s);
  if (st == MC_OK) {
    if (mc_sync_claim(sync, s.peer_uid, strcmp(s.peer_uid, sync->device_uid) < 0, ch)) {
      s.user = mc_sync_take_user(sync, s.peer_uid);
      st = mc_session_run(&s);
      mc_sync_release(sync, s.peer_uid);
      if (s.we_removed) mc_sync_forget_group(sync, &c);
      mc_sync_result r = {.device_uid = s.peer_uid, .status = st,
                          .library_synced = s.library_synced,
                          .converged = st == MC_OK && s.converged, .rows_sent = s.rows_sent,
                          .rows_received = s.rows_received};
      mc_transfer_fill_result(&s, &r);
      mc_sync_session_ended(sync, &c, s.peer_uid, &r, s.rows_new, s.follow_up, NULL, 0);
    } else {
      /* A session with it is running already. */
      mc_session_refuse(ch, "busy", "already syncing");
    }
  }
  mc_sync_untrack(sync, ch);
  mc_channel_close(ch);
  mc_sync_conn_close(&c);
}

static void listen_main(void *arg) {
  mc_sync *sync = arg;
  for (;;) {
    mc_mutex_lock(&sync->lock);
    int stop = sync->stopping;
    mc_listener *l = sync->listener;
    mc_mutex_unlock(&sync->lock);
    if (stop) return;
    /* The preamble is read on the worker: a connection that never sends one mustn't hold
     * up the others, or stopping. */
    mc_channel *ch = NULL;
    mc_status st = mc_channel_accept_raw(l, ACCEPT_WAIT_MS, &ch);
    if (st != MC_OK) mc_sleep_ms(100);
    if (!ch) continue;
    serve_arg *a = malloc(sizeof *a);
    if (!a || mc_sync_spawn(sync, serve_worker, a) != MC_OK) {
      free(a);
      mc_channel_close(ch);
      continue;
    }
    a->sync = sync;
    a->ch = ch;
  }
}

/* --- Scheduler --- */

static int64_t max_seq(sqlite3 *db) {
  sqlite3_stmt *stmt;
  int64_t v = 0;
  if (sqlite3_prepare_v2(db, "SELECT max(seq) FROM changelog", -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return v;
}

typedef struct member_row {
  char uid[MC_UID_STRLEN];
  char name[MC_SYNC_NAME_MAX];
  char host[MC_SYNC_NAME_MAX];
  char kind[16];
} member_row;

static int load_members(mc_sync_conn *c, member_row **out) {
  *out = NULL;
  if (!mc_sync_db_group_uid(c->sdb)) return 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                         "SELECT uid, name, kind, host FROM devices WHERE removed = 0"
                         " AND uid <> ?1 AND length(public_key) = ?2",
                         -1, &stmt, NULL) != SQLITE_OK)
    return 0;
  sqlite3_bind_text(stmt, 1, mc_sync_db_device_uid(c->sdb), -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 2, MC_SYNC_KEY_LEN);
  member_row *m = NULL;
  int n = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    member_row *grown = realloc(m, (size_t)(n + 1) * sizeof *grown);
    if (!grown) break;
    m = grown;
    memcpy(m[n].uid, uid, MC_UID_STRLEN);
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    const char *kind = (const char *)sqlite3_column_text(stmt, 2);
    const char *host = (const char *)sqlite3_column_text(stmt, 3);
    snprintf(m[n].name, sizeof m[n].name, "%s", name ? name : "");
    snprintf(m[n].kind, sizeof m[n].kind, "%s", kind ? kind : "");
    snprintf(m[n].host, sizeof m[n].host, "%s", host ? host : "");
    n++;
  }
  sqlite3_finalize(stmt);
  *out = m;
  return n;
}

static void sched_main(void *arg) {
  mc_sync *sync = arg;
  mc_sync_conn c;
  if (mc_sync_conn_open(sync, &c) != MC_OK) return;
  int64_t seen_lib = max_seq(mc_library_db(c.lib));
  int64_t seen_group = max_seq(mc_sync_db_handle(c.sdb));
  uint64_t next_poll = 0, next_hints = mc_mono_ms() + HINTS_MS;

  for (;;) {
    reap(sync, 0);
    uint64_t now = mc_mono_ms();

    if (now >= next_hints) {
      next_hints = now + HINTS_MS;
      char hints[MC_SYNC_HINTS_MAX];
      compute_hints(sync, hints);
      mc_mutex_lock(&sync->lock);
      int changed = strcmp(hints, sync->hints) != 0;
      if (changed) memcpy(sync->hints, hints, sizeof hints);
      mc_mutex_unlock(&sync->lock);
      if (changed) mc_session_refresh_self(sync, &c);
    }
    if (now >= next_poll) {
      next_poll = now + POLL_MS;
      int64_t lib = max_seq(mc_library_db(c.lib)), group = max_seq(mc_sync_db_handle(c.sdb));
      mc_mutex_lock(&sync->lock);
      /* Rows a session applied land here too; mc_sync_session_ended() has already passed
       * those on, so only count changes since the last session. */
      if (sync->absorb)
        sync->absorb = 0;
      else if ((lib != seen_lib || group != seen_group) && !sync->changed_ms)
        sync->changed_ms = now;
      mc_mutex_unlock(&sync->lock);
      seen_lib = lib;
      seen_group = group;
    }

    member_row *ms;
    int n = load_members(&c, &ms);
    session_job *jobs[16];
    int njobs = 0;
    uint64_t wake = next_poll < next_hints ? next_poll : next_hints;

    mc_mutex_lock(&sync->lock);
    if (sync->stopping) {
      mc_mutex_unlock(&sync->lock);
      free(ms);
      break;
    }
    for (int i = 0; i < sync->npeers; i++) sync->peers[i].member = 0;
    for (int i = 0; i < n; i++) {
      mc_sync_peer *p = find_peer(sync, ms[i].uid, 1);
      if (!p) continue;
      p->member = 1;
      if (!p->discovered) {
        snprintf(p->name, sizeof p->name, "%s", ms[i].name);
        snprintf(p->kind, sizeof p->kind, "%s", ms[i].kind);
        snprintf(p->host, sizeof p->host, "%s", ms[i].host);
      }
    }
    free(ms);
    if (sync->changed_ms && now >= sync->changed_ms + CHANGE_DELAY_MS) {
      sync->changed_ms = 0;
      trigger_members(sync, NULL, MC_TRIGGER_CHANGED, now);
    } else if (sync->changed_ms && sync->changed_ms + CHANGE_DELAY_MS < wake) {
      wake = sync->changed_ms + CHANGE_DELAY_MS;
    }
    for (int i = 0; i < sync->npeers && njobs < 16; i++) {
      mc_sync_peer *p = &sync->peers[i];
      if (!p->member) continue;
      if (!p->due_ms) {
        p->due_ms = now;
        p->trigger = MC_TRIGGER_PERIODIC;
        p->trigger_ms = now;
      }
      if (p->busy || (sync->manual_only && p->trigger != MC_TRIGGER_NOW)) continue;
      if (now < p->due_ms) {
        if (p->due_ms < wake) wake = p->due_ms;
        continue;
      }
      int go = p->trigger == MC_TRIGGER_CHANGED || p->trigger == MC_TRIGGER_NOW ||
               strcmp(sync->device_uid, p->uid) < 0;
      if (!go) {
        if (p->contact_ms && p->contact_ms >= p->trigger_ms) {
          /* The lower uid has been in touch since: nothing to do until next time. */
          p->due_ms = p->contact_ms + PERIOD_MS;
          p->trigger = MC_TRIGGER_PERIODIC;
          p->trigger_ms = p->due_ms;
          continue;
        }
        if (now < p->trigger_ms + HIGHER_GRACE_MS) {
          p->due_ms = p->trigger_ms + HIGHER_GRACE_MS;
          if (p->due_ms < wake) wake = p->due_ms;
          continue;
        }
      }
      session_job *j = calloc(1, sizeof *j);
      if (!j) break;
      j->sync = sync;
      memcpy(j->uid, p->uid, MC_UID_STRLEN);
      if (p->discovered) job_add(j, p->address, p->port);
      /* Until the session reports back. */
      p->due_ms = now + PERIOD_MS;
      p->trigger = MC_TRIGGER_PERIODIC;
      p->trigger_ms = p->due_ms;
      jobs[njobs++] = j;
    }
    mc_mutex_unlock(&sync->lock);

    for (int i = 0; i < njobs; i++)
      if (mc_sync_spawn(sync, session_worker, jobs[i]) != MC_OK) free(jobs[i]);

    mc_mutex_lock(&sync->lock);
    now = mc_mono_ms();
    if (!sync->stopping && !njobs && wake > now)
      mc_cond_wait_ms(&sync->cond, &sync->lock, (uint32_t)(wake - now > POLL_MS ? POLL_MS : wake - now));
    mc_mutex_unlock(&sync->lock);
  }
  mc_sync_conn_close(&c);
}

/* --- Discovery --- */

static void peer_seen(mc_sync *sync, const char *uid, const char *name, const char *kind,
                      const char *host, const char *grp, const char *address, uint16_t port,
                      int online) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 1);
  int member = 0, changed = 0;
  if (p) {
    int was = p->discovered || p->reached;
    if (online) {
      snprintf(p->name, sizeof p->name, "%s", name ? name : "");
      snprintf(p->kind, sizeof p->kind, "%s", kind ? kind : "");
      snprintf(p->host, sizeof p->host, "%s", host ? host : "");
      snprintf(p->grp, sizeof p->grp, "%s", grp ? grp : "");
      snprintf(p->address, sizeof p->address, "%s", address ? address : "");
      p->port = port;
      if (!p->discovered) {
        p->seen_ms = mc_mono_ms();
        if (p->member) {
          p->trigger = MC_TRIGGER_ONLINE;
          p->trigger_ms = p->seen_ms;
          p->due_ms = p->seen_ms;
        }
      }
      p->discovered = 1;
    } else {
      p->discovered = 0;
      p->reached = 0;
    }
    member = p->member;
    changed = (p->discovered || p->reached) != was;
    mc_cond_broadcast(&sync->cond);
  }
  mc_mutex_unlock(&sync->lock);
  if (changed) emit_peer(sync, uid, online, member);
}

static void on_discovered(const mc_peer_info *peer, int online, void *user) {
  peer_seen(user, peer->device_uid, peer->name, peer->kind, peer->host, peer->group_hash,
            peer->address, peer->port, online);
}

/* --- Public API --- */

mc_status mc_sync_new(mc_library *lib, const char *config_dir, const char *device_name,
                      mc_device_kind kind, const mc_sync_events *events, void *user,
                      mc_sync **out) {
  if (out) *out = NULL;
  if (!lib || !config_dir || !*config_dir || !out ||
      (kind != MC_DEVICE_DESKTOP && kind != MC_DEVICE_PHONE && kind != MC_DEVICE_WATCH))
    return MC_ERR_INVALID_ARG;
  mc_sync *sync = calloc(1, sizeof *sync);
  if (!sync) return MC_ERR_NOMEM;
  sync->config_dir = mc_strdup(config_dir);
  sync->lib_dir = mc_strdup(mc_library_dir(lib));
  if (!sync->config_dir || !sync->lib_dir) {
    free(sync->config_dir);
    free(sync->lib_dir);
    free(sync);
    return MC_ERR_NOMEM;
  }
  mc_mutex_init(&sync->cb_lock);
  mc_mutex_init(&sync->transcode_lock);
  mc_mutex_init(&sync->api_lock);
  mc_mutex_init(&sync->lock);
  mc_cond_init(&sync->cond);
  sync->kind = kind;
  if (events) sync->ev = *events;
  sync->user = user;
  sync->use_discovery = 1;
  sync->listen_port = -1;
  if (mc_hostname(sync->host, sizeof sync->host) != MC_OK) sync->host[0] = '\0';
  clean_name(device_name, sync->name);
  if (!sync->name[0]) clean_name(sync->host[0] ? sync->host : "musicapp", sync->name);

  mc_status st = mc_sync_conn_open(sync, &sync->api);
  if (st != MC_OK) {
    mc_sync_free(sync);
    return st;
  }
  memcpy(sync->device_uid, mc_sync_db_device_uid(sync->api.sdb), MC_UID_STRLEN);
  const char *group = mc_sync_db_group_uid(sync->api.sdb);
  if (group) mc_discovery_group_hash(group, sync->group_hash);
  *out = sync;
  return MC_OK;
}

void mc_sync_free(mc_sync *sync) {
  if (!sync) return;
  mc_sync_stop(sync);
  mc_sync_conn_close(&sync->api);
  mc_pair_responder_wipe(&sync->in_r);
  mc_cond_destroy(&sync->cond);
  mc_mutex_destroy(&sync->lock);
  mc_mutex_destroy(&sync->api_lock);
  mc_mutex_destroy(&sync->cb_lock);
  mc_mutex_destroy(&sync->transcode_lock);
  free(sync->peers);
  free(sync->chans);
  free(sync->config_dir);
  free(sync->lib_dir);
  free(sync);
}

const char *mc_sync_device_uid(const mc_sync *sync) {
  return sync ? sync->device_uid : NULL;
}

uint16_t mc_sync_port(const mc_sync *sync) {
  return sync && sync->started ? sync->port : 0;
}

mc_status mc_sync_start(mc_sync *sync) {
  if (!sync) return MC_ERR_INVALID_ARG;
  if (sync->started) return MC_OK;
  mc_listener *l = NULL;
  mc_status st;
  if (sync->listen_port >= 0) {
    st = mc_listener_open((uint16_t)sync->listen_port, &l);
  } else {
    st = mc_listener_open(MC_SYNC_DEFAULT_PORT, &l);
    if (st != MC_OK) st = mc_listener_open(0, &l); /* taken: any port will do */
  }
  if (st != MC_OK) return st;

  mc_mutex_lock(&sync->lock);
  sync->listener = l;
  sync->port = mc_listener_port(l);
  sync->stopping = 0;
  sync->started = 1;
  mc_mutex_unlock(&sync->lock);
  char hints[MC_SYNC_HINTS_MAX];
  compute_hints(sync, hints);
  mc_mutex_lock(&sync->lock);
  memcpy(sync->hints, hints, sizeof hints);
  mc_mutex_unlock(&sync->lock);
  mc_mutex_lock(&sync->api_lock);
  mc_sync_take_group_name(sync, &sync->api); /* before discovery announces the name */
  mc_session_refresh_self(sync, &sync->api);
  mc_mutex_unlock(&sync->api_lock);

  if (sync->use_discovery) {
    mc_discovery *d = NULL;
    char name[MC_SYNC_NAME_MAX];
    mc_mutex_lock(&sync->lock);
    memcpy(name, sync->name, sizeof name);
    mc_mutex_unlock(&sync->lock);
    st = mc_discovery_start(sync->device_uid, name, mc_sync_kind_str(sync->kind),
                            sync->group_hash, sync->port, 0, on_discovered, sync, &d);
    if (st != MC_OK)
      fprintf(stderr, "musiccore: sync: no discovery (%s); using known addresses only\n",
              mc_status_str(st));
    mc_mutex_lock(&sync->lock);
    sync->disc = d;
    mc_mutex_unlock(&sync->lock);
  }
  st = mc_thread_start(&sync->listen_thread, listen_main, sync);
  if (st == MC_OK) {
    st = mc_thread_start(&sync->sched_thread, sched_main, sync);
    if (st != MC_OK) {
      mc_mutex_lock(&sync->lock);
      sync->stopping = 1;
      mc_mutex_unlock(&sync->lock);
      mc_listener_stop(l);
      mc_thread_join(&sync->listen_thread);
    }
  }
  if (st != MC_OK) {
    mc_discovery_stop(sync->disc);
    mc_listener_close(l);
    mc_mutex_lock(&sync->lock);
    sync->disc = NULL;
    sync->listener = NULL;
    sync->started = 0;
    sync->stopping = 0;
    mc_mutex_unlock(&sync->lock);
  }
  return st;
}

void mc_sync_stop(mc_sync *sync) {
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  if (!sync->started) {
    mc_mutex_unlock(&sync->lock);
    return;
  }
  sync->stopping = 1;
  abort_channels(sync, -1);
  mc_cond_broadcast(&sync->cond);
  mc_discovery *d = sync->disc;
  sync->disc = NULL;
  mc_mutex_unlock(&sync->lock);

  mc_listener_stop(sync->listener);
  mc_thread_join(&sync->listen_thread);
  mc_thread_join(&sync->sched_thread);
  mc_discovery_stop(d);
  /* Workers may still be connecting; whatever they open now is aborted at once. */
  reap(sync, 1);

  mc_mutex_lock(&sync->lock);
  mc_listener_close(sync->listener);
  sync->listener = NULL;
  sync->port = 0;
  sync->started = 0;
  sync->stopping = 0;
  sync->out_active = sync->out_has_code = sync->out_cancel = 0;
  sync->in_active = sync->in_conn = sync->in_cancel = 0;
  for (int i = 0; i < sync->npeers; i++) {
    mc_sync_peer *p = &sync->peers[i];
    p->discovered = p->reached = p->busy = 0;
    p->busy_ch = NULL;
    p->due_ms = 0;
  }
  mc_mutex_unlock(&sync->lock);
}

/* --- Pairing API --- */

static mc_status start_pairing(mc_sync *sync, const char *host, uint16_t port,
                               const char *device_uid) {
  mc_pair_target *t = calloc(1, sizeof *t);
  if (!t) return MC_ERR_NOMEM;
  t->sync = sync;
  snprintf(t->host, sizeof t->host, "%s", host);
  t->port = port ? port : MC_SYNC_DEFAULT_PORT;
  snprintf(t->device_uid, sizeof t->device_uid, "%s", device_uid ? device_uid : "");
  mc_mutex_lock(&sync->lock);
  mc_status st = !sync->started ? MC_ERR_INVALID_ARG : sync->out_active ? MC_ERR_LOCKED : MC_OK;
  if (st == MC_OK) {
    sync->out_active = 1;
    sync->out_has_code = 0;
    sync->out_cancel = 0;
  }
  mc_mutex_unlock(&sync->lock);
  if (st == MC_OK && (st = mc_sync_spawn(sync, mc_pairing_initiate, t)) != MC_OK) {
    mc_mutex_lock(&sync->lock);
    sync->out_active = 0;
    mc_mutex_unlock(&sync->lock);
  }
  if (st != MC_OK) free(t);
  return st;
}

mc_status mc_sync_pair_request(mc_sync *sync, const char *device_uid) {
  if (!sync || !is_uid(device_uid)) return MC_ERR_INVALID_ARG;
  char host[MC_SYNC_ADDR_MAX] = "";
  uint16_t port = 0;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, device_uid, 0);
  if (p && p->discovered) {
    memcpy(host, p->address, sizeof host);
    port = p->port;
  }
  mc_mutex_unlock(&sync->lock);
  if (!host[0] || !port) return MC_ERR_NOT_FOUND;
  return start_pairing(sync, host, port, device_uid);
}

mc_status mc_sync_pair_request_address(mc_sync *sync, const char *host, uint16_t port) {
  if (!sync || !host || !*host || strlen(host) >= 256) return MC_ERR_INVALID_ARG;
  return start_pairing(sync, host, port, NULL);
}

mc_status mc_sync_pair_enter(mc_sync *sync, const char *code) {
  if (!sync || !code || strlen(code) != MC_PAIR_CODE_DIGITS) return MC_ERR_INVALID_ARG;
  for (int i = 0; i < MC_PAIR_CODE_DIGITS; i++)
    if (code[i] < '0' || code[i] > '9') return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  mc_status st = sync->out_active ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK) {
    memcpy(sync->out_code, code, MC_PAIR_CODE_DIGITS + 1);
    sync->out_has_code = 1;
    mc_cond_broadcast(&sync->cond);
  }
  mc_mutex_unlock(&sync->lock);
  return st;
}

void mc_sync_pair_cancel(mc_sync *sync) {
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  if (sync->out_active) {
    sync->out_cancel = 1;
    abort_channels(sync, 1);
  }
  if (sync->in_conn) {
    sync->in_cancel = 1;
    abort_channels(sync, 2);
  } else if (sync->in_active) {
    sync->in_active = 0;
    mc_pair_responder_wipe(&sync->in_r);
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

/* --- Group API --- */

static int count_query(sqlite3 *db, const char *sql, const char *arg) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (arg) sqlite3_bind_text(stmt, 1, arg, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

/* Under api_lock. MC_ERR_LOCKED if uid is the only archive among non-removed members, a
 * void demotion counting as one (sync/archive.h). */
static mc_status check_not_last_archive(sqlite3 *db, const char *uid) {
  int is_archive = mc_archive_is(db, uid);
  int archives = mc_archive_count(db);
  if (is_archive < 0 || archives < 0) return MC_ERR_DB;
  return is_archive > 0 && archives <= 1 ? MC_ERR_LOCKED : MC_OK;
}

static mc_status mark_removed(mc_sync_db *sdb, const char *uid) {
  mc_status st = mc_sync_db_begin_write(sdb);
  if (st != MC_OK) return st;
  sqlite3_stmt *stmt;
  st = MC_ERR_DB;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), "UPDATE devices SET removed = 1 WHERE uid = ?1",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_DONE) st = MC_OK;
    sqlite3_finalize(stmt);
  }
  if (st == MC_OK) st = mc_sync_db_commit(sdb);
  if (st != MC_OK) mc_sync_db_rollback(sdb);
  return st;
}

mc_status mc_sync_leave_group(mc_sync *sync) {
  if (!sync) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->api_lock);
  sqlite3 *db = mc_sync_db_handle(sync->api.sdb);
  mc_status st = mc_sync_db_group_uid(sync->api.sdb) ? MC_OK : MC_ERR_NOT_FOUND;
  int others = 0;
  if (st == MC_OK) {
    others = count_query(db, "SELECT count(*) FROM devices WHERE removed = 0 AND uid <> ?1",
                         sync->device_uid);
    if (others > 0) st = check_not_last_archive(db, sync->device_uid);
  }
  if (st == MC_OK && others > 0) st = mark_removed(sync->api.sdb, sync->device_uid);
  member_row *ms = NULL;
  int n = st == MC_OK && others > 0 ? load_members(&sync->api, &ms) : 0;
  mc_mutex_unlock(&sync->api_lock);
  if (st != MC_OK) return st;

  /* Tell one member, so the group learns of it; gossip does the rest. */
  mc_mutex_lock(&sync->lock);
  int started = sync->started && !sync->stopping;
  mc_mutex_unlock(&sync->lock);
  mc_sync_conn c;
  if (started && n > 0 && mc_sync_conn_open(sync, &c) == MC_OK) {
    for (int i = 0; i < n; i++) {
      session_job j;
      memset(&j, 0, sizeof j);
      j.sync = sync;
      memcpy(j.uid, ms[i].uid, MC_UID_STRLEN);
      mc_mutex_lock(&sync->lock);
      mc_sync_peer *p = find_peer(sync, j.uid, 0);
      char host[MC_SYNC_ADDR_MAX] = "";
      uint16_t port = 0;
      if (p && p->discovered) {
        memcpy(host, p->address, sizeof host);
        port = p->port;
      }
      mc_mutex_unlock(&sync->lock);
      job_addresses(&c, &j, host, port);
      if (initiate(sync, &c, &j, 0) == MC_OK) break;
    }
    mc_sync_conn_close(&c);
  }
  free(ms);

  mc_mutex_lock(&sync->api_lock);
  if (mc_sync_db_group_uid(sync->api.sdb)) st = mc_sync_db_clear_group(sync->api.sdb);
  mc_sync_group_changed(sync, &sync->api);
  mc_mutex_unlock(&sync->api_lock);
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->npeers; i++) sync->peers[i].member = 0;
  mc_mutex_unlock(&sync->lock);
  return st;
}

mc_status mc_sync_remove_device(mc_sync *sync, const char *device_uid) {
  if (!sync || !is_uid(device_uid)) return MC_ERR_INVALID_ARG;
  if (!strcmp(device_uid, sync->device_uid)) return mc_sync_leave_group(sync);
  mc_mutex_lock(&sync->api_lock);
  sqlite3 *db = mc_sync_db_handle(sync->api.sdb);
  mc_status st = mc_sync_db_group_uid(sync->api.sdb) &&
                     count_query(db, "SELECT count(*) FROM devices WHERE uid = ?1 AND removed = 0",
                                 device_uid) > 0
                   ? MC_OK
                   : MC_ERR_NOT_FOUND;
  if (st == MC_OK) st = check_not_last_archive(db, device_uid);
  if (st == MC_OK) st = mark_removed(sync->api.sdb, device_uid);
  mc_mutex_unlock(&sync->api_lock);
  if (st == MC_OK) {
    mc_mutex_lock(&sync->lock);
    mc_sync_peer *p = find_peer(sync, device_uid, 0);
    if (p) p->member = 0;
    trigger_members(sync, device_uid, MC_TRIGGER_CHANGED, mc_mono_ms());
    mc_mutex_unlock(&sync->lock);
  }
  return st;
}

mc_status mc_sync_now(mc_sync *sync, const char *device_uid) {
  if (!sync || (device_uid && !is_uid(device_uid))) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  mc_status st = sync->started ? MC_OK : MC_ERR_INVALID_ARG;
  if (st == MC_OK && device_uid) {
    mc_sync_peer *p = find_peer(sync, device_uid, 1);
    if (p) {
      p->trigger = MC_TRIGGER_NOW;
      p->trigger_ms = mc_mono_ms();
      p->due_ms = p->trigger_ms;
    }
    mc_cond_broadcast(&sync->cond);
  } else if (st == MC_OK) {
    trigger_members(sync, NULL, MC_TRIGGER_NOW, mc_mono_ms());
  }
  mc_mutex_unlock(&sync->lock);
  return st;
}

mc_status mc_sync_confirm_now(mc_sync *sync, const char *device_uid) {
  if (!sync || (device_uid && !is_uid(device_uid))) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  int started = sync->started;
  mc_mutex_unlock(&sync->lock);
  if (!started) return MC_ERR_INVALID_ARG;
  /* This device's profile at once, so it's confirmed even if no member is in reach. The
   * write is group data, which the change poll passes on. */
  mc_mutex_lock(&sync->api_lock);
  mc_status st = mc_profile_confirm(sync->api.sdb, NULL);
  mc_mutex_unlock(&sync->api_lock);
  if (st != MC_OK && st != MC_ERR_NOT_FOUND) return st;
  uint64_t now = mc_mono_ms();
  /* Before the trigger, so the session it starts carries the press. */
  mc_mutex_lock(&sync->lock);
  if (device_uid) {
    mc_sync_peer *p = find_peer(sync, device_uid, 1);
    if (p) p->user_ms = now;
  } else {
    for (int i = 0; i < sync->npeers; i++)
      if (sync->peers[i].member) sync->peers[i].user_ms = now;
  }
  mc_mutex_unlock(&sync->lock);
  return mc_sync_now(sync, device_uid);
}

void mc_sync_notify_changed(mc_sync *sync) {
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  if (!sync->changed_ms) sync->changed_ms = mc_mono_ms();
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

mc_status mc_sync_library_state(mc_sync *sync, mc_sync_lib_state *out) {
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->api_lock);
  const char *group = mc_sync_db_group_uid(sync->api.sdb);
  char group_lib[MC_UID_STRLEN] = "", mine[MC_UID_STRLEN] = "";
  sqlite3_stmt *stmt;
  mc_status st = MC_OK;
  if (!group) {
    *out = MC_SYNC_LIBRARY_NO_GROUP;
  } else {
    if (sqlite3_prepare_v2(mc_sync_db_handle(sync->api.sdb),
                           "SELECT library_uid FROM sync_group WHERE uid = ?1", -1, &stmt,
                           NULL) == SQLITE_OK) {
      sqlite3_bind_text(stmt, 1, group, -1, SQLITE_STATIC);
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (is_uid(v)) memcpy(group_lib, v, MC_UID_STRLEN);
      }
      sqlite3_finalize(stmt);
    }
    if (sqlite3_prepare_v2(mc_library_db(sync->api.lib),
                           "SELECT value FROM meta WHERE key = 'library_uid'", -1, &stmt,
                           NULL) == SQLITE_OK) {
      if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (is_uid(v)) memcpy(mine, v, MC_UID_STRLEN);
      }
      sqlite3_finalize(stmt);
    }
    *out = !group_lib[0]               ? MC_SYNC_LIBRARY_UNKNOWN
           : !strcmp(group_lib, mine) ? MC_SYNC_LIBRARY_GROUP
                                      : MC_SYNC_LIBRARY_OTHER;
  }
  mc_mutex_unlock(&sync->api_lock);
  return st;
}

/* --- Snapshots --- */

static char *col_dup(sqlite3_stmt *stmt, int col) {
  const char *s = (const char *)sqlite3_column_text(stmt, col);
  return dup_or_null(s);
}

mc_status mc_sync_list_devices(mc_sync *sync, mc_sync_device_list **out) {
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_sync_device_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  mc_mutex_lock(&sync->api_lock);
  sqlite3_stmt *stmt = NULL;
  if (mc_sync_db_group_uid(sync->api.sdb) &&
      sqlite3_prepare_v2(mc_sync_db_handle(sync->api.sdb),
                         "SELECT d.uid, coalesce(nullif(p.name, ''), d.name) AS shown, d.host,"
                         " d.kind, d.removed, coalesce(p.archive, 0),"
                         " pc.address, pc.port, pc.last_sync_ms, s.device_uid IS NOT NULL,"
                         " s.verified_ms, s.expected, s.ok, s.missing, s.corrupt, s.bytes"
                         " FROM devices d LEFT JOIN profiles p ON p.device_uid = d.uid"
                         " LEFT JOIN peer_contact pc ON pc.device_uid = d.uid"
                         " LEFT JOIN sync_status s ON s.device_uid = d.uid"
                         " ORDER BY shown COLLATE NOCASE, d.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  int cap = 0;
  /* A device whose demotion is void still counts as an archive. */
  char void_uid[MC_UID_STRLEN] = "";
  if (st == MC_OK && stmt && mc_archive_void(mc_sync_db_handle(sync->api.sdb), void_uid) != MC_OK)
    st = MC_ERR_DB;
  while (st == MC_OK && stmt && sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    if (!is_uid(uid)) continue;
    if (list->count == cap) {
      int grown = cap ? cap * 2 : 8;
      mc_sync_device *more = realloc(list->items, (size_t)grown * sizeof *more);
      if (!more) {
        st = MC_ERR_NOMEM;
        break;
      }
      list->items = more;
      cap = grown;
    }
    mc_sync_device *d = &list->items[list->count++];
    memset(d, 0, sizeof *d);
    memcpy(d->uid, uid, MC_UID_STRLEN);
    d->name = col_dup(stmt, 1);
    d->host = col_dup(stmt, 2);
    d->kind = mc_sync_kind_parse((const char *)sqlite3_column_text(stmt, 3));
    d->removed = sqlite3_column_int(stmt, 4) != 0;
    d->archive = sqlite3_column_int(stmt, 5) != 0 || !strcmp(uid, void_uid);
    d->address = col_dup(stmt, 6);
    d->port = (uint16_t)sqlite3_column_int(stmt, 7);
    d->last_sync_ms = sqlite3_column_int64(stmt, 8);
    d->has_status = sqlite3_column_int(stmt, 9);
    d->verified_ms = sqlite3_column_int64(stmt, 10);
    d->expected = sqlite3_column_int64(stmt, 11);
    d->ok = sqlite3_column_int64(stmt, 12);
    d->missing = sqlite3_column_int64(stmt, 13);
    d->corrupt = sqlite3_column_int64(stmt, 14);
    d->bytes = sqlite3_column_int64(stmt, 15);
    d->is_self = !strcmp(uid, sync->device_uid);
    int confirm = mc_profile_confirm_state(mc_sync_db_handle(sync->api.sdb), uid);
    if (confirm < 0) st = MC_ERR_DB;
    d->confirm = confirm < 0 ? MC_SYNC_UNCONFIRMED : (mc_sync_confirm_state)confirm;
  }
  sqlite3_finalize(stmt);
  mc_mutex_unlock(&sync->api_lock);

  /* Whereabouts from the peer table. */
  mc_mutex_lock(&sync->lock);
  for (int i = 0; st == MC_OK && i < list->count; i++) {
    mc_sync_device *d = &list->items[i];
    if (d->is_self) {
      d->online = 1;
      continue;
    }
    mc_sync_peer *p = find_peer(sync, d->uid, 0);
    if (!p) continue;
    d->online = p->discovered || p->reached;
    if (p->address[0] && (p->discovered || !d->address)) {
      free(d->address);
      d->address = mc_strdup(p->address);
      d->port = p->port;
    }
  }
  mc_mutex_unlock(&sync->lock);
  if (st != MC_OK) {
    mc_sync_device_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_sync_device_list_free(mc_sync_device_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].name);
    free(list->items[i].host);
    free(list->items[i].address);
  }
  free(list->items);
  free(list);
}

static int cmp_nearby(const void *a, const void *b) {
  const mc_sync_nearby *x = a, *y = b;
  int c = strcmp(x->name ? x->name : "", y->name ? y->name : "");
  return c ? c : strcmp(x->uid, y->uid);
}

mc_status mc_sync_list_nearby(mc_sync *sync, mc_sync_nearby_list **out) {
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_sync_nearby_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  mc_mutex_lock(&sync->lock);
  if (sync->npeers) list->items = calloc((size_t)sync->npeers, sizeof *list->items);
  if (sync->npeers && !list->items) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < sync->npeers; i++) {
    const mc_sync_peer *p = &sync->peers[i];
    int ours = sync->group_hash[0] && !strcmp(p->grp, sync->group_hash);
    if (!p->discovered || p->member || ours) continue;
    /* Not in a group ourselves, we'd just join theirs; only two groups clash. */
    int other = p->grp[0] && sync->group_hash[0];
    mc_sync_nearby *n = &list->items[list->count++];
    memcpy(n->uid, p->uid, MC_UID_STRLEN);
    n->name = mc_strdup(p->name);
    n->host = dup_or_null(p->host);
    n->kind = mc_sync_kind_parse(p->kind);
    n->in_other_group = other;
    n->address = dup_or_null(p->address);
    n->port = p->port;
  }
  mc_mutex_unlock(&sync->lock);
  if (st != MC_OK) {
    mc_sync_nearby_list_free(list);
    return st;
  }
  if (list->count > 1) qsort(list->items, (size_t)list->count, sizeof *list->items, cmp_nearby);
  *out = list;
  return MC_OK;
}

void mc_sync_nearby_list_free(mc_sync_nearby_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].name);
    free(list->items[i].host);
    free(list->items[i].address);
  }
  free(list->items);
  free(list);
}

/* --- Names and addresses --- */

mc_status mc_sync_set_device_name(mc_sync *sync, const char *name) {
  if (!sync) return MC_ERR_INVALID_ARG;
  char clean[MC_SYNC_NAME_MAX];
  clean_name(name, clean);
  if (!clean[0]) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->api_lock);
  mc_status st = MC_OK;
  if (mc_sync_db_group_uid(sync->api.sdb))
    st = mc_sync_write_profile_name(&sync->api, clean, 0, NULL);
  if (st == MC_OK) st = adopt_name(sync, &sync->api, clean);
  mc_mutex_unlock(&sync->api_lock);
  return st;
}

mc_status mc_sync_get_device_name(mc_sync *sync, char *out, size_t cap) {
  if (!sync || !out || !cap) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  size_t n = utf8_cut(sync->name, cap - 1);
  memcpy(out, sync->name, n);
  mc_mutex_unlock(&sync->lock);
  out[n] = '\0';
  return MC_OK;
}

/* local_addr.h's list is already in this order and leaves loopback out. */
int mc_sync_local_addresses(char (*out)[MC_SYNC_ADDR_STRLEN], int max) {
  if (!out || max <= 0) return 0;
  return mc_local_addresses(out, max);
}

/* --- Test hooks --- */

void mc_sync_set_test_options(mc_sync *sync, uint16_t port, int use_discovery) {
  sync->listen_port = port;
  sync->use_discovery = use_discovery;
}

void mc_sync_set_test_manual(mc_sync *sync, int manual_only) {
  mc_mutex_lock(&sync->lock);
  sync->manual_only = manual_only;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_set_test_hints(mc_sync *sync, const char *json) {
  mc_mutex_lock(&sync->lock);
  snprintf(sync->hint_override, sizeof sync->hint_override, "%s", json ? json : "");
  memcpy(sync->hints, sync->hint_override, sizeof sync->hints);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_test_peer(mc_sync *sync, const char *device_uid, const char *name, const char *kind,
                       const char *group_hash, const char *address, uint16_t port, int online) {
  peer_seen(sync, device_uid, name, kind, "", group_hash, address, port, online);
}
