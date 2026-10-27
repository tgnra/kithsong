/* The sync file phase: plan computation (wanted set, formats, budget mode), then two
 * devices over loopback in test_session.c style. A holds the library as WAV originals and
 * is the group's archive; B's profile decides what it fetches: originals, then Opus at
 * 64 kbps, then a selection. Also checksums, reconcile (unwanted, wrong-format, deleted and
 * unknown files, and the originals imported on B that go once the archive holds them),
 * sync_status and mc_sync_verify(). */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#ifdef _WIN32
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#include <mbedtls/sha256.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/plan.h"
#include "sync/rows.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
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
                                        on_sync_done};

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

static int lib_file_exists(dev *d, const char *rel) {
  char p[2048];
  abs_path(d, rel, p, sizeof p);
  return mc_file_exists(p);
}

/* Every file of d's files table is on disk with the recorded hash and size. */
static int files_match(dev *d) {
  sqlite3_stmt *stmt;
  int ok = 1;
  if (sqlite3_prepare_v2(mc_library_db(d->lib), "SELECT path, sha256, size_bytes FROM files", -1,
                         &stmt, NULL) != SQLITE_OK)
    return 0;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    char p[2048], sha[65];
    abs_path(d, (const char *)sqlite3_column_text(stmt, 0), p, sizeof p);
    const char *want = (const char *)sqlite3_column_text(stmt, 1);
    file_sha(p, sha);
    int64_t size = 0, mtime = 0;
    mc_file_info(p, &size, &mtime);
    if (!want || strcmp(want, sha) != 0 || size != sqlite3_column_int64(stmt, 2)) {
      fprintf(stderr, "%s: %s doesn't match its row\n", d->label, p);
      ok = 0;
    }
  }
  sqlite3_finalize(stmt);
  return ok;
}

typedef struct track_ref {
  char uid[MC_UID_STRLEN];
  char file[MC_UID_STRLEN];
  char rel[256];
} track_ref;

