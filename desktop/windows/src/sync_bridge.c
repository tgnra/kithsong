#include "sync_bridge.h"

#include <stdlib.h>
#include <string.h>

enum {
  PROGRESS_MS = 150,   /* at most one progress event per device this often */
  PROGRESS_SLOTS = 16, /* devices throttled at once; more are rare, and just share a slot */
};

static struct {
  HWND wnd;            /* set before the mc_sync exists, so its threads only read it */
  mc_sync *sync;
  mc_status error;
  volatile LONG gen;   /* bumped by every stop; events carry the one they were made in */
} g_bridge;

/* The last progress event posted for each device, and what the ones dropped since changed.
 * Only the callbacks touch it, and core runs them one at a time, so it needs no lock;
 * start() clears it while no threads run. */
static struct {
  char uid[MC_UID_STRLEN];
  ULONGLONG at;
  mc_sync_phase phase;
  int files_posted;      /* files fetched and sent in the last event posted */
  int64_t rows_received; /* in the last event seen, posted or not */
  int files_done;
  BOOL changed; /* the library changed since the last event posted */
} g_progress[PROGRESS_SLOTS];
static int g_progress_next; /* the slot to reuse when a new device needs one */

static void copy_uid(char *dst, const char *src) {
  if (!src) return;
  strncpy(dst, src, MC_UID_STRLEN - 1);
  dst[MC_UID_STRLEN - 1] = '\0';
}

/* UTF-8 to UTF-16, cut to fit name[128]: a name that doesn't fit is converted in shorter
 * byte prefixes until one does, so the cut never leaves half a character. */
static void copy_name(wchar_t *dst, int cap, const char *src) {
  dst[0] = L'\0';
  if (!src || !*src) return;
  int len = (int)strlen(src);
  while (len > 0) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, len, dst, cap - 1);
    if (n > 0) {
      dst[n] = L'\0';
      return;
    }
    len--; /* too long, or cut inside a character */
  }
}

/* UTF-8 into a fixed buffer, cut short (at a character boundary) if it doesn't fit. */
static void copy_utf8(char *dst, size_t cap, const char *src) {
  dst[0] = '\0';
  if (!src) return;
  size_t len = strlen(src);
  if (len >= cap) {
    len = cap - 1;
    while (len > 0 && ((unsigned char)src[len] & 0xC0) == 0x80) len--; /* not mid-character */
  }
  memcpy(dst, src, len);
  dst[len] = '\0';
}

static sync_event *new_event(sync_event_kind kind) {
  sync_event *ev = calloc(1, sizeof *ev);
  if (ev) ev->kind = kind;
  return ev;
}

/* Hands ev to the UI thread, which frees it. Never SendMessage: the UI thread may be waiting
 * in mc_sync_stop() for this very thread. */
static void post(void *user, sync_event *ev) {
  if (!ev) return;
  if (!PostMessageW(g_bridge.wnd, WM_APP_SYNC, (WPARAM)(uintptr_t)user, (LPARAM)ev)) free(ev);
}

static void on_peer(void *user, const char *device_uid, int online, int member) {
  sync_event *ev = new_event(SYNC_EV_PEER);
  if (!ev) return;
  copy_uid(ev->uid, device_uid);
  ev->online = online;
  ev->member = member;
  post(user, ev);
}

static void on_pair_code(void *user, const char *code, const char *requester_name, int attempts_left) {
  sync_event *ev = new_event(SYNC_EV_PAIR_CODE);
  if (!ev) return;
  if (code) {
    strncpy(ev->code, code, MC_PAIR_CODE_LEN);
    ev->code[MC_PAIR_CODE_LEN] = '\0';
  }
  copy_name(ev->name, (int)(sizeof ev->name / sizeof ev->name[0]), requester_name);
  ev->attempts_left = attempts_left;
  post(user, ev);
}

static void on_pair_result(void *user, const mc_pair_result *result) {
  sync_event *ev = new_event(SYNC_EV_PAIR_RESULT);
  if (!ev) return;
  copy_uid(ev->uid, result->device_uid);
  copy_name(ev->name, (int)(sizeof ev->name / sizeof ev->name[0]), result->name);
  ev->initiator = result->initiator;
  ev->reason = result->reason;
  ev->attempts_left = result->attempts_left;
  ev->status = result->status;
  ev->joined = result->joined;
  ev->archives = result->archives;
  copy_name(ev->archive_name, (int)(sizeof ev->archive_name / sizeof ev->archive_name[0]), result->archive_name);
  ev->merged = result->merged;
  ev->kept_here = result->kept_here;
  copy_utf8(ev->scrobble_user, sizeof ev->scrobble_user, result->scrobble_signed_out);
  post(user, ev);
}

