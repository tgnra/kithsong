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
 * then their address hints, then the address that last worked.
 *
 * Rate limit (MC_SYNC_AUTO_INTERVAL_MS). A complete session sets the peer's auto_after_ms;
 * until then every trigger but MC_TRIGGER_NOW is kept but its due time moved to
 * auto_after_ms. A failed or incomplete session clears it. Seeded from
 * peer_contact.last_sync_ms so a restart doesn't reset it.
 *
 * Cancel. Besides held_ms here, the channel sends the peer CANCEL once both HELLOs say
 * "cancel" (mc_channel_cancel(), best effort). The peer's session ends with peer_cancelled,
 * which sets its peer_held_ms: every trigger but MC_TRIGGER_NOW waits it out, without moving
 * due_ms.
 *
 * Reachability. Connections made, failed while the peer is online, or received, and reach
 * checks (reach.c, on first sight and every REACH_EVERY_MS) are evidence; the latest each way
 * wins.
 *
 * Waiting tracks (waiting.h) are recomputed on the scheduler thread after
 * mc_sync_waiting_changed(). Reachability for them is the last session attempt
 * (mc_sync_peer.contact), never discovery or reach checks, which come and go. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <yyjson.h>

#include "discovery/local_addr.h"
#include "library/folder_gate.h"
#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/issues.h"
#include "sync/takeover.h"
#include "sync/plan.h"
#include "sync/reach.h"
#include "sync/rows.h"
#include "sync/session.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "sync/problems.h"
#include "sync/waiting.h"
#include "util/log.h"
#include "util/power.h"

#define PERIOD_MS (5 * 60 * 1000)
#define CHANGE_DELAY_MS 3000
#define HIGHER_GRACE_MS 30000
#define POLL_MS 2000
#define HINTS_MS 60000
/* How long after start a desktop first looks whether the weekly deep check is due. */
#define DEEP_FIRST_MS (10 * 60 * 1000)
#define BACKOFF_MIN_MS 10000
#define CONNECT_MS 3000
#define ACCEPT_WAIT_MS 1000
#define CLAIM_WAIT_MS 5000
/* How long a cancel that sent CANCEL, or left it to a send under way (mc_channel_cancel()),
 * waits for the session to end before the connection is aborted. */
#define CANCEL_GRACE_MS 1000
#define MAX_ADDRS 8
/* How long a press of Sync waits for a session to carry it: long enough for a member that
 * only it can reach out from to call back. */
#define USER_WINDOW_MS (PERIOD_MS + HIGHER_GRACE_MS)
/* Reach checks with a device that stays online. */
#define REACH_EVERY_MS (15 * 60 * 1000)
/* A device that connected to us counts as online this long (see reach_alive()). */
#define REACH_ALIVE_MS (PERIOD_MS + HIGHER_GRACE_MS)
/* After start, members not tried yet count as reachable for the waiting tracks this long:
 * the sessions the scheduler starts at once (and the higher uid's, after its grace) go
 * first, so a member about to sync isn't named for a moment. */
#define WAITING_GRACE_MS (HIGHER_GRACE_MS + 30000)

/* --- Small helpers --- */

static int is_uid(const char *s) {
  return s && strlen(s) == MC_UID_STRLEN - 1;
}

const char *mc_sync_kind_str(mc_device_kind kind) {
  switch (kind) {
  case MC_DEVICE_PHONE: return "phone";
  default: return "desktop";
  }
}

mc_device_kind mc_sync_kind_parse(const char *s) {
  if (s && !strcmp(s, "phone")) return MC_DEVICE_PHONE;
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
  if (st == MC_OK) st = mc_library_open_internal(sync->lib_dir, &c->lib);
  if (st != MC_OK) mc_sync_conn_close(c);
  return st;
}

void mc_sync_conn_close(mc_sync_conn *c) {
  mc_library_close(c->lib);
  mc_sync_db_close(c->sdb);
  c->lib = NULL;
  c->sdb = NULL;
}

/* Kept connections stay valid: the databases are only ever changed in place, never
 * replaced, while sync runs. What a handle caches of the library's identity is read again,
 * since a pairing or another session may have changed it since (the other handles do the
 * same). */
mc_status mc_sync_conn_take(mc_sync *sync, mc_sync_conn *c) {
  memset(c, 0, sizeof *c);
  mc_mutex_lock(&sync->lock);
  if (sync->nidle) *c = sync->idle[--sync->nidle];
  mc_mutex_unlock(&sync->lock);
  if (c->lib && mc_library_reload_ids(c->lib) == MC_OK) return MC_OK;
  mc_sync_conn_close(c);
  return mc_sync_conn_open(sync, c);
}

void mc_sync_conn_give(mc_sync *sync, mc_sync_conn *c) {
  int keep = c->lib && c->sdb && sqlite3_get_autocommit(mc_library_db(c->lib)) &&
             sqlite3_get_autocommit(mc_sync_db_handle(c->sdb));
  mc_mutex_lock(&sync->lock);
  keep = keep && !sync->stopping && sync->started && sync->nidle < MC_SYNC_IDLE_CONNS;
  if (keep) sync->idle[sync->nidle++] = *c;
  mc_mutex_unlock(&sync->lock);
  if (keep)
    memset(c, 0, sizeof *c);
  else
    mc_sync_conn_close(c);
}

/* Only once the workers are gone (mc_sync_stop()). */
static void close_idle(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  int n = sync->nidle;
  mc_sync_conn idle[MC_SYNC_IDLE_CONNS];
  memcpy(idle, sync->idle, sizeof idle);
  sync->nidle = 0;
  mc_mutex_unlock(&sync->lock);
  for (int i = 0; i < n; i++) mc_sync_conn_close(&idle[i]);
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

int mc_sync_peer_member(mc_sync *sync, const char *uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  int member = p && p->member;
  mc_mutex_unlock(&sync->lock);
  return member;
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

int mc_sync_pressed(mc_sync *sync, const char *peer_uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 0);
  int pressed = p && ((p->user_ms && mc_mono_ms() < p->user_ms + USER_WINDOW_MS) ||
                      p->trigger == MC_TRIGGER_NOW);
  mc_mutex_unlock(&sync->lock);
  return pressed;
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

int mc_sync_tracing(void) {
  static int on = -1; /* the environment doesn't change, so a race only reads it twice */
  if (on < 0) {
    const char *v = getenv("KITHSONG_SYNC_TRACE");
    on = v && *v && strcmp(v, "0") != 0;
  }
  return on;
}

/* Trace lines are buffered for trace_main(): a write to stderr redirected to a file on
 * Windows costs 15-100 ms, more than most steps it times. Process-wide, so guarded by SQLite's
 * static mutexes, which need no setup: APP2 for the buffer, APP3 for writing it out in
 * order. */
static char *g_trace_buf;
static size_t g_trace_len, g_trace_cap;
static int g_trace_writer;

static void trace_flush(void) {
  sqlite3_mutex *out = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP3);
  sqlite3_mutex *buf = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP2);
  sqlite3_mutex_enter(out);
  sqlite3_mutex_enter(buf);
  char *text = g_trace_buf;
  size_t len = g_trace_len;
  g_trace_buf = NULL;
  g_trace_len = g_trace_cap = 0;
  sqlite3_mutex_leave(buf);
  if (len) {
    fwrite(text, 1, len, stderr);
    fflush(stderr);
  }
  sqlite3_mutex_leave(out);
  free(text);
}

static void trace_main(void *arg) {
  (void)arg;
  for (;;) {
    mc_sleep_ms(20);
    trace_flush();
  }
}

void mc_sync_trace(const char *fmt, ...) {
  if (!mc_sync_tracing()) return;
  char line[1024];
  int n = snprintf(line, sizeof line, "[sync %9.3f] ",
                   (double)(mc_mono_ms() % 1000000000) / 1000.0);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
  va_end(ap);
  size_t len = strlen(line);
  line[len++] = '\n';
  sqlite3_mutex *buf = sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP2);
  sqlite3_mutex_enter(buf);
  if (g_trace_len + len > g_trace_cap) {
    size_t cap = g_trace_cap ? g_trace_cap * 2 : 16384;
    while (cap < g_trace_len + len) cap *= 2;
    char *grown = realloc(g_trace_buf, cap);
    if (grown) {
      g_trace_buf = grown;
      g_trace_cap = cap;
    }
  }
  if (g_trace_len + len <= g_trace_cap) {
    memcpy(g_trace_buf + g_trace_len, line, len);
    g_trace_len += len;
  }
  int start = !g_trace_writer;
  g_trace_writer = 1;
  sqlite3_mutex_leave(buf);
  mc_thread writer;
  if (!start) return;
  if (mc_thread_start(&writer, trace_main, NULL) == MC_OK) {
    mc_thread_detach(&writer);
  } else {
    sqlite3_mutex_enter(buf);
    g_trace_writer = 0; /* the next line tries again */
    sqlite3_mutex_leave(buf);
    trace_flush();
  }
}

static void emit_reach(mc_sync *sync, const char *uid) {
  if (!sync->ev.on_reach) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_reach(sync->user, uid);
  mc_mutex_unlock(&sync->cb_lock);
}

static void emit_problems(mc_sync *sync) {
  if (!sync->ev.on_problems) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_problems(sync->user);
  mc_mutex_unlock(&sync->cb_lock);
}

static void emit_waiting(mc_sync *sync) {
  if (!sync->ev.on_waiting) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_waiting(sync->user);
  mc_mutex_unlock(&sync->cb_lock);
}

static void emit_outlook(mc_sync *sync) {
  if (!sync->ev.on_outlook) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_outlook(sync->user);
  mc_mutex_unlock(&sync->cb_lock);
}

/* On the scheduler's thread, so a callback that calls mc_sync_set_power() can't deadlock. */
static void emit_transcoding(mc_sync *sync) {
  int active = mc_sync_transcoding(sync);
  mc_mutex_lock(&sync->cb_lock);
  if (active != sync->transcoding_shown) {
    sync->transcoding_shown = active;
    if (sync->ev.on_transcoding) sync->ev.on_transcoding(sync->user, active);
  }
  mc_mutex_unlock(&sync->cb_lock);
}

static void emit_dissolved(mc_sync *sync, const char *name) {
  if (!sync->ev.on_group_dissolved) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_group_dissolved(sync->user, name);
  mc_mutex_unlock(&sync->cb_lock);
}

static void emit_version(mc_sync *sync, const char *uid) {
  if (!sync->ev.on_version) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_version(sync->user, uid);
  mc_mutex_unlock(&sync->cb_lock);
}

void mc_sync_self_version(mc_sync *sync, mc_version_self *out) {
  mc_mutex_lock(&sync->lock);
  *out = sync->self_version;
  mc_mutex_unlock(&sync->lock);
}

/* Under lock. 1 if it changed. */
static int set_version(mc_sync_peer *p, const mc_sync_version *v) {
  if (!memcmp(&p->version, v, sizeof *v)) return 0;
  p->version = *v;
  return 1;
}

