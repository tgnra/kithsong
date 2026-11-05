/* Verification: what the checks note, how a session makes up for it, the weekly deep check, and
 * what counts as missing for good. Removing a track deletes it on every device, so most of this
 * is about when it must not happen. Three real devices over loopback: A the archive, B on Opus, C
 * on originals. See "Verification" in docs/sync-protocol.md. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <unistd.h>
#include <utime.h>
#endif

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/plan.h"
#include "sync/rows.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/orphans.h"
#include "library/delete_internal.h"
#include "util/power.h"
#include "sync/transfer.h"
#include "tagger/import_db.h"
#include "musiccore/edit.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 20000
#define MAX_EVENTS 64

#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)

/* --- Scratch dirs --- */

typedef struct names {
  char (*items)[512];
  int count;
} names;

static void collect(void *user, const char *name, int is_dir) {
  names *n = user;
  char (*more)[512] = realloc(n->items, (size_t)(n->count + 1) * sizeof *more);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {NULL, 0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd')
      remove_tree(path);
    else
      mc_remove(path);
  }
  free(n.items);
  mc_rmdir(dir);
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

/* A 16-bit stereo PCM WAV of a tone; pitch makes each file different. */
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

static void make_old(const char *path) {
  struct utimbuf t;
  t.actime = t.modtime = 1000000000; /* 2001 */
  utime(path, &t);
}

static int write_bytes(const char *path, const char *text) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  fputs(text, f);
  return fclose(f) == 0;
}

/* The trash sync uses: a folder under the scratch dir, never the Recycle Bin. Files are
 * numbered in the order they arrive, keeping their names. */
static char g_bin[1100];
static int g_binned;

static mc_status fake_trash(const char *path) {
  const char *name = strrchr(path, '/'), *sep = strrchr(path, MC_PATH_SEP);
  if (sep > name) name = sep;
  char to[1300];
  snprintf(to, sizeof to, "%s%c%d-%s", g_bin, MC_PATH_SEP, g_binned, name ? name + 1 : path);
  mc_status st = mc_rename(path, to, 0);
  if (st == MC_OK) g_binned++;
  return st;
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

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes;
  int results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
  int files_progress; /* MC_SYNC_PHASE_FILES progress events */
  int done_files, done_total; /* in the last MC_SYNC_PHASE_DONE progress */
  int done_sends, done_sends_total;
  int64_t done_files_bytes, done_sends_bytes;
  int sends_ahead; /* sends_total in a files progress before any request was answered */
  int deep_delay;  /* look for the weekly check at once */
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

static void on_progress(void *user, const mc_sync_progress *p) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  if (p->phase == MC_SYNC_PHASE_FILES) d->files_progress++;
  if (p->phase == MC_SYNC_PHASE_FILES && p->sends_done == 0 && p->sends_total > 0)
    d->sends_ahead = p->sends_total;
  if (p->phase == MC_SYNC_PHASE_DONE) {
    d->done_files = p->files_done;
    d->done_total = p->files_total;
    d->done_sends = p->sends_done;
    d->done_sends_total = p->sends_total;
    d->done_files_bytes = p->files_bytes;
    d->done_sends_bytes = p->sends_bytes;
  }
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

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, on_progress,
                                        on_sync_done, NULL, NULL, NULL, NULL, NULL, NULL};

static void dev_paths(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
}

static int dev_start(dev *d) {
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
  if (d->deep_delay) mc_sync_set_test_deep_delay(d->sync, 0);
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

static int done_seen(dev *d, const char *peer) {
  mc_mutex_lock(&d->m);
  int n = count_done(d, peer);
  mc_mutex_unlock(&d->m);
  return n;
}

/* x's user presses Sync for y (confirming both profiles); waits for both sides. rx and ry
 * (may be NULL) get the results. */
static mc_status sync_now(dev *x, dev *y, mc_sync_result *rx, mc_sync_result *ry) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_confirm_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx, rx);
  if (st == MC_OK) wait_done(y, x->uid, sy, ry);
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

