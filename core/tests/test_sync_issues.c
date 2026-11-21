/* Sync issues ("Issues" in docs/sync-protocol.md): what keeps sync from doing what it should,
 * each with what happened and how to fix it. Real devices over loopback, discovery off: A the
 * archive that imported everything, B on originals, and C, a stranger that answers at an
 * address of B's. Covers a drive too full or a folder that can't be written (nothing fetched),
 * a refused file named, unknown free space, low space, the holds a device publishes and gives
 * when it refuses a session, the device rows' notes and warnings, the last complete sync,
 * another address tried after a failed handshake, a network change, and the notices. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "kithcore/i18n.h"
#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 20000
#define MAX_EVENTS 64

#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)

/* --- Files --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A short 16-bit mono PCM WAV of a tone; pitch makes each file different. */
static int write_wav(const char *path, double pitch) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t frames = 22050, bytes = frames * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, 44100);
  put32(f, 88200);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++) put16(f, (unsigned)(uint16_t)(int16_t)((i * (int)pitch) % 20000));
  return fclose(f) == 0;
}

/* --- A device --- */

typedef struct done_event {
  char peer[MC_UID_STRLEN];
  mc_sync_result r;
} done_event;

typedef struct dev {
  const char *label;
  char config[1024], libdir[1024];
  mc_library *lib;
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes, results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
} dev;

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)name, (void)attempts;
  mc_mutex_lock(&d->m);
  snprintf(d->code, sizeof d->code, "%s", code);
  d->codes++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_pair_result(void *user, const mc_pair_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->reason = r->reason;
  d->results++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_sync_done(void *user, const mc_sync_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  if (d->ndone < MAX_EVENTS) {
    done_event *e = &d->done[d->ndone++];
    snprintf(e->peer, sizeof e->peer, "%s", r->device_uid);
    e->r = *r;
    e->r.device_uid = NULL;
  }
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done,
                                        NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static int dev_start(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) == MC_OK);
  if (!d->sync) return 0;
  snprintf(d->uid, sizeof d->uid, "%s", mc_sync_device_uid(d->sync));
  mc_sync_set_test_options(d->sync, 0, 0);
  mc_sync_set_test_manual(d->sync, 1);
  CHECK(mc_sync_start(d->sync) == MC_OK);
  char hints[64];
  snprintf(hints, sizeof hints, "[\"127.0.0.1:%u\"]", (unsigned)mc_sync_port(d->sync));
  mc_sync_set_test_hints(d->sync, hints);
  return mc_sync_port(d->sync) != 0;
}

static void dev_stop(dev *d) {
  if (!d->lib) return;
  mc_sync_free(d->sync);
  mc_library_close(d->lib);
  d->lib = NULL;
  mc_cond_destroy(&d->c);
  mc_mutex_destroy(&d->m);
}

static int count_done(dev *d, const char *peer) {
  int n = 0;
  for (int i = 0; i < d->ndone; i++)
    if (!strcmp(d->done[i].peer, peer)) n++;
  return n;
}

static int done_seen(dev *d, const char *peer) {
  mc_mutex_lock(&d->m);
  int n = count_done(d, peer);
  mc_mutex_unlock(&d->m);
  return n;
}

/* Waits for a new session report on d about peer: its status, and the result in r. */
static mc_status wait_done(dev *d, const char *peer, int seen, mc_sync_result *r, int wait_ms) {
  uint64_t end = mc_mono_ms() + (uint64_t)wait_ms;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 50);
  mc_status st = MC_ERR_UNIMPLEMENTED;
  for (int i = d->ndone - 1; i >= 0 && count_done(d, peer) > seen; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      st = d->done[i].r.status;
      if (r) *r = d->done[i].r;
      break;
    }
  mc_mutex_unlock(&d->m);
  return st;
}

/* x's user presses Sync for y. */
static mc_status sync_now(dev *x, dev *y, mc_sync_result *rx) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx, rx, WAIT_MS);
  if (st == MC_OK) wait_done(y, x->uid, sy, NULL, WAIT_MS);
  return st;
}