/* Lets through a device's first progress event, a change of phase, the one that finishes
 * the session's files, and otherwise one every PROGRESS_MS. *changed says whether library
 * rows or files arrived since the last one let through, this one included. */
static BOOL progress_due(const mc_sync_progress *p, BOOL *changed) {
  const char *uid = p->device_uid ? p->device_uid : "";
  ULONGLONG now = GetTickCount64();
  int slot = -1;
  for (int i = 0; i < PROGRESS_SLOTS && slot < 0; i++)
    if (!strcmp(g_progress[i].uid, uid)) slot = i;
  BOOL first = slot < 0;
  if (first) {
    slot = g_progress_next;
    g_progress_next = (g_progress_next + 1) % PROGRESS_SLOTS;
    memset(&g_progress[slot], 0, sizeof g_progress[slot]);
    copy_uid(g_progress[slot].uid, uid);
  }
  if (p->phase == MC_SYNC_PHASE_CONNECTING) { /* a new session: its counts start again */
    g_progress[slot].rows_received = 0;
    g_progress[slot].files_done = g_progress[slot].files_posted = 0;
  }
  /* Rows are applied a batch per transaction, and a file is recorded once it's in place,
   * so both are in the library by the time their progress arrives. */
  if ((p->phase == MC_SYNC_PHASE_LIBRARY && p->rows_received > g_progress[slot].rows_received) ||
      p->files_done > g_progress[slot].files_done)
    g_progress[slot].changed = TRUE;
  g_progress[slot].rows_received = p->rows_received;
  g_progress[slot].files_done = p->files_done;

  /* Files both ways: each turn's last file goes through. */
  int moved = (p->files_done < p->files_total ? p->files_done : p->files_total) +
              (p->sends_done < p->sends_total ? p->sends_done : p->sends_total);
  int total = p->files_total + p->sends_total;
  BOOL files_finished = total > 0 && moved >= total && moved != g_progress[slot].files_posted;
  if (!first && !files_finished && g_progress[slot].phase == p->phase && now - g_progress[slot].at < PROGRESS_MS)
    return FALSE;
  g_progress[slot].at = now;
  g_progress[slot].phase = p->phase;
  g_progress[slot].files_posted = moved;
  *changed = g_progress[slot].changed;
  g_progress[slot].changed = FALSE;
  return TRUE;
}

static void on_progress(void *user, const mc_sync_progress *progress) {
  BOOL changed = FALSE;
  if (!progress_due(progress, &changed)) return;
  sync_event *ev = new_event(SYNC_EV_PROGRESS);
  if (!ev) return;
  copy_uid(ev->uid, progress->device_uid);
  ev->phase = progress->phase;
  ev->rows_sent = progress->rows_sent;
  ev->rows_received = progress->rows_received;
  ev->files_done = progress->files_done;
  ev->files_total = progress->files_total;
  ev->sends_done = progress->sends_done;
  ev->sends_total = progress->sends_total;
  ev->bytes_received = progress->bytes_received;
  ev->bytes_sent = progress->bytes_sent;
  ev->library_changed = changed;
  post(user, ev);
}

static void on_sync_done(void *user, const mc_sync_result *result) {
  sync_event *ev = new_event(SYNC_EV_DONE);
  if (!ev) return;
  copy_uid(ev->uid, result->device_uid);
  ev->status = result->status;
  ev->library_synced = result->library_synced;
  ev->converged = result->converged;
  ev->rows_sent = result->rows_sent;
  ev->rows_received = result->rows_received;
  ev->files_received = result->files_received;
  ev->files_sent = result->files_sent;
  ev->bytes_received = result->bytes_received;
  ev->bytes_sent = result->bytes_sent;
  ev->cancelled = result->cancelled;
  post(user, ev);
}

