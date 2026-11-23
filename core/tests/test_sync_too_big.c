/* Sync and files too big for the build (mc_file_too_big(), util/platform.h): a 32-bit
 * Android phone holds no file of 2^31 bytes or more, while desktops (and 64-bit phones) sync
 * them as ever. Two devices over loopback, as in test_sync_lossy.c: A is a desktop archive
 * holding WAV originals, B fetches originals and plays the 32-bit phone, the limit being a
 * variable the test sets (both devices share it, but only a receiver goes by it). A's rows
 * claim the sizes the test needs, so the boundary is checked at 2^31 without moving 2 GiB:
 * the sizes a peer gives in its HAVE answer are its rows'.
 *
 *   - Under (2^31 - 1 bytes by its row) is fetched; Over (2^31) isn't even asked for: B's
 *     plan leaves it out from then on, it isn't missing, and the next sync is settled.
 *   - Old's row claims a small size, but its file is over a small limit, and A plays a
 *     version from before FILE_REQ's max: B refuses it at FILE_HDR, and leaves it out too.
 *   - With no limit (a desktop or 64-bit phone), B fetches Over and Old as usual.
 *   - Back on the limit, B keeps the Over and Old it holds (nothing is deleted), and Late,
 *     whose row claims a small size but whose (sparse) file is 2^31 bytes, is answered with
 *     FILE_ERR "too_big" by A before a byte of it is read, and left out. Not on Windows,
 *     which makes no sparse files without asking. */
#ifndef _WIN32
#define _DEFAULT_SOURCE /* truncate */
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/plan.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define MAX_EVENTS 64
#define TRACK_MS 500

/* --- Files --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A 16-bit stereo PCM WAV of a tone. */
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

static mc_status lots_free(const char *dir, int64_t *out) {
  (void)dir;
  *out = (int64_t)1 << 40;
  return MC_OK;
}

/* --- Devices --- */

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

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes;
  int results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
} dev;

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
  d->sync = NULL;
  mc_library_close(d->lib);
  d->lib = NULL;
  mc_cond_destroy(&d->c);
  mc_mutex_destroy(&d->m);
}

static int wait_codes(dev *d, int seen) {
  uint64_t end = mc_mono_ms() + 100;
  mc_mutex_lock(&d->m);
  while (d->codes <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  int ok = d->codes > seen;
  mc_mutex_unlock(&d->m);
  return ok;
}

static mc_pair_reason wait_result(dev *d, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (d->results <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_pair_reason r = d->results > seen ? d->reason : (mc_pair_reason)-1;
  mc_mutex_unlock(&d->m);
  return r;
}

static int count_done(dev *d, const char *peer) {
  int n = 0;
  for (int i = 0; i < d->ndone; i++)
    if (!strcmp(d->done[i].peer, peer)) n++;
  return n;
}

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

static int done_seen(dev *d, const char *peer) {
  mc_mutex_lock(&d->m);
  int n = count_done(d, peer);
  mc_mutex_unlock(&d->m);
  return n;
}

/* x's user presses Sync for y; waits for both sides. */
static mc_status sync_now(dev *x, dev *y, mc_sync_result *rx, mc_sync_result *ry) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx, rx);
  if (st == MC_OK) st = wait_done(y, x->uid, sy, ry);
  return st;
}

static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results, ry = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  while (mc_mono_ms() < end && !wait_codes(y, codes)) {
  }
  char code[MC_PAIR_CODE_LEN + 1];
  mc_mutex_lock(&y->m);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_pair_reason r = wait_result(x, rx);
  CHECK(wait_result(y, ry) == r);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx, NULL) == MC_OK);
    CHECK(wait_done(y, x->uid, sy, NULL) == MC_OK);
  }
  return r;
}

/* The device's user chooses originals, everything. */
static void set_original(dev *editor, const char *uid) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  p->sync_all = 1;
  p->format.format = MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = 0;
  p->budget_enabled = 0;
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
  mc_mutex_lock(&editor->sync->api_lock);
  CHECK(mc_profile_choose(editor->sync->api.sdb, NULL) == MC_OK);
  mc_mutex_unlock(&editor->sync->api_lock);
  mc_sync_profile_free(p);
}