void mc_sync_note_version(mc_sync *sync, const char *uid, const mc_sync_version *v) {
  if (!is_uid(uid)) return;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 1);
  int changed = p && set_version(p, v);
  mc_mutex_unlock(&sync->lock);
  if (changed) emit_version(sync, uid);
}

static void emit_done(mc_sync *sync, const mc_sync_result *r) {
  if (!sync->ev.on_sync_done) return;
  mc_mutex_lock(&sync->cb_lock);
  sync->ev.on_sync_done(sync->user, r);
  mc_mutex_unlock(&sync->cb_lock);
}

/* --- Reachability: what's known --- */

static mc_sync_reach verdict(uint64_t ok_ms, uint64_t fail_ms) {
  if (!ok_ms && !fail_ms) return MC_REACH_UNKNOWN;
  return ok_ms >= fail_ms ? MC_REACH_OK : MC_REACH_BLOCKED;
}

/* Under lock. Online by evidence other than our own connecting to it: discovery sees it, or
 * it connected to us lately. A failure to connect to a device that isn't means it's away,
 * not blocked. */
static int reach_alive(const mc_sync_peer *p, uint64_t now) {
  return p->discovered || (p->in_ok_ms && now < p->in_ok_ms + REACH_ALIVE_MS);
}

/* Under lock. */
static void self_reach_locked(mc_sync *sync, mc_sync_self_reach *out) {
  memset(out, 0, sizeof *out);
  for (int i = 0; i < sync->npeers; i++) {
    mc_sync_reach v = verdict(sync->peers[i].in_ok_ms, sync->peers[i].in_fail_ms);
    if (v == MC_REACH_OK) out->ok++;
    if (v == MC_REACH_BLOCKED) out->blocked++;
  }
  out->state = out->blocked ? MC_REACH_BLOCKED : out->ok ? MC_REACH_OK : MC_REACH_UNKNOWN;
  out->port = sync->started ? sync->port : 0;
  out->default_port = out->port == MC_SYNC_DEFAULT_PORT;
}

enum { REACH_OUT_OK, REACH_OUT_FAIL, REACH_IN_OK, REACH_IN_FAIL };

/* Records evidence about uid and reports what it changed. from, if given, is the address
 * of a connection uid made to us. create: add uid to the peer table if it isn't there. */
static void reach_note(mc_sync *sync, const char *uid, int what, const char *from, int create) {
  uint64_t now = mc_mono_ms();
  int peer_changed = 0, self_changed = 0;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, create);
  if (p) {
    mc_sync_self_reach before, after;
    self_reach_locked(sync, &before);
    mc_sync_reach out = verdict(p->out_ok_ms, p->out_fail_ms);
    mc_sync_reach in = verdict(p->in_ok_ms, p->in_fail_ms);
    switch (what) {
      case REACH_OUT_OK: p->out_ok_ms = now; break;
      case REACH_OUT_FAIL:
        if (reach_alive(p, now)) p->out_fail_ms = now;
        break;
      case REACH_IN_OK: p->in_ok_ms = now; break;
      default: p->in_fail_ms = now; break;
    }
    if (from) mc_reach_host_of(from, p->in_addr);
    peer_changed = out != verdict(p->out_ok_ms, p->out_fail_ms) ||
                   in != verdict(p->in_ok_ms, p->in_fail_ms);
    self_reach_locked(sync, &after);
    self_changed = before.state != after.state || before.ok != after.ok ||
                   before.blocked != after.blocked;
  }
  mc_mutex_unlock(&sync->lock);
  if (peer_changed) emit_reach(sync, uid);
  if (self_changed) emit_reach(sync, sync->device_uid);
}

/* This device's network changed: what was known held for the old one. */
static void reach_forget(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->npeers; i++) {
    mc_sync_peer *p = &sync->peers[i];
    p->out_ok_ms = p->out_fail_ms = p->in_ok_ms = p->in_fail_ms = 0;
    p->reach_ms = 0;
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  emit_reach(sync, NULL);
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
  if (p && !p->busy && !sync->stopping && mc_mono_ms() >= p->held_ms) {
    p->busy = 1;
    p->busy_lower = lower;
    p->busy_ch = ch;
    p->abort = 0;
    p->cancel = 0;
    p->cancel_ms = 0;
    ok = 1;
  }
  mc_mutex_unlock(&sync->lock);
  return ok;
}

int mc_sync_release(mc_sync *sync, const char *peer_uid) {
  int cancelled = 0;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 0);
  if (p) {
    cancelled = p->cancel;
    p->busy = 0;
    p->busy_ch = NULL;
    p->cancel = 0;
    p->cancel_ms = 0;
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  return cancelled;
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

static int alone_in_group(mc_sync_db *sdb, const char *self);
static int dissolve_if_alone(mc_sync *sync);

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
  mc_log("kithcore: sync: this device was removed from its group\n");
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
    /* A press of Sync not yet acted on mustn't become a trigger the rate limit holds, nor
     * a member waiting for copies. */
    if (p->trigger == MC_TRIGGER_NOW && why != MC_TRIGGER_NOW) continue;
    if (p->trigger == MC_TRIGGER_COPIES && why != MC_TRIGGER_NOW) continue;
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
    /* Members only: a session that found this device removed has forgotten the group (and
     * may have put back the one from before a pairing it undid), so nothing of it is kept.
     * The last sync is the last complete one, which the rate limit and the waiting tracks
     * count from; any session that got through says where it is and that it was seen. */
    int complete = result->converged && !follow_up;
    if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                           "INSERT INTO peer_contact (device_uid, address, port, last_sync_ms,"
                           " seen_ms) SELECT ?1, ?2, ?3, ?4, ?5 WHERE EXISTS (SELECT 1 FROM devices"
                           " WHERE uid = ?1 AND removed = 0)"
                           " ON CONFLICT (device_uid) DO UPDATE SET"
                           " address = coalesce(excluded.address, peer_contact.address),"
                           " port = coalesce(excluded.port, peer_contact.port),"
                           " last_sync_ms = coalesce(excluded.last_sync_ms,"
                           " peer_contact.last_sync_ms), seen_ms = excluded.seen_ms",
                           -1, &stmt, NULL) == SQLITE_OK) {
      int64_t wall = (int64_t)mc_wall_ms();
      sqlite3_bind_text(stmt, 1, peer_uid, -1, SQLITE_STATIC);
      if (address) {
        sqlite3_bind_text(stmt, 2, address, -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 3, port);
      }
      if (complete) sqlite3_bind_int64(stmt, 4, wall);
      sqlite3_bind_int64(stmt, 5, wall);
      sqlite3_step(stmt);
      sqlite3_finalize(stmt);
    }
    /* Before on_sync_done, so a frontend that asks then sees the new name. */
    mc_sync_take_group_name(sync, c);
  }
  uint64_t now = mc_mono_ms();
  int changed = 0, online = 0, member = 0, version_changed = 0;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, peer_uid, 1);
  if (p) {
    int was = p->discovered || p->reached;
    p->contact_ms = now;
    if (result->status == MC_OK) {
      static const mc_sync_version compatible;
      version_changed = set_version(p, &compatible);
      p->reached = 1;
      p->contact = MC_CONTACT_REACHED;
      p->seen_ms = now;
      p->backoff_ms = 0;
      p->due_ms = now + PERIOD_MS;
      p->trigger = MC_TRIGGER_PERIODIC;
      p->trigger_ms = p->due_ms;
      /* Complete: nothing left that the next automatic session would carry at once. */
      int complete = result->converged && !follow_up;
      p->auto_after_ms = complete && sync->auto_interval_ms ? now + sync->auto_interval_ms : 0;
      p->peer_held_ms = 0; /* whoever cancelled has synced since */
      p->refused_hold = MC_HOLD_NONE;
      p->refused_until_ms = 0;
      if (address && !p->discovered) {
        snprintf(p->address, sizeof p->address, "%s", address);
        p->port = port;
      }
    } else if (result->peer_cancelled) {
      /* Its user cancelled: no automatic session with it for a while, and no retry. Its
       * own sessions are still served. */
      p->peer_held_ms = now + sync->peer_cancel_hold_ms;
      mc_log("kithcore: sync: %s cancelled the session\n", peer_uid);
    } else if (result->status != MC_ERR_LOCKED && !result->cancelled) {
      p->reached = 0;
      /* For the waiting tracks: one that never got past connecting (its handshake included)
       * couldn't be reached. One that answered and then failed was there a moment ago: the
       * retry below says which it is now. */
      if (!result->connected) p->contact = MC_CONTACT_FAILED;
      p->auto_after_ms = 0; /* the retries below go ahead */
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
  /* Files may have come or gone, and profiles or members changed. */
  sync->waiting_dirty = 1;
  if (rows_new > 0) trigger_members(sync, peer_uid, MC_TRIGGER_CHANGED, now + CHANGE_DELAY_MS);
  if (follow_up && p && result->status == MC_OK) {
    p->trigger = MC_TRIGGER_CHANGED;
    p->trigger_ms = now;
    p->due_ms = now + CHANGE_DELAY_MS;
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  if (changed) emit_peer(sync, peer_uid, online, member);
  if (version_changed) emit_version(sync, peer_uid);
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
  int user; /* this device's user asked for it: see "Showing sync" in sync.h */
  int now;  /* started for mc_sync_now(): the recycle bin's deletes may go (here.c) */
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

int mc_sync_is_stopping(mc_sync *sync) {
  return stopping(sync);
}

/* A session couldn't connect to uid: for the waiting tracks it can't be reached now. Only a
 * change asks for them again, so a member that stays away costs nothing. */
static void contact_failed(mc_sync *sync, const char *uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  if (p && p->contact != MC_CONTACT_FAILED) {
    p->contact = MC_CONTACT_FAILED;
    sync->waiting_dirty = 1;
    mc_cond_broadcast(&sync->cond);
  }
  mc_mutex_unlock(&sync->lock);
}

/* Something other than uid answered at host:port: its handshake failed. */
static void note_wrong(mc_sync *sync, const char *uid, const char *host, uint16_t port) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  if (p) {
    p->wrong_count++;
    int v6 = strchr(host, ':') != NULL;
    snprintf(p->wrong_addr, sizeof p->wrong_addr, "%s%s%s:%u", v6 ? "[" : "", host, v6 ? "]" : "",
             (unsigned)port);
    if (p->wrong_count >= MC_SYNC_WRONG_SHOW) sync->waiting_dirty = 1;
    mc_cond_broadcast(&sync->cond);
  }
  mc_mutex_unlock(&sync->lock);
  mc_log("kithcore: sync: something else answered for %.8s at %s:%u\n", uid, host, (unsigned)port);
}

static void note_handshake(mc_sync *sync, const char *uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  if (p && p->wrong_count) {
    if (p->wrong_count >= MC_SYNC_WRONG_SHOW) sync->waiting_dirty = 1;
    p->wrong_count = 0;
    p->wrong_addr[0] = '\0';
  }
  mc_mutex_unlock(&sync->lock);
}

/* A session with uid got going, whichever side started it: it holds nothing back from this
 * device, whatever it said when it last refused one, and the problems no longer name its
 * hold (from_peers(), from_capabilities() in issues.c) while the session runs. */
static void note_session_started(mc_sync *sync, const char *uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  if (p) {
    p->refused_hold = MC_HOLD_NONE;
    p->refused_until_ms = 0;
  }
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

/* The peer refused a session as busy, holding sync back with hold (MC_HOLD_NONE: one is
 * running). */
static void note_refused(mc_sync *sync, const char *uid, mc_sync_hold hold, int64_t until) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  if (p && (p->refused_hold != hold || p->refused_until_ms != until)) {
    p->refused_hold = hold;
    p->refused_until_ms = hold != MC_HOLD_NONE ? until : 0;
    sync->waiting_dirty = 1;
    mc_cond_broadcast(&sync->cond);
  }
  mc_mutex_unlock(&sync->lock);
}

/* Connects to a member and runs a session as the initiator. quiet: an attempt that gets no
 * session (unreachable, or one with it is running already) goes unreported, but for the
 * MC_SYNC_PHASE_DONE that ends its CONNECTING. One the user asked for (j->user) is reported
 * when nothing answered, so a press of Sync that can't get through says so. */
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

  mc_sync_progress prog = {.device_uid = j->uid, .phase = MC_SYNC_PHASE_CONNECTING,
                          .user = j->user};
  mc_sync_emit_progress(sync, &prog);
  mc_channel *ch = NULL;
  mc_status st = MC_ERR_NET;
  int at = -1, answered = 0, handshook = 0;
  uint64_t t0 = mc_mono_ms();
  mc_session s;
  memset(&s, 0, sizeof s);
  s.sync = sync;
  s.conn = c;
  memcpy(s.peer_uid, j->uid, MC_UID_STRLEN);
  s.asked = j->user;
  s.pressed = j->now;
  int claimed = 0, claimed_any = 0, cancelled = 0, peer_cancelled = 0;
  /* Each address in turn, until one answers as the member: what answers at an old one may be
   * another device, or another program. */
  for (int i = 0; i < j->n && !ch && !stopping(sync); i++) {
    mc_channel *tried = NULL;
    if (mc_channel_connect(j->host[i], j->port[i], MC_MODE_SESSION, CONNECT_MS, &tried) != MC_OK)
      continue;
    answered = 1;
    mc_sync_track(sync, tried);
    claimed = mc_sync_claim(sync, j->uid, strcmp(sync->device_uid, j->uid) < 0, tried);
    if (!claimed) {
      /* A session with it is running already, or the user cancelled: no other address. */
      st = MC_ERR_LOCKED;
      ch = tried;
      at = i;
      break;
    }
    claimed_any = 1;
    s.ch = tried;
    uint64_t t_hs = mc_mono_ms();
    st = mc_session_handshake_initiate(&s, key);
    mc_sync_trace("initiate %.8s: handshake at %d +%llu ms", j->uid, i,
                  (unsigned long long)(mc_mono_ms() - t0));
    if (st == MC_OK) {
      ch = tried;
      at = i;
      handshook = 1;
      break;
    }
    /* Silence until the timeout is the member, slow or stuck, as much as anything else:
     * another address would only wait as long again. */
    int silent = mc_mono_ms() - t_hs + 100 >= (uint64_t)mc_channel_default_timeout();
    mc_sync_release(sync, j->uid);
    claimed = 0;
    mc_sync_untrack(sync, tried);
    mc_channel_close(tried);
    s.ch = NULL;
    if (silent || stopping(sync)) break;
    note_wrong(sync, j->uid, j->host[i], j->port[i]);
  }
  mc_sync_trace("initiate %.8s: connect %s in %llu ms (%d addresses)", j->uid,
                answered ? "ok" : "failed", (unsigned long long)(mc_mono_ms() - t0), j->n);
  if (answered)
    reach_note(sync, j->uid, REACH_OUT_OK, NULL, 0);
  else if (j->n && !stopping(sync)) {
    reach_note(sync, j->uid, REACH_OUT_FAIL, NULL, 0);
    contact_failed(sync, j->uid);
  }
  if (ch) {
    if (handshook) {
      note_handshake(sync, j->uid);
      mc_sync_take_user(sync, j->uid); /* this session carries the press */
      note_session_started(sync, j->uid);
      st = mc_session_run(&s);
      if (st == MC_ERR_LOCKED) note_refused(sync, j->uid, s.refused_hold, s.refused_until_ms);
    }
    if (claimed) cancelled = mc_sync_release(sync, j->uid) && st != MC_OK;
    peer_cancelled = handshook && !cancelled && st != MC_OK && mc_channel_peer_cancelled(ch);
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
  } else if (answered && st == MC_OK) {
    st = MC_ERR_NET;
  }
  if (s.we_removed) mc_sync_forget_group(sync, c);
  if (!claimed_any && quiet && (answered || !j->user)) {
    /* Unreachable, or someone else is syncing with it: nothing to report, but the
     * CONNECTING above said a session began, so say it's over. */
    prog.phase = MC_SYNC_PHASE_DONE;
    mc_sync_emit_progress(sync, &prog);
    return st;
  }
  mc_sync_result r = {.device_uid = j->uid, .status = st, .library_synced = s.library_synced,
                      .converged = st == MC_OK && s.converged, .rows_sent = s.rows_sent,
                      .rows_received = s.rows_received, .cancelled = cancelled,
                      .peer_cancelled = peer_cancelled, .user = j->user, .connected = handshook};
  mc_transfer_fill_result(&s, &r);
  mc_sync_session_ended(sync, c, j->uid, &r, s.rows_new, s.follow_up, at >= 0 ? j->host[at] : NULL,
                        at >= 0 ? j->port[at] : 0);
  return st;
}

static void session_worker(void *arg) {
  session_job *j = arg;
  mc_sync_conn c;
  if (mc_sync_conn_take(j->sync, &c) == MC_OK) {
    job_addresses(&c, j, NULL, 0);
    initiate(j->sync, &c, j, 1);
    mc_sync_conn_give(j->sync, &c);
  }
  free(j);
}

/* --- Reach checks --- */

static void reach_done(mc_sync *sync, const char *uid) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  if (p) p->reaching = 0;
  mc_mutex_unlock(&sync->lock);
}

