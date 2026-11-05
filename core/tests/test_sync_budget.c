/* A budget set from free space (see "Confirming profiles" in docs/sync-protocol.md): a device
 * whose profile nobody has confirmed yet, and whose free space is less than the originals
 * its profile wants, turns its own budget on, so it fills with what fits. Devices over
 * loopback in test_sync_confirm.c style: A holds the library (three WAVs) and is the
 * archive; B, C, D and E join it with their free space stubbed through
 * mc_sync_test_free_space. B gets its budget in an automatic session, C from its own press
 * of Sync; D (free space unknown, then confirmed with the budget off) and E (Opus) are left
 * alone. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define MAX_EVENTS 64
#define TRACK_MS 1500
#define GIB ((int64_t)1 << 30)
/* Room for the three tracks as Opus 256 kbps (3 x 52096 bytes estimated), not as WAV
 * originals (3 x 264644 bytes). */
#define SMALL_FREE ((int64_t)200000)
/* Room for Opus up to 128 kbps (3 x 28096), not 256. */
#define SMALLER_FREE ((int64_t)100000)
#define BIG_FREE ((int64_t)10 * GIB)
#define UNKNOWN_FREE ((int64_t)-1)
#define REAL_FREE ((int64_t)-2)

/* --- Scratch dirs --- */

typedef struct names {
  char (*items)[512];
  int count;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  void *more = realloc(n->items, (size_t)(n->count + 1) * sizeof *n->items);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd')
      remove_tree(p);
    else
      remove(p);
  }
  free(n.items);
  mc_rmdir(dir);
}

/* --- Looking inside --- */

static int exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc;
}

static int64_t query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int64_t n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
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

/* --- A device --- */

typedef struct done_event {
  char peer[MC_UID_STRLEN];
  mc_status status;
  mc_sync_result r;
} done_event;

typedef struct dev {
  const char *label;
  char config[1024], libdir[1024];
  mc_library *lib;
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  int64_t free_bytes; /* what mc_sync_free_space() finds: REAL_FREE measures, UNKNOWN_FREE fails */

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes;
  int results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
} dev;

static dev g_devs[5];
#define NDEVS ((int)(sizeof g_devs / sizeof g_devs[0]))

/* The hook: answers for the device whose library folder dir is, by its label, so each of
 * the devices in this process has its own free space. */
static mc_status stub_free_space(const char *dir, int64_t *out) {
  for (int i = 0; i < NDEVS; i++) {
    dev *d = &g_devs[i];
    if (!d->label) continue;
    char fwd[64], back[64];
    snprintf(fwd, sizeof fwd, "/%s/music", d->label);
    snprintf(back, sizeof back, "\\%s\\music", d->label);
    if (!strstr(dir, fwd) && !strstr(dir, back)) continue;
    if (d->free_bytes == REAL_FREE) return mc_free_space(dir, out);
    if (d->free_bytes == UNKNOWN_FREE) return MC_ERR_IO;
    *out = d->free_bytes;
    return MC_OK;
  }
  return mc_free_space(dir, out);
}

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)name;
  (void)attempts;
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
    e->status = r->status;
    e->r = *r;
    e->r.device_uid = NULL;
  }
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done, NULL,
                                        NULL, NULL, NULL, NULL, NULL};

static int dev_start(dev *d, const char *root, const char *label, int64_t free_bytes) {
  d->label = label;
  d->free_bytes = free_bytes;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, d->label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) ==
        MC_OK);
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

/* Waits for a new session report on d about peer; returns its status and, in r, the
 * result. */
static mc_status wait_done(dev *d, const char *peer, int seen, mc_sync_result *r) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_status st = MC_ERR_UNIMPLEMENTED;
  for (int i = d->ndone - 1; i >= 0; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      if (count_done(d, peer) > seen) {
        st = d->done[i].status;
        if (r) *r = d->done[i].r;
      }
      break;
    }
  mc_mutex_unlock(&d->m);
  return st;
}

/* x syncs with y, automatically (user = 0: as the scheduler or background work would) or as
 * x's user pressing Sync. Waits for both sides; rx and ry (may be NULL) get the results. */
