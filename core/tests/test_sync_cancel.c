/* mc_sync_cancel(): two devices over loopback in test_sync_files.c style. A holds WAV
 * originals and serves them slowly; B's user cancels while a file is halfway over. The
 * session ends reported as cancelled, the partial copy is gone and files that arrived whole
 * are kept. A hears of it (CANCEL): reported as peer_cancelled, and it starts no automatic
 * session with B for MC_SYNC_PEER_CANCEL_HOLD_MS. Then the hold: B starts no session,
 * background or not, and refuses A's, until B's Sync press lifts it and the rest arrive; A
 * serves that session for all its own hold. Then A cancels: B's Sync press still starts a
 * session and lifts B's hold, and a hold that runs out lets background work go ahead. Last,
 * a peer as an older version is told nothing and holds nothing. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define MAX_EVENTS 64

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

/* What's in a library folder: audio files and partial ones. */
typedef struct tally {
  int wav, part;
} tally;

static int on_file(void *user, const char *path) {
  tally *t = user;
  size_t len = strlen(path);
  if (len > 4 && !strcmp(path + len - 4, ".wav")) t->wav++;
  if (len > 5 && !strcmp(path + len - 5, ".part")) t->part++;
  return 0;
}

static tally count_files(const char *dir) {
  tally t = {0, 0};
  mc_walk(dir, on_file, &t);
  return t;
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
    e->r = *r;
    e->r.device_uid = NULL;
  }
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done,
                                        NULL, NULL, NULL, NULL, NULL, NULL};

static int dev_start(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) ==
        MC_OK);
  if (!d->sync) return 0;
  snprintf(d->uid, sizeof d->uid, "%s", mc_sync_device_uid(d->sync));
  mc_sync_set_test_options(d->sync, 0, 0);
  mc_sync_set_test_manual(d->sync, 1);
  mc_sync_set_test_auto_interval(d->sync, 0); /* so the rate limit holds nothing back */
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

/* Waits up to wait_ms for a new session report on d about peer; 1 and its result in r if
 * one came. */
static int wait_done(dev *d, const char *peer, int seen, int wait_ms, mc_sync_result *r) {
  uint64_t end = mc_mono_ms() + (uint64_t)wait_ms;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  int got = count_done(d, peer) > seen;
  for (int i = d->ndone - 1; got && i >= 0; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      *r = d->done[i].r;
      break;
    }
  mc_mutex_unlock(&d->m);
  return got;
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
  mc_mutex_lock(&x->m);
  while ((x->results <= rx || y->results <= ry) && mc_mono_ms() < end)
    mc_cond_wait_ms(&x->c, &x->m, 100);
  mc_pair_reason r = x->results > rx ? x->reason : (mc_pair_reason)-1;
  mc_mutex_unlock(&x->m);
  if (r == MC_PAIR_PAIRED) {
    mc_sync_result ignored;
    CHECK(wait_done(x, y->uid, sx, WAIT_MS, &ignored));
    CHECK(wait_done(y, x->uid, sy, WAIT_MS, &ignored));
  }
  return r;
}

/* --- The library --- */

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

/* An album of n tracks, each a WAV original of ms at its organised path. */
static void add_album(dev *d, const char *title, int n, unsigned ms) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[1024];
  mc_uid_new(ar);
  mc_uid_new(al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO artists (uid, name) VALUES ('%s', 'Low');"
           "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', '%s', '%s', 1994)",
           ar, al, title, ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  char dir[2048];
  snprintf(dir, sizeof dir, "%s/Low/%s (1994)", d->libdir, title);
  CHECK(mc_mkdirs(dir) == MC_OK);
  for (int i = 1; i <= n; i++) {
    char t[MC_UID_STRLEN], f[MC_UID_STRLEN], rel[256], p[2048];
    mc_uid_new(t);
    mc_uid_new(f);
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
             " VALUES ('%s', '%s', '%s', 'Song %d', %d, %u)",
             t, al, ar, i, i, ms);
    CHECK(exec(db, sql) == SQLITE_OK);
    snprintf(rel, sizeof rel, "Low/%s (1994)/%02d Song %d.wav", title, i, i);
    snprintf(p, sizeof p, "%s/%s", d->libdir, rel);
    CHECK(write_wav(p, ms, 220.0 * i));
    int64_t size = 0, mtime = 0;
    CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
    snprintf(sql, sizeof sql,
             "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
             " VALUES ('%s', '%s', '%s', 'wav', 1411, %lld, %lld)",
             f, t, rel, (long long)size, (long long)mtime);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* uid's profile: everything, as originals. */
static void sync_everything(dev *editor, const char *uid) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  p->sync_all = 1;
  p->format.format = MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = 0;
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
  mc_sync_profile_free(p);
}