/* Asks a peer to connect back. The job holds its discovered or last known address; a
 * member's hints are added here, and the address its sessions come from at each port. */
static void reach_worker(void *arg) {
  session_job *j = arg;
  mc_sync *sync = j->sync;
  char from[MC_SYNC_ADDR_MAX] = "";
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, j->uid, 0);
  int member = p && p->member;
  if (p) memcpy(from, p->in_addr, sizeof from);
  uint16_t port = sync->reach_port_override ? sync->reach_port_override : sync->port;
  mc_mutex_unlock(&sync->lock);
  mc_sync_conn c;
  if (member && mc_sync_conn_take(sync, &c) == MC_OK) {
    job_addresses(&c, j, NULL, 0);
    mc_sync_conn_give(sync, &c);
  }
  for (int i = 0, n = j->n; from[0] && i < n; i++) job_add(j, from, j->port[i]);

  mc_reach_answer a;
  memset(&a, 0, sizeof a);
  int connected = 0;
  for (int i = 0; i < j->n && !connected && !stopping(sync); i++) {
    mc_channel *ch = NULL;
    if (mc_channel_connect(j->host[i], j->port[i], MC_MODE_REACH, CONNECT_MS, &ch) != MC_OK)
      continue;
    connected = 1;
    mc_sync_track(sync, ch);
    mc_reach_ask(ch, sync->device_uid, port, &a);
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
  }
  if (a.answered && strcmp(a.uid, j->uid) != 0) {
    /* Another device has its address now: nothing learnt about this one. */
  } else if (connected) {
    reach_note(sync, j->uid, REACH_OUT_OK, NULL, 0);
    if (a.answered) reach_note(sync, j->uid, a.ok ? REACH_IN_OK : REACH_IN_FAIL, NULL, 0);
  } else if (j->n && !stopping(sync)) {
    reach_note(sync, j->uid, REACH_OUT_FAIL, NULL, 0);
  }
  reach_done(sync, j->uid);
  free(j);
}

/* Another device's reach check. Nothing in it is authenticated, so it counts as evidence
 * about the device it names only when it comes from where that device is known to be. */
static void serve_reach(mc_sync *sync, mc_channel *ch) {
  char claimed[MC_UID_STRLEN], host[MC_SYNC_ADDR_MAX];
  int ok;
  mc_status st = mc_reach_serve(ch, sync->device_uid, claimed, host, &ok);
  if (st == MC_ERR_FORMAT || !claimed[0] || !strcmp(claimed, sync->device_uid)) return;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, claimed, 0);
  int known = p && host[0] && (!strcmp(p->address, host) || !strcmp(p->in_addr, host));
  mc_mutex_unlock(&sync->lock);
  if (!known) return;
  reach_note(sync, claimed, REACH_IN_OK, NULL, 0);
  reach_note(sync, claimed, ok ? REACH_OUT_OK : REACH_OUT_FAIL, NULL, 0);
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
  if (mc_channel_read_preamble(ch) != MC_OK || mc_sync_conn_take(sync, &c) != MC_OK) {
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
    return;
  }
  if (mc_channel_mode(ch) == MC_MODE_PAIRING) {
    mc_pairing_respond(sync, &c, ch);
    mc_sync_conn_close(&c); /* rare, and it may set the library aside: not kept */
    return;
  }
  if (mc_channel_mode(ch) == MC_MODE_REACH) {
    serve_reach(sync, ch);
    mc_sync_untrack(sync, ch);
    mc_channel_close(ch);
    mc_sync_conn_give(sync, &c);
    return;
  }
  mc_session s;
  memset(&s, 0, sizeof s);
  s.sync = sync;
  s.conn = &c;
  s.ch = ch;
  mc_status st = mc_session_handshake_respond(&s);
  if (st == MC_OK) {
    reach_note(sync, s.peer_uid, REACH_IN_OK, mc_channel_peer_addr(ch), 1);
    s.pressed = mc_sync_pressed(sync, s.peer_uid);
    if (!s.pressed && c.lib && mc_delete_bin_count(c.lib) > 0) {
      /* Deletes in the recycle bin wait for this device's user to press Sync: the other
       * device keeps its schedule, as for a cancelled one. */
      mc_session_refuse_busy(ch, "bin", 0, "deletes here wait for a press of Sync");
    } else if (mc_sync_claim(sync, s.peer_uid, strcmp(s.peer_uid, sync->device_uid) < 0, ch)) {
      s.asked = mc_sync_take_user(sync, s.peer_uid); /* pressed here, and it called back */
      note_session_started(sync, s.peer_uid);
      st = mc_session_run(&s);
      int cancelled = mc_sync_release(sync, s.peer_uid) && st != MC_OK;
      int peer_cancelled = !cancelled && st != MC_OK && mc_channel_peer_cancelled(ch);
      if (s.we_removed) mc_sync_forget_group(sync, &c);
      mc_sync_result r = {.device_uid = s.peer_uid, .status = st,
                          .library_synced = s.library_synced,
                          .converged = st == MC_OK && s.converged, .rows_sent = s.rows_sent,
                          .rows_received = s.rows_received, .cancelled = cancelled,
                          .peer_cancelled = peer_cancelled, .user = s.asked, .connected = 1};
      mc_transfer_fill_result(&s, &r);
      mc_sync_session_ended(sync, &c, s.peer_uid, &r, s.rows_new, s.follow_up, NULL, 0);
    } else {
      /* A session with it is running already, or this device's user cancelled one. */
      int64_t until = 0;
      mc_mutex_lock(&sync->lock);
      mc_sync_peer *p = find_peer(sync, s.peer_uid, 0);
      if (p && !p->busy && mc_mono_ms() < p->held_ms) until = p->held_wall_ms;
      mc_mutex_unlock(&sync->lock);
      if (until)
        mc_session_refuse_busy(ch, "cancelled", until, "sync was cancelled here");
      else
        mc_session_refuse_busy(ch, "running", 0, "already syncing");
    }
  }
  mc_sync_untrack(sync, ch);
  mc_channel_close(ch);
  mc_sync_conn_give(sync, &c);
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
    /* Filled in before the worker starts, which may be at once. */
    serve_arg *a = malloc(sizeof *a);
    if (a) {
      a->sync = sync;
      a->ch = ch;
    }
    if (!a || mc_sync_spawn(sync, serve_worker, a) != MC_OK) {
      free(a);
      mc_channel_close(ch);
    }
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