static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  char code[MC_PAIR_CODE_LEN + 1] = "";
  mc_mutex_lock(&y->m);
  while (y->codes == codes && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 50);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_mutex_lock(&x->m);
  while (x->results == rx && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 50);
  mc_pair_reason r = x->results > rx ? x->reason : (mc_pair_reason)-1;
  mc_mutex_unlock(&x->m);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx, NULL, WAIT_MS) == MC_OK);
    CHECK(wait_done(y, x->uid, sy, NULL, WAIT_MS) == MC_OK);
  }
  return r;
}

/* --- Looking inside --- */

static int exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc;
}

static void query_text(sqlite3 *db, const char *sql, char *out, size_t cap) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, cap, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
}

/* sql on d's sync.db through its API connection, unlogged, as if a peer had sent it. */
static void group_sql(dev *d, const char *sql) {
  mc_mutex_lock(&d->sync->api_lock);
  mc_sync_db *sdb = d->sync->api.sdb;
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  sdb->applying_remote = 1;
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  sdb->applying_remote = 0;
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  mc_mutex_unlock(&d->sync->api_lock);
}

static void group_text(dev *d, const char *sql, char *out, size_t cap) {
  mc_mutex_lock(&d->sync->api_lock);
  query_text(mc_sync_db_handle(d->sync->api.sdb), sql, out, cap);
  mc_mutex_unlock(&d->sync->api_lock);
}

typedef struct track_ref {
  char uid[MC_UID_STRLEN];
  char rel[256];
} track_ref;

/* A track with a WAV original at its organised path, imported on d. */
static void add_track(dev *d, const char *album, const char *title, int no, track_ref *out) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[1024], file[MC_UID_STRLEN];
  query_text(db, "SELECT uid FROM artists WHERE name = 'Low'", ar, sizeof ar);
  snprintf(sql, sizeof sql, "SELECT uid FROM albums WHERE title = '%s'", album);
  query_text(db, sql, al, sizeof al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  if (!al[0]) {
    mc_uid_new(al);
    snprintf(sql, sizeof sql,
             "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', '%s', '%s', 1994)",
             al, album, ar);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  mc_uid_new(out->uid);
  mc_uid_new(file);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, 500)",
           out->uid, al, ar, title, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  char dir[2048], p[2400];
  snprintf(out->rel, sizeof out->rel, "Low/%s (1994)/%02d %s.wav", album, no, title);
  snprintf(dir, sizeof dir, "%s/Low/%s (1994)", d->libdir, album);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(p, sizeof p, "%s/%s", d->libdir, out->rel);
  CHECK(write_wav(p, 100 + 37 * no));
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', 'wav', 705, %lld, %lld)",
           file, out->uid, out->rel, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* B keeps everything as originals, chosen by its user. */
static void choose_originals(dev *d) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(d->sync, d->uid, &p) == MC_OK);
  if (!p) return;
  p->format.format = MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = 0;
  p->sync_all = 1;
  p->budget_enabled = 0;
  CHECK(mc_sync_set_profile(d->sync, d->uid, p) == MC_OK);
  mc_sync_profile_free(p);
  mc_mutex_lock(&d->sync->api_lock);
  CHECK(mc_profile_choose(d->sync->api.sdb, NULL) == MC_OK);
  mc_mutex_unlock(&d->sync->api_lock);
}

/* The issue of kind about uid (NULL: any device) in p, NULL if none. */
static const mc_sync_issue *find_issue(const mc_sync_problems *p, mc_sync_issue_kind kind,
                                       const char *uid) {
  for (int i = 0; p && i < p->issue_count; i++)
    if (p->issues[i].kind == kind && (!uid || !strcmp(p->issues[i].device_uid, uid)))
      return &p->issues[i];
  return NULL;
}

/* Waits until d's issues have (want 1) or lack (0) one of kind about uid; then *out (may be
 * NULL) is a copy of the problems, to free. */
static int wait_issue(dev *d, mc_sync_issue_kind kind, const char *uid, int want,
                      mc_sync_problems **out) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  for (;;) {
    mc_sync_problems *p = NULL;
    CHECK(mc_sync_get_problems(d->sync, &p) == MC_OK);
    int has = find_issue(p, kind, uid) != NULL;
    if (has == want || mc_mono_ms() >= end) {
      if (has != want) {
        fprintf(stderr, "%s: issue %d %s, wanted %s; has:", d->label, kind,
                has ? "there" : "missing", want ? "there" : "gone");
        for (int i = 0; p && i < p->issue_count; i++) fprintf(stderr, " %d", p->issues[i].kind);
        fprintf(stderr, "\n");
      }
      if (out)
        *out = p;
      else
        mc_sync_problems_free(p);
      return has == want;
    }
    mc_sync_problems_free(p);
    mc_sync_waiting_changed(d->sync);
    mc_sleep_ms(50);
  }
}

