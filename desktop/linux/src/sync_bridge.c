#include "sync_bridge.h"

#include <string.h>

enum {
  PROGRESS_MS = 150,   /* at most one progress event per device this often */
  PROGRESS_SLOTS = 16, /* devices throttled at once; more are rare, and just share a slot */
};

static struct {
  mc_sync *sync;
  mc_status error;
  sync_bridge_event_fn on_event; /* set before the mc_sync exists, so its threads only read it */
  void *user;
  gint gen; /* bumped by every stop; events carry the one they were made in */
} g_bridge;

/* The last progress event delivered for each device, and what the ones dropped since
 * changed. Only the callbacks touch it, and core runs them one at a time, so it needs no
 * lock; start() clears it while no threads run. */
static struct {
  char uid[MC_UID_STRLEN];
  gint64 at; /* monotonic, microseconds */
  mc_sync_phase phase;
  int files_posted;      /* files fetched and sent in the last event delivered */
  int64_t rows_received; /* in the last event seen, delivered or not */
  int files_done;
  gboolean changed; /* the library changed since the last event delivered */
} g_progress[PROGRESS_SLOTS];
static int g_progress_next; /* the slot to reuse when a new device needs one */

typedef struct posted {
  gint gen;
  sync_event ev;
} posted;

static void copy_str(char *dst, size_t cap, const char *src) {
  dst[0] = '\0';
  if (!src) return;
  g_strlcpy(dst, src, cap);
  /* Cut at a character boundary: drop a trailing partial UTF-8 sequence. */
  const char *end = NULL;
  if (!g_utf8_validate(dst, -1, &end)) dst[end - dst] = '\0';
}

static posted *new_event(void *user, sync_event_kind kind) {
  posted *p = g_new0(posted, 1);
  p->gen = GPOINTER_TO_INT(user);
  p->ev.kind = kind;
  return p;
}

static gboolean deliver(gpointer data) {
  posted *p = data;
  if (g_bridge.sync && p->gen == g_atomic_int_get(&g_bridge.gen) && g_bridge.on_event)
    g_bridge.on_event(g_bridge.user, &p->ev);
  return G_SOURCE_REMOVE;
}

/* Hands p to the main thread, which frees it. Never waits: the main thread may be waiting
 * in mc_sync_free() for this very thread. */
static void post(posted *p) {
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, deliver, p, g_free);
}

static void on_peer(void *user, const char *device_uid, int online, int member) {
  posted *p = new_event(user, SYNC_EV_PEER);
  copy_str(p->ev.uid, sizeof p->ev.uid, device_uid);
  p->ev.online = online;
  p->ev.member = member;
  post(p);
}

static void on_pair_code(void *user, const char *code, const char *requester_name, int attempts_left) {
  posted *p = new_event(user, SYNC_EV_PAIR_CODE);
  copy_str(p->ev.code, sizeof p->ev.code, code);
  copy_str(p->ev.name, sizeof p->ev.name, requester_name);
  p->ev.attempts_left = attempts_left;
  post(p);
}

static void on_pair_result(void *user, const mc_pair_result *result) {
  posted *p = new_event(user, SYNC_EV_PAIR_RESULT);
  copy_str(p->ev.uid, sizeof p->ev.uid, result->device_uid);
  copy_str(p->ev.name, sizeof p->ev.name, result->name);
  p->ev.initiator = result->initiator;
  p->ev.reason = result->reason;
  p->ev.attempts_left = result->attempts_left;
  p->ev.status = result->status;
  p->ev.joined = result->joined;
  p->ev.archives = result->archives;
  copy_str(p->ev.archive_name, sizeof p->ev.archive_name, result->archive_name);
  p->ev.merged = result->merged;
  p->ev.kept_here = result->kept_here;
  copy_str(p->ev.scrobble_user, sizeof p->ev.scrobble_user, result->scrobble_signed_out);
  post(p);
}

/* Lets through a device's first progress event, a change of phase, the one that finishes
 * the session's files, and otherwise one every PROGRESS_MS. *changed says whether library
 * rows or files arrived since the last one let through, this one included. */