/* mono ms left of d's hold on peer after the peer cancelled (peer_held_ms); 0 if none. */
static uint64_t peer_held(dev *d, const char *peer) {
  uint64_t left = 0, now = mc_mono_ms();
  mc_mutex_lock(&d->sync->lock);
  for (int i = 0; i < d->sync->npeers; i++)
    if (!strcmp(d->sync->peers[i].uid, peer) && d->sync->peers[i].peer_held_ms > now)
      left = d->sync->peers[i].peer_held_ms - now;
  mc_mutex_unlock(&d->sync->lock);
  return left;
}

/* Sets d's hold on peer to run out in ms, as if the peer's CANCEL had come a while ago. */
static void set_peer_held(dev *d, const char *peer, uint64_t ms) {
  mc_mutex_lock(&d->sync->lock);
  for (int i = 0; i < d->sync->npeers; i++)
    if (!strcmp(d->sync->peers[i].uid, peer)) d->sync->peers[i].peer_held_ms = mc_mono_ms() + ms;
  mc_mutex_unlock(&d->sync->lock);
}

/* d's mc_sync_device.auto_after_ms for peer, -1 if it isn't listed. */
static int64_t auto_after(dev *d, const char *peer) {
  mc_sync_device_list *list = NULL;
  int64_t v = -1;
  CHECK(mc_sync_list_devices(d->sync, &list) == MC_OK);
  for (int i = 0; list && i < list->count; i++)
    if (!strcmp(list->items[i].uid, peer)) v = list->items[i].auto_after_ms;
  mc_sync_device_list_free(list);
  return v;
}

/* 1 if d's trigger for peer is a change: in manual mode one never runs, and it takes the
 * place of a background ask still waiting (the change poll may see a session's rows). */
static int change_trigger(dev *d, const char *peer) {
  int changed = 0;
  mc_mutex_lock(&d->sync->lock);
  for (int i = 0; i < d->sync->npeers; i++)
    if (!strcmp(d->sync->peers[i].uid, peer))
      changed = d->sync->peers[i].trigger == MC_TRIGGER_CHANGED;
  mc_mutex_unlock(&d->sync->lock);
  return changed;
}

/* Background work on d asks for a session with peer, again if a change trigger took its
 * place, until one is reported, up to WAIT_MS. 1 and its result in r if one came. */
static int background_done(dev *d, const char *peer, mc_sync_result *r) {
  int seen = done_seen(d, peer);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  CHECK(mc_sync_background(d->sync, peer) == MC_OK);
  while (mc_mono_ms() < end) {
    if (wait_done(d, peer, seen, 300, r)) return 1;
    if (change_trigger(d, peer)) CHECK(mc_sync_background(d->sync, peer) == MC_OK);
  }
  return 0;
}

/* x's user presses Sync for y, then y's user cancels once a file is partly on x. Returns
 * 1 with both ends' reports if it got that far. */
