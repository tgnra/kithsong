/* Where transcodes come from: only ever the track's original, whatever its format, never
 * another transcode. Two devices over loopback, as in test_sync_files.c: A is the archive
 * and serves, B wants Opus. A holds Lossless as a WAV original, and of Lossy at first only a
 * 256 kbps transcode another device made of its MP3 original.
 *
 *   - B at 128 kbps: Lossless is transcoded from its original; Lossy is not made from the
 *     256 kbps copy. It stays missing, which is no error: nothing failed or was corrupt.
 *   - B at 256 kbps: the 256 kbps copy is the very format B wants, so it is sent as it is.
 *   - B at 128 kbps again: still nothing for Lossy, and B keeps its 256 kbps copy meanwhile.
 *   - A gets Lossy's MP3 original (a lossy original is still the original, and too big to
 *     stand in for 128 kbps): B's 128 kbps copy is transcoded from that MP3, and the 256
 *     kbps copy goes. The 256 kbps copy was made from a longer recording, so the length of
 *     what B receives tells which file it was made from. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "musiccore/transcode.h"
#include "playback/decoder.h"
#include "sync/plan.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define MAX_EVENTS 64
#define TRACK_MS 1500
#define LONG_MS 3000 /* what the 256 kbps copy was made from */

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

/* A 320 kbps mono MP3 of silence, about ms long: MPEG-1 Layer III frames at 44.1 kHz whose
 * side info and main data are all zero. 1044 bytes per frame of 1152 samples. */
static int write_mp3(const char *path, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  static const unsigned char head[] = {0xFF, 0xFB, 0xE0, 0xC0};
  unsigned frames = (unsigned)((uint64_t)ms * 44100 / 1000 / 1152) + 1;
  for (unsigned i = 0; i < frames; i++) {
    fwrite(head, 1, sizeof head, f);
    for (int b = (int)sizeof head; b < 1044; b++) fputc(0, f);
  }
  return fclose(f) == 0;
}

/* hex SHA-256 of a file, "" if it can't be read. */
static void file_sha(const char *path, char out[65]) {
  out[0] = '\0';
  FILE *f = mc_fopen(path, "rb");
  if (!f) return;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  unsigned char buf[8192], digest[32];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&ctx, buf, n);
  fclose(f);
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

/* The length of an audio file in ms, as the player's decoder reads it; -1 if it can't. */
static int64_t audio_ms(const char *path) {
  mc_decoder *d = NULL;
  if (mc_decoder_open(path, &d) != MC_OK) return -1;
  uint64_t frames = mc_decoder_length(d);
  unsigned rate = mc_decoder_rate(d);
  mc_decoder_close(d);
  return rate ? (int64_t)(frames * 1000 / rate) : -1;
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

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL,
                                        on_sync_done, NULL, NULL, NULL, NULL, NULL};

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
  CHECK(mc_sync_confirm_now(x->sync, y->uid) == MC_OK);
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
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
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