/* d's row for uid in its device list, copied into out; 0 if not listed. Free out->name and
 * the rest with free_row(). */
static int row_of(dev *d, const char *uid, mc_sync_device_list **list, const mc_sync_device **out) {
  *out = NULL;
  CHECK(mc_sync_list_devices(d->sync, list) == MC_OK);
  for (int i = 0; *list && i < (*list)->count; i++)
    if (!strcmp((*list)->items[i].uid, uid)) *out = &(*list)->items[i];
  return *out != NULL;
}

static void clock_text(int64_t ms, char *buf, size_t size) {
  time_t t = (time_t)(ms / 1000);
  struct tm *tm = localtime(&t);
  if (!tm || !strftime(buf, size, "%H:%M", tm)) snprintf(buf, size, "?");
}

/* --- Free space, as a test sets it --- */

static char g_small_dir[1024];
static int64_t g_small_free;

static mc_status test_free(const char *dir, int64_t *out) {
  if (g_small_dir[0] && !strcmp(dir, g_small_dir)) {
    if (g_small_free < 0) return MC_ERR_IO;
    *out = g_small_free;
    return MC_OK;
  }
  return mc_free_space(dir, out);
}

/* --- Tests --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mc_strings_set_locale("en");
  mc_sync_test_free_space = test_free;
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_issues.d", scratch);
  mc_remove_tree(root);

  static dev a, b, c;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  track_ref t[5];
  add_track(&a, "Things", "One", 1, &t[0]);
  add_track(&a, "Things", "Two", 2, &t[1]);
  add_track(&a, "Things", "Three", 3, &t[2]);
  add_track(&a, "Things", "Four", 4, &t[3]);
  add_track(&a, "Other", "Five", 1, &t[4]);
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);

  /* B joined: its storage limit was set from its free space, which it's told of once. */
  {
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_NOTICE_BUDGET, b.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_NOTICE_BUDGET, b.uid);
    CHECK(is && is->notice && is->bytes > 0);
    char text[512];
    mc_sync_problems_text(p, text, sizeof text);
    CHECK_TEXT(text, "A storage limit was set for this device");
    mc_sync_problems_free(p);
    mc_sync_dismiss_notices(b.sync);
    CHECK(wait_issue(&b, MC_ISSUE_NOTICE_BUDGET, b.uid, 0, NULL));
  }

  /* B hasn't chosen what it keeps, and says so in its capabilities: A shows it. */
  {
    char state[256];
    group_text(&b, "SELECT state FROM capabilities WHERE device_uid = (SELECT device_uid FROM local)",
               state, sizeof state);
    CHECK(strstr(state, "\"not_chosen\"") != NULL);
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&a, MC_ISSUE_HELD, b.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_HELD, b.uid);
    CHECK(is && is->hold == MC_HOLD_NOT_CHOSEN);
    mc_sync_issue_text it;
    mc_sync_issue_describe(is, 0, &it);
    CHECK_TEXT(it.title, "b hasn't chosen what to keep");
    CHECK_TEXT(it.fix, "Open Kithsong on b, choose what to sync and press Sync.");
    mc_sync_problems_free(p);
    mc_sync_device_list *list = NULL;
    const mc_sync_device *row = NULL;
    CHECK(row_of(&a, b.uid, &list, &row));
    if (row) {
      CHECK(row->peer_hold == MC_HOLD_NOT_CHOSEN);
      char warn[512];
      mc_sync_device_warning(a.sync, row, 0, warn, sizeof warn);
      CHECK_TEXT(warn, "b hasn't chosen what to keep Open Kithsong on b, choose what to sync and press Sync.");
    }
    mc_sync_device_list_free(list);
  }

  /* B chooses, with too little room: it fetches nothing and says why. Its free space is low
   * for converting too. */
  choose_originals(&b);
  snprintf(g_small_dir, sizeof g_small_dir, "%s", b.libdir);
  g_small_free = 10 << 20;
  {
    mc_sync_result r;
    CHECK(sync_now(&b, &a, &r) == MC_OK);
    CHECK(r.files_received == 0 && r.files_missing == 5);
    char text[256];
    mc_sync_finished_text(&r, text, sizeof text);
    CHECK_TEXT(text, "Finished, but 5 tracks are still missing");
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_DISK_FULL, b.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_DISK_FULL, b.uid);
    CHECK(is && is->bytes > 0 && is->bytes_free == (10 << 20) && is->detail);
    CHECK(find_issue(p, MC_ISSUE_LOW_SPACE, b.uid) != NULL);
    mc_sync_problems_free(p);
    mc_sync_result none = {0};
    mc_sync_finished_text(&none, text, sizeof text);
    CHECK_TEXT(text, "");
    /* A hears of B's space with the next session, and B no longer holds back. */
    CHECK(sync_now(&b, &a, NULL) == MC_OK);
    CHECK(wait_issue(&a, MC_ISSUE_LOW_SPACE, b.uid, 1, NULL));
    CHECK(wait_issue(&a, MC_ISSUE_HELD, b.uid, 0, NULL));
    mc_sync_device_list *list = NULL;
    const mc_sync_device *row = NULL;
    CHECK(row_of(&a, b.uid, &list, &row));
    if (row) {
      CHECK(row->low_space && row->peer_hold == MC_HOLD_NONE);
      char warn[512];
      mc_sync_device_warning(a.sync, row, 0, warn, sizeof warn);
      CHECK_TEXT(warn, "Not transcoding for other devices: under 256 MB free");
    }
    mc_sync_device_list_free(list);
  }