static gboolean progress_due(const mc_sync_progress *pr, gboolean *changed) {
  const char *uid = pr->device_uid ? pr->device_uid : "";
  gint64 now = g_get_monotonic_time();
  int slot = -1;
  for (int i = 0; i < PROGRESS_SLOTS && slot < 0; i++)
    if (!strcmp(g_progress[i].uid, uid)) slot = i;
  gboolean first = slot < 0;
  if (first) {
    slot = g_progress_next;
    g_progress_next = (g_progress_next + 1) % PROGRESS_SLOTS;
    memset(&g_progress[slot], 0, sizeof g_progress[slot]);
    g_strlcpy(g_progress[slot].uid, uid, sizeof g_progress[slot].uid);
  }
  if (pr->phase == MC_SYNC_PHASE_CONNECTING) { /* a new session: its counts start again */
    g_progress[slot].rows_received = 0;
    g_progress[slot].files_done = g_progress[slot].files_posted = 0;
  }
  /* Rows are applied a batch per transaction, and a file is recorded once it's in place,
   * so both are in the library by the time their progress arrives. */
  if ((pr->phase == MC_SYNC_PHASE_LIBRARY && pr->rows_received > g_progress[slot].rows_received) ||
      pr->files_done > g_progress[slot].files_done)
    g_progress[slot].changed = TRUE;
  g_progress[slot].rows_received = pr->rows_received;
  g_progress[slot].files_done = pr->files_done;

  /* Files both ways: each turn's last file goes through. */
  int moved = MIN(pr->files_done, pr->files_total) + MIN(pr->sends_done, pr->sends_total);
  int total = pr->files_total + pr->sends_total;
  gboolean files_finished = total > 0 && moved >= total && moved != g_progress[slot].files_posted;
  if (!first && !files_finished && g_progress[slot].phase == pr->phase &&
      now - g_progress[slot].at < (gint64)PROGRESS_MS * 1000)
    return FALSE;
  g_progress[slot].at = now;
  g_progress[slot].phase = pr->phase;
  g_progress[slot].files_posted = moved;
  *changed = g_progress[slot].changed;
  g_progress[slot].changed = FALSE;
  return TRUE;
}

static void on_progress(void *user, const mc_sync_progress *progress) {
  gboolean changed = FALSE;
  if (!progress_due(progress, &changed)) return;
  posted *p = new_event(user, SYNC_EV_PROGRESS);
  copy_str(p->ev.uid, sizeof p->ev.uid, progress->device_uid);
  p->ev.phase = progress->phase;
  p->ev.rows_sent = progress->rows_sent;
  p->ev.rows_received = progress->rows_received;
  p->ev.files_done = progress->files_done;
  p->ev.files_total = progress->files_total;
  p->ev.sends_done = progress->sends_done;
  p->ev.sends_total = progress->sends_total;
  p->ev.bytes_received = progress->bytes_received;
  p->ev.bytes_sent = progress->bytes_sent;
  p->ev.library_changed = changed;
  post(p);
}

static void on_sync_done(void *user, const mc_sync_result *result) {
  posted *p = new_event(user, SYNC_EV_DONE);
  copy_str(p->ev.uid, sizeof p->ev.uid, result->device_uid);
  p->ev.status = result->status;
  p->ev.library_synced = result->library_synced;
  p->ev.converged = result->converged;
  p->ev.rows_sent = result->rows_sent;
  p->ev.rows_received = result->rows_received;
  p->ev.files_received = result->files_received;
  p->ev.files_sent = result->files_sent;
  p->ev.bytes_received = result->bytes_received;
  p->ev.bytes_sent = result->bytes_sent;
  p->ev.cancelled = result->cancelled;
  post(p);
}

static void on_reach(void *user, const char *device_uid) {
  posted *p = new_event(user, SYNC_EV_REACH);
  copy_str(p->ev.uid, sizeof p->ev.uid, device_uid);
  post(p);
}