/* --- Looking inside --- */

static int exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc;
}

static int64_t lib_int(dev *d, const char *sql) {
  sqlite3_stmt *stmt;
  int64_t n = -1;
  if (sqlite3_prepare_v2(mc_library_db(d->lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static void lib_text(dev *d, const char *sql, char *out, size_t cap) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(d->lib), sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, cap, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
}

static void abs_path(dev *d, const char *rel, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s", d->libdir, rel);
}

/* Adds a track of album Things by Low, with no file yet. */
static void add_track(dev *d, const char *title, int no, char uid[MC_UID_STRLEN]) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN] = "", al[MC_UID_STRLEN] = "", sql[1024];
  lib_text(d, "SELECT uid FROM artists WHERE name = 'Low'", ar, sizeof ar);
  lib_text(d, "SELECT uid FROM albums WHERE title = 'Things'", al, sizeof al);
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
  mc_uid_new(uid);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, %d)",
           uid, al, ar, title, no, TRACK_MS);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* A files row on A for an original imported there, already at rel, whose row says it holds
 * size bytes (the file's own size if 0) and, if sha, that it was hashed. */
static void add_file(dev *d, const char *track, const char *rel, int64_t size, int sha) {
  char p[2048], sql[2048], uid[MC_UID_STRLEN];
  abs_path(d, rel, p, sizeof p);
  int64_t real = 0, mtime = 0;
  CHECK(mc_file_info(p, &real, &mtime) == MC_OK);
  mc_uid_new(uid);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms, sha256)"
           " VALUES ('%s', '%s', '%s', 'wav', 1411, %lld, %lld, %s)",
           uid, track, rel, (long long)(size ? size : real), (long long)mtime,
           sha ? "'0000000000000000000000000000000000000000000000000000000000000000'" : "NULL");
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  CHECK(exec(mc_library_db(d->lib), sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* A new track on A with a WAV original; its row claims size bytes (0: as it is). */
static void add_wav(dev *a, const char *title, int no, int64_t size, char uid[MC_UID_STRLEN]) {
  char rel[256], p[2048];
  add_track(a, title, no, uid);
  snprintf(rel, sizeof rel, "Low/Things (1994)/%02d %s.wav", no, title);
  abs_path(a, rel, p, sizeof p);
  CHECK(write_wav(p, TRACK_MS, 200 + 50 * no));
  add_file(a, uid, rel, size, 0);
}

static int64_t files_of(dev *d, const char *track) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE track_uid = '%s'", track);
  return lib_int(d, sql);
}

/* What B's sync.db says it was offered too big of track: its size, or -1 for nothing. */
static int64_t too_big_size(dev *b, const char *track) {
  mc_sync_db *sdb = NULL;
  int64_t size = -1;
  if (mc_sync_db_open(b->config, &sdb) != MC_OK) return -2;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), "SELECT size, kbps FROM too_big WHERE track_uid = ?1",
                         -1, &stmt, NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, track, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      size = sqlite3_column_int64(stmt, 0);
      CHECK(sqlite3_column_int(stmt, 1) == 0); /* as an original */
    }
    sqlite3_finalize(stmt);
  }
  mc_sync_db_close(sdb);
  return size;
}