#ifndef _WIN32
  /* A library folder B can't write to: nothing fetched, and why. */
  if (geteuid() != 0) {
    g_small_dir[0] = '\0';
    CHECK(chmod(b.libdir, 0555) == 0);
    mc_sync_result r;
    CHECK(sync_now(&b, &a, &r) == MC_OK);
    CHECK(r.files_received == 0);
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_FOLDER_UNWRITABLE, b.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_FOLDER_UNWRITABLE, b.uid);
    CHECK(is && is->error && is->detail);
    CHECK(find_issue(p, MC_ISSUE_DISK_FULL, NULL) == NULL);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "Can't save music in the library folder");
      CHECK(strstr(it.body, b.libdir) != NULL);
    }
    mc_sync_problems_free(p);
    CHECK(chmod(b.libdir, 0755) == 0);
  }
#endif

#ifndef _WIN32
  /* A file A can't read: A refuses it, and B names it. The rest arrives. */
  if (geteuid() != 0) {
    g_small_dir[0] = '\0';
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", a.libdir, t[4].rel);
    CHECK(chmod(path, 0) == 0);
    mc_sync_result r;
    CHECK(sync_now(&b, &a, &r) == MC_OK);
    CHECK(r.files_received == 4 && r.files_missing == 1);
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_REFUSED, a.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_REFUSED, a.uid);
    CHECK(is && is->refusal == MC_REFUSED_IO && is->tracks == 1 && is->nexamples == 1);
    CHECK(find_issue(p, MC_ISSUE_FOLDER_UNWRITABLE, NULL) == NULL);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "a couldn't read 1 track");
      CHECK_TEXT(it.body, "a couldn't read or transcode the files of “Five”.");
    }
    char text[512];
    mc_sync_problems_text(p, text, sizeof text);
    CHECK(strstr(text, "a couldn't read 1 track") == text);
    mc_sync_problems_free(p);
    CHECK(chmod(path, 0644) == 0);
  }