static void on_pair_merge(void *user, const mc_pair_merge *merge) {
  posted *p = new_event(user, SYNC_EV_PAIR_MERGE);
  copy_str(p->ev.uid, sizeof p->ev.uid, merge->device_uid);
  copy_str(p->ev.name, sizeof p->ev.name, merge->name);
  p->ev.initiator = merge->initiator;
  p->ev.kept_here = merge->kept_here;
  p->ev.needs_confirm = merge->needs_confirm;
  p->ev.tracks_here = merge->tracks_here;
  p->ev.tracks_there = merge->tracks_there;
  p->ev.scrobble_sign_out = merge->scrobble_sign_out;
  copy_str(p->ev.scrobble_user, sizeof p->ev.scrobble_user, merge->scrobble_user);
  p->ev.forget_count = merge->forget_count;
  /* The names as one list; a name that doesn't fit whole is left out. */
  GString *names = g_string_new(NULL);
  for (int i = 0; i < merge->forget_count && merge->forget_names; i++) {
    const char *n = merge->forget_names[i] ? merge->forget_names[i] : "";
    const char *sep = names->len ? mc_str(MC_STR_IMPORT_LIST_SEPARATOR) : "";
    if (names->len + strlen(sep) + strlen(n) >= sizeof p->ev.forget_names) break;
    g_string_append(names, sep);
    g_string_append(names, n);
  }
  copy_str(p->ev.forget_names, sizeof p->ev.forget_names, names->str);
  g_string_free(names, TRUE);
  post(p);
}

/* Merge progress, at most one every PROGRESS_MS, but always a change of phase and the last of
 * a phase. Only the merge callbacks touch these, one at a time. */
static gint64 g_merge_at;
static int g_merge_phase = -1;

static void on_merge_progress(void *user, const mc_merge_progress *progress) {
  gint64 now = g_get_monotonic_time();
  gboolean last = progress->done >= progress->total;
  if ((int)progress->phase == g_merge_phase && !last && now - g_merge_at < (gint64)PROGRESS_MS * 1000) return;
  g_merge_at = now;
  g_merge_phase = (int)progress->phase;
  posted *p = new_event(user, SYNC_EV_MERGE_PROGRESS);
  p->ev.merge_phase = progress->phase;
  p->ev.merge_done = progress->done;
  p->ev.merge_total = progress->total;
  post(p);
}

static void on_merge_done(void *user, const mc_merge_result *result) {
  g_merge_phase = -1;
  posted *p = new_event(user, SYNC_EV_MERGE_DONE);
  p->ev.status = result->status;
  p->ev.merge = *result;
  p->ev.merge.backup_path = NULL;
  p->ev.merge.scrobble_signed_out = NULL;
  copy_str(p->ev.backup_path, sizeof p->ev.backup_path, result->backup_path);
  copy_str(p->ev.scrobble_user, sizeof p->ev.scrobble_user, result->scrobble_signed_out);
  post(p);
}

void sync_bridge_start(mc_library *lib, const char *config_dir, const char *device_name,
                       sync_bridge_event_fn on_event, void *user) {
  sync_bridge_stop();
  g_bridge.on_event = on_event;
  g_bridge.user = user;
  memset(g_progress, 0, sizeof g_progress);
  g_progress_next = 0;
  g_merge_phase = -1;
  if (!lib || !config_dir) {
    g_bridge.error = MC_ERR_INVALID_ARG; /* no library open */
    return;
  }

  static const mc_sync_events events = {on_peer, on_pair_code, on_pair_result, on_progress, on_sync_done,
                                        on_reach, on_pair_merge, on_merge_progress, on_merge_done};
  mc_sync *sync = NULL;
  void *gen = GINT_TO_POINTER(g_atomic_int_get(&g_bridge.gen));
  mc_status st = mc_sync_new(lib, config_dir, device_name, MC_DEVICE_DESKTOP, &events, gen, &sync);
  if (st == MC_OK) st = mc_sync_start(sync);
  if (st != MC_OK) {
    mc_sync_free(sync);
    sync = NULL;
    g_atomic_int_inc(&g_bridge.gen); /* drop anything it posted before failing */
  }
  g_bridge.sync = sync;
  g_bridge.error = st;
}

void sync_bridge_stop(void) {
  if (!g_bridge.sync) return;
  /* Bump first: whatever the threads post while stopping is already stale. */
  g_atomic_int_inc(&g_bridge.gen);
  mc_sync_free(g_bridge.sync);
  g_bridge.sync = NULL;
  g_bridge.error = MC_OK;
}

mc_sync *sync_bridge_get(void) {
  return g_bridge.sync;
}

mc_status sync_bridge_error(void) {
  return g_bridge.error;
}