/* B's own plan: how many tracks it wants, and whether track is one. */
static int b_plan(dev *b, const char *track, int *wanted) {
  int has = -1;
  *wanted = -1;
  mc_mutex_lock(&b->sync->api_lock);
  mc_plan *plan = NULL;
  if (mc_plan_compute(b->sync->api.sdb, b->lib, b->uid, -1, &plan) == MC_OK && plan->status == MC_PLAN_OK) {
    has = mc_plan_find(plan, track) != NULL;
    *wanted = plan->count;
  }
  mc_plan_free(plan);
  mc_mutex_unlock(&b->sync->api_lock);
  return has;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], p[2048];
  snprintf(root, sizeof root, "%s/test_sync_too_big.d", scratch);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  mc_sync_test_free_space = lots_free; /* Under's 2 GiB, by its row, has to fit */

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  CHECK(pair(&b, &a) == MC_PAIR_PAIRED); /* A is the archive */
  snprintf(p, sizeof p, "%s/Low/Things (1994)", a.libdir);
  CHECK(mc_mkdirs(p) == MC_OK);

  char under[MC_UID_STRLEN], over[MC_UID_STRLEN], old[MC_UID_STRLEN];
  add_wav(&a, "Under", 1, MC_32BIT_MAX_FILE_BYTES - 1, under);
  add_wav(&a, "Over", 2, MC_32BIT_MAX_FILE_BYTES, over);
  set_original(&b, b.uid);
  mc_sync_result ra, rb;
  int wanted = 0;

  /* B as a 32-bit phone: Under comes, Over doesn't, and isn't missing. */
  mc_max_file_bytes = MC_32BIT_MAX_FILE_BYTES;
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 1 && rb.files_missing == 0 && rb.files_corrupt == 0);
  CHECK(ra.files_sent == 1);
  CHECK(files_of(&b, under) == 1 && files_of(&b, over) == 0);
  CHECK(too_big_size(&b, under) == -1);
  CHECK(too_big_size(&b, over) == MC_32BIT_MAX_FILE_BYTES);
  CHECK(b_plan(&b, over, &wanted) == 0 && wanted == 1);
  CHECK(b_plan(&b, under, &wanted) == 1);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_missing == 0 && ra.files_sent == 0);

  /* An older A sends whatever it's asked for: B refuses Old at FILE_HDR. */
  add_wav(&a, "Old", 3, 1000, old);
  snprintf(p, sizeof p, "SELECT path FROM files WHERE track_uid = '%s'", old);
  char rel[512], abs[2048];
  lib_text(&a, p, rel, sizeof rel);
  abs_path(&a, rel, abs, sizeof abs);
  int64_t old_size = 0, mtime = 0;
  CHECK(mc_file_info(abs, &old_size, &mtime) == MC_OK && old_size > 1000);
  mc_max_file_bytes = old_size;
  mc_transfer_test_old_sender = 1;
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_missing == 0);
  CHECK(files_of(&b, old) == 0 && too_big_size(&b, old) == old_size);
  CHECK(b_plan(&b, old, &wanted) == 0 && wanted == 1);
  mc_transfer_test_old_sender = 0;

  /* No limit: everything comes, as between desktops. */
  mc_max_file_bytes = 0;
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 2 && rb.files_missing == 0);
  CHECK(files_of(&b, over) == 1 && files_of(&b, old) == 1);

#ifndef _WIN32
  /* The limit again: what B holds stays, and Late's 2 GiB file is refused by A unread. */
  char late[MC_UID_STRLEN];
  add_track(&a, "Late", 4, late);
  snprintf(rel, sizeof rel, "Low/Things (1994)/04 Late.wav");
  abs_path(&a, rel, abs, sizeof abs);
  CHECK(write_wav(abs, TRACK_MS, 900));
  CHECK(truncate(abs, (off_t)MC_32BIT_MAX_FILE_BYTES) == 0);
  add_file(&a, late, rel, 1000, 1); /* hashed, so nothing reads it ahead */
  mc_max_file_bytes = MC_32BIT_MAX_FILE_BYTES;
  uint64_t t0 = mc_mono_ms();
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(mc_mono_ms() - t0 < 20000);
  CHECK(rb.files_received == 0 && rb.files_missing == 0 && rb.files_deleted == 0);
  CHECK(files_of(&b, late) == 0 && too_big_size(&b, late) == MC_32BIT_MAX_FILE_BYTES);
  CHECK(files_of(&b, over) == 1 && files_of(&b, old) == 1);
  CHECK(b_plan(&b, over, &wanted) == 1 && wanted == 3);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_missing == 0 && rb.files_deleted == 0);
  mc_remove(abs);
#endif

  mc_max_file_bytes = 0;
  dev_stop(&a);
  dev_stop(&b);
  mc_remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