static mc_status sync_with(dev *x, dev *y, int user, mc_sync_result *rx, mc_sync_result *ry) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK((user ? mc_sync_confirm_now(x->sync, y->uid) : mc_sync_now(x->sync, y->uid)) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx, rx);
  if (st == MC_OK) st = wait_done(y, x->uid, sy, ry);
  return st;
}

static mc_pair_reason pair(dev *x, dev *y, mc_sync_result *rx) {
  int codes = y->codes, rx_seen = x->results, ry_seen = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&y->m);
  while (y->codes <= codes && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 100);
  char code[MC_PAIR_CODE_LEN + 1];
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_pair_reason r = (mc_pair_reason)-1;
  mc_mutex_lock(&x->m);
  while (x->results <= rx_seen && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 100);
  if (x->results > rx_seen) r = x->reason;
  mc_mutex_unlock(&x->m);
  mc_mutex_lock(&y->m);
  while (y->results <= ry_seen && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 100);
  mc_mutex_unlock(&y->m);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx, rx) == MC_OK);
    CHECK(wait_done(y, x->uid, sy, NULL) == MC_OK);
  }
  return r;
}

/* uid's confirm state as d's device list shows it; -1 if it isn't listed. */
static int confirm_of(dev *d, const char *uid) {
  mc_sync_device_list *list = NULL;
  CHECK(mc_sync_list_devices(d->sync, &list) == MC_OK);
  int state = -1;
  for (int i = 0; list && i < list->count; i++)
    if (!strcmp(list->items[i].uid, uid)) state = (int)list->items[i].confirm;
  mc_sync_device_list_free(list);
  return state;
}

static int64_t files_of(dev *d) {
  return query_int(mc_library_db(d->lib), "SELECT count(*) FROM files");
}

/* How many of d's files are Opus copies at kbps received from a peer. */
static int64_t copies_of(dev *d, int kbps) {
  char sql[256];
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM files WHERE source_file_uid IS NOT NULL AND bitrate_kbps = %d",
           kbps);
  return query_int(mc_library_db(d->lib), sql);
}

/* --- Files --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A 16-bit stereo PCM WAV of a tone: a real original, so the archive can transcode it. */
static int write_wav(const char *path, unsigned ms, double pitch) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  unsigned rate = 44100, channels = 2;
  uint32_t frames = rate * ms / 1000, bytes = frames * channels * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, channels);
  put32(f, rate);
  put32(f, rate * channels * 2);
  put16(f, channels * 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++)
    for (unsigned c = 0; c < channels; c++) {
      double v = 0.4 * sin(2 * 3.14159265358979 * pitch * (1 + 0.1 * c) * i / rate);
      put16(f, (unsigned)(uint16_t)(int16_t)(v * 32767));
    }
  return fclose(f) == 0;
}

/* Adds a track of album Things by Low with a WAV original at its organised path. */
static void add_track(dev *d, const char *title, int no) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], tr[MC_UID_STRLEN], fi[MC_UID_STRLEN], sql[1024];
  query_text(db, "SELECT uid FROM artists WHERE name = 'Low'", ar, sizeof ar);
  query_text(db, "SELECT uid FROM albums WHERE title = 'Things'", al, sizeof al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  if (!al[0]) {
    mc_uid_new(al);
    snprintf(sql, sizeof sql,
             "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', 'Things', '%s', 1994)",
             al, ar);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  mc_uid_new(tr);
  mc_uid_new(fi);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, %d)",
           tr, al, ar, title, no, TRACK_MS);
  CHECK(exec(db, sql) == SQLITE_OK);
  char rel[512], dir[2048], p[2048];
  snprintf(rel, sizeof rel, "Low/Things (1994)/%02d %s.wav", no, title);
  snprintf(dir, sizeof dir, "%s/Low/Things (1994)", d->libdir);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(p, sizeof p, "%s/%s", d->libdir, rel);
  CHECK(write_wav(p, TRACK_MS, 220 * no));
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', 'wav', 1411, %lld, %lld)",
           fi, tr, rel, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* --- Profiles --- */

