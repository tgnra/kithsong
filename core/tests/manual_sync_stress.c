/* Sync with a big library, for the performance workflow (.github/workflows/perf.yml) and for
 * timing by hand: two devices over loopback in one process, as in manual_opus_sync.c, and a
 * check that what arrived is all there and intact.
 *
 *   manual_sync_stress <scratch root> <tracks> [<file kB, default 32>]
 *
 * A (the archive) gets <tracks> tracks, about 12 to an album and 40 to an artist, each with a
 * WAV of its own: noise from a fixed seed, so files differ and runs are the same. B pairs and
 * takes everything as originals. Then, each timed:
 *
 *   full         the first sync: every row and file
 *   noop         again with nothing changed: should move no files and take little time
 *   incremental  after 1% more tracks and 1% of titles changed on A: only those
 *
 * After full and incremental, B's library has to match A's (tracks, albums and artists, and
 * every file on disk at its size), and a session must report no corrupt files. Any failed check
 * exits non-zero. The figures go to stdout in the profiler's report format (kithcore/perf.h):
 * "key value" lines under [section]s, for scripts/perf/compare.py. Run it on a Release build:
 * see the sync-profiling notes. The root is deleted first. */
#define _POSIX_C_SOURCE 200809L /* clock_gettime() under -std=c11 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "kithcore/perf.h"
#include "library/library_internal.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS (60 * 60000)
#define MAX_EVENTS 64
#define TRACKS_PER_ALBUM 12
#define TRACKS_PER_ARTIST 40

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
  int codes;
  int results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
} dev;

/* --- Two devices, as manual_opus_sync.c --- */

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
  if (d->ndone == MAX_EVENTS) { /* only the latest matter */
    memmove(d->done, d->done + 1, sizeof d->done - sizeof d->done[0]);
    d->ndone--;
  }
  done_event *e = &d->done[d->ndone++];
  snprintf(e->peer, sizeof e->peer, "%s", r->device_uid);
  e->r = *r;
  e->r.device_uid = NULL;
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
  CHECK(mc_sync_new(d->lib, d->config, d->label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) == MC_OK);
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

/* Events from peer so far (they're kept, the oldest giving way: this counts what's kept, so
 * waiters compare against the latest's position, not a total). Under d->m. */
static int count_done(dev *d, const char *peer) {
  int n = 0;
  for (int i = 0; i < d->ndone; i++)
    if (!strcmp(d->done[i].peer, peer)) n++;
  return n;
}

static int done_seen(dev *d, const char *peer) {
  mc_mutex_lock(&d->m);
  int n = d->ndone ? count_done(d, peer) : 0;
  mc_mutex_unlock(&d->m);
  return n;
}

/* Waits for a session with peer after the seen-th, and gives its result. A full queue can't
 * happen here: sessions are only on request, and each is waited for. */
static mc_status wait_done(dev *d, const char *peer, int seen, mc_sync_result *r) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_status st = MC_ERR_NET; /* none came in time */
  if (count_done(d, peer) > seen)
    for (int i = d->ndone - 1; i >= 0; i--)
      if (!strcmp(d->done[i].peer, peer)) {
        st = d->done[i].r.status;
        if (r) *r = d->done[i].r;
        break;
      }
  mc_mutex_unlock(&d->m);
  return st;
}

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
  mc_mutex_lock(&y->m);
  while (y->codes <= codes && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 100);
  char code[MC_PAIR_CODE_LEN + 1];
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_pair_reason r = (mc_pair_reason)-1;
  mc_mutex_lock(&x->m);
  while (x->results <= rx && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 100);
  if (x->results > rx) r = x->reason;
  mc_mutex_unlock(&x->m);
  mc_mutex_lock(&y->m);
  while (y->results <= ry && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 100);
  mc_mutex_unlock(&y->m);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx, NULL) == MC_OK);
    CHECK(wait_done(y, x->uid, sy, NULL) == MC_OK);
  }
  return r;
}

/* --- A's library --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A mono 16-bit WAV of about kb kilobytes of noise from seed: every track's differs. */
static int write_wav(const char *path, int kb, uint32_t seed) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t bytes = (uint32_t)kb * 1024u & ~1u, rate = 22050;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, rate);
  put32(f, rate * 2);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  uint32_t x = seed * 2654435761u + 1;
  for (uint32_t i = 0; i < bytes / 2; i++) {
    x ^= x << 13; /* xorshift32 */
    x ^= x >> 17;
    x ^= x << 5;
    put16(f, (x >> 8) & 0x0fff); /* quiet */
  }
  return fclose(f) == 0;
}