/* <config>/transcode-cache/<file_uid>[/opus-<kbps>.opus] on d; kbps 0 for the folder. */
static void cache_file(dev *d, const char *file_uid, int kbps, char *out, size_t cap) {
  if (kbps)
    snprintf(out, cap, "%s/transcode-cache/%s/opus-%d.opus", d->config, file_uid, kbps);
  else
    snprintf(out, cap, "%s/transcode-cache/%s", d->config, file_uid);
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

/* B's file of track, with its row's bitrate and source; "" if it has none. */
static void b_file(dev *b, const char *track, char *rel, size_t cap, int *kbps, char *source,
                   size_t source_cap) {
  char sql[512];
  snprintf(sql, sizeof sql, "SELECT path FROM files WHERE track_uid = '%s' ORDER BY path", track);
  lib_text(b, sql, rel, cap);
  snprintf(sql, sizeof sql, "SELECT bitrate_kbps FROM files WHERE track_uid = '%s'", track);
  *kbps = (int)lib_int(b, sql);
  snprintf(sql, sizeof sql, "SELECT source_file_uid FROM files WHERE track_uid = '%s'", track);
  lib_text(b, sql, source, source_cap);
}

static int64_t track_files(dev *d, const char *track) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE track_uid = '%s'", track);
  return lib_int(d, sql);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], p[2048], q[2048], sql[2048];
  snprintf(root, sizeof root, "%s/test_sync_lossy.d", scratch);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED); /* A asked: A is the new group's archive */

  /* Lossless: a WAV original on A. */
  char lossless[MC_UID_STRLEN], lossless_file[MC_UID_STRLEN];
  const char *lossless_rel = "Low/Things (1994)/01 Lossless.wav";
  add_track(&a, "Lossless", 1, lossless);
  mc_uid_new(lossless_file);
  snprintf(p, sizeof p, "%s/Low/Things (1994)", a.libdir);
  CHECK(mc_mkdirs(p) == MC_OK);
  abs_path(&a, lossless_rel, p, sizeof p);
  CHECK(write_wav(p, TRACK_MS, 440));
  add_file(&a, lossless_file, lossless, lossless_rel, "wav", 1411, NULL, NULL);

  /* Lossy: its original is an MP3 on a third device, C, that isn't here. A holds only the
   * 256 kbps transcode C made of it (made here from a longer recording, to tell it apart). */
  char lossy[MC_UID_STRLEN], lossy_orig[MC_UID_STRLEN], lossy_copy[MC_UID_STRLEN],
    c_uid[MC_UID_STRLEN];
  const char *copy_rel = "Low/Things (1994)/02 Lossy.opus";
  add_track(&a, "Lossy", 2, lossy);
  mc_uid_new(lossy_orig);
  mc_uid_new(lossy_copy);
  mc_uid_new(c_uid);
  {
    char wav[1200], cache[1200], *made = NULL;
    snprintf(wav, sizeof wav, "%s/long.wav", root);
    snprintf(cache, sizeof cache, "%s/c-cache", root);
    CHECK(write_wav(wav, LONG_MS, 330));
    mc_transcode_opts opts = {MC_FORMAT_OPUS, 256};
    CHECK(mc_transcode_cached(cache, lossy_orig, wav, &opts, &made) == MC_OK);
    abs_path(&a, copy_rel, p, sizeof p);
    CHECK(made && mc_copy_file(made, p, 0) == MC_OK);
    free(made);
  }
  add_file(&a, lossy_copy, lossy, copy_rel, "opus", 256, lossy_orig, c_uid);
  char copy_sha[65];
  abs_path(&a, copy_rel, p, sizeof p);
  file_sha(p, copy_sha);
  CHECK(copy_sha[0]);

  mc_sync_result ra, rb;
  char rel[512], source[MC_UID_STRLEN];
  int kbps = 0;

  /* B at 128 kbps. Lossless is transcoded from its WAV. Lossy: the only copy at hand is a
   * transcode at another bitrate, which is never encoded again. It stays missing, and
   * nothing failed. */
  set_opus(&a, b.uid, 128);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 1 && rb.files_missing == 1 && rb.files_corrupt == 0);
  CHECK(ra.files_sent == 1);
  b_file(&b, lossless, rel, sizeof rel, &kbps, source, sizeof source);
  CHECK(kbps == 128 && !strcmp(source, lossless_file));
  cache_file(&a, lossless_file, 128, p, sizeof p);
  CHECK(mc_file_exists(p));
  CHECK(track_files(&b, lossy) == 0);
  cache_file(&a, lossy_orig, 0, p, sizeof p); /* nothing was encoded for Lossy */
  CHECK(!mc_file_exists(p));
  cache_file(&a, lossy_copy, 0, p, sizeof p);
  CHECK(!mc_file_exists(p));
  abs_path(&a, copy_rel, p, sizeof p);
  file_sha(p, q);
  CHECK(!strcmp(q, copy_sha)); /* A's copy is as it was */
  CHECK(track_files(&a, lossy) == 1);

  /* B at 256 kbps: A's copy is just what B wants, so it goes as it is, byte for byte. */
  set_opus(&a, b.uid, 256);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 2 && rb.files_missing == 0 && rb.files_corrupt == 0);
  b_file(&b, lossy, rel, sizeof rel, &kbps, source, sizeof source);
  CHECK(kbps == 256 && !strcmp(source, lossy_orig));
  abs_path(&b, rel, p, sizeof p);
  file_sha(p, q);
  CHECK(!strcmp(q, copy_sha));
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM files WHERE track_uid = '%s' AND source_peer_uid = '%s'"
           " AND format = 'opus'",
           lossy, a.uid);
  CHECK(lib_int(&b, sql) == 1);
  cache_file(&a, lossy_orig, 0, p, sizeof p);
  CHECK(!mc_file_exists(p));

  /* B at 128 kbps again. Neither A's copy nor B's own is a source: Lossy stays missing, and
   * B keeps its 256 kbps copy, so the track stays playable. */
  set_opus(&a, b.uid, 128);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 1 && rb.files_missing == 1 && rb.files_corrupt == 0);
  b_file(&b, lossy, rel, sizeof rel, &kbps, source, sizeof source);
  CHECK(track_files(&b, lossy) == 1 && kbps == 256);
  cache_file(&a, lossy_orig, 0, p, sizeof p);
  CHECK(!mc_file_exists(p));
  {
    /* sync_status as B reports it to the group (written at once by a check). */
    mc_verify_result v;
    CHECK(mc_sync_verify(b.sync, MC_VERIFY_FAST, &v) == MC_OK);
    CHECK(v.failed == 0 && v.deleted == 0);
    mc_sync_db *sdb = NULL;
    int64_t missing = -1, corrupt = -1;
    if (mc_sync_db_open(b.config, &sdb) == MC_OK) {
      sqlite3_stmt *stmt;
      if (sqlite3_prepare_v2(mc_sync_db_handle(sdb),
                             "SELECT missing, corrupt FROM sync_status WHERE device_uid = ?1", -1,
                             &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, b.uid, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
          missing = sqlite3_column_int64(stmt, 0);
          corrupt = sqlite3_column_int64(stmt, 1);
        }
        sqlite3_finalize(stmt);
      }
      mc_sync_db_close(sdb);
    }
    CHECK(missing == 1 && corrupt == 0); /* waiting, not damaged */
  }

  /* The MP3 original reaches A. It is lossy, but the original, and bigger than Opus at
   * 128 kbps would be, so it is transcoded, once, from the MP3 itself; B's 256 kbps copy then
   * goes. */
  {
    const char *mp3_rel = "Low/Things (1994)/02 Lossy.mp3";
    abs_path(&a, mp3_rel, p, sizeof p);
    CHECK(write_mp3(p, TRACK_MS));
    int64_t size = 0, mtime = 0;
    CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
    CHECK(!mc_plan_keep_lossy("mp3", 320, size, TRACK_MS, 128));
    int64_t ms = audio_ms(p);
    CHECK(ms > TRACK_MS - 200 && ms < TRACK_MS + 200);
    add_file(&a, lossy_orig, lossy, mp3_rel, "mp3", 320, NULL, c_uid);
  }
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 1 && rb.files_missing == 0 && rb.files_corrupt == 0);
  CHECK(rb.files_deleted == 1);
  CHECK(track_files(&b, lossy) == 1);
  b_file(&b, lossy, rel, sizeof rel, &kbps, source, sizeof source);
  CHECK(kbps == 128 && !strcmp(source, lossy_orig));
  cache_file(&a, lossy_orig, 128, p, sizeof p);
  CHECK(mc_file_exists(p));
  abs_path(&b, rel, q, sizeof q);
  {
    int64_t ms = audio_ms(q); /* the MP3's length, not the 256 kbps copy's */
    CHECK(ms > TRACK_MS - 200 && ms < TRACK_MS + 200);
    char cached_sha[65], got_sha[65];
    file_sha(p, cached_sha);
    file_sha(q, got_sha);
    CHECK(cached_sha[0] && !strcmp(cached_sha, got_sha));
  }

  /* Settled. */
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_missing == 0 && rb.files_deleted == 0);

  dev_stop(&a);
  dev_stop(&b);
  mc_remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