/* --- Waiting tracks --- */

typedef struct waiting_ctx {
  mc_sync *sync;
  mc_sync_conn *c;
} waiting_ctx;

/* mc_waiting_reach_fn: the last real session attempt with uid (kept across stop and start),
 * or one running with it. Automatic sync being off makes no attempts, so nothing changes then.
 * For a member not tried yet: reachable while automatic sync is off or within the grace after
 * start, then if its last complete session was recent. */
static int waiting_reachable(void *user, const char *uid) {
  waiting_ctx *x = user;
  mc_sync *sync = x->sync;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 0);
  mc_sync_contact contact = p ? p->contact : MC_CONTACT_NONE;
  int busy = p && p->busy;
  /* Not tried yet: no news while the scheduler may not try it (automatic sync is off, as on
   * a phone off screen) or hasn't had its chance since it started or came back on. */
  int unknown = sync->waiting_grace_ms != 0 || sync->no_auto;
  mc_mutex_unlock(&sync->lock);
  if (busy || contact == MC_CONTACT_REACHED) return 1;
  if (contact == MC_CONTACT_FAILED) return 0;
  if (unknown) return 1;
  int64_t last = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(x->c->sdb),
                         "SELECT last_sync_ms FROM peer_contact WHERE device_uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) last = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
  }
  int64_t now = (int64_t)mc_wall_ms();
  return last > 0 && now - last < MC_SYNC_AUTO_INTERVAL_MS;
}

/* On the scheduler's thread: works the waiting tracks out again, and says if they changed.
 * On an error the last answer stands. */
static void update_waiting(mc_sync *sync, mc_sync_conn *c) {
  waiting_ctx x = {sync, c};
  mc_sync_waiting *w = NULL;
  mc_sync_outlook o;
  mc_waiting_extra *extra = calloc(1, sizeof *extra);
  if (!extra) return;
  if (mc_waiting_compute(c->sdb, c->lib, waiting_reachable, &x, &w, &o, extra) != MC_OK) {
    free(extra);
    return;
  }
  mc_mutex_lock(&sync->lock);
  mc_waiting_extra *old_extra = sync->wextra;
  sync->wextra = extra;
  int changed = !mc_waiting_equal(sync->waiting, w) || sync->waiting_announce;
  sync->waiting_announce = 0;
  if (changed) {
    mc_sync_waiting *old = sync->waiting;
    sync->waiting = w;
    w = old;
  }
  int outlook_changed = memcmp(&sync->outlook, &o, sizeof o) != 0;
  sync->outlook = o;
  mc_mutex_unlock(&sync->lock);
  if (old_extra) {
    mc_waiting_extra_free(old_extra);
    free(old_extra);
  }
  mc_sync_waiting_free(w);
  if (changed) emit_waiting(sync);
  if (outlook_changed) emit_outlook(sync);
}

/* As update_waiting(), for the problems. */
static void update_problems(mc_sync *sync, mc_sync_conn *c) {
  mc_sync_problems *p = NULL;
  if (mc_problems_compute(c->sdb, c->lib, &p) != MC_OK) return;
  int64_t due = 0;
  mc_issues_build(sync, c, p, &due);
  mc_mutex_lock(&sync->lock);
  sync->issues_due_ms = due;
  int changed = !mc_problems_equal(sync->problems, p);
  mc_sync_problems *old = sync->problems;
  sync->problems = p;
  mc_mutex_unlock(&sync->lock);
  mc_sync_problems_free(old);
  if (changed) emit_problems(sync);
}

mc_status mc_sync_get_problems(mc_sync *sync, mc_sync_problems **out) {
  if (out) *out = NULL;
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  *out = mc_problems_copy(sync->problems);
  mc_mutex_unlock(&sync->lock);
  return *out ? MC_OK : MC_ERR_NOMEM;
}

/* The weekly deep check, on a desktop or laptop: it gives way to the battery and to stopping. */
static int deep_stop(void *user) {
  return mc_sync_is_stopping(user) || !mc_power_on_ac();
}

static int deep_claim(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  int got = !sync->deep_running;
  sync->deep_running = 1;
  mc_mutex_unlock(&sync->lock);
  return got;
}

static void deep_release(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  sync->deep_running = 0;
  mc_mutex_unlock(&sync->lock);
}

static void deep_worker(void *arg) {
  mc_sync *sync = arg;
  mc_sync_conn c;
  if (mc_sync_conn_open(sync, &c) == MC_OK) {
    int done;
    mc_transfer_deep_run(sync, &c, 0, deep_stop, sync, &done);
    mc_sync_conn_close(&c);
  }
  deep_release(sync);
}

mc_status mc_sync_verify_weekly(mc_sync *sync, int64_t budget_ms, int *done) {
  if (done) *done = 0;
  if (!sync || !done || budget_ms < 0) return MC_ERR_INVALID_ARG;
  if (!deep_claim(sync)) return MC_OK; /* a run's under way: not done, so ask again */
  mc_sync_conn c;
  mc_status st = mc_sync_conn_open(sync, &c);
  if (st == MC_OK) {
    st = mc_transfer_deep_run(sync, &c, budget_ms, NULL, NULL, done);
    mc_sync_conn_close(&c);
  }
  deep_release(sync);
  return st;
}

void mc_sync_set_test_deep_delay(mc_sync *sync, uint32_t ms) {
  sync->deep_delay_ms = ms;
}

void mc_sync_waiting_changed(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

mc_status mc_sync_get_outlook(mc_sync *sync, mc_sync_outlook *out) {
  if (out) memset(out, 0, sizeof *out);
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  *out = sync->outlook;
  mc_mutex_unlock(&sync->lock);
  return MC_OK;
}

mc_status mc_sync_get_waiting(mc_sync *sync, mc_sync_waiting **out) {
  if (out) *out = NULL;
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  *out = mc_waiting_copy(sync->waiting);
  mc_mutex_unlock(&sync->lock);
  return *out ? MC_OK : MC_ERR_NOMEM;
}

/* --- Scheduler loop --- */

/* This device may transcode now: members that want Opus and are missing tracks, as their own
 * reports say, get a session soon. */
static void wake_for_copies(mc_sync *sync, mc_sync_conn *c) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                         "SELECT d.uid FROM devices d JOIN profiles p ON p.device_uid = d.uid"
                         " JOIN sync_status s ON s.device_uid = d.uid"
                         " WHERE d.removed = 0 AND d.uid <> ?1 AND p.archive = 0"
                         " AND (p.format = 'opus' OR p.budget_enabled = 1) AND s.missing > 0",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(stmt, 1, sync->device_uid, -1, SQLITE_STATIC);
  uint64_t now = mc_mono_ms();
  mc_mutex_lock(&sync->lock);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    mc_sync_peer *p = is_uid(uid) ? find_peer(sync, uid, 1) : NULL;
    if (!p || p->trigger == MC_TRIGGER_NOW) continue;
    p->trigger = MC_TRIGGER_COPIES;
    p->trigger_ms = now;
    p->due_ms = now;
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  sqlite3_finalize(stmt);
}

void mc_sync_self_hold(mc_sync *sync, mc_sync_conn *c, mc_sync_hold *hold, int64_t *until_ms) {
  *hold = MC_HOLD_NONE;
  *until_ms = 0;
  if (c->lib && mc_delete_bin_count(c->lib) > 0) {
    *hold = MC_HOLD_BIN;
    return;
  }
  uint64_t now = mc_mono_ms();
  int64_t until = 0;
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->npeers; i++) {
    const mc_sync_peer *p = &sync->peers[i];
    if (p->member && now < p->held_ms && p->held_wall_ms > until) until = p->held_wall_ms;
  }
  int no_auto = sync->no_auto;
  mc_mutex_unlock(&sync->lock);
  if (until) {
    *hold = MC_HOLD_CANCELLED;
    *until_ms = until;
  } else if (no_auto) {
    *hold = MC_HOLD_AUTO_OFF;
  } else if (mc_sync_db_group_uid(c->sdb) && mc_profile_chosen(c->sdb) == 0) {
    *hold = MC_HOLD_NOT_CHOSEN;
  }
}

/* On the scheduler's thread: writes this device's hold into its capabilities when it
 * changed, so the others can say why it doesn't sync. */
static void publish_hold(mc_sync *sync, mc_sync_conn *c) {
  if (!mc_sync_db_group_uid(c->sdb)) return;
  mc_sync_hold hold;
  int64_t until;
  mc_sync_self_hold(sync, c, &hold, &until);
  char *state = mc_self_state(sync, c);
  mc_mutex_lock(&sync->lock);
  int changed = hold != sync->pub_hold || until != sync->pub_until_ms ||
                strcmp(state ? state : "", sync->pub_state ? sync->pub_state : "") != 0;
  sync->pub_hold = hold;
  sync->pub_until_ms = until;
  free(sync->pub_state);
  sync->pub_state = state;
  mc_mutex_unlock(&sync->lock);
  if (changed) mc_session_refresh_self(sync, c);
}

/* Notes when this device last saw each member discovery sees (peer_contact.seen_ms), at most
 * once a minute each. */