typedef struct seeder {
  sqlite3_stmt *artist, *album, *track, *file;
  char artist_uid[MC_UID_STRLEN], album_uid[MC_UID_STRLEN];
} seeder;

static int prepare(sqlite3 *db, const char *sql, sqlite3_stmt **s) {
  int rc = sqlite3_prepare_v2(db, sql, -1, s, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc == SQLITE_OK;
}

static int step(sqlite3 *db, sqlite3_stmt *s) {
  int rc = sqlite3_step(s);
  if (rc != SQLITE_DONE) fprintf(stderr, "sql failed: %s\n", sqlite3_errmsg(db));
  sqlite3_reset(s);
  return rc == SQLITE_DONE;
}

/* The uid of the row sql (one ? for name) finds, into out; 0 if none. */
static int find_uid(sqlite3 *db, const char *sql, const char *name, char out[MC_UID_STRLEN]) {
  sqlite3_stmt *s = NULL;
  int found = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
    sqlite3_bind_text(s, 1, name, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_text(s, 0)) {
      snprintf(out, MC_UID_STRLEN, "%s", (const char *)sqlite3_column_text(s, 0));
      found = 1;
    }
  }
  sqlite3_finalize(s);
  return found;
}

/* Tracks first to first + n - 1 of A's library, with their albums, artists and files, in one
 * write. Track i is on album i / 12 and by artist i / 40; a batch starting part-way through one
 * adds to the album or artist already there, as two of the same name would be one on B. */