static int cancel_halfway(dev *x, dev *y, dev *canceller, mc_sync_result *rx,
                          mc_sync_result *ry) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_confirm_now(x->sync, y->uid) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  while (count_files(x->libdir).part == 0 && mc_mono_ms() < end) mc_sleep_ms(10);
  CHECK(count_files(x->libdir).part == 1);
  CHECK(mc_sync_cancel(canceller->sync, canceller == x ? y->uid : x->uid) == MC_OK);
  int got = wait_done(x, y->uid, sx, WAIT_MS, rx);
  CHECK(got);
  got = wait_done(y, x->uid, sy, WAIT_MS, ry) && got;
  CHECK(got);
  return got;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_cancel.d", scratch);
  remove_tree(root);

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  /* 3 s of CD audio is about 530 KB, nine frames: at 100 ms a frame, a second a file. */
  add_album(&a, "Things", 3, 3000);
  CHECK(mc_sync_cancel(NULL, NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_sync_cancel(b.sync, "not a uid") == MC_ERR_INVALID_ARG);

  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  sync_everything(&a, b.uid);
  CHECK(lib_int(&b, "SELECT count(*) FROM files") == 0);

  /* B's user presses Sync, and Cancel once a file is partly here. */
  mc_sync_set_test_send_delay(a.sync, 100);
  int sa = done_seen(&a, b.uid), sb = done_seen(&b, a.uid);
  CHECK(mc_sync_confirm_now(b.sync, a.uid) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  while (count_files(b.libdir).part == 0 && mc_mono_ms() < end) mc_sleep_ms(10);
  CHECK(count_files(b.libdir).part == 1);
  CHECK(mc_sync_cancel(b.sync, NULL) == MC_OK);
  mc_sync_result rb, ra;
  CHECK(wait_done(&b, a.uid, sb, WAIT_MS, &rb));
  CHECK(rb.cancelled && rb.status != MC_OK);
  CHECK(rb.files_received < 3);
  /* A's user cancelled nothing, but B told A it did. */
  CHECK(wait_done(&a, b.uid, sa, WAIT_MS, &ra));
  CHECK(!ra.cancelled && ra.peer_cancelled && ra.status != MC_OK);
  CHECK(!rb.peer_cancelled);
  /* No partial copy, and every file that's here arrived whole and is recorded. */
  tally t = count_files(b.libdir);
  CHECK(t.part == 0);
  CHECK(t.wav == rb.files_received);
  CHECK(lib_int(&b, "SELECT count(*) FROM files") == rb.files_received);
  int got = rb.files_received;

  /* A holds B back for MC_SYNC_PEER_CANCEL_HOLD_MS: no background session, and the device
   * list says when the next automatic one may go. B holds nothing of A's. */
  mc_sync_set_test_send_delay(a.sync, 0);
  uint64_t held = peer_held(&a, b.uid);
  CHECK(held > MC_SYNC_PEER_CANCEL_HOLD_MS - 60000 && held <= MC_SYNC_PEER_CANCEL_HOLD_MS);
  CHECK(peer_held(&b, a.uid) == 0);
  int64_t after = auto_after(&a, b.uid);
  CHECK(after > (int64_t)mc_wall_ms() + MC_SYNC_PEER_CANCEL_HOLD_MS - 60000);
  CHECK(after <= (int64_t)mc_wall_ms() + MC_SYNC_PEER_CANCEL_HOLD_MS);
  sa = done_seen(&a, b.uid);
  CHECK(mc_sync_background(a.sync, b.uid) == MC_OK);
  CHECK(!wait_done(&a, b.uid, sa, 1500, &ra));

  /* Held: B starts nothing in the background. */
  sb = done_seen(&b, a.uid);
  CHECK(mc_sync_background(b.sync, a.uid) == MC_OK);
  CHECK(!wait_done(&b, a.uid, sb, 1500, &rb));
  CHECK(done_seen(&a, b.uid) == sa);
  CHECK(lib_int(&b, "SELECT count(*) FROM files") == got);

  /* B's user presses Sync again: the hold lifts, A serves it for all its own hold, and the
   * rest arrive. A's hold is gone once they've synced. */
  sb = done_seen(&b, a.uid);
  sa = done_seen(&a, b.uid);
  CHECK(mc_sync_confirm_now(b.sync, a.uid) == MC_OK);
  CHECK(wait_done(&b, a.uid, sb, WAIT_MS, &rb));
  CHECK(rb.status == MC_OK && !rb.cancelled && !rb.peer_cancelled);
  CHECK(rb.files_received == 3 - got && rb.files_missing == 0);
  CHECK(wait_done(&a, b.uid, sa, WAIT_MS, &ra));
  CHECK(ra.status == MC_OK && !ra.peer_cancelled);
  t = count_files(b.libdir);
  CHECK(t.part == 0 && t.wav == 3);
  CHECK(lib_int(&b, "SELECT count(*) FROM files") == 3);
  CHECK(peer_held(&a, b.uid) == 0);
  CHECK(auto_after(&a, b.uid) == 0);

  /* A's user cancels this time, as the one serving the files. */
  add_album(&a, "Long Division", 3, 3000);
  mc_sync_set_test_send_delay(a.sync, 100);
  if (cancel_halfway(&b, &a, &a, &rb, &ra)) {
    CHECK(ra.cancelled && !ra.peer_cancelled && ra.status != MC_OK);
    CHECK(rb.peer_cancelled && !rb.cancelled && rb.status != MC_OK);
  }
  mc_sync_set_test_send_delay(a.sync, 0);
  CHECK(peer_held(&b, a.uid) > MC_SYNC_PEER_CANCEL_HOLD_MS - 60000);
  CHECK(count_files(b.libdir).part == 0);
  sb = done_seen(&b, a.uid);
  CHECK(mc_sync_background(b.sync, a.uid) == MC_OK);
  CHECK(!wait_done(&b, a.uid, sb, 1500, &rb));
  /* B's Sync still starts one (which A refuses as busy, for its own hold) and lifts B's
   * hold, so background work goes ahead again. */
  sb = done_seen(&b, a.uid);
  CHECK(mc_sync_now(b.sync, a.uid) == MC_OK);
  CHECK(wait_done(&b, a.uid, sb, WAIT_MS, &rb));
  CHECK(rb.status == MC_ERR_LOCKED && !rb.cancelled && !rb.peer_cancelled);
  CHECK(peer_held(&b, a.uid) == 0);
  CHECK(background_done(&b, a.uid, &rb));
  CHECK(rb.status == MC_ERR_LOCKED);
  /* Background work goes ahead once a hold runs out, not before. */
  set_peer_held(&b, a.uid, 1500);
  uint64_t t0 = mc_mono_ms();
  sb = done_seen(&b, a.uid);
  CHECK(mc_sync_background(b.sync, a.uid) == MC_OK);
  CHECK(!wait_done(&b, a.uid, sb, 1000, &rb));
  uint64_t end2 = mc_mono_ms() + WAIT_MS;
  while (!wait_done(&b, a.uid, sb, 300, &rb) && mc_mono_ms() < end2)
    if (change_trigger(&b, a.uid)) CHECK(mc_sync_background(b.sync, a.uid) == MC_OK);
  CHECK(done_seen(&b, a.uid) > sb);
  CHECK(mc_mono_ms() - t0 >= 1500);
  CHECK(rb.status == MC_ERR_LOCKED);
  /* A's Sync lifts its own hold, and the rest arrive. */
  sb = done_seen(&b, a.uid);
  sa = done_seen(&a, b.uid);
  CHECK(mc_sync_confirm_now(a.sync, b.uid) == MC_OK);
  CHECK(wait_done(&b, a.uid, sb, WAIT_MS, &rb));
  CHECK(rb.status == MC_OK && rb.files_missing == 0);
  CHECK(wait_done(&a, b.uid, sa, WAIT_MS, &ra)); /* A's end done too, before the next */
  CHECK(lib_int(&b, "SELECT count(*) FROM files") == 6);

  /* B as an older version: its HELLO lacks "cancel", so A's Cancel sends nothing and B's
   * session just fails, holding nothing back. */
  mc_sync_set_test_no_cancel(b.sync, 1);
  add_album(&a, "Secret Name", 3, 3000);
  mc_sync_set_test_send_delay(a.sync, 100);
  if (cancel_halfway(&b, &a, &a, &rb, &ra)) {
    CHECK(ra.cancelled && ra.status != MC_OK);
    CHECK(!rb.peer_cancelled && !rb.cancelled && rb.status != MC_OK);
  }
  mc_sync_set_test_send_delay(a.sync, 0);
  CHECK(peer_held(&b, a.uid) == 0);
  CHECK(background_done(&b, a.uid, &rb));
  CHECK(rb.status == MC_ERR_LOCKED); /* A's own hold */
  mc_sync_set_test_no_cancel(b.sync, 0);
  sb = done_seen(&b, a.uid);
  sa = done_seen(&a, b.uid);
  CHECK(mc_sync_confirm_now(a.sync, b.uid) == MC_OK);
  CHECK(wait_done(&b, a.uid, sb, WAIT_MS, &rb));
  CHECK(rb.status == MC_OK && rb.files_missing == 0);
  CHECK(wait_done(&a, b.uid, sa, WAIT_MS, &ra));
  CHECK(lib_int(&b, "SELECT count(*) FROM files") == 9);

  /* Cancelling with nothing running just holds, which Sync now lifts at once. */
  CHECK(mc_sync_cancel(a.sync, b.uid) == MC_OK);
  sa = done_seen(&a, b.uid);
  CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
  CHECK(wait_done(&a, b.uid, sa, WAIT_MS, &ra));
  CHECK(ra.status == MC_OK && !ra.cancelled);

  dev_stop(&a);
  dev_stop(&b);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
