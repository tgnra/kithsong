/* Transcoding for others as the platform reports it (mc_sync_set_power()). Two devices over
 * loopback, as in test_sync_lossy.c: A is a phone and the archive, holding WAV originals; B
 * is a desktop wanting Opus.
 *
 *   - A on battery, not allowed (a phone's default): B gets no Opus, and the tracks are
 *     missing, not failed; B's copy of A's capabilities says it may not transcode.
 *   - A allowed on battery: B gets every track as Opus, encoded on A.
 *   - Withdrawn for a moment mid-way, with one encode at a time and slowed down, as leaving a
 *     Sync page on battery and coming back does: the encode-ahead the withdrawal cancelled is
 *     queued again, so later files don't each wait for an encode of their own.
 *   - Withdrawn mid-way, with A's encodes slowed down so the transfer is caught after its
 *     first file: B keeps what arrived whole, and the rest are missing, not corrupt, with no
 *     part file left behind. A reported transcoding while it ran, and stopped. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "kithcore/transcode.h"
#include "library/library_internal.h"
#include "sync/encode.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define MAX_EVENTS 64
#define TRACK_MS 1500
#define TRACKS 6

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
  int transcoding_on, transcoding_off;
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

static void on_transcoding(void *user, int active) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  if (active) d->transcoding_on++;
  else d->transcoding_off++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done,
                                        NULL, NULL, NULL, on_transcoding, NULL, NULL, NULL};

static int dev_start(dev *d, const char *root, const char *label, mc_device_kind kind) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, d->label, kind, &k_events, d, &d->sync) ==
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

static void set_opus(dev *editor, const char *uid, int kbps) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  p->sync_all = 1;
  p->format.format = MC_FORMAT_OPUS;
  p->format.bitrate_kbps = kbps;
  p->budget_enabled = 0;
  if (p->format.format == MC_FORMAT_AUTO) p->format.format = MC_FORMAT_ORIGINAL;
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
  /* The device's user chose it. */
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

/* A files row for a file already at rel; source_file and peer NULL for an original
 * imported here. */
static void add_file(dev *d, const char *file_uid, const char *track, const char *rel,
                     const char *format, int kbps, const char *source_file, const char *peer) {
  char p[2048], sql[2048];
  abs_path(d, rel, p, sizeof p);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms,"
           " source_file_uid, source_peer_uid)"
           " VALUES ('%s', '%s', '%s', '%s', %d, %lld, %lld, %s%s%s, %s%s%s)",
           file_uid, track, rel, format, kbps, (long long)size, (long long)mtime,
           source_file ? "'" : "", source_file ? source_file : "NULL", source_file ? "'" : "",
           peer ? "'" : "", peer ? peer : "NULL", peer ? "'" : "");
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  CHECK(exec(mc_library_db(d->lib), sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}


/* B's copy of A's capabilities: can_transcode, -1 if there's no row. */
static int caps_transcode(dev *b, const char *uid) {
  mc_sync_db *sdb = NULL;
  int v = -1;
  if (mc_sync_db_open(b->config, &sdb) != MC_OK) return -1;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                         "SELECT can_transcode FROM capabilities WHERE device_uid = ?1", -1, &stmt,
                         NULL) == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, uid, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  mc_sync_db_close(sdb);
  return v;
}

static int64_t opus_files(dev *d, int kbps) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE format = 'opus' AND bitrate_kbps = %d",
           kbps);
  return lib_int(d, sql);
}