static void on_pair_merge(void *user, const mc_pair_merge *merge) {
  sync_event *ev = new_event(SYNC_EV_PAIR_MERGE);
  if (!ev) return;
  copy_uid(ev->uid, merge->device_uid);
  copy_name(ev->name, (int)(sizeof ev->name / sizeof ev->name[0]), merge->name);
  ev->initiator = merge->initiator;
  ev->kept_here = merge->kept_here;
  ev->needs_confirm = merge->needs_confirm;
  ev->tracks_here = merge->tracks_here;
  ev->tracks_there = merge->tracks_there;
  ev->scrobble_sign_out = merge->scrobble_sign_out;
  copy_utf8(ev->scrobble_user, sizeof ev->scrobble_user, merge->scrobble_user);
  ev->forget_count = merge->forget_count;
  /* The names as one list; a name that doesn't fit whole is left out. */
  const char *sep = mc_str(MC_STR_IMPORT_LIST_SEPARATOR);
  size_t used = 0;
  for (int i = 0; i < merge->forget_count && merge->forget_names; i++) {
    const char *n = merge->forget_names[i] ? merge->forget_names[i] : "";
    size_t need = (used ? strlen(sep) : 0) + strlen(n);
    if (used + need >= sizeof ev->forget_names) break;
    if (used) {
      memcpy(ev->forget_names + used, sep, strlen(sep));
      used += strlen(sep);
    }
    memcpy(ev->forget_names + used, n, strlen(n));
    used += strlen(n);
    ev->forget_names[used] = '\0';
  }
  post(user, ev);
}

/* Merge progress, at most one every PROGRESS_MS, but always a change of phase and the last of
 * a phase. Only this callback touches these. */
static ULONGLONG g_merge_at;
static int g_merge_phase = -1;

static void on_merge_progress(void *user, const mc_merge_progress *progress) {
  ULONGLONG now = GetTickCount64();
  BOOL last = progress->done >= progress->total;
  if ((int)progress->phase == g_merge_phase && !last && now - g_merge_at < PROGRESS_MS) return;
  g_merge_at = now;
  g_merge_phase = (int)progress->phase;
  sync_event *ev = new_event(SYNC_EV_MERGE_PROGRESS);
  if (!ev) return;
  ev->merge_phase = progress->phase;
  ev->merge_done = progress->done;
  ev->merge_total = progress->total;
  post(user, ev);
}

static void on_merge_done(void *user, const mc_merge_result *result) {
  g_merge_phase = -1;
  sync_event *ev = new_event(SYNC_EV_MERGE_DONE);
  if (!ev) return;
  ev->status = result->status;
  ev->merge = *result;
  ev->merge.backup_path = NULL;
  ev->merge.scrobble_signed_out = NULL;
  copy_utf8(ev->backup_path, sizeof ev->backup_path, result->backup_path);
  copy_utf8(ev->scrobble_user, sizeof ev->scrobble_user, result->scrobble_signed_out);
  post(user, ev);
}

void sync_bridge_start(HWND wnd, mc_library *lib, const char *config_dir, const char *device_name) {
  sync_bridge_stop();
  g_bridge.wnd = wnd;
  memset(g_progress, 0, sizeof g_progress);
  g_progress_next = 0;
  g_merge_phase = -1;
  if (!lib || !config_dir) {
    g_bridge.error = MC_ERR_INVALID_ARG; /* no library open */
    return;
  }

  static const mc_sync_events events = {on_peer, on_pair_code, on_pair_result, on_progress, on_sync_done,
                                        NULL,          on_pair_merge,  on_merge_progress, on_merge_done};
  mc_sync *sync = NULL;
  void *gen = (void *)(uintptr_t)(ULONG)g_bridge.gen;
  mc_status st = mc_sync_new(lib, config_dir, device_name, MC_DEVICE_DESKTOP, &events, gen, &sync);
  if (st == MC_OK) st = mc_sync_start(sync);
  if (st != MC_OK) {
    mc_sync_free(sync);
    sync = NULL;
    InterlockedIncrement(&g_bridge.gen); /* drop anything it posted before failing */
  }
  g_bridge.sync = sync;
  g_bridge.error = st;
}

void sync_bridge_stop(void) {
  if (!g_bridge.sync) return;
  /* Bump first: whatever the threads post while stopping is already stale. */
  InterlockedIncrement(&g_bridge.gen);
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

sync_event *sync_bridge_receive(WPARAM wp, LPARAM lp) {
  sync_event *ev = (sync_event *)lp;
  if (!ev) return NULL;
  if (!g_bridge.sync || (ULONG)wp != (ULONG)g_bridge.gen) {
    free(ev);
    return NULL;
  }
  return ev;
}