static int64_t lib_int(dev *d, const char *sql) {
  return query_int(mc_library_db(d->lib), sql);
}

static int64_t group_int(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return -1;
  int64_t n = query_int(mc_sync_db_handle(sdb), sql);
  mc_sync_db_close(sdb);
  return n;
}

/* d's absolute path for a library-relative one. */
static void abs_path(dev *d, const char *rel, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s", d->libdir, rel);
}

typedef struct track_ref {
  char uid[MC_UID_STRLEN];
  char file[MC_UID_STRLEN];
  char rel[256];
} track_ref;

/* Adds a track (and its album, artist) with a WAV original at its organised path; with album
 * NULL, a track with no album. */
static void add_track(dev *d, const char *artist, const char *album, int year, const char *title,
                      int no, double pitch, track_ref *out) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN] = "", sql[1024];
  snprintf(sql, sizeof sql, "SELECT uid FROM artists WHERE name = '%s'", artist);
  query_text(db, sql, ar, sizeof ar);
  snprintf(sql, sizeof sql, "SELECT uid FROM albums WHERE title = '%s'", album ? album : "");
  if (album) query_text(db, sql, al, sizeof al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', '%s')", ar, artist);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  if (album && !al[0]) {
    mc_uid_new(al);
    snprintf(sql, sizeof sql,
             "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', '%s', '%s', %d)", al,
             album, ar, year);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  mc_uid_new(out->uid);
  mc_uid_new(out->file);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', nullif('%s', ''), '%s', '%s', nullif(%d, 0), 1500)",
           out->uid, al, ar, title, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  char dir[2048], p[2048];
  if (album) {
    snprintf(out->rel, sizeof out->rel, "%s/%s (%d)/%02d %s.wav", artist, album, year, no, title);
    snprintf(dir, sizeof dir, "%s/%s/%s (%d)", d->libdir, artist, album, year);
  } else {
    snprintf(out->rel, sizeof out->rel, "%s/%s.wav", artist, title);
    snprintf(dir, sizeof dir, "%s/%s", d->libdir, artist);
  }
  CHECK(mc_mkdirs(dir) == MC_OK);
  abs_path(d, out->rel, p, sizeof p);
  CHECK(write_wav(p, 1500, pitch));
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', 'wav', 1411, %lld, %lld)",
           out->file, out->uid, out->rel, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

static void set_profile(dev *editor, const char *uid, int opus_kbps, const char *album_uid) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  p->format.format = opus_kbps ? MC_FORMAT_OPUS : MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = opus_kbps;
  mc_profile_item item;
  memset(&item, 0, sizeof item);
  if (album_uid) {
    item.kind = MC_PROFILE_ALBUM;
    snprintf(item.ref_uid, sizeof item.ref_uid, "%s", album_uid);
    free(p->items);
    p->items = NULL;
    p->count = 0;
  }
  mc_profile_item *saved = p->items;
  int saved_count = p->count;
  p->sync_all = album_uid == NULL;
  if (album_uid) {
    p->items = &item;
    p->count = 1;
  }
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
  p->items = saved;
  p->count = saved_count;
  mc_sync_profile_free(p);
}

/* --- Plan --- */


/* --- Problems --- */

/* Waits for the scheduler's next answer to say this. */
static int problems_are(dev *d, int failed, int unknown, int lost) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  for (;;) {
    mc_sync_problems *p = NULL;
    CHECK(mc_sync_get_problems(d->sync, &p) == MC_OK);
    int same = p && p->failed == failed && p->unknown == unknown && p->lost == lost;
    if (!same && mc_mono_ms() >= end)
      fprintf(stderr, "%s: failed %d, unknown %d, lost %d\n", d->label, p ? p->failed : -1,
              p ? p->unknown : -1, p ? p->lost : -1);
    mc_sync_problems_free(p);
    if (same) return 1;
    if (mc_mono_ms() >= end) return 0;
    mc_sync_waiting_changed(d->sync);
    mc_sleep_ms(50);
  }
}