static int seed(dev *a, int first, int n, int kb) {
  sqlite3 *db = mc_library_db(a->lib);
  seeder sd;
  memset(&sd, 0, sizeof sd);
  int ok = prepare(db, "INSERT INTO artists (uid, name) VALUES (?, ?)", &sd.artist) &&
           prepare(db, "INSERT INTO albums (uid, title, artist_uid, year) VALUES (?, ?, ?, ?)", &sd.album) &&
           prepare(db,
                   "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
                   " VALUES (?, ?, ?, ?, ?, ?)",
                   &sd.track) &&
           prepare(db,
                   "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
                   " VALUES (?, ?, ?, 'wav', 352, ?, ?)",
                   &sd.file);
  ok = ok && mc_library_begin_write(a->lib) == MC_OK;
  int duration_ms = (int)((int64_t)kb * 1024 * 1000 / (22050 * 2));
  for (int i = first; ok && i < first + n; i++) {
    char name[64], rel[256], path[1400], dir[1400], uid[MC_UID_STRLEN], file_uid[MC_UID_STRLEN];
    int artist = i / TRACKS_PER_ARTIST, album = i / TRACKS_PER_ALBUM, no = i % TRACKS_PER_ALBUM + 1;
    snprintf(name, sizeof name, "Artist %05d", artist);
    if (i == first && i % TRACKS_PER_ARTIST && find_uid(db, "SELECT uid FROM artists WHERE name = ?", name, sd.artist_uid)) {
      /* on with the one there */
    } else if (i == first || i % TRACKS_PER_ARTIST == 0) {
      mc_uid_new(sd.artist_uid);
      sqlite3_bind_text(sd.artist, 1, sd.artist_uid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(sd.artist, 2, name, -1, SQLITE_TRANSIENT);
      ok = step(db, sd.artist);
    }
    snprintf(name, sizeof name, "Album %05d", album);
    if (i == first && i % TRACKS_PER_ALBUM && find_uid(db, "SELECT uid FROM albums WHERE title = ?", name, sd.album_uid)) {
      /* on with the one there */
    } else if (ok && (i == first || i % TRACKS_PER_ALBUM == 0)) {
      mc_uid_new(sd.album_uid);
      sqlite3_bind_text(sd.album, 1, sd.album_uid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(sd.album, 2, name, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(sd.album, 3, sd.artist_uid, -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(sd.album, 4, 1970 + album % 50);
      ok = step(db, sd.album);
    }
    if (!ok) break;
    snprintf(dir, sizeof dir, "%s/Artist %05d/Album %05d", a->libdir, artist, album);
    snprintf(rel, sizeof rel, "Artist %05d/Album %05d/%02d Track %06d.wav", artist, album, no, i);
    snprintf(path, sizeof path, "%s/%s", a->libdir, rel);
    int64_t size = 0, mtime = 0;
    ok = mc_mkdirs(dir) == MC_OK && write_wav(path, kb, (uint32_t)i) && mc_file_info(path, &size, &mtime) == MC_OK;
    if (!ok) {
      fprintf(stderr, "couldn't write %s\n", path);
      break;
    }
    mc_uid_new(uid);
    mc_uid_new(file_uid);
    snprintf(name, sizeof name, "Track %06d", i);
    sqlite3_bind_text(sd.track, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sd.track, 2, sd.album_uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sd.track, 3, sd.artist_uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sd.track, 4, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(sd.track, 5, no);
    sqlite3_bind_int(sd.track, 6, duration_ms);
    ok = step(db, sd.track);
    sqlite3_bind_text(sd.file, 1, file_uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sd.file, 2, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(sd.file, 3, rel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(sd.file, 4, size);
    sqlite3_bind_int64(sd.file, 5, mtime);
    ok = ok && step(db, sd.file);
  }
  if (ok) ok = mc_library_commit(a->lib) == MC_OK;
  else mc_library_rollback(a->lib);
  sqlite3_finalize(sd.artist);
  sqlite3_finalize(sd.album);
  sqlite3_finalize(sd.track);
  sqlite3_finalize(sd.file);
  return ok;
}

/* Renames every hundredth track of A's first n. Returns how many. */
static int rename_some(dev *a, int n) {
  sqlite3 *db = mc_library_db(a->lib);
  int count = 0;
  if (mc_library_begin_write(a->lib) != MC_OK) return -1;
  sqlite3_stmt *s = NULL;
  int ok = prepare(db, "UPDATE tracks SET title = ? WHERE title = ?", &s);
  for (int i = 0; ok && i < n; i += 100) {
    char from[32], to[48];
    snprintf(from, sizeof from, "Track %06d", i);
    snprintf(to, sizeof to, "Track %06d (renamed)", i);
    sqlite3_bind_text(s, 1, to, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, from, -1, SQLITE_TRANSIENT);
    ok = step(db, s);
    count += ok && sqlite3_changes(db) == 1;
  }
  sqlite3_finalize(s);
  if (ok) ok = mc_library_commit(a->lib) == MC_OK;
  else mc_library_rollback(a->lib);
  return ok ? count : -1;
}

/* --- Checks --- */

static int64_t count(dev *d, const char *sql) {
  sqlite3_stmt *s = NULL;
  int64_t n = -1;
  if (sqlite3_prepare_v2(mc_library_db(d->lib), sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
    n = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return n;
}

/* Whether B has what A has: the same rows, and each of its files on disk at its size. */
static int same_library(dev *a, dev *b, const char *when) {
  static const char *const counts[] = {
      "SELECT count(*) FROM tracks", "SELECT count(*) FROM albums", "SELECT count(*) FROM artists",
      "SELECT count(*) FROM tracks WHERE title LIKE '%(renamed)'"};
  int ok = 1;
  for (size_t i = 0; i < sizeof counts / sizeof counts[0]; i++) {
    int64_t x = count(a, counts[i]), y = count(b, counts[i]);
    if (x != y || x < 0) {
      fprintf(stderr, "%s: A and B differ: %s gives %lld and %lld\n", when, counts[i], (long long)x, (long long)y);
      ok = 0;
    }
  }
  sqlite3_stmt *s = NULL;
  int files = 0, bad = 0;
  if (sqlite3_prepare_v2(mc_library_db(b->lib), "SELECT path, size_bytes FROM files", -1, &s, NULL) != SQLITE_OK) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", b->libdir, (const char *)sqlite3_column_text(s, 0));
    int64_t size = -1, mtime = 0;
    files++;
    if (mc_file_info(path, &size, &mtime) != MC_OK || size != sqlite3_column_int64(s, 1)) {
      if (bad++ < 5) fprintf(stderr, "%s: B's file %s is missing or the wrong size\n", when, path);
    }
  }
  sqlite3_finalize(s);
  int64_t want = count(a, "SELECT count(*) FROM files");
  if (bad || files != want) {
    fprintf(stderr, "%s: B has %d of A's %lld files, %d of them missing or wrong\n", when, files, (long long)want, bad);
    ok = 0;
  }
  return ok;
}

/* --- The report --- */

typedef struct reading {
  const char *name;
  double at_ms;
  mc_perf_memory m;
} reading;

static reading g_readings[16];
static int g_nreadings;
static uint64_t g_t0;

static void take_reading(const char *name) {
  if (g_nreadings == (int)(sizeof g_readings / sizeof g_readings[0])) return;
  reading *r = &g_readings[g_nreadings++];
  r->name = name;
  r->at_ms = (double)(mc_perf_now_us() - g_t0) / 1000.0;
  mc_perf_memory_now(&r->m);
}

static double ms_since(uint64_t t0_us) {
  return (double)(mc_perf_now_us() - t0_us) / 1000.0;
}

static void phase(const char *name, double ms, const mc_sync_result *ra, const mc_sync_result *rb) {
  printf("\n[sync %s]\n", name);
  printf("ms %.0f\n", ms);
  printf("files %d\n", rb->files_received);
  printf("corrupt %d\n", rb->files_corrupt + ra->files_corrupt);
  printf("missing %d\n", rb->files_missing);
  printf("rows %lld\n", (long long)(rb->rows_received + ra->rows_received));
  printf("mb %.1f\n", (double)rb->bytes_received / 1048576.0);
  printf("mb_per_s %.1f\n", ms > 0 ? (double)rb->bytes_received / 1048576.0 / (ms / 1000.0) : 0.0);
  printf("converged %d\n", ra->converged && rb->converged);
  fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: manual_sync_stress <scratch root> <tracks> [<file kB>]\n");
    return 2;
  }
  const char *root = argv[1];
  int tracks = atoi(argv[2]), kb = argc > 3 ? atoi(argv[3]) : 32;
  if (tracks < 100 || kb < 1) {
    fprintf(stderr, "at least 100 tracks and 1 kB a file\n");
    return 2;
  }
  int more = tracks / 100;
  g_t0 = mc_perf_now_us();
  CHECK(mc_init() == MC_OK);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  printf("# Kithsong sync stress\n\n[run]\n");
  printf("version %s\n", mc_version());
#ifdef _WIN32
  printf("platform windows\n");
#else
  printf("platform linux\n");
#endif
#ifdef NDEBUG
  printf("build release\n");
#else
  printf("build debug\n");
#endif
  printf("cpus %d\n", mc_cpu_count());
  printf("tracks %d\n", tracks);
  printf("file_kb %d\n", kb);
  fflush(stdout);

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  take_reading("started");
  uint64_t t = mc_perf_now_us();
  CHECK(pair(&b, &a) == MC_PAIR_PAIRED); /* B asks, so A is the archive */
  double pair_ms = ms_since(t);

  t = mc_perf_now_us();
  if (!seed(&a, 0, tracks, kb)) {
    fprintf(stderr, "couldn't make A's library\n");
    return 1;
  }
  double seed_ms = ms_since(t);
  take_reading("seeded");
  printf("\n[setup]\npair_ms %.0f\nseed_ms %.0f\n", pair_ms, seed_ms);
  fflush(stdout);

  mc_sync_result ra, rb;
  memset(&ra, 0, sizeof ra);
  memset(&rb, 0, sizeof rb);
  t = mc_perf_now_us();
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  phase("full", ms_since(t), &ra, &rb);
  take_reading("full");
  CHECK(rb.files_received == tracks);
  CHECK(rb.files_corrupt == 0 && ra.files_corrupt == 0);
  CHECK(same_library(&a, &b, "full"));

  t = mc_perf_now_us();
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  phase("noop", ms_since(t), &ra, &rb);
  take_reading("noop");
  CHECK(rb.files_received == 0 && rb.files_deleted == 0);

  CHECK(seed(&a, tracks, more, kb));
  int renamed = rename_some(&a, tracks);
  CHECK(renamed > 0);
  t = mc_perf_now_us();
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  phase("incremental", ms_since(t), &ra, &rb);
  take_reading("incremental");
  CHECK(rb.files_received == more);
  CHECK(rb.files_corrupt == 0 && ra.files_corrupt == 0);
  CHECK(same_library(&a, &b, "incremental"));

  mc_sync_free(a.sync);
  mc_sync_free(b.sync);
  mc_library_close(a.lib);
  mc_library_close(b.lib);
  take_reading("closed");

  /* Both devices are in this process: these are the two together. */
  printf("\n[memory]\n# sample  ms_since_process_start  resident_mb  peak_mb  private_mb\n");
  for (int i = 0; i < g_nreadings; i++) {
    const reading *r = &g_readings[i];
    printf("%s %.1f %.1f %.1f %.1f\n", r->name, r->at_ms, (double)r->m.resident / 1048576.0,
           (double)r->m.peak / 1048576.0, (double)r->m.private_bytes / 1048576.0);
  }
  printf("\n[result]\nok %d\n", TEST_RESULT() == 0);
  mc_shutdown();
  return TEST_RESULT();
}