/* Adds a track (and its album, artist) with a WAV original at its organised path. */
static void add_track(dev *d, const char *artist, const char *album, int year, const char *title,
                      int no, double pitch, track_ref *out) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[1024];
  snprintf(sql, sizeof sql, "SELECT uid FROM artists WHERE name = '%s'", artist);
  query_text(db, sql, ar, sizeof ar);
  snprintf(sql, sizeof sql, "SELECT uid FROM albums WHERE title = '%s'", album);
  query_text(db, sql, al, sizeof al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', '%s')", ar, artist);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  if (!al[0]) {
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
           " VALUES ('%s', '%s', '%s', '%s', %d, 1500)",
           out->uid, al, ar, title, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(out->rel, sizeof out->rel, "%s/%s (%d)/%02d %s.wav", artist, album, year, no, title);
  char dir[2048], p[2048];
  snprintf(dir, sizeof dir, "%s/%s/%s (%d)", d->libdir, artist, album, year);
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

static void plan_profile(mc_sync_db *sdb, const char *me, const char *values) {
  char sql[512];
  snprintf(sql, sizeof sql,
           "INSERT OR REPLACE INTO profiles (device_uid, archive, mode, format, bitrate_kbps,"
           " budget_bytes, budget_enabled) VALUES ('%s', %s)",
           me, values);
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
}

static void check_plan(const char *root) {
  char config[1024], libdir[1024];
  snprintf(config, sizeof config, "%s/plan/config", root);
  snprintf(libdir, sizeof libdir, "%s/plan/music", root);
  mc_sync_db *sdb = NULL;
  mc_library *lib = NULL;
  CHECK(mc_sync_db_open(config, &sdb) == MC_OK);
  CHECK(mc_library_open(libdir, &lib) == MC_OK);
  if (!sdb || !lib) return;
  const char *me = mc_sync_db_device_uid(sdb);
  sqlite3 *db = mc_library_db(lib);

  /* Album one: three tracks of 100 s; album two: two of 200 s; a playlist of one track of
   * album two. Track 1 has a FLAC original of 5 MB here, track 2 a small MP3. */
  char ar[MC_UID_STRLEN], al1[MC_UID_STRLEN], al2[MC_UID_STRLEN], pl[MC_UID_STRLEN];
  char t[5][MC_UID_STRLEN], sql[1024];
  mc_uid_new(ar);
  mc_uid_new(al1);
  mc_uid_new(al2);
  mc_uid_new(pl);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO artists (uid, name) VALUES ('%s', 'A');"
           "INSERT INTO albums (uid, title, artist_uid) VALUES ('%s', 'One', '%s'), ('%s', 'Two', '%s');"
           "INSERT INTO playlists (uid, name) VALUES ('%s', 'P')",
           ar, al1, ar, al2, ar, pl);
  CHECK(exec(db, sql) == SQLITE_OK);
  for (int i = 0; i < 5; i++) {
    mc_uid_new(t[i]);
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, duration_ms)"
             " VALUES ('%s', '%s', '%s', 'T', %d)",
             t[i], i < 3 ? al1 : al2, ar, i < 3 ? 100000 : 200000);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  char e[MC_UID_STRLEN], f1[MC_UID_STRLEN], f2[MC_UID_STRLEN];
  mc_uid_new(e);
  mc_uid_new(f1);
  mc_uid_new(f2);
  snprintf(sql, sizeof sql,
           "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position)"
           " VALUES ('%s', '%s', '%s', 'a0');"
           "INSERT INTO files (uid, track_uid, path, format, size_bytes) VALUES"
           " ('%s', '%s', 'x/1.flac', 'flac', 5000000), ('%s', '%s', 'x/2.mp3', 'mp3', 1000)",
           e, pl, t[4], f1, t[0], f2, t[1]);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);

  mc_plan *p = NULL;
  /* No profile: nothing. */
  CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
  CHECK(p && p->status == MC_PLAN_NO_PROFILE && p->count == 0);
  mc_plan_free(p);
  CHECK(mc_plan_compute(sdb, lib, "bad", -1, &p) == MC_ERR_INVALID_ARG);

  /* Everything as originals: known sizes, others estimated. */
  plan_profile(sdb, me, "0, 'all', 'original', NULL, NULL, 0");
  CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
  if (p) {
    CHECK(p->status == MC_PLAN_OK && p->count == 5 && p->kbps == 0);
    const mc_plan_track *x = mc_plan_find(p, t[0]);
    CHECK(x && x->format.format == MC_FORMAT_ORIGINAL && x->est_bytes == 5000000);
    x = mc_plan_find(p, t[2]);
    CHECK(x && x->est_bytes == 100000LL * MC_PLAN_ORIGINAL_KBPS / 8 + 4096);
    for (int i = 1; i < p->count; i++)
      CHECK(strcmp(p->tracks[i - 1].track_uid, p->tracks[i].track_uid) < 0);
    CHECK(mc_plan_find(p, "nope") == NULL);
  }
  mc_plan_free(p);

  /* Opus 128: the small MP3 stays as it is. */
  plan_profile(sdb, me, "0, 'all', 'opus', 128, NULL, 0");
  CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
  if (p) {
    CHECK(p->status == MC_PLAN_OK && p->count == 5 && p->kbps == 128);
    const mc_plan_track *x = mc_plan_find(p, t[0]);
    CHECK(x && x->format.format == MC_FORMAT_OPUS && x->format.bitrate_kbps == 128 &&
          x->est_bytes == 100000LL * 128 / 8 + 4096);
    x = mc_plan_find(p, t[1]);
    CHECK(x && x->format.format == MC_FORMAT_ORIGINAL && x->est_bytes == 1000);
    CHECK(mc_plan_keep_lossy("mp3", 0, 1000, 100000, 128));
    CHECK(!mc_plan_keep_lossy("flac", 0, 1000, 100000, 128));
    CHECK(!mc_plan_keep_lossy("m4a", 0, 1000, 100000, 128)); /* might be ALAC */
    CHECK(mc_plan_keep_lossy("m4a", 256, 1000, 100000, 128));
  }
  mc_plan_free(p);

  /* A selection: album one, the playlist, and a uid the library doesn't have. */
  {
    sqlite3 *g = mc_sync_db_handle(sdb);
    char missing[MC_UID_STRLEN], u1[MC_UID_STRLEN], u2[MC_UID_STRLEN], u3[MC_UID_STRLEN];
    mc_uid_new(missing);
    mc_uid_new(u1);
    mc_uid_new(u2);
    mc_uid_new(u3);
    snprintf(sql, sizeof sql,
             "INSERT INTO profile_items (uid, device_uid, kind, ref_uid) VALUES"
             " ('%s', '%s', 'album', '%s'), ('%s', '%s', 'playlist', '%s'),"
             " ('%s', '%s', 'album', '%s')",
             u1, me, al1, u2, me, pl, u3, me, missing);
    CHECK(exec(g, sql) == SQLITE_OK);
    plan_profile(sdb, me, "0, 'selected', 'original', NULL, NULL, 0");
    CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
    CHECK(p && p->count == 4 && mc_plan_find(p, t[4]) && !mc_plan_find(p, t[3]));
    mc_plan_free(p);
    /* An archive gets everything as originals, whatever its row says. */
    plan_profile(sdb, me, "1, 'selected', 'opus', 64, 10, 1");
    CHECK(mc_plan_compute(sdb, lib, me, 0, &p) == MC_OK);
    CHECK(p && p->status == MC_PLAN_OK && p->archive && p->count == 5 && p->kbps == 0);
    mc_plan_free(p);
    CHECK(exec(g, "DELETE FROM profile_items") == SQLITE_OK);
  }

  /* Budget mode. Sizes of the set at each bitrate (one 1000-byte MP3 kept as is). */
  int64_t at[7];
  for (int l = 1; l < 7; l++) {
    int kbps = mc_opus_bitrates[l - 1];
    at[l] = 1000;
    for (int i = 0; i < 5; i++) {
      if (i == 1) continue;
      int64_t est = 0;
      mc_transcode_estimate_bytes(i < 3 ? 100000 : 200000, kbps, &est);
      at[l] += est;
    }
  }
  /* 160 fits, 192 doesn't: 160 is taken. */
  snprintf(sql, sizeof sql, "0, 'all', 'original', NULL, %lld, 1", (long long)(at[3] + 10));
  plan_profile(sdb, me, sql);
  CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
  CHECK(p && p->status == MC_PLAN_OK && p->kbps == 160 && p->total_bytes == at[3]);
  mc_plan_free(p);
  /* Free space counts too, plus what's held of the set (5,001,000 bytes here): with no free
   * space at all, only what's held makes room, enough for 64 kbps but not 96. */
  CHECK(at[6] <= 5001000 && at[5] > 5001000);
  CHECK(mc_plan_compute(sdb, lib, me, 0, &p) == MC_OK);
  CHECK(p && p->status == MC_PLAN_OK && p->kbps == 64);
  mc_plan_free(p);
  /* A budget picks the format: a row an older version wrote with Opus 128 beside it still
   * walks from originals, which fit. */
  snprintf(sql, sizeof sql, "0, 'all', 'opus', 128, %lld, 1", (long long)(at[1] * 10));
  plan_profile(sdb, me, sql);
  CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
  CHECK(p && p->status == MC_PLAN_OK && p->kbps == 0);
  mc_plan_free(p);
  /* Not even 64 fits: over budget, nothing wanted, and by how much. */
  snprintf(sql, sizeof sql, "0, 'all', 'original', NULL, %lld, 1", (long long)(at[6] - 500));
  plan_profile(sdb, me, sql);
  CHECK(mc_plan_compute(sdb, lib, me, -1, &p) == MC_OK);
  CHECK(p && p->status == MC_PLAN_OVER_BUDGET && p->count == 0 && p->shortfall_bytes == 500);
  mc_plan_free(p);

  mc_library_close(lib);
  mc_sync_db_close(sdb);
}