static int64_t failed_state(dev *d, const track_ref *t) {
  char sql[256];
  snprintf(sql, sizeof sql,
           "SELECT coalesce((SELECT state FROM file_failed WHERE file_uid = '%s'), 0)", t->file);
  return lib_int(d, sql);
}

static int64_t track_rows(dev *d, const track_ref *t) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM tracks WHERE uid = '%s'", t->uid);
  return lib_int(d, sql);
}

static int64_t file_rows(dev *d, const track_ref *t) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE track_uid = '%s'", t->uid);
  return lib_int(d, sql);
}

/* Takes t's file off d: gone from disk, and for a copy from a peer its row with it. */
static void lose(dev *d, const track_ref *t) {
  char sql[256], rel[1024], p[2048];
  snprintf(sql, sizeof sql, "SELECT path FROM files WHERE track_uid = '%s'", t->uid);
  query_text(mc_library_db(d->lib), sql, rel, sizeof rel);
  CHECK(rel[0] != '\0');
  abs_path(d, rel, p, sizeof p);
  CHECK(mc_remove(p) == MC_OK);
  CHECK(mc_sync_verify(d->sync, MC_VERIFY_FAST, NULL) == MC_OK);
}

/* Overwrites a byte of the file at rel on d and puts its time back, as a failing disk would. */
static void rot(dev *d, const char *rel) {
  char p[2048];
  abs_path(d, rel, p, sizeof p);
  FILE *f = mc_fopen(p, "r+b");
  CHECK(f != NULL);
  if (!f) return;
  fseek(f, 1000, SEEK_SET);
  int c = fgetc(f);
  fseek(f, 1000, SEEK_SET);
  fputc(c ^ 0xff, f);
  fclose(f);
  make_old(p);
}

/* Gives every file of d the old time make_old() sets, in its row too. */
static void age_files(dev *d) {
  sqlite3_stmt *stmt;
  CHECK(sqlite3_prepare_v2(mc_library_db(d->lib), "SELECT path FROM files", -1, &stmt, NULL) ==
        SQLITE_OK);
  int64_t size = 0, mtime = 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    char p[2048];
    abs_path(d, (const char *)sqlite3_column_text(stmt, 0), p, sizeof p);
    make_old(p);
    mc_file_info(p, &size, &mtime);
  }
  sqlite3_finalize(stmt);
  char sql[128];
  snprintf(sql, sizeof sql, "UPDATE files SET mtime_ms = %lld", (long long)mtime);
  CHECK(exec(mc_library_db(d->lib), sql) == SQLITE_OK);
}

static void group_exec(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(d->config, &sdb) == MC_OK);
  if (!sdb) return;
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  mc_sync_db_close(sdb);
}

/* The weekly check, as the scheduler runs it: at once here, and only on mains power. */
static void check_schedule(const char *root) {
  static dev d;
  dev_paths(&d, root, "laptop");
  d.deep_delay = 1;
  mc_power_set_override(0);
  if (!dev_start(&d)) return;
  mc_sleep_ms(500);
  CHECK(group_int(&d, "SELECT deep_check_ms IS NULL FROM local") == 1);
  dev_stop(&d);
  mc_power_set_override(1);
  if (!dev_start(&d)) return;
  uint64_t end = mc_mono_ms() + WAIT_MS;
  while (group_int(&d, "SELECT deep_check_ms IS NULL FROM local") && mc_mono_ms() < end)
    mc_sleep_ms(50);
  CHECK(group_int(&d, "SELECT deep_check_ms IS NULL FROM local") == 0);
  dev_stop(&d);
  mc_power_set_override(-1);
}

