/* A real Opus sync to time, for opus.md: two devices over loopback, as in test_sync_lossy.c,
 * in one process. A is the archive and holds the originals; B wants Opus.
 *
 *   KITHSONG_SYNC_TRACE=1 manual_opus_sync <dir of tNN/track.*> <scratch root> <kbps> <files>
 *                                          [<cold|warm|both>]
 *
 * Originals are copied in from tNN/, with any cover beside them. Run it on a Release build,
 * and pipe stderr: see the sync-profiling notes. cold is a session with an empty transcode
 * cache; warm is the same session over again with B's files gone, from a full cache. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "musiccore/transcode.h"
#include "playback/decoder.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
#include "test.h"
#include "util/platform.h"

#ifdef _WIN32
#include <windows.h>
static double process_cpu_s(void) {
  FILETIME a, b, k, u;
  GetProcessTimes(GetCurrentProcess(), &a, &b, &k, &u);
  uint64_t kk = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime;
  uint64_t uu = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
  return (double)(kk + uu) / 1e7;
}
#else
#include <time.h>
static double process_cpu_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
  return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}
#endif

#define WAIT_MS (60 * 60000)
#define MAX_EVENTS 64

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

static int exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc;
}

/* Track no of its own album, its files row and its folder, by copying fixture tNN/. Returns its
 * size, and adds to *audio_s. */
static int64_t add_one(dev *a, const char *fixtures, int fixture, int no, double *audio_s) {
  char src[1200], rel[128], dst[2048], sql[2048];
  const char *ext = "flac";
  snprintf(src, sizeof src, "%s/t%02d/track.flac", fixtures, fixture);
  if (!mc_file_exists(src)) {
    ext = "mp3";
    snprintf(src, sizeof src, "%s/t%02d/track.mp3", fixtures, fixture);
  }
  mc_decoder *d;
  if (mc_decoder_open(src, &d) != MC_OK) return -1;
  double seconds = (double)mc_decoder_length(d) / mc_decoder_rate(d);
  mc_decoder_close(d);
  *audio_s += seconds;

  snprintf(rel, sizeof rel, "Bench/Album %02d (2000)/01 Track.%s", no, ext);
  snprintf(dst, sizeof dst, "%s/Bench/Album %02d (2000)", a->libdir, no);
  CHECK(mc_mkdirs(dst) == MC_OK);
  char cover_src[1200], cover_dst[2200];
  snprintf(cover_src, sizeof cover_src, "%s/t%02d/cover.jpg", fixtures, fixture);
  snprintf(cover_dst, sizeof cover_dst, "%s/cover.jpg", dst);
  if (mc_file_exists(cover_src)) mc_copy_file(cover_src, cover_dst, 0);
  snprintf(dst, sizeof dst, "%s/%s", a->libdir, rel);
  CHECK(mc_copy_file(src, dst, 0) == MC_OK);

  char artist[MC_UID_STRLEN], album[MC_UID_STRLEN], track[MC_UID_STRLEN], file[MC_UID_STRLEN];
  mc_uid_new(artist);
  mc_uid_new(album);
  mc_uid_new(track);
  mc_uid_new(file);
  sqlite3 *db = mc_library_db(a->lib);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(dst, &size, &mtime) == MC_OK);
  CHECK(mc_library_begin_write(a->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Bench %d')", artist, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', 'Album %02d', '%s', 2000)",
           album, no, artist);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', 'Track', 1, %d)",
           track, album, artist, (int)(seconds * 1000));
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', '%s', 1000, %lld, %lld)",
           file, track, rel, ext, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(a->lib) == MC_OK);
  return size;
}

static void report(const char *name, double wall, double cpu, double in, double audio,
                   const mc_sync_result *ra, const mc_sync_result *rb) {
  double out = (double)rb->bytes_received;
  printf("%s: %d files, wall %.2f s, process cpu %.2f s | %.2f MB/s in, %.3f MB/s out, %.1fx rt |"
         " received %d, corrupt %d, sent %d\n",
         name, rb->files_received, wall, cpu, in / 1e6 / wall, out / 1e6 / wall, audio / wall,
         rb->files_received, rb->files_corrupt, ra->files_sent);
  fflush(stdout);
}

int main(int argc, char **argv) {
  if (argc < 5) return 2;
  const char *fixtures = argv[1], *root = argv[2], *mode = argc > 5 ? argv[5] : "cold";
  int kbps = atoi(argv[3]), files = atoi(argv[4]);
  CHECK(mc_init() == MC_OK);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  CHECK(pair(&b, &a) == MC_PAIR_PAIRED); /* B asks, so A is the archive */

  double audio = 0, in = 0;
  for (int i = 0; i < files; i++) {
    int64_t size = add_one(&a, fixtures, i % 24, i, &audio);
    if (size < 0) return 1;
    in += (double)size;
  }
  set_opus(&a, b.uid, kbps);

  mc_sync_result ra, rb;
  double c0 = process_cpu_s();
  uint64_t t0 = mc_mono_ms();
  mc_sync_trace("BENCH start");
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  mc_sync_trace("BENCH end");
  report("cold", (double)(mc_mono_ms() - t0) / 1000.0, process_cpu_s() - c0, in, audio, &ra, &rb);
  CHECK(rb.files_received == files); /* else the MB/s above counts files that never went */

  if (!strcmp(mode, "both") || !strcmp(mode, "warm")) {
    /* B gets another bitrate's worth of nothing: the same files, from A's full cache. */
    char sql[64];
    (void)sql;
    set_opus(&a, b.uid, kbps == 128 ? 96 : 128);
    CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
    set_opus(&a, b.uid, kbps);
    c0 = process_cpu_s();
    t0 = mc_mono_ms();
    mc_sync_trace("BENCH warm start");
    CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
    mc_sync_trace("BENCH warm end");
    report("warm", (double)(mc_mono_ms() - t0) / 1000.0, process_cpu_s() - c0, in, audio, &ra, &rb);
  }

  mc_sync_free(a.sync);
  mc_sync_free(b.sync);
  mc_library_close(a.lib);
  mc_library_close(b.lib);
  mc_shutdown();
  return TEST_RESULT();
}