/* --- Transfers --- */

static int64_t b_files(dev *b, const char *where) {
  char sql[512];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM files%s%s", where ? " WHERE " : "",
           where ? where : "");
  return lib_int(b, sql);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], sql[2048];
  snprintf(root, sizeof root, "%s/test_sync_files.d", scratch);
  remove_tree(root);

  check_plan(root);

  static dev a, b;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  if (!dev_start(&a) || !dev_start(&b)) return TEST_RESULT();
  track_ref t1, t2, t3, o1;
  add_track(&a, "Low", "Things", 1994, "Song One", 1, 440, &t1);
  add_track(&a, "Low", "Things", 1994, "Song Two", 2, 550, &t2);
  add_track(&a, "Low", "Things", 1994, "Song Three", 3, 660, &t3);
  add_track(&a, "Low", "Other", 1996, "Lone", 1, 330, &o1);
  char things[MC_UID_STRLEN];
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Things'", things,
             sizeof things);

  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  /* B has no profile row yet: nothing moves. */
  CHECK(b_files(&b, NULL) == 0);

  /* B: everything, originals. They arrive byte for byte, at the same organised paths. */
  set_profile(&a, b.uid, 0, NULL);
  mc_sync_result ra, rb;
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(!ra.files_paused && !rb.files_paused);
  CHECK(rb.files_received == 4 && rb.files_missing == 0 && rb.files_corrupt == 0);
  CHECK(ra.files_sent == 4 && ra.bytes_sent == rb.bytes_received && rb.bytes_received > 0);
  CHECK(b_files(&b, NULL) == 4);
  CHECK(files_match(&b));
  {
    track_ref *all[] = {&t1, &t2, &t3, &o1};
    for (int i = 0; i < 4; i++) {
      char pa[2048], pb[2048], ha[65], hb[65];
      abs_path(&a, all[i]->rel, pa, sizeof pa);
      abs_path(&b, all[i]->rel, pb, sizeof pb);
      file_sha(pa, ha);
      file_sha(pb, hb);
      CHECK(ha[0] && !strcmp(ha, hb));
      snprintf(sql, sizeof sql,
               "SELECT count(*) FROM files WHERE track_uid = '%s' AND path = '%s'"
               " AND sha256 = '%s' AND source_file_uid IS NULL AND source_peer_uid = '%s'"
               " AND format = 'wav'",
               all[i]->uid, all[i]->rel, ha, a.uid);
      CHECK(lib_int(&b, sql) == 1);
    }
  }
  CHECK(a.files_progress > 0 && b.files_progress > 0);
  /* Nothing more to do: a second session moves nothing, and carries B's sync_status. */
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(rb.files_received == 0 && ra.files_received == 0 && rb.files_deleted == 0);
  /* A, the archive, found itself complete in the first confirmed session (the pairing one
   * fetched nothing, unconfirmed); its sync_status is written before the next group
   * exchange, so both have it now. */
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM sync_status WHERE device_uid = '%s' AND expected = 4 AND ok = 4",
           a.uid);
  CHECK(group_int(&a, sql) == 1 && group_int(&b, sql) == 1);
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM sync_status WHERE device_uid = '%s' AND expected = 4 AND ok = 4"
           " AND missing = 0 AND corrupt = 0 AND bytes > 0 AND verified_ms > 0",
           b.uid);
  CHECK(group_int(&b, sql) == 1 && group_int(&a, sql) == 1);
  {
    char g1[MC_DIGEST_STRLEN], g2[MC_DIGEST_STRLEN];
    mc_sync_db *sdb = NULL;
    g1[0] = g2[0] = '\0';
    if (mc_sync_db_open(a.config, &sdb) == MC_OK) mc_rows_vv_digest(mc_sync_db_handle(sdb), g1);
    mc_sync_db_close(sdb);
    sdb = NULL;
    if (mc_sync_db_open(b.config, &sdb) == MC_OK) mc_rows_vv_digest(mc_sync_db_handle(sdb), g2);
    mc_sync_db_close(sdb);
    CHECK(g1[0] && !strcmp(g1, g2)); /* the status write didn't unsettle the group data */
  }

  /* B: Opus at 64 kbps. A transcodes; B swaps its WAVs for Opus files. */
  set_profile(&a, b.uid, 64, NULL);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 4 && rb.files_deleted == 4 && rb.files_missing == 0);
  CHECK(b_files(&b, NULL) == 4);
  CHECK(b_files(&b, "format = 'opus' AND bitrate_kbps = 64") == 4);
  CHECK(files_match(&b));
  snprintf(sql, sizeof sql, "source_file_uid = '%s' AND source_peer_uid = '%s'", t1.file, a.uid);
  CHECK(b_files(&b, sql) == 1);
  CHECK(lib_int(&b, "SELECT count(*) FROM files WHERE path = 'Low/Things (1994)/01 Song One.opus'") == 1);
  CHECK(!lib_file_exists(&b, t1.rel));
  {
    char cached[2048];
    snprintf(cached, sizeof cached, "%s/transcode-cache/%s/opus-64.opus", a.config, t1.file);
    CHECK(mc_file_exists(cached));
  }
  CHECK(b_files(&a, NULL) == 4 && lib_file_exists(&a, t1.rel)); /* the archive keeps all */

  /* A selection: album Things only. Other's file goes, and its folder with it. */
  set_profile(&a, b.uid, 64, things);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_deleted == 1 && b_files(&b, NULL) == 3);
  {
    char dir[2048];
    snprintf(dir, sizeof dir, "%s/Low/Other (1996)", b.libdir);
    CHECK(!mc_file_exists(dir));
    snprintf(dir, sizeof dir, "%s/Low/Things (1994)", b.libdir);
    CHECK(mc_file_exists(dir));
  }

  /* Unknown files: deleted on B, only reported on the archive. New ones are left alone. */
  {
    char dir[1100], old_b[1200], new_b[1200], old_a[1200], text[1200];
    snprintf(dir, sizeof dir, "%s/Junk", b.libdir);
    CHECK(mc_mkdirs(dir) == MC_OK);
    snprintf(old_b, sizeof old_b, "%s/old.mp3", dir);
    snprintf(new_b, sizeof new_b, "%s/new.mp3", dir);
    snprintf(text, sizeof text, "%s/notes.txt", dir);
    CHECK(write_bytes(old_b, "x") && write_bytes(new_b, "y") && write_bytes(text, "z"));
    make_old(old_b);
    make_old(text);
    snprintf(dir, sizeof dir, "%s/Junk", a.libdir);
    CHECK(mc_mkdirs(dir) == MC_OK);
    snprintf(old_a, sizeof old_a, "%s/old.mp3", dir);
    CHECK(write_bytes(old_a, "x"));
    make_old(old_a);
    CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
    CHECK(!mc_file_exists(old_b) && mc_file_exists(new_b) && mc_file_exists(text));
    CHECK(rb.files_unknown == 1);
    CHECK(mc_file_exists(old_a) && ra.files_unknown == 1 && ra.files_deleted == 0);
    mc_remove(old_a);
  }

  /* A track deleted on B: the archive removes its file too. */
  {
    CHECK(mc_library_begin_write(b.lib) == MC_OK);
    snprintf(sql, sizeof sql, "DELETE FROM tracks WHERE uid = '%s'", t3.uid);
    CHECK(exec(mc_library_db(b.lib), sql) == SQLITE_OK);
    CHECK(mc_library_commit(b.lib) == MC_OK);
    CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
    CHECK(!lib_file_exists(&a, t3.rel));
    snprintf(sql, sizeof sql, "SELECT count(*) FROM files WHERE track_uid = '%s'", t3.uid);
    CHECK(lib_int(&a, sql) == 0);
    CHECK(lib_file_exists(&a, t1.rel));
  }

  /* An original imported on B, wanted there as Opus: it goes to the archive first, then B
   * swaps it for an Opus copy and deletes it once the archive holds it verified. */
  {
    track_ref m1;
    add_track(&b, "Low", "Things", 1994, "Mine", 9, 770, &m1);
    char imported[65];
    {
      char p[2048];
      abs_path(&b, m1.rel, p, sizeof p);
      file_sha(p, imported);
    }
    for (int i = 0; i < 3 && lib_file_exists(&b, m1.rel); i++)
      CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
    CHECK(!lib_file_exists(&b, m1.rel));
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM files WHERE track_uid = '%s' AND format = 'opus'"
             " AND source_file_uid IS NOT NULL",
             m1.uid);
    CHECK(lib_int(&b, sql) == 1);
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM files WHERE track_uid = '%s' AND sha256 = '%s'"
             " AND source_peer_uid = '%s' AND source_file_uid IS NULL",
             m1.uid, imported, b.uid);
    CHECK(lib_int(&a, sql) == 1);
    CHECK(files_match(&b));
  }

  /* Verification. A fast check passes; a wrong recorded hash only fails a deep one, which
   * deletes the copy; the next session fetches it again. */
  {
    mc_verify_result v;
    CHECK(mc_sync_verify(b.sync, MC_VERIFY_FAST, &v) == MC_OK);
    CHECK(v.checked == 3 && v.ok == 3 && v.failed == 0);
    snprintf(sql, sizeof sql,
             "UPDATE files SET sha256 = '%064d' WHERE track_uid = '%s'", 0, t1.uid);
    CHECK(exec(mc_library_db(b.lib), sql) == SQLITE_OK);
    CHECK(mc_sync_verify(b.sync, MC_VERIFY_FAST, &v) == MC_OK);
    CHECK(v.failed == 0);
    CHECK(mc_sync_verify(b.sync, MC_VERIFY_DEEP, &v) == MC_OK);
    CHECK(v.checked == 3 && v.failed == 1 && v.deleted == 1);
    CHECK(b_files(&b, NULL) == 2);
    snprintf(sql, sizeof sql,
             "SELECT missing FROM sync_status WHERE device_uid = '%s'", b.uid);
    CHECK(group_int(&b, sql) == 1);
    CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
    CHECK(rb.files_received == 1 && b_files(&b, NULL) == 3 && files_match(&b));

    /* A file changed on disk fails the fast check after a session, and comes back in the
     * next one. */
    char p[2048];
    snprintf(p, sizeof p, "%s/Low/Things (1994)/02 Song Two.opus", b.libdir);
    FILE *f = mc_fopen(p, "ab");
    CHECK(f != NULL);
    if (f) {
      fputs("extra", f);
      fclose(f);
    }
    CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
    CHECK(rb.files_deleted == 1 && b_files(&b, NULL) == 2);
    CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
    CHECK(rb.files_received == 1 && b_files(&b, NULL) == 3 && files_match(&b));

    /* A deep check on the archive records hashes of its own originals and deletes none. */
    CHECK(mc_sync_verify(a.sync, MC_VERIFY_DEEP, &v) == MC_OK);
    CHECK(v.failed == 0 && v.deleted == 0 && v.checked == 4);
    CHECK(lib_int(&a, "SELECT count(*) FROM files WHERE sha256 IS NULL") == 0);
    CHECK(files_match(&a));
  }
  CHECK(mc_sync_verify(NULL, MC_VERIFY_FAST, NULL) == MC_ERR_INVALID_ARG);

  dev_stop(&a);
  dev_stop(&b);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