/* uid's profile as d sees it: whether the budget is on, its size, and the format. */
static void profile_of(dev *d, const char *uid, int *budget, int64_t *bytes, mc_format *format) {
  *budget = -1;
  *bytes = -1;
  *format = (mc_format)-1;
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(d->sync, uid, &p) == MC_OK);
  if (!p) return;
  *budget = p->budget_enabled;
  *bytes = p->budget_bytes;
  *format = p->format.format;
  mc_sync_profile_free(p);
}

static void check_budget_off(dev *d, const char *uid, mc_format format) {
  int budget;
  int64_t bytes;
  mc_format f;
  profile_of(d, uid, &budget, &bytes, &f);
  CHECK(budget == 0 && bytes == 0 && f == format);
}

static void check_budget_on(dev *d, const char *uid) {
  int budget;
  int64_t bytes;
  mc_format f;
  profile_of(d, uid, &budget, &bytes, &f);
  CHECK(budget == 1 && bytes == GIB && f == MC_FORMAT_AUTO);
}

/* What d's next sync would do for itself: the bitrate the budget picks and its limit. */
static void check_plan(dev *d, int kbps, int64_t limit) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(d->sync, d->uid, &p) == MC_OK);
  if (!p) return;
  mc_sync_estimate e;
  CHECK(mc_sync_estimate_profile(d->sync, d->uid, p, &e) == MC_OK);
  CHECK(e.tracks == 3 && !e.over_budget && e.kbps == kbps && e.limit_bytes == limit);
  mc_sync_profile_free(p);
}