int main(int argc, char **argv) {
  mc_orphans_test_mtime_only = 1; /* make_old() can only reach the modified time */
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], sql[512], p[2048];
  snprintf(root, sizeof root, "%s/test_sync_verify.d", scratch);
  remove_tree(root);
  snprintf(g_bin, sizeof g_bin, "%s/bin", root);
  CHECK(mc_mkdirs(g_bin) == MC_OK);
  mc_transfer_trash = fake_trash;
  mc_delete_trash = fake_trash;

  check_schedule(root);

  /* A is the archive, holding everything it imported. Opus goes to B, originals to C. */
  static dev a, b, c;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  dev_paths(&c, root, "c");
  if (!dev_start(&a) || !dev_start(&b) || !dev_start(&c)) return TEST_RESULT();
  track_ref t1, t2, t3, t4, t5, t6;
  add_track(&a, "Low", "Things", 1994, "One", 1, 440, &t1);
  add_track(&a, "Low", "Things", 1994, "Two", 2, 550, &t2);
  add_track(&a, "Low", "Things", 1994, "Three", 3, 660, &t3);
  add_track(&a, "Low", "Things", 1994, "Four", 4, 770, &t4);
  add_track(&a, "Low", "Things", 1994, "Five", 5, 880, &t5);
  add_track(&a, "Low", "Things", 1994, "Six", 6, 990, &t6);
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  CHECK(pair(&c, &a) == MC_PAIR_PAIRED);
  set_profile(&a, b.uid, 64, NULL);
  set_profile(&a, c.uid, 0, NULL);
  mc_sync_result ra, rx;
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK);
  CHECK(rx.files_received == 6);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(rx.files_received == 6);
  CHECK(problems_are(&a, 0, 0, 0));

  /* An imported original gone from disk: a check only notes it, and its row stays. Nobody is
   * offered it, and the next session fetches it from a member that has it, under its old name
   * with the row replaced. */
  mc_verify_result v;
  abs_path(&a, t1.rel, p, sizeof p);
  CHECK(mc_remove(p) == MC_OK);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_FAST, &v) == MC_OK);
  CHECK(v.failed == 1 && v.deleted == 0);
  CHECK(failed_state(&a, &t1) == MC_CHECK_MISSING && file_rows(&a, &t1) == 1);
  CHECK(problems_are(&a, 1, 0, 0));
  {
    char text[256];
    mc_sync_problems *q = NULL;
    CHECK(mc_sync_get_problems(a.sync, &q) == MC_OK);
    mc_sync_problems_text(q, text, sizeof text);
    CHECK_TEXT(text, "1 file here is missing or damaged.");
    mc_sync_problems_free(q);
  }
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(ra.files_received == 1 && mc_file_exists(p));
  CHECK(file_rows(&a, &t1) == 1 && failed_state(&a, &t1) == MC_CHECK_OK);
  CHECK(problems_are(&a, 0, 0, 0));

  /* Bit rot, which only a deep check sees. It holds through fast checks, and goes the same way. */
  age_files(&a);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  CHECK(v.failed == 0);
  rot(&a, t2.rel);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_FAST, &v) == MC_OK);
  CHECK(v.failed == 0 && failed_state(&a, &t2) == MC_CHECK_OK);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  CHECK(v.failed == 1 && v.deleted == 0 && failed_state(&a, &t2) == MC_CHECK_CORRUPT);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_FAST, &v) == MC_OK);
  CHECK(v.failed == 1 && failed_state(&a, &t2) == MC_CHECK_CORRUPT);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(ra.files_received == 1 && file_rows(&a, &t2) == 1 && failed_state(&a, &t2) == MC_CHECK_OK);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  CHECK(v.failed == 0);

  /* A file only touched: the deep check takes its new time. One really written to may be the
   * user's: it's reported, nothing fetches over it, and it's never lost. */
  age_files(&a);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  abs_path(&a, t3.rel, p, sizeof p);
  {
    struct utimbuf u;
    u.actime = u.modtime = 1100000000;
    CHECK(utime(p, &u) == 0);
  }
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  CHECK(v.failed == 0 && failed_state(&a, &t3) == MC_CHECK_OK);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_FAST, &v) == MC_OK);
  CHECK(v.failed == 0);

  /* The rest is about what's missing for good. No device has Four but B's lossy copy: A takes it,
   * and it isn't lost. */
  lose(&a, &t4);
  lose(&c, &t4);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(ra.files_received == 0);
  CHECK(problems_are(&a, 1, 0, 0));
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK);
  CHECK(ra.files_received == 1);
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM files WHERE track_uid = '%s' AND format = 'opus'"
           " AND source_file_uid IS NOT NULL", t4.uid);
  CHECK(lib_int(&a, sql) == 1);
  CHECK(problems_are(&a, 1, 0, 0)); /* the original's row, still gone */
  const char *four[] = {t4.uid};
  mc_delete_result dr;
  CHECK(mc_sync_remove_lost(a.sync, four, 1, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 0 && track_rows(&a, &t4) == 1);

  /* Five, gone from A and B. C hasn't been asked since, so however sure the others are it isn't
   * lost, nor can it be removed; and it was right to wait, as C has it. */
  const char *five[] = {t5.uid};
  lose(&a, &t5);
  lose(&b, &t5);
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK);
  CHECK(problems_are(&a, 2, 0, 0));
  CHECK(mc_sync_remove_lost(a.sync, five, 1, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 0 && track_rows(&a, &t5) == 1);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(ra.files_received == 1 && failed_state(&a, &t5) == MC_CHECK_OK);
  CHECK(problems_are(&a, 1, 0, 0));
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK); /* B gets its copy back */

  /* Gone everywhere and every member has said so: lost. */
  lose(&a, &t5);
  lose(&b, &t5);
  lose(&c, &t5);
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(ra.files_received == 0);
  CHECK(problems_are(&a, 1, 0, 1));
  {
    char text[256];
    mc_sync_problems *q = NULL;
    CHECK(mc_sync_get_problems(a.sync, &q) == MC_OK);
    CHECK(q && q->lost == 1 && !strcmp(q->tracks[0].uid, t5.uid));
    CHECK(q && !strcmp(q->tracks[0].title, "Five") && !strcmp(q->tracks[0].artist, "Low"));
    mc_sync_problems_text(q, text, sizeof text);
    CHECK_TEXT(text, "1 track is gone for good: no device has it. 1 file here is missing or damaged.");
    mc_sync_problems_free(q);
  }
  /* Only an archive offers it, and on B or C nothing goes whatever it's asked. */
  CHECK(problems_are(&b, 0, 0, 0));
  CHECK(mc_sync_remove_lost(b.sync, five, 1, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 0 && track_rows(&b, &t5) == 1);

  /* A member's word goes stale: a week on it may have the track, so it must say so again. */
  snprintf(sql, sizeof sql, "UPDATE peer_lacks SET said_ms = 1 WHERE device_uid = '%s'", c.uid);
  group_exec(&a, sql);
  CHECK(mc_sync_remove_lost(a.sync, five, 1, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 0 && track_rows(&a, &t5) == 1);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);

  /* A uid that isn't lost is left out of a list that has one that is. */
  const char *both[] = {t1.uid, t5.uid};
  CHECK(mc_sync_remove_lost(a.sync, both, 2, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 1 && dr.tracks_kept == 0);
  CHECK(track_rows(&a, &t5) == 0 && track_rows(&a, &t1) == 1);
  CHECK(problems_are(&a, 1, 0, 0));
  /* And it's gone from every device. */
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(track_rows(&b, &t5) == 0 && track_rows(&c, &t5) == 0);
  CHECK(track_rows(&b, &t1) == 1 && track_rows(&c, &t1) == 1);

  /* An original imported here keeps its row: if its file comes back after the card was worked
   * out, it stays, whatever the user pressed. Removing it with the file still gone works. */
  const char *six[] = {t6.uid};
  lose(&a, &t6);
  lose(&b, &t6);
  lose(&c, &t6);
  CHECK(sync_now(&a, &b, &ra, &rx) == MC_OK);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(failed_state(&a, &t6) == MC_CHECK_MISSING);
  CHECK(problems_are(&a, 1, 0, 1));
  abs_path(&a, t6.rel, p, sizeof p);
  CHECK(write_bytes(p, "back"));
  CHECK(mc_sync_remove_lost(a.sync, six, 1, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 0 && track_rows(&a, &t6) == 1);
  CHECK(mc_remove(p) == MC_OK);
  CHECK(mc_sync_remove_lost(a.sync, six, 1, &dr) == MC_OK);
  CHECK(dr.tracks_deleted == 1 && track_rows(&a, &t6) == 0);
  CHECK(problems_are(&a, 1, 0, 0));

  /* Unknown files an archive only reports go on the card, and off it once they've gone. */
  snprintf(p, sizeof p, "%s/stray.txt", a.libdir);
  CHECK(write_bytes(p, "stray"));
  make_old(p);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(ra.files_unknown == 1 && mc_file_exists(p));
  CHECK(problems_are(&a, 1, 1, 0));
  CHECK(mc_remove(p) == MC_OK);
  CHECK(sync_now(&a, &c, &ra, &rx) == MC_OK);
  CHECK(problems_are(&a, 1, 0, 0));

  /* The weekly check goes on from where it stopped: rot behind that point isn't seen until the
   * next run, which starts over. The week's up only when one has been through it all. */
  int done = 0;
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  CHECK(group_int(&a, "SELECT deep_check_ms IS NOT NULL AND deep_check_at IS NULL FROM local") == 1);
  CHECK(mc_sync_verify_weekly(a.sync, 0, &done) == MC_OK);
  CHECK(done == 1);
  char first[MC_UID_STRLEN], rel[1024];
  query_text(mc_library_db(a.lib),
             "SELECT track_uid FROM files WHERE track_uid IN (SELECT track_uid FROM files"
             " WHERE sha256 IS NOT NULL) ORDER BY track_uid", first, sizeof first);
  snprintf(sql, sizeof sql, "SELECT path FROM files WHERE track_uid = '%s'", first);
  query_text(mc_library_db(a.lib), sql, rel, sizeof rel);
  age_files(&a);
  CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
  rot(&a, rel);
  snprintf(sql, sizeof sql, "UPDATE local SET deep_check_ms = NULL, deep_check_at = '%s'", first);
  group_exec(&a, sql);
  CHECK(mc_sync_verify_weekly(a.sync, 0, &done) == MC_OK);
  CHECK(done == 1);
  /* A copy that fails goes with its row, an original imported here is noted. */
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files f WHERE f.track_uid = '%s' AND NOT EXISTS"
                            " (SELECT 1 FROM file_failed x WHERE x.file_uid = f.uid)", first);
  CHECK(lib_int(&a, sql) == 1);
  group_exec(&a, "UPDATE local SET deep_check_ms = 0");
  CHECK(mc_sync_verify_weekly(a.sync, 0, &done) == MC_OK);
  CHECK(done == 1 && lib_int(&a, sql) == 0);
  CHECK(group_int(&a, "SELECT deep_check_ms > 0 FROM local") == 1);
  /* Out of time before it starts: it stops, and says it isn't over. */
  group_exec(&a, "UPDATE local SET deep_check_ms = 0");
  CHECK(mc_sync_verify_weekly(a.sync, 1, &done) == MC_OK);
  mc_sleep_ms(5);
  if (!done) CHECK(group_int(&a, "SELECT deep_check_ms = 0 FROM local") == 1);

  dev_stop(&a);
  dev_stop(&b);
  dev_stop(&c);
  remove_tree(root);
  return TEST_RESULT();
}