static void write_seen(mc_sync *sync, mc_sync_conn *c, uint64_t now) {
  char uids[16][MC_UID_STRLEN];
  int n = 0;
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->npeers && n < 16; i++) {
    mc_sync_peer *p = &sync->peers[i];
    if (!p->member || !p->discovered) continue;
    if (p->seen_written_ms && now < p->seen_written_ms + 60000) continue;
    p->seen_written_ms = now;
    memcpy(uids[n++], p->uid, MC_UID_STRLEN);
  }
  mc_mutex_unlock(&sync->lock);
  sqlite3_stmt *stmt;
  if (!n || sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                               "INSERT INTO peer_contact (device_uid, seen_ms) VALUES (?1, ?2)"
                               " ON CONFLICT (device_uid) DO UPDATE SET seen_ms = excluded.seen_ms",
                               -1, &stmt, NULL) != SQLITE_OK)
    return;
  for (int i = 0; i < n; i++) {
    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, uids[i], -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)mc_wall_ms());
    sqlite3_step(stmt);
  }
  sqlite3_finalize(stmt);
}

static void sched_main(void *arg) {
  mc_sync *sync = arg;
  mc_sync_conn c;
  if (mc_sync_conn_open(sync, &c) != MC_OK) return;
  int64_t seen_lib = max_seq(mc_library_db(c.lib));
  int64_t seen_group = max_seq(mc_sync_db_handle(c.sdb));
  uint64_t next_poll = 0, next_hints = mc_mono_ms() + HINTS_MS;
  uint64_t next_deep = mc_mono_ms() + sync->deep_delay_ms;

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
      if (changed) {
        mc_session_refresh_self(sync, &c);
        reach_forget(sync);
      }
    }
    mc_mutex_lock(&sync->lock);
    int power = sync->power_dirty, copies = sync->power_copies, transcoding = sync->transcoding_dirty;
    sync->power_dirty = sync->power_copies = sync->transcoding_dirty = 0;
    mc_mutex_unlock(&sync->lock);
    if (transcoding) emit_transcoding(sync);
    if (power) mc_session_refresh_self(sync, &c);
    if (copies) wake_for_copies(sync, &c);
    mc_mutex_lock(&sync->lock);
    int network = sync->network_dirty;
    sync->network_dirty = 0;
    mc_mutex_unlock(&sync->lock);
    if (network) next_hints = now; /* the addresses this device announces */
    if (now >= next_deep) {
      next_deep = now + HINTS_MS;
      if (sync->kind == MC_DEVICE_DESKTOP && mc_power_on_ac() && mc_transfer_deep_due(c.sdb) &&
          deep_claim(sync) && mc_sync_spawn(sync, deep_worker, sync) != MC_OK)
        deep_release(sync);
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
      /* Profiles or members changed, here or applied by a session. The
       * library's changes (plays, edits) leave the waiting tracks as they were, and a
       * session that brings new tracks asks for them on its own. */
      if (group != seen_group) sync->waiting_dirty = 1;
      mc_mutex_unlock(&sync->lock);
      seen_lib = lib;
      seen_group = group;
      /* "A group of one goes": at start, and once a session or an edit has left nobody else. */
      if (alone_in_group(c.sdb, sync->device_uid) && dissolve_if_alone(sync))
        seen_group = max_seq(mc_sync_db_handle(c.sdb));
      publish_hold(sync, &c);
      write_seen(sync, &c, now);
    }
    mc_mutex_lock(&sync->lock);
    /* An issue that lapses by itself (a hold's end) is worked out again then. */
    if (sync->issues_due_ms && (int64_t)mc_wall_ms() >= sync->issues_due_ms) {
      sync->issues_due_ms = 0;
      sync->waiting_dirty = 1;
    }
    if (sync->waiting_grace_ms && now >= sync->waiting_grace_ms) {
      sync->waiting_grace_ms = 0; /* over: work them out with what's known */
      sync->waiting_dirty = 1;
    }
    int redo_waiting = sync->waiting_dirty;
    sync->waiting_dirty = 0;
    mc_mutex_unlock(&sync->lock);
    if (redo_waiting) {
      update_waiting(sync, &c);
      update_problems(sync, &c);
    }

    member_row *ms;
    int n = load_members(&c, &ms);
    /* While the recycle bin holds deletes, only a press of Sync starts a session. */
    int bin_held = c.lib && mc_delete_bin_count(c.lib) > 0;
    session_job *jobs[16];
    int njobs = 0;
    uint64_t wake = next_poll < next_hints ? next_poll : next_hints;

    mc_mutex_lock(&sync->lock);
    if (sync->stopping) {
      mc_mutex_unlock(&sync->lock);
      free(ms);
      break;
    }
    /* A cancel left to a send under way that hasn't ended the session yet: cut it off. */
    uint64_t cut = mc_mono_ms();
    for (int i = 0; i < sync->npeers; i++) {
      mc_sync_peer *p = &sync->peers[i];
      if (!p->cancel_ms) continue;
      if (p->busy && cut < p->cancel_ms + CANCEL_GRACE_MS) {
        if (p->cancel_ms + CANCEL_GRACE_MS < wake) wake = p->cancel_ms + CANCEL_GRACE_MS;
        continue;
      }
      if (p->busy && p->busy_ch) mc_channel_abort(p->busy_ch);
      p->cancel_ms = 0;
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
      if (p->busy || (sync->manual_only && p->trigger != MC_TRIGGER_NOW &&
                      p->trigger != MC_TRIGGER_AUTO))
        continue;
      if (sync->no_auto && p->trigger != MC_TRIGGER_NOW) continue; /* kept for later */
      /* Deletes in the recycle bin go only when this device's user says so. */
      if (bin_held && p->trigger != MC_TRIGGER_NOW) continue;
      /* Refused for its version: only a press of Sync, or news of another version, tries. */
      if (p->version.outdated != MC_SYNC_VERSION_OK && p->trigger != MC_TRIGGER_NOW) continue;
      if (now < p->held_ms) {
        /* Cancelled: whatever is due waits until the hold runs out. */
        if (p->held_ms < wake) wake = p->held_ms;
        continue;
      }
      if (now < p->due_ms) {
        if (p->due_ms < wake) wake = p->due_ms;
        continue;
      }
      if (p->trigger != MC_TRIGGER_NOW && now < p->peer_held_ms) {
        /* Its user cancelled a session lately: only a press of Sync goes ahead. */
        if (p->peer_held_ms < wake) wake = p->peer_held_ms;
        continue;
      }
      if (p->trigger != MC_TRIGGER_NOW && p->trigger != MC_TRIGGER_COPIES &&
          now < p->auto_after_ms) {
        /* Synced lately: hold the trigger until the limit runs out. A periodic or online
         * one counts from then, for the higher uid's grace. */
        p->due_ms = p->auto_after_ms;
        if (p->trigger == MC_TRIGGER_PERIODIC || p->trigger == MC_TRIGGER_ONLINE)
          p->trigger_ms = p->due_ms;
        if (p->due_ms < wake) wake = p->due_ms;
        continue;
      }
      int go = p->trigger == MC_TRIGGER_CHANGED || p->trigger == MC_TRIGGER_NOW ||
               p->trigger == MC_TRIGGER_AUTO || p->trigger == MC_TRIGGER_COPIES ||
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
      j->user = p->asked && p->trigger == MC_TRIGGER_NOW;
      j->now = p->trigger == MC_TRIGGER_NOW;
      p->asked = 0;
      /* Until the session reports back. */
      p->due_ms = now + PERIOD_MS;
      p->trigger = MC_TRIGGER_PERIODIC;
      p->trigger_ms = p->due_ms;
      jobs[njobs++] = j;
    }
    /* Reach checks: each peer online, when first seen and every REACH_EVERY_MS. */
    session_job *checks[16];
    int nchecks = 0;
    for (int i = 0; i < sync->npeers && nchecks < 16; i++) {
      mc_sync_peer *p = &sync->peers[i];
      if (p->reaching) continue;
      int online = !sync->manual_only && !sync->no_auto && (p->reached || reach_alive(p, now));
      uint64_t due = p->reach_ms ? p->reach_ms + REACH_EVERY_MS : now;
      if (!p->reach_now && !(online && now >= due)) {
        if (online && due < wake) wake = due;
        continue;
      }
      session_job *j = calloc(1, sizeof *j);
      if (!j) break;
      j->sync = sync;
      memcpy(j->uid, p->uid, MC_UID_STRLEN);
      job_add(j, p->address, p->port);
      p->reaching = 1;
      p->reach_now = 0;
      p->reach_ms = now;
      checks[nchecks++] = j;
    }
    mc_mutex_unlock(&sync->lock);

    for (int i = 0; i < njobs; i++)
      if (mc_sync_spawn(sync, session_worker, jobs[i]) != MC_OK) free(jobs[i]);
    for (int i = 0; i < nchecks; i++) {
      if (mc_sync_spawn(sync, reach_worker, checks[i]) == MC_OK) continue;
      reach_done(sync, checks[i]->uid);
      free(checks[i]);
    }

    mc_mutex_lock(&sync->lock);
    now = mc_mono_ms();
    if (!sync->stopping && !njobs && wake > now)
      mc_cond_wait_ms(&sync->cond, &sync->lock, (uint32_t)(wake - now > POLL_MS ? POLL_MS : wake - now));
    mc_mutex_unlock(&sync->lock);
  }
  mc_sync_conn_close(&c);
}

/* --- Discovery --- */