#endif

  /* Free space B can't measure is left out, not taken for none. */
  {
    snprintf(g_small_dir, sizeof g_small_dir, "%s", b.libdir);
    g_small_free = -1;
    CHECK(sync_now(&b, &a, NULL) == MC_OK);
    char v[64];
    group_text(&b, "SELECT coalesce(free_bytes, 'null') FROM capabilities"
                   " WHERE device_uid = (SELECT device_uid FROM local)", v, sizeof v);
    CHECK_TEXT(v, "null");
    CHECK(wait_issue(&b, MC_ISSUE_LOW_SPACE, b.uid, 0, NULL));
  }

  /* The last sync is the last complete one; an incomplete one only says where and when the
   * device was seen. */
  {
    char before[64], after[64], seen[64];
    group_text(&a, "SELECT last_sync_ms FROM peer_contact", before, sizeof before);
    mc_sync_result r = {.device_uid = b.uid, .status = MC_OK, .converged = 0};
    mc_sleep_ms(5);
    mc_mutex_lock(&a.sync->api_lock);
    mc_sync_session_ended(a.sync, &a.sync->api, b.uid, &r, 0, 0, NULL, 0);
    mc_mutex_unlock(&a.sync->api_lock);
    group_text(&a, "SELECT last_sync_ms FROM peer_contact", after, sizeof after);
    group_text(&a, "SELECT seen_ms > last_sync_ms FROM peer_contact", seen, sizeof seen);
    CHECK(before[0] && !strcmp(before, after));
    CHECK_TEXT(seen, "1");
  }

  /* B's user cancels: B refuses A's sessions as cancelled, until when it says, and publishes
   * it; A shows it. B's own row says the same of A. */
  {
    CHECK(mc_sync_cancel(b.sync, a.uid) == MC_OK);
    int sa = done_seen(&a, b.uid);
    CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
    CHECK(wait_done(&a, b.uid, sa, NULL, WAIT_MS) == MC_ERR_LOCKED);
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&a, MC_ISSUE_HELD, b.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_HELD, b.uid);
    int64_t now = mc_wall_ms();
    CHECK(is && is->hold == MC_HOLD_CANCELLED);
    CHECK(is && is->until_ms > now + MC_SYNC_CANCEL_HOLD_MS - 60000 &&
          is->until_ms <= now + MC_SYNC_CANCEL_HOLD_MS);
    if (is) {
      char at[32], want[256];
      clock_text(is->until_ms, at, sizeof at);
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "Sync was cancelled on b");
      snprintf(want, sizeof want, "b holds sync back until %s after a Cancel.", at);
      CHECK_TEXT(it.body, want);
      CHECK_TEXT(it.fix, "Open Kithsong on b and press Sync.");
    }
    mc_sync_problems_free(p);
    mc_sync_device_list *list = NULL;
    const mc_sync_device *row = NULL;
    CHECK(row_of(&a, b.uid, &list, &row));
    CHECK(row && row->peer_hold == MC_HOLD_CANCELLED && row->peer_hold_until_ms > now);
    mc_sync_device_list_free(list);
    CHECK(row_of(&b, a.uid, &list, &row));
    if (row) {
      CHECK(row->hold == MC_HOLD_CANCELLED && row->next_auto_ms > now);
      char note[256], at[32], want[256];
      mc_sync_device_note(row, 0, 0, note, sizeof note);
      clock_text(row->next_auto_ms, at, sizeof at);
      snprintf(want, sizeof want, "Held after Cancel until %s. Press Sync to sync now.", at);
      CHECK_TEXT(note, want);
    }
    mc_sync_device_list_free(list);
    /* Published too, after B's next look. */
    uint64_t end = mc_mono_ms() + WAIT_MS;
    char state[256] = "";
    while (!strstr(state, "\"cancelled\"") && mc_mono_ms() < end) {
      mc_sleep_ms(100);
      group_text(&b, "SELECT state FROM capabilities WHERE device_uid = (SELECT device_uid FROM local)",
                 state, sizeof state);
    }
    CHECK(strstr(state, "\"cancelled\"") != NULL);
    CHECK(row_of(&b, b.uid, &list, &row));
    CHECK(row && row->peer_hold == MC_HOLD_CANCELLED);
    mc_sync_device_list_free(list);
    /* A press of Sync on B lifts it, and A forgets the refusal once a session gets through. */
    CHECK(sync_now(&b, &a, NULL) == MC_OK);
    CHECK(wait_issue(&a, MC_ISSUE_HELD, b.uid, 0, NULL));
  }

  /* Synced lately: the row says when the next automatic sync is. */
  {
    mc_sync_device_list *list = NULL;
    const mc_sync_device *row = NULL;
    CHECK(sync_now(&a, &b, NULL) == MC_OK);
    uint64_t end = mc_mono_ms() + WAIT_MS;
    int rate = 0;
    while (!rate && mc_mono_ms() < end) {
      mc_sync_device_list_free(list);
      list = NULL;
      if (row_of(&a, b.uid, &list, &row)) rate = row->hold == MC_HOLD_RATE;
      if (!rate) mc_sleep_ms(100);
    }
    CHECK(rate);
    if (rate) {
      CHECK(row->next_auto_ms > (int64_t)mc_wall_ms() + (int64_t)MC_SYNC_AUTO_INTERVAL_MS - 120000);
      char note[256], at[32], want[256];
      clock_text(row->next_auto_ms, at, sizeof at);
      mc_sync_device_note(row, 0, 0, note, sizeof note);
      snprintf(want, sizeof want, "Next automatic sync after %s", at);
      CHECK_TEXT(note, want);
      mc_sync_device_note(row, MC_SYNC_TEXT_APP_OPEN, 0, note, sizeof note);
      snprintf(want, sizeof want, "Next sync: the next time Kithsong is open after %s", at);
      CHECK_TEXT(note, want);
      CHECK(row->last_seen_ms >= row->last_sync_ms && row->last_sync_ms > 0);
    }
    mc_sync_device_list_free(list);
  }

  /* The network changed: a member that failed is tried again at once, not after its backoff.
   * A knows only a dead address for B, fails, and backs off; then B's address comes back. */
  {
    mc_sync_set_test_manual(a.sync, 0);
    mc_sync_set_test_auto_interval(a.sync, 0);
    char sql[512];
    snprintf(sql, sizeof sql,
             "UPDATE peer_contact SET port = 1 WHERE device_uid = '%s';"
             " UPDATE capabilities SET addresses = '[\"127.0.0.1:1\"]' WHERE device_uid = '%s'",
             b.uid, b.uid);
    group_sql(&a, sql);
    mc_sync_test_peer(a.sync, b.uid, "b", "desktop", "", "127.0.0.1", 1, 1);
    int sa = done_seen(&a, b.uid);
    CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
    CHECK(wait_done(&a, b.uid, sa, NULL, WAIT_MS) == MC_ERR_NET);
    uint32_t backoff = 0;
    mc_mutex_lock(&a.sync->lock);
    for (int i = 0; i < a.sync->npeers; i++)
      if (!strcmp(a.sync->peers[i].uid, b.uid)) backoff = a.sync->peers[i].backoff_ms;
    mc_mutex_unlock(&a.sync->lock);
    CHECK(backoff >= 10000);
    mc_sync_test_peer(a.sync, b.uid, "b", "desktop", "", "127.0.0.1", mc_sync_port(b.sync), 1);
    sa = done_seen(&a, b.uid);
    uint64_t t0 = mc_mono_ms();
    mc_sync_network_changed(a.sync);
    CHECK(wait_done(&a, b.uid, sa, NULL, WAIT_MS) == MC_OK);
    CHECK(mc_mono_ms() - t0 < 8000);
    mc_sync_set_test_manual(a.sync, 1);
    mc_sleep_ms(1000);
  }

  /* Something else answers at B's discovered address: A tries B's other addresses and gets
   * through. With B gone, three such failures running show as an issue. */
  {
    if (!dev_start(&c, root, "c")) return TEST_RESULT();
    mc_sync_test_peer(a.sync, b.uid, "b", "desktop", "", "127.0.0.1", mc_sync_port(c.sync), 1);
    mc_sync_result r;
    CHECK(sync_now(&a, &b, &r) == MC_OK);
    CHECK(r.connected);
    uint16_t c_port = mc_sync_port(c.sync);
    dev_stop(&b);
    for (int i = 0; i < MC_SYNC_WRONG_SHOW; i++) {
      int sa = done_seen(&a, b.uid);
      CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
      CHECK(wait_done(&a, b.uid, sa, NULL, WAIT_MS) != MC_OK);
    }
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&a, MC_ISSUE_WRONG_DEVICE, b.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_WRONG_DEVICE, b.uid);
    char addr[64];
    snprintf(addr, sizeof addr, "127.0.0.1:%u", (unsigned)c_port);
    CHECK(is && is->count >= MC_SYNC_WRONG_SHOW && is->detail && !strcmp(is->detail, addr));
    mc_sync_problems_free(p);
    mc_sync_device_list *list = NULL;
    const mc_sync_device *row = NULL;
    CHECK(row_of(&a, b.uid, &list, &row));
    CHECK(row && row->wrong_address && !strcmp(row->wrong_address, addr));
    mc_sync_device_list_free(list);
  }

  dev_stop(&a);
  dev_stop(&c);
  mc_remove_tree(root);
  return TEST_RESULT();
}