/* Part files anywhere under dir. */
static int parts;
static int on_part(void *user, const char *path) {
  (void)user;
  size_t n = strlen(path);
  if (n > 5 && !strcmp(path + n - 5, ".part")) parts++;
  return 0;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], p[2048];
  snprintf(root, sizeof root, "%s/test_sync_power.d", scratch);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  static dev a, b;
  if (!dev_start(&a, root, "a", MC_DEVICE_PHONE) || !dev_start(&b, root, "b", MC_DEVICE_DESKTOP))
    return TEST_RESULT();
  CHECK(pair(&b, &a) == MC_PAIR_PAIRED); /* A, the one asked, is the archive */

  snprintf(p, sizeof p, "%s/Low/Things (1994)", a.libdir);
  CHECK(mc_mkdirs(p) == MC_OK);
  for (int i = 0; i < TRACKS; i++) {
    char uid[MC_UID_STRLEN], file[MC_UID_STRLEN], title[32], rel[256];
    snprintf(title, sizeof title, "Song %d", i + 1);
    snprintf(rel, sizeof rel, "Low/Things (1994)/%02d %s.wav", i + 1, title);
    add_track(&a, title, i + 1, uid);
    mc_uid_new(file);
    abs_path(&a, rel, p, sizeof p);
    CHECK(write_wav(p, TRACK_MS, 220 + 40 * i));
    add_file(&a, file, uid, rel, "wav", 1411, NULL, NULL);
  }

  mc_sync_result ra, rb;

  /* A phone on battery, as it starts: B gets nothing, and nothing failed. */
  set_opus(&b, b.uid, 128);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_missing == TRACKS && rb.files_corrupt == 0);
  CHECK(ra.files_sent == 0);
  CHECK(caps_transcode(&b, a.uid) == 0);
  CHECK(!mc_sync_transcoding(a.sync));

  /* Allowed on battery: every track, as Opus made on A. */
  mc_sync_set_power(a.sync, 0, 1);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == TRACKS && rb.files_missing == 0 && rb.files_corrupt == 0);
  CHECK(ra.files_sent == TRACKS);
  CHECK(caps_transcode(&b, a.uid) == 1);
  CHECK(opus_files(&b, 128) == TRACKS);
  {
    uint64_t end = mc_mono_ms() + WAIT_MS;
    mc_mutex_lock(&a.m);
    while ((!a.transcoding_on || a.transcoding_off < a.transcoding_on) && mc_mono_ms() < end)
      mc_cond_wait_ms(&a.c, &a.m, 100);
    CHECK(a.transcoding_on >= 1 && a.transcoding_off == a.transcoding_on);
    mc_mutex_unlock(&a.m);
  }

  /* Withdrawn for a moment once the first file has landed. At 64 kbps everything is to be
   * made again, one encode at a time, so most of the encode-ahead is still queued then. The
   * file being encoded may go missing, and the next one asked for has lost its job; every one
   * after that finds its job queued again, rather than encoding when it's asked for. */
  {
    mc_encode_test_delay_ms = 300;
    mc_sync_test_pool_size = 1;
    set_opus(&b, b.uid, 64);
    mc_encode_stats st0, st1;
    mc_encode_pool_stats(a.sync->enc, &st0);
    int sb64 = done_seen(&b, a.uid), sa64 = done_seen(&a, b.uid);
    CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
    uint64_t end = mc_mono_ms() + WAIT_MS;
    while (opus_files(&b, 64) < 1 && mc_mono_ms() < end) mc_sleep_ms(5);
    mc_sync_set_power(a.sync, 0, 0);
    mc_sync_set_power(a.sync, 0, 1);
    CHECK(wait_done(&a, b.uid, sa64, &ra) == MC_OK);
    CHECK(wait_done(&b, a.uid, sb64, &rb) == MC_OK);
    mc_encode_pool_stats(a.sync->enc, &st1);
    mc_encode_test_delay_ms = 0;
    mc_sync_test_pool_size = 0;
    CHECK(rb.files_corrupt == 0);
    CHECK(rb.files_received + rb.files_missing == TRACKS);
    CHECK(rb.files_received >= TRACKS - 2);
    CHECK(st1.unasked - st0.unasked <= 1);
    if (st1.unasked - st0.unasked > 1)
      fprintf(stderr, "  %d files encoded only when asked for\n", st1.unasked - st0.unasked);
  }

  /* Withdrawn mid-way. At 96 kbps everything is to be made again, one encode at a time (with
   * one per core they'd all be done together), each taking a while, so the first file lands
   * well before the second encode is done. */
  mc_encode_test_delay_ms = 700;
  mc_sync_test_pool_size = 1;
  set_opus(&b, b.uid, 96);
  int sb = done_seen(&b, a.uid), sa = done_seen(&a, b.uid);
  CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
  {
    uint64_t end = mc_mono_ms() + WAIT_MS;
    while (opus_files(&b, 96) < 1 && mc_mono_ms() < end) mc_sleep_ms(10);
  }
  mc_sync_set_power(a.sync, 0, 0);
  CHECK(wait_done(&a, b.uid, sa, &ra) == MC_OK);
  CHECK(wait_done(&b, a.uid, sb, &rb) == MC_OK);
  mc_encode_test_delay_ms = 0;
  mc_sync_test_pool_size = 0;
  CHECK(rb.files_received >= 1 && rb.files_received < TRACKS);
  CHECK(rb.files_corrupt == 0);
  CHECK(rb.files_missing == TRACKS - rb.files_received);
  CHECK(opus_files(&b, 96) == rb.files_received);
  parts = 0;
  mc_walk(b.libdir, on_part, NULL);
  CHECK(parts == 0);
  {
    /* B checks what it holds: every copy that arrived is whole. */
    mc_verify_result v;
    CHECK(mc_sync_verify(b.sync, MC_VERIFY_DEEP, &v) == MC_OK);
    CHECK(v.failed == 0);
  }
  {
    uint64_t end = mc_mono_ms() + WAIT_MS;
    while (mc_sync_transcoding(a.sync) && mc_mono_ms() < end) mc_sleep_ms(10);
    CHECK(!mc_sync_transcoding(a.sync));
  }

  /* Still withdrawn: the next session brings nothing that would need encoding, and B now
   * knows A may not transcode. */
  int before = (int)opus_files(&b, 96);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_corrupt == 0 && rb.files_missing + (int)opus_files(&b, 96) == TRACKS);
  CHECK(caps_transcode(&b, a.uid) == 0);
  CHECK(opus_files(&b, 96) >= before);

  dev_stop(&a);
  dev_stop(&b);
  mc_remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