/* ver: what its announcement says of its version, NULL if nothing. */
static void peer_seen(mc_sync *sync, const char *uid, const char *name, const char *kind,
                      const char *host, const char *grp, const char *address, uint16_t port,
                      const mc_sync_version *ver, int online) {
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, uid, 1);
  int member = 0, changed = 0, version_changed = 0;
  if (p) {
    int was = p->discovered || p->reached;
    /* Discovery only reports a peer again when its announcement changed, so this is news
     * (an upgrade, say) rather than an echo overriding what a session found. */
    if (online && ver) version_changed = set_version(p, ver);
    if (online) {
      snprintf(p->name, sizeof p->name, "%s", name ? name : "");
      snprintf(p->kind, sizeof p->kind, "%s", kind ? kind : "");
      snprintf(p->host, sizeof p->host, "%s", host ? host : "");
      snprintf(p->grp, sizeof p->grp, "%s", grp ? grp : "");
      snprintf(p->address, sizeof p->address, "%s", address ? address : "");
      p->port = port;
      if (!p->discovered) {
        p->seen_ms = mc_mono_ms();
        if (p->member && p->trigger != MC_TRIGGER_NOW) {
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
  if (version_changed) emit_version(sync, uid);
}

static void on_discovered(const mc_peer_info *peer, int online, void *user) {
  mc_sync *sync = user;
  mc_version_self self;
  mc_sync_self_version(sync, &self);
  mc_sync_version ver;
  memset(&ver, 0, sizeof ver);
  ver.protocol = peer->version > 0 ? peer->version : 0;
  ver.protocol_min = peer->min_version > 0 ? peer->min_version : 0;
  snprintf(ver.app, sizeof ver.app, "%s", peer->app ? peer->app : "");
  mc_version_judge(&self, &ver);
  peer_seen(sync, peer->device_uid, peer->name, peer->kind, peer->host, peer->group_hash,
            peer->address, peer->port, online ? &ver : NULL, online);
}

/* --- Public API --- */

/* Seeds the rate limit from peer_contact.last_sync_ms (written after each successful
 * session). Failures aren't recorded there, so after a restart a retry may be held back for
 * one interval at most. A future last_sync_ms (clock went back) counts as now. Before any
 * thread runs. */
static void seed_auto_after(mc_sync *sync) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sync->api.sdb),
                         "SELECT device_uid, last_sync_ms FROM peer_contact"
                         " WHERE last_sync_ms > 0",
                         -1, &stmt, NULL) != SQLITE_OK)
    return;
  uint64_t mono = mc_mono_ms();
  int64_t wall = mc_wall_ms(), interval = (int64_t)sync->auto_interval_ms;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *uid = (const char *)sqlite3_column_text(stmt, 0);
    int64_t age = wall - sqlite3_column_int64(stmt, 1);
    if (age < 0) age = 0;
    if (!is_uid(uid) || age >= interval) continue;
    mc_sync_peer *p = find_peer(sync, uid, 1);
    if (p) p->auto_after_ms = mono + (uint64_t)(interval - age);
  }
  sqlite3_finalize(stmt);
}

static void pool_busy_changed(void *user) {
  mc_sync *sync = user;
  mc_mutex_lock(&sync->lock);
  sync->transcoding_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

int mc_sync_test_pool_size = 0;

int mc_sync_pool_size(void) {
  if (mc_sync_test_pool_size > 0) return mc_sync_test_pool_size;
  int cores = mc_cpu_count() - 1;
  return cores > 1 ? cores : 1;
}

void mc_sync_set_power(mc_sync *sync, int on_power, int may_transcode) {
  if (!sync) return;
  on_power = on_power != 0;
  may_transcode = may_transcode != 0;
  mc_mutex_lock(&sync->lock);
  int was = sync->may_transcode;
  int changed = on_power != sync->on_power || may_transcode != was;
  sync->on_power = on_power;
  sync->may_transcode = may_transcode;
  if (changed) sync->power_dirty = 1;
  if (may_transcode && !was) sync->power_copies = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  if (!changed) return;
  /* Not under lock: the pool's notify takes it. As fast as it can whenever it may transcode,
   * on battery too: the user allowed that (a Sync page open, or Background pressed). */
  mc_encode_pool_set_size(sync->enc, mc_sync_pool_size());
  mc_encode_pool_set_closed(sync->enc, !may_transcode);
}

void mc_sync_power(mc_sync *sync, int *on_power, int *may_transcode) {
  mc_mutex_lock(&sync->lock);
  if (on_power) *on_power = sync->on_power;
  if (may_transcode) *may_transcode = sync->may_transcode;
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_copying(mc_sync *sync, int delta) {
  mc_mutex_lock(&sync->lock);
  sync->copying += delta;
  sync->transcoding_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

int mc_sync_transcoding(mc_sync *sync) {
  if (!sync) return 0;
  mc_mutex_lock(&sync->lock);
  int copying = sync->copying > 0;
  mc_mutex_unlock(&sync->lock);
  return copying || mc_encode_pool_busy(sync->enc);
}

int mc_sync_transcoding_for(mc_sync *sync, mc_sync_transcode_target *out, int max) {
  if (!sync || !sync->enc || !out || max <= 0) return 0;
  mc_encode_device *ds = calloc((size_t)max, sizeof *ds);
  if (!ds) return 0;
  int n = mc_encode_pool_devices(sync->enc, ds, max);
  for (int i = 0; i < n; i++) {
    memcpy(out[i].device_uid, ds[i].device, MC_UID_STRLEN);
    out[i].tracks = ds[i].jobs;
  }
  free(ds);
  return n;
}

mc_status mc_sync_new(mc_library *lib, const char *config_dir, const char *device_name,
                      mc_device_kind kind, const mc_sync_events *events, void *user,
                      mc_sync **out) {
  if (out) *out = NULL;
  if (!lib || !config_dir || !*config_dir || !out ||
      (kind != MC_DEVICE_DESKTOP && kind != MC_DEVICE_PHONE))
    return MC_ERR_INVALID_ARG;
  /* The group is the device's, not the library's: sync.db would offer this one to it. */
  if (mc_library_is_stress(lib)) return MC_ERR_STRESS_LIBRARY;
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
  mc_mutex_init(&sync->api_lock);
  mc_mutex_init(&sync->lock);
  mc_cond_init(&sync->cond);
  sync->kind = kind;
  sync->on_power = sync->may_transcode = kind == MC_DEVICE_DESKTOP;
  sync->deep_delay_ms = DEEP_FIRST_MS;
  if (events) sync->ev = *events;
  sync->user = user;
  sync->use_discovery = 1;
  sync->listen_port = -1;
  sync->auto_interval_ms = MC_SYNC_AUTO_INTERVAL_MS;
  sync->peer_cancel_hold_ms = MC_SYNC_PEER_CANCEL_HOLD_MS;
  mc_version_self_default(&sync->self_version);
  if (mc_hostname(sync->host, sizeof sync->host) != MC_OK) sync->host[0] = '\0';
  clean_name(device_name, sync->name);
  if (!sync->name[0]) clean_name(sync->host[0] ? sync->host : "Kithsong", sync->name);

  size_t len = strlen(config_dir) + sizeof MC_TRANSCODE_CACHE_DIR + 1;
  char *cache = malloc(len);
  if (cache) snprintf(cache, len, "%s%c%s", config_dir, MC_PATH_SEP, MC_TRANSCODE_CACHE_DIR);
  mc_status st = cache ? mc_encode_pool_new(cache, MC_TRANSCODE_CACHE_MAX, &sync->enc) : MC_ERR_NOMEM;
  free(cache);
  if (st == MC_OK) {
    mc_encode_pool_set_closed(sync->enc, !sync->may_transcode);
    mc_encode_pool_set_notify(sync->enc, pool_busy_changed, sync);
    st = mc_sync_conn_open(sync, &sync->api);
  }
  if (st != MC_OK) {
    mc_sync_free(sync);
    return st;
  }
  memcpy(sync->device_uid, mc_sync_db_device_uid(sync->api.sdb), MC_UID_STRLEN);
  seed_auto_after(sync);
  const char *group = mc_sync_db_group_uid(sync->api.sdb);
  if (group) mc_discovery_group_hash(group, sync->group_hash);
  mc_folder_note_config(sync->lib_dir, sync->config_dir);
  *out = sync;
  return MC_OK;
}

void mc_sync_free(mc_sync *sync) {
  if (!sync) return;
  mc_sync_stop(sync);
  if (sync->enc) mc_encode_pool_set_notify(sync->enc, NULL, NULL); /* it takes lock, freed below */
  mc_sync_conn_close(&sync->api);
  mc_pair_responder_wipe(&sync->in_r);
  mc_sync_waiting_free(sync->waiting);
  mc_sync_problems_free(sync->problems);
  for (int i = 0; i < sync->nfindings; i++) mc_findings_free(sync->findings[i]);
  free(sync->findings);
  if (sync->wextra) mc_waiting_extra_free(sync->wextra);
  free(sync->wextra);
  mc_issues_free(sync->notices, sync->nnotices);
  free(sync->pub_state);
  mc_takeover_free(sync);
  mc_cond_destroy(&sync->cond);
  mc_mutex_destroy(&sync->lock);
  mc_mutex_destroy(&sync->api_lock);
  mc_mutex_destroy(&sync->cb_lock);
  mc_encode_pool_free(sync->enc);
  free(sync->peers);
  free(sync->chans);
  if (mc_sync_tracing()) trace_flush(); /* the app may be about to exit */
  mc_folder_note_config(sync->lib_dir, NULL);
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

/* A watch of the retired Wear OS app can't sync with this version, nor ever will: it's
 * removed from the group, as a member would remove it. Every member does this when it starts,
 * so they agree, and a device left alone with it forgets the group (the scheduler's check). */
static void drop_watches(mc_sync_db *sdb) {
  if (!mc_sync_db_group_uid(sdb) || mc_sync_db_begin_write(sdb) != MC_OK) return;
  int ok = sqlite3_exec(mc_sync_db_handle(sdb),
                        "UPDATE devices SET removed = 1 WHERE kind = 'watch' AND removed = 0",
                        NULL, NULL, NULL) == SQLITE_OK;
  if (ok && mc_sync_db_commit(sdb) == MC_OK) return;
  mc_sync_db_rollback(sdb);
  mc_log("kithcore: sync: couldn't remove the group's watches\n");
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
  sync->waiting_grace_ms = mc_mono_ms() + WAITING_GRACE_MS;
  sync->waiting_dirty = 1; /* the problems are known at once, and so is what they hold back */
  mc_mutex_unlock(&sync->lock);
  mc_encode_pool_resume(sync->enc);
  char hints[MC_SYNC_HINTS_MAX];
  compute_hints(sync, hints);
  mc_mutex_lock(&sync->lock);
  memcpy(sync->hints, hints, sizeof hints);
  mc_mutex_unlock(&sync->lock);
  mc_mutex_lock(&sync->api_lock);
  drop_watches(sync->api.sdb);
  if (mc_profile_claim(sync->api.sdb) != MC_OK)
    mc_log("kithcore: sync: couldn't claim this device's profile as its own\n");
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
      mc_log("kithcore: sync: no discovery (%s); using known addresses only\n",
              mc_status_str(st));
    mc_mutex_lock(&sync->lock);
    sync->disc = d;
    mc_mutex_unlock(&sync->lock);
  }
  st = mc_thread_start(&sync->listen_thread, listen_main, sync);
  if (st == MC_OK) {
    st = mc_thread_start(&sync->sched_thread, sched_main, sync);
    if (st == MC_OK) mc_takeover_resume(sync);
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
  /* Queued encodes go and a session waiting for one gives up; running ones finish. */
  mc_encode_pool_stop(sync->enc);
  /* Workers may still be connecting; whatever they open now is aborted at once. */
  reap(sync, 1);
  close_idle(sync);

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
  while (sync->dissolving) mc_cond_wait_ms(&sync->cond, &sync->lock, 100); /* then it's no group */
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
  const char *group = mc_sync_db_group_uid(sync->api.sdb);
  char left[MC_UID_STRLEN];
  snprintf(left, sizeof left, "%s", group ? group : "");
  mc_status st = group ? MC_OK : MC_ERR_NOT_FOUND;
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
      j.user = 1; /* the user is leaving */
      memcpy(j.uid, ms[i].uid, MC_UID_STRLEN);
      mc_mutex_lock(&sync->lock);
      mc_sync_peer *p = find_peer(sync, j.uid, 0);
      char host[MC_SYNC_ADDR_MAX] = "";
      uint16_t port = 0;
      if (p && p->discovered) {
        memcpy(host, p->address, sizeof host);
        port = p->port;
      }
      if (p) p->held_ms = p->peer_held_ms = 0; /* leaving outweighs a cancel */
      mc_mutex_unlock(&sync->lock);
      job_addresses(&c, &j, host, port);
      if (initiate(sync, &c, &j, 0) == MC_OK) break;
    }
    mc_sync_conn_close(&c);
  }
  free(ms);

  mc_mutex_lock(&sync->api_lock);
  /* The session may have forgotten it already: only the group being left is cleared. */
  group = mc_sync_db_group_uid(sync->api.sdb);
  if (group && !strcmp(group, left)) st = mc_sync_db_clear_group(sync->api.sdb);
  mc_sync_group_changed(sync, &sync->api);
  mc_mutex_lock(&sync->lock);
  for (int i = 0; i < sync->npeers; i++) sync->peers[i].member = 0;
  mc_mutex_unlock(&sync->lock);
  mc_mutex_unlock(&sync->api_lock);
  return st;
}

/* --- A group of one --- */

/* In a group with no other current member, and not removed itself. */
static int alone_in_group(mc_sync_db *sdb, const char *self) {
  if (!mc_sync_db_group_uid(sdb)) return 0;
  sqlite3 *db = mc_sync_db_handle(sdb);
  return count_query(db, "SELECT count(*) FROM devices WHERE removed = 0 AND uid <> ?1", self) == 0 &&
         count_query(db, "SELECT count(*) FROM devices WHERE uid = ?1 AND removed <> 0", self) == 0;
}

/* Forgets the group if this device is alone in it, unless a pairing, a session or the
 * telling of a removed member is under way (the scheduler looks again). 1 if it did. */
static int dissolve_if_alone(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  int busy = sync->out_active || sync->in_active || sync->in_conn || sync->dissolving ||
             sync->dissolve_telling || sync->stopping;
  for (int i = 0; i < sync->npeers && !busy; i++) busy = sync->peers[i].busy;
  if (!busy) sync->dissolving = 1;
  mc_mutex_unlock(&sync->lock);
  if (busy) return 0;

  mc_mutex_lock(&sync->api_lock);
  int done = alone_in_group(sync->api.sdb, sync->device_uid) &&
             mc_sync_db_clear_group(sync->api.sdb) == MC_OK;
  if (done) mc_sync_group_changed(sync, &sync->api);
  mc_mutex_unlock(&sync->api_lock);

  char name[MC_SYNC_NAME_MAX];
  mc_mutex_lock(&sync->lock);
  sync->dissolving = 0;
  if (done) {
    for (int i = 0; i < sync->npeers; i++) sync->peers[i].member = 0;
    sync->waiting_dirty = 1;
  }
  memcpy(name, sync->name, sizeof name);
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  if (done) {
    mc_log("kithcore: sync: this device was the last in its group, which is gone\n");
    emit_dissolved(sync, name);
  }
  return done;
}

/* Tells the removed device uid so, as a member answering it would, but as the initiator:
 * once nobody here answers it as a member, it couldn't hear it otherwise. Best effort. */
static void tell_removed(mc_sync *sync, mc_sync_conn *c, session_job *j) {
  unsigned char key[MC_SYNC_KEY_LEN];
  int have_key = 0;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(c->sdb),
                         "SELECT public_key FROM devices WHERE uid = ?1 AND length(public_key) = ?2",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, j->uid, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, MC_SYNC_KEY_LEN);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      memcpy(key, sqlite3_column_blob(stmt, 0), MC_SYNC_KEY_LEN);
      have_key = 1;
    }
    sqlite3_finalize(stmt);
  }
  if (!have_key) return;
  mc_channel *ch = NULL;
  for (int i = 0; i < j->n && !ch && !stopping(sync); i++)
    if (mc_channel_connect(j->host[i], j->port[i], MC_MODE_SESSION, CONNECT_MS, &ch) != MC_OK) ch = NULL;
  if (!ch) return;
  mc_sync_track(sync, ch);
  mc_session s;
  memset(&s, 0, sizeof s);
  s.sync = sync;
  s.conn = c;
  s.ch = ch;
  memcpy(s.peer_uid, j->uid, MC_UID_STRLEN);
  if (mc_session_handshake_initiate(&s, key) == MC_OK)
    mc_session_refuse(ch, "removed", "this device was removed from the group");
  mc_sync_untrack(sync, ch);
  mc_channel_close(ch);
}