/* editor sets uid's profile to Opus at kbps. */
static void set_opus(dev *editor, const char *uid, int kbps) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  p->format.format = MC_FORMAT_OPUS;
  p->format.bitrate_kbps = kbps;
  p->budget_enabled = 0;
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
  mc_sync_profile_free(p);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_budget.d", scratch);
  remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  mc_sync_test_free_space = stub_free_space;

  dev *a = &g_devs[0], *b = &g_devs[1], *c = &g_devs[2], *d = &g_devs[3], *e = &g_devs[4];
  /* A's room is real: a sender short of space doesn't transcode for others. */
  if (!dev_start(a, root, "a", REAL_FREE) || !dev_start(b, root, "b", SMALL_FREE) ||
      !dev_start(c, root, "c", SMALL_FREE) || !dev_start(d, root, "d", UNKNOWN_FREE) ||
      !dev_start(e, root, "e", SMALLER_FREE))
    return TEST_RESULT();
  add_track(a, "One", 1);
  add_track(a, "Two", 2);
  add_track(a, "Three", 3);
  mc_sync_result ra, rb, rc, rd, re;
  memset(&rb, 0, sizeof rb);

  /* B joins. The pairing session finds B's library empty, so its profile (everything,
   * originals, no budget) stands, unconfirmed. */
  CHECK(pair(b, a, &rb) == MC_PAIR_PAIRED);
  CHECK(rb.library_synced && rb.files_paused && files_of(b) == 0);
  CHECK(query_int(mc_library_db(b->lib), "SELECT count(*) FROM tracks") == 3);
  check_budget_off(b, b->uid, MC_FORMAT_ORIGINAL);
  CHECK(confirm_of(b, b->uid) == MC_SYNC_UNCONFIRMED);

  /* The next session, automatic: three originals (unknown here, estimated at 1000 kbps) are
   * more than B's 200 kB, so B turns its budget on: a size of 1 GiB (the floor), Auto. Still
   * unconfirmed, so no files yet; the plan it would run picks 256 kbps within its free
   * space. */
  CHECK(sync_with(a, b, 0, &ra, &rb) == MC_OK);
  CHECK(rb.files_paused && files_of(b) == 0);
  check_budget_on(b, b->uid);
  CHECK(confirm_of(b, b->uid) == MC_SYNC_UNCONFIRMED);
  check_plan(b, 256, SMALL_FREE);
  /* A hears of the edit in the follow-up session: its editor shows the budget on. */
  CHECK(sync_with(a, b, 0, &ra, &rb) == MC_OK);
  check_budget_on(a, b->uid);

  /* B's user presses Sync: the budget is part of what's confirmed, and B fetches Opus 256
   * copies, transcoded by A from its WAVs. */
  CHECK(sync_with(b, a, 1, &rb, &ra) == MC_OK);
  CHECK(!rb.files_paused && rb.files_received == 3);
  CHECK(files_of(b) == 3 && copies_of(b, 256) == 3);
  CHECK(confirm_of(b, b->uid) == MC_SYNC_CONFIRMED);
  check_budget_on(b, b->uid);

  /* C joins and its user presses Sync straight away, with no session in between: the press
   * sets the budget before it confirms, so C is confirmed with it and fetches copies. */
  memset(&rc, 0, sizeof rc);
  CHECK(pair(c, a, &rc) == MC_PAIR_PAIRED);
  CHECK(rc.files_paused && files_of(c) == 0);
  check_budget_off(c, c->uid, MC_FORMAT_ORIGINAL);
  CHECK(sync_with(c, a, 1, &rc, &ra) == MC_OK);
  CHECK(!rc.files_paused && rc.files_received == 3);
  CHECK(files_of(c) == 3 && copies_of(c, 256) == 3);
  CHECK(confirm_of(c, c->uid) == MC_SYNC_CONFIRMED);
  check_budget_on(c, c->uid);

  /* D's free space is unknown: nothing changes, however many sessions. */
  memset(&rd, 0, sizeof rd);
  CHECK(pair(d, a, &rd) == MC_PAIR_PAIRED);
  CHECK(sync_with(a, d, 0, &ra, &rd) == MC_OK);
  CHECK(rd.files_paused && files_of(d) == 0);
  check_budget_off(d, d->uid, MC_FORMAT_ORIGINAL);
  /* With plenty of room, its user confirms the profile as it is: originals, no budget. */
  d->free_bytes = BIG_FREE;
  CHECK(sync_with(d, a, 1, &rd, &ra) == MC_OK);
  CHECK(!rd.files_paused && rd.files_received == 3 && files_of(d) == 3 && copies_of(d, 0) == 0);
  CHECK(confirm_of(d, d->uid) == MC_SYNC_CONFIRMED);
  check_budget_off(d, d->uid, MC_FORMAT_ORIGINAL);
  /* Room runs short later: a confirmed choice stays, and D goes on holding originals. */
  d->free_bytes = SMALL_FREE;
  CHECK(sync_with(d, a, 0, &rd, &ra) == MC_OK);
  CHECK(sync_with(a, d, 0, &ra, &rd) == MC_OK);
  CHECK(!rd.files_paused && files_of(d) == 3);
  check_budget_off(d, d->uid, MC_FORMAT_ORIGINAL);
  CHECK(confirm_of(d, d->uid) == MC_SYNC_CONFIRMED);

  /* E was given Opus 256 kbps by A's user before anyone confirmed it, and 100 kB isn't room
   * for that either; a format chosen is left alone, the rule is for originals. */
  memset(&re, 0, sizeof re);
  CHECK(pair(e, a, &re) == MC_PAIR_PAIRED);
  set_opus(a, e->uid, 256);
  CHECK(sync_with(a, e, 0, &ra, &re) == MC_OK);
  CHECK(sync_with(a, e, 0, &ra, &re) == MC_OK);
  CHECK(re.files_paused && files_of(e) == 0);
  check_budget_off(e, e->uid, MC_FORMAT_OPUS);
  CHECK(confirm_of(e, e->uid) == MC_SYNC_UNCONFIRMED);

  /* The archive, never confirmed either, is as it was. */
  check_budget_off(a, a->uid, MC_FORMAT_ORIGINAL);
  CHECK(files_of(a) == 3);

  for (int i = 0; i < NDEVS; i++) dev_stop(&g_devs[i]);
  mc_sync_test_free_space = NULL;
  remove_tree(root);
  return TEST_RESULT();
}