static void tell_worker(void *arg) {
  session_job *j = arg;
  mc_sync *sync = j->sync;
  mc_sync_conn c;
  if (mc_sync_conn_take(sync, &c) == MC_OK) {
    job_addresses(&c, j, NULL, 0);
    tell_removed(sync, &c, j);
    mc_sync_conn_give(sync, &c);
  }
  mc_mutex_lock(&sync->lock);
  sync->dissolve_telling = 0;
  mc_mutex_unlock(&sync->lock);
  dissolve_if_alone(sync);
  free(j);
}

/* This device just removed uid, the last other member. */
static void dissolve_after_removing(mc_sync *sync, const char *uid) {
  session_job *j = calloc(1, sizeof *j);
  mc_mutex_lock(&sync->lock);
  int tell = j && sync->started && !sync->stopping;
  if (tell) {
    j->sync = sync;
    memcpy(j->uid, uid, MC_UID_STRLEN);
    mc_sync_peer *p = find_peer(sync, uid, 0);
    if (p && p->discovered) job_add(j, p->address, p->port);
    sync->dissolve_telling = 1;
  }
  mc_mutex_unlock(&sync->lock);
  if (tell && mc_sync_spawn(sync, tell_worker, j) == MC_OK) return;
  if (tell) {
    mc_mutex_lock(&sync->lock);
    sync->dissolve_telling = 0;
    mc_mutex_unlock(&sync->lock);
  }
  free(j);
  dissolve_if_alone(sync);
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
  int alone = st == MC_OK && alone_in_group(sync->api.sdb, sync->device_uid);
  mc_mutex_unlock(&sync->api_lock);
  if (st == MC_OK) {
    mc_mutex_lock(&sync->lock);
    mc_sync_peer *p = find_peer(sync, device_uid, 0);
    if (p) p->member = 0;
    trigger_members(sync, device_uid, MC_TRIGGER_CHANGED, mc_mono_ms());
    mc_mutex_unlock(&sync->lock);
  }
  if (alone) dissolve_after_removing(sync, device_uid);
  return st;
}

/* mc_sync_now() (MC_TRIGGER_NOW) and mc_sync_background() (MC_TRIGGER_AUTO). */
static mc_status sync_soon(mc_sync *sync, const char *device_uid, mc_sync_trigger why) {
  if (!sync || (device_uid && !is_uid(device_uid))) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  mc_status st = sync->started ? MC_OK : MC_ERR_INVALID_ARG;
  if (st == MC_OK && device_uid) {
    mc_sync_peer *p = find_peer(sync, device_uid, 1);
    if (p && why == MC_TRIGGER_NOW) p->held_ms = p->peer_held_ms = 0;
    if (why == MC_TRIGGER_NOW) sync->waiting_dirty = 1;
    if (p && (why == MC_TRIGGER_NOW || p->trigger != MC_TRIGGER_NOW)) {
      p->trigger = why;
      p->trigger_ms = mc_mono_ms();
      p->due_ms = p->trigger_ms;
    }
    mc_cond_broadcast(&sync->cond);
  } else if (st == MC_OK) {
    if (why == MC_TRIGGER_NOW) {
      for (int i = 0; i < sync->npeers; i++) sync->peers[i].held_ms = sync->peers[i].peer_held_ms = 0;
      sync->waiting_dirty = 1;
    }
    trigger_members(sync, NULL, why, mc_mono_ms());
  }
  mc_mutex_unlock(&sync->lock);
  return st;
}

/* Under lock. The session's channel tells the peer (CANCEL) if it's armed: see
 * mc_channel_cancel(), which never waits on the network. */
static void cancel_peer(mc_sync_peer *p, uint64_t until) {
  p->held_ms = until;
  p->held_wall_ms = (int64_t)mc_wall_ms() + (int64_t)(until - mc_mono_ms());
  p->user_ms = 0; /* a press no session has carried yet is withdrawn */
  p->asked = 0;
  if (p->trigger == MC_TRIGGER_NOW) p->trigger = MC_TRIGGER_PERIODIC;
  if (p->busy) {
    p->cancel = 1;
    if (p->busy_ch && !mc_channel_cancel(p->busy_ch) && !p->cancel_ms)
      p->cancel_ms = mc_mono_ms(); /* the scheduler aborts it if the session doesn't end soon */
  }
}

mc_status mc_sync_cancel(mc_sync *sync, const char *device_uid) {
  if (!sync || (device_uid && !is_uid(device_uid))) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  uint64_t until = mc_mono_ms() + MC_SYNC_CANCEL_HOLD_MS;
  if (device_uid) {
    mc_sync_peer *p = find_peer(sync, device_uid, 1);
    if (p) cancel_peer(p, until);
  } else {
    for (int i = 0; i < sync->npeers; i++) cancel_peer(&sync->peers[i], until);
  }
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  return MC_OK;
}

mc_status mc_sync_now(mc_sync *sync, const char *device_uid) {
  if (!sync || (device_uid && !is_uid(device_uid))) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  int started = sync->started;
  mc_mutex_unlock(&sync->lock);
  if (!started) return MC_ERR_INVALID_ARG;
  /* A press of Sync is this device's user going with its profile as it is. */
  mc_mutex_lock(&sync->api_lock);
  int chose = 0;
  mc_status st = mc_profile_choose(sync->api.sdb, &chose);
  mc_mutex_unlock(&sync->api_lock);
  if (st != MC_OK && st != MC_ERR_NOT_FOUND) return st;
  if (chose) mc_sync_waiting_changed(sync);
  uint64_t now = mc_mono_ms();
  /* Before the trigger, so the session it starts carries the press. */
  mc_mutex_lock(&sync->lock);
  if (device_uid) {
    mc_sync_peer *p = find_peer(sync, device_uid, 1);
    if (p) {
      p->user_ms = now;
      p->asked = 1;
    }
  } else {
    /* For every member: shown only for those online, so a press doesn't show (or report)
     * an attempt at each member that is away. */
    for (int i = 0; i < sync->npeers; i++) {
      mc_sync_peer *p = &sync->peers[i];
      if (!p->member) continue;
      p->user_ms = now;
      p->asked = p->discovered || p->reached;
    }
  }
  mc_mutex_unlock(&sync->lock);
  return sync_soon(sync, device_uid, MC_TRIGGER_NOW);
}

mc_status mc_sync_background(mc_sync *sync, const char *device_uid) {
  return sync_soon(sync, device_uid, MC_TRIGGER_AUTO);
}

void mc_sync_set_automatic(mc_sync *sync, int on) {
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  /* Back on: members get their sessions before any not tried yet counts as away. */
  if (on && sync->no_auto && sync->started)
    sync->waiting_grace_ms = mc_mono_ms() + WAITING_GRACE_MS;
  if (sync->no_auto != !on) sync->waiting_dirty = 1;
  sync->no_auto = !on;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_network_changed(mc_sync *sync) {
  if (!sync) return;
  mc_mutex_lock(&sync->lock);
  if (!sync->started || sync->stopping) {
    mc_mutex_unlock(&sync->lock);
    return;
  }
  if (sync->disc) mc_discovery_network_changed(sync->disc);
  uint64_t now = mc_mono_ms();
  for (int i = 0; i < sync->npeers; i++) {
    mc_sync_peer *p = &sync->peers[i];
    if (!p->member) continue;
    p->backoff_ms = 0;
    p->wrong_count = 0;
    p->wrong_addr[0] = '\0';
    /* What failed may get through now. The holds stay as they are. */
    if (!p->busy && !p->reached && p->trigger != MC_TRIGGER_NOW &&
        p->trigger != MC_TRIGGER_COPIES) {
      p->trigger = MC_TRIGGER_CHANGED;
      p->trigger_ms = now;
      p->due_ms = now;
    }
  }
  sync->network_dirty = 1;
  sync->waiting_dirty = 1;
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  reach_forget(sync);
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
                         " s.verified_ms, s.expected, s.ok, s.missing, s.corrupt, s.bytes,"
                         " max(coalesce(pc.last_sync_ms, 0), coalesce(pc.seen_ms, 0)),"
                         " c.can_transcode, c.free_bytes, c.state"
                         " FROM devices d LEFT JOIN profiles p ON p.device_uid = d.uid"
                         " LEFT JOIN peer_contact pc ON pc.device_uid = d.uid"
                         " LEFT JOIN sync_status s ON s.device_uid = d.uid"
                         " LEFT JOIN capabilities c ON c.device_uid = d.uid"
                         " ORDER BY shown COLLATE NOCASE, d.uid",
                         -1, &stmt, NULL) != SQLITE_OK)
    st = MC_ERR_DB;
  int bin_held = sync->api.lib && mc_delete_bin_count(sync->api.lib) > 0;
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
    d->last_seen_ms = sqlite3_column_int64(stmt, 16);
    d->low_space = sqlite3_column_int(stmt, 17) && sqlite3_column_type(stmt, 18) != SQLITE_NULL &&
                   sqlite3_column_int64(stmt, 18) < MC_SYNC_TRANSCODE_MIN_FREE;
    mc_archive_state as;
    mc_archive_state_parse((const char *)sqlite3_column_text(stmt, 19), &as);
    if (as.seen_ms > d->last_seen_ms) d->last_seen_ms = as.seen_ms;
    mc_archive_state_free(&as);
    mc_state_parse_hold((const char *)sqlite3_column_text(stmt, 19), &d->peer_hold,
                        &d->peer_hold_until_ms);
  }
  sqlite3_finalize(stmt);
  mc_mutex_unlock(&sync->api_lock);

  /* Whereabouts and the rate limit from the peer table. */
  uint64_t mono_now = mc_mono_ms();
  int64_t wall_now = mc_wall_ms();
  mc_mutex_lock(&sync->lock);
  mc_sync_self_reach self;
  self_reach_locked(sync, &self);
  for (int i = 0; st == MC_OK && i < list->count; i++) {
    mc_sync_device *d = &list->items[i];
    if (d->is_self) {
      d->online = 1;
      d->reach = d->reach_us = self.state;
      continue;
    }
    if (d->peer_hold_until_ms && d->peer_hold_until_ms <= wall_now) {
      d->peer_hold = MC_HOLD_NONE;
      d->peer_hold_until_ms = 0;
    }
    mc_sync_peer *p = find_peer(sync, d->uid, 0);
    if (!p) continue;
    d->online = p->discovered || p->reached;
    d->reach = verdict(p->out_ok_ms, p->out_fail_ms);
    d->reach_us = verdict(p->in_ok_ms, p->in_fail_ms);
    d->version = p->version;
    uint64_t after = p->auto_after_ms > p->peer_held_ms ? p->auto_after_ms : p->peer_held_ms;
    if (after > mono_now) d->auto_after_ms = wall_now + (int64_t)(after - mono_now);
    /* What holds automatic sync with it back here, and when it next goes on its own. */
    if (d->removed) {
    } else if (p->version.outdated != MC_SYNC_VERSION_OK) {
      d->hold = MC_HOLD_VERSION;
    } else if (bin_held) {
      d->hold = MC_HOLD_BIN;
    } else if (mono_now < p->held_ms) {
      d->hold = MC_HOLD_CANCELLED;
    } else if (sync->no_auto) {
      d->hold = MC_HOLD_AUTO_OFF;
    } else if (mono_now < p->peer_held_ms) {
      d->hold = MC_HOLD_PEER_CANCELLED;
    } else if (p->trigger != MC_TRIGGER_NOW && mono_now < p->auto_after_ms) {
      d->hold = MC_HOLD_RATE;
    }
    if (!d->removed && !p->busy) {
      uint64_t next = p->due_ms;
      if (p->trigger != MC_TRIGGER_NOW && p->auto_after_ms > next) next = p->auto_after_ms;
      if (p->held_ms > next) next = p->held_ms;
      if (p->trigger != MC_TRIGGER_NOW && p->peer_held_ms > next) next = p->peer_held_ms;
      if (next) d->next_auto_ms = wall_now + (next > mono_now ? (int64_t)(next - mono_now) : 0);
    }
    /* What it gave for refusing a session outweighs what it last published. */
    if (p->refused_hold != MC_HOLD_NONE &&
        (!p->refused_until_ms || p->refused_until_ms > wall_now)) {
      d->peer_hold = p->refused_hold;
      d->peer_hold_until_ms = p->refused_until_ms;
    }
    /* Syncing with this device now: whatever it holds back, it isn't us (as in the problems). */
    if (p->busy) {
      d->peer_hold = MC_HOLD_NONE;
      d->peer_hold_until_ms = 0;
    }
    if (p->wrong_count >= MC_SYNC_WRONG_SHOW && p->wrong_addr[0]) {
      d->wrong_address = mc_strdup(p->wrong_addr);
      d->wrong_count = p->wrong_count;
    }
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
    free(list->items[i].wrong_address);
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
    n->reach = verdict(p->out_ok_ms, p->out_fail_ms);
    n->reach_us = verdict(p->in_ok_ms, p->in_fail_ms);
    n->version = p->version;
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

/* --- Reachability --- */

mc_status mc_sync_reach_self(mc_sync *sync, mc_sync_self_reach *out) {
  if (!sync || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  self_reach_locked(sync, out);
  mc_status st = sync->started ? MC_OK : MC_ERR_INVALID_ARG;
  mc_mutex_unlock(&sync->lock);
  return st;
}

mc_status mc_sync_reach_peer(mc_sync *sync, const char *device_uid, mc_sync_reach *to_them,
                             mc_sync_reach *to_us) {
  if (!sync || !device_uid) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  mc_sync_peer *p = find_peer(sync, device_uid, 0);
  if (p && to_them) *to_them = verdict(p->out_ok_ms, p->out_fail_ms);
  if (p && to_us) *to_us = verdict(p->in_ok_ms, p->in_fail_ms);
  mc_mutex_unlock(&sync->lock);
  return p ? MC_OK : MC_ERR_NOT_FOUND;
}

mc_status mc_sync_check_reach(mc_sync *sync, const char *device_uid) {
  if (!sync || (device_uid && !is_uid(device_uid))) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&sync->lock);
  mc_status st = sync->started ? MC_OK : MC_ERR_INVALID_ARG;
  uint64_t now = mc_mono_ms();
  if (st == MC_OK && device_uid) {
    mc_sync_peer *p = find_peer(sync, device_uid, 0);
    if (p)
      p->reach_now = 1;
    else
      st = MC_ERR_NOT_FOUND;
  } else if (st == MC_OK) {
    for (int i = 0; i < sync->npeers; i++) {
      mc_sync_peer *p = &sync->peers[i];
      if (p->reached || reach_alive(p, now)) p->reach_now = 1;
    }
  }
  mc_cond_broadcast(&sync->cond);
  mc_mutex_unlock(&sync->lock);
  return st;
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

void mc_sync_set_test_auto_interval(mc_sync *sync, uint32_t ms) {
  mc_mutex_lock(&sync->lock);
  sync->auto_interval_ms = ms;
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_set_test_hints(mc_sync *sync, const char *json) {
  mc_mutex_lock(&sync->lock);
  snprintf(sync->hint_override, sizeof sync->hint_override, "%s", json ? json : "");
  memcpy(sync->hints, sync->hint_override, sizeof sync->hints);
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_set_test_reach_port(mc_sync *sync, uint16_t port) {
  mc_mutex_lock(&sync->lock);
  sync->reach_port_override = port;
  mc_mutex_unlock(&sync->lock);
}


void mc_sync_set_test_no_files_plan(mc_sync *sync, int no_files_plan) {
  mc_mutex_lock(&sync->lock);
  sync->no_files_plan = no_files_plan;
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_set_test_no_cancel(mc_sync *sync, int no_cancel) {
  mc_mutex_lock(&sync->lock);
  sync->no_cancel = no_cancel;
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_set_test_version(mc_sync *sync, int protocol, int protocol_min, const char *app,
                              int check, unsigned features) {
  mc_mutex_lock(&sync->lock);
  sync->self_version.protocol = protocol;
  sync->self_version.protocol_min = protocol_min;
  snprintf(sync->self_version.app, sizeof sync->self_version.app, "%s", app ? app : "");
  sync->self_version.check = check;
  sync->self_version.features = features;
  mc_mutex_unlock(&sync->lock);
}

unsigned mc_sync_test_last_features(mc_sync *sync) {
  mc_mutex_lock(&sync->lock);
  unsigned f = sync->last_features;
  mc_mutex_unlock(&sync->lock);
  return f;
}

void mc_sync_set_test_peer_cancel_hold(mc_sync *sync, uint32_t ms) {
  mc_mutex_lock(&sync->lock);
  sync->peer_cancel_hold_ms = ms;
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_set_test_send_delay(mc_sync *sync, uint32_t ms) {
  mc_mutex_lock(&sync->lock);
  sync->send_delay_ms = ms;
  mc_mutex_unlock(&sync->lock);
}

void mc_sync_test_peer(mc_sync *sync, const char *device_uid, const char *name, const char *kind,
                       const char *group_hash, const char *address, uint16_t port, int online) {
  peer_seen(sync, device_uid, name, kind, "", group_hash, address, port, NULL, online);
}
