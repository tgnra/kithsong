/* Removing an album or artist from one device (musiccore/sync_here.h), in test_sync_files.c
 * style: A holds the library as WAV originals and is the group's archive; B syncs
 * everything, then removes an album, an artist, and brings them back, in "Sync everything"
 * and in a selection. Also what the menus are offered on an archive and outside a group,
 * that the exclusion syncs to A and survives A's edits of B's profile, and that an original
 * imported on B stays until an archive holds it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/transfer.h"
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
  unsigned rate = 8000;
  uint32_t frames = rate / 2, bytes = frames * 2;
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
  for (uint32_t i = 0; i < frames; i++)
    put16(f, (unsigned)(uint16_t)(int16_t)(0.4 * sin(2 * 3.14159265358979 * pitch * i / rate) * 32767));
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
                                        NULL, NULL, NULL, NULL};

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
  if (st == MC_OK) wait_done(y, x->uid, sy, ry);
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
  end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&x->m);
  while (x->results <= rx && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 100);
  mc_pair_reason r = x->results > rx ? x->reason : (mc_pair_reason)-1;
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

static int lib_file_exists(dev *d, const char *rel) {
  char p[2048];
  snprintf(p, sizeof p, "%s/%s", d->libdir, rel);
  return mc_file_exists(p);
}

static int64_t files_of(dev *d, const char *album) {
  char sql[512];
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM files f JOIN tracks t ON t.uid = f.track_uid"
           " JOIN albums a ON a.uid = t.album_uid WHERE a.title = '%s'",
           album);
  return lib_int(d, sql);
}

/* Rows of kind in d's sync.db profile_items for device. */
static int64_t items_of(dev *d, const char *device, const char *kind) {
  char sql[512];
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM profile_items WHERE device_uid = '%s' AND kind = '%s'", device,
           kind);
  return group_int(d, sql);
}

typedef struct track_ref {
  char uid[MC_UID_STRLEN];
  char rel[256];
} track_ref;

/* Adds a track (and its album, artist) with a WAV original at its organised path. */
static void add_track(dev *d, const char *artist, const char *album, int year, const char *title,
                      int no, double pitch, track_ref *out) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], file[MC_UID_STRLEN], sql[1024];
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
  mc_uid_new(file);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, 500)",
           out->uid, al, ar, title, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(out->rel, sizeof out->rel, "%s/%s (%d)/%02d %s.wav", artist, album, year, no, title);
  char dir[2048], p[2048];
  snprintf(dir, sizeof dir, "%s/%s/%s (%d)", d->libdir, artist, album, year);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(p, sizeof p, "%s/%s", d->libdir, out->rel);
  CHECK(write_wav(p, pitch));
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', 'wav', 128, %lld, %lld)",
           file, out->uid, out->rel, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

static mc_here_state state_of(dev *d, mc_profile_item_kind kind, const char *uid) {
  mc_here_state s = (mc_here_state)-1;
  CHECK(mc_sync_here_state(d->sync, kind, uid, &s) == MC_OK);
  return s;
}

/* Removes uid from d; the plan's counts and the result's into the out-params. */
static mc_status remove_here(dev *d, mc_profile_item_kind kind, const char *uid, int *files,
                             int *kept, mc_here_result *res) {
  mc_here_plan *plan = NULL;
  mc_status st = mc_sync_here_plan_new(d->sync, kind, uid, &plan);
  if (st != MC_OK) return st;
  *files = mc_here_plan_files(plan);
  *kept = mc_here_plan_kept(plan);
  st = mc_sync_remove_here(d->sync, plan, res);
  mc_here_plan_free(plan);
  return st;
}

static int confirmed(dev *d) {
  mc_sync_db *sdb = NULL;
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return 0;
  int state = mc_profile_confirm_state(mc_sync_db_handle(sdb), d->uid);
  mc_sync_db_close(sdb);
  return state == MC_SYNC_CONFIRMED;
}

/* editor sets device's profile to a selection of these albums (NULL-terminated), or with
 * none, to everything. */
static void select_albums(dev *editor, const char *device, const char *const *albums) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, device, &p) == MC_OK);
  if (!p) return;
  free(p->items);
  p->items = NULL;
  p->count = 0;
  int n = 0;
  while (albums && albums[n]) n++;
  p->sync_all = n == 0;
  if (n) {
    p->items = calloc((size_t)n, sizeof *p->items);
    for (int i = 0; p->items && i < n; i++) {
      p->items[i].kind = MC_PROFILE_ALBUM;
      snprintf(p->items[i].ref_uid, sizeof p->items[i].ref_uid, "%s", albums[i]);
    }
    p->count = p->items ? n : 0;
  }
  CHECK(mc_sync_set_profile(editor->sync, device, p) == MC_OK);
  mc_sync_profile_free(p);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  CHECK(mc_transfer_trash != mc_trash); /* safe_trash.c's stand-in, not the real trash */
  char root[1024], sql[1024];
  snprintf(root, sizeof root, "%s/test_sync_here.d", scratch);
  remove_tree(root);

  static dev a, b, c;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  dev_paths(&c, root, "c");
  if (!dev_start(&a) || !dev_start(&b) || !dev_start(&c)) return TEST_RESULT();
  track_ref t1, t2, t3, o1, k1;
  add_track(&a, "Low", "Things", 1994, "Song One", 1, 440, &t1);
  add_track(&a, "Low", "Things", 1994, "Song Two", 2, 550, &t2);
  add_track(&a, "Low", "Things", 1994, "Song Three", 3, 660, &t3);
  add_track(&a, "Low", "Other", 1996, "Lone", 1, 330, &o1);
  add_track(&a, "Kid", "Kid Album", 2000, "Kid Song", 1, 770, &k1);
  char things[MC_UID_STRLEN], other[MC_UID_STRLEN], kid[MC_UID_STRLEN], low[MC_UID_STRLEN];
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Things'", things,
             sizeof things);
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Other'", other,
             sizeof other);
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Kid Album'", kid,
             sizeof kid);
  query_text(mc_library_db(a.lib), "SELECT uid FROM artists WHERE name = 'Low'", low, sizeof low);

  /* Outside a group there's nowhere else the music lives: nothing to offer. */
  CHECK(state_of(&a, MC_PROFILE_ALBUM, things) == MC_HERE_UNAVAILABLE);
  CHECK(mc_sync_here_state(a.sync, MC_PROFILE_PLAYLIST, things, &(mc_here_state){0}) ==
        MC_ERR_INVALID_ARG);

  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  select_albums(&a, b.uid, NULL); /* B: everything, originals */
  mc_sync_result ra, rb;
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 5);
  CHECK(confirmed(&b));

  /* The archive keeps the whole library; C, in no group, has no other copy. */
  CHECK(state_of(&a, MC_PROFILE_ALBUM, things) == MC_HERE_UNAVAILABLE);
  CHECK(state_of(&a, MC_PROFILE_ARTIST, low) == MC_HERE_UNAVAILABLE);
  CHECK(state_of(&c, MC_PROFILE_ALBUM, things) == MC_HERE_UNAVAILABLE);
  {
    mc_here_plan *plan = NULL;
    CHECK(mc_sync_here_plan_new(a.sync, MC_PROFILE_ALBUM, things, &plan) == MC_ERR_LOCKED);
    CHECK(plan == NULL);
  }

  /* B removes Things while syncing everything: its three files go at once, the rows stay. */
  CHECK(state_of(&b, MC_PROFILE_ALBUM, things) == MC_HERE_HELD);
  {
    mc_here_plan *plan = NULL;
    CHECK(mc_sync_here_plan_new(b.sync, MC_PROFILE_ALBUM, things, &plan) == MC_OK);
    CHECK(mc_here_plan_files(plan) == 3 && mc_here_plan_kept(plan) == 0);
    CHECK(mc_here_plan_has(plan, t2.uid) && !mc_here_plan_has(plan, o1.uid));
    mc_here_result res;
    CHECK(mc_sync_remove_here(b.sync, plan, &res) == MC_OK);
    CHECK(res.files_removed == 3 && res.files_kept == 0);
    mc_here_plan_free(plan);
  }
  CHECK(!lib_file_exists(&b, t1.rel) && !lib_file_exists(&b, t3.rel));
  CHECK(!lib_file_exists(&b, "Low/Things (1994)")); /* the emptied folder went too */
  CHECK(lib_file_exists(&b, o1.rel));
  CHECK(files_of(&b, "Things") == 0 && files_of(&b, "Other") == 1);
  CHECK(lib_int(&b, "SELECT count(*) FROM tracks") == 5); /* still in the library */
  CHECK(lib_file_exists(&a, t1.rel) && files_of(&a, "Things") == 3); /* A's untouched */
  CHECK(state_of(&b, MC_PROFILE_ALBUM, things) == MC_HERE_REMOVED);
  CHECK(items_of(&b, b.uid, "exclude_album") == 1);
  CHECK(confirmed(&b)); /* B's own user asked for it: it isn't paused */
  {
    mc_sync_profile *p = NULL;
    CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
    CHECK(p && p->sync_all && p->count == 0); /* editors don't see the exclusion */
    mc_sync_profile_free(p);
    mc_sync_estimate est;
    CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
    CHECK(p && mc_sync_estimate_profile(b.sync, b.uid, p, &est) == MC_OK && est.tracks == 2);
    mc_sync_profile_free(p);
  }
  /* Removing again isn't offered. */
  {
    mc_here_plan *plan = NULL;
    CHECK(mc_sync_here_plan_new(b.sync, MC_PROFILE_ALBUM, things, &plan) == MC_ERR_LOCKED);
  }

  /* A sync doesn't bring it back, and A learns of the exclusion. */
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_deleted == 0 && !rb.files_paused);
  CHECK(files_of(&b, "Things") == 0);
  CHECK(items_of(&a, b.uid, "exclude_album") == 1);
  /* A edits B's profile (Opus, then back): the exclusion it can't see stays. */
  {
    mc_sync_profile *p = NULL;
    CHECK(mc_sync_get_profile(a.sync, b.uid, &p) == MC_OK);
    if (p) {
      CHECK(p->sync_all && p->count == 0);
      p->format.format = MC_FORMAT_OPUS;
      p->format.bitrate_kbps = 64;
      CHECK(mc_sync_set_profile(a.sync, b.uid, p) == MC_OK);
      p->format.format = MC_FORMAT_ORIGINAL;
      p->format.bitrate_kbps = 0;
      CHECK(mc_sync_set_profile(a.sync, b.uid, p) == MC_OK);
    }
    mc_sync_profile_free(p);
  }
  CHECK(items_of(&a, b.uid, "exclude_album") == 1);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && files_of(&b, "Things") == 0 && files_of(&b, "Other") == 1);

  /* The artist: Other's file goes; Things is still removed on its own, Other only through
   * Low, so only Low's menu offers it back. */
  CHECK(state_of(&b, MC_PROFILE_ARTIST, low) == MC_HERE_HELD);
  int files = 0, kept = 0;
  mc_here_result res;
  CHECK(remove_here(&b, MC_PROFILE_ARTIST, low, &files, &kept, &res) == MC_OK);
  CHECK(files == 1 && kept == 0 && res.files_removed == 1);
  CHECK(files_of(&b, "Other") == 0 && files_of(&b, "Kid Album") == 1);
  CHECK(state_of(&b, MC_PROFILE_ARTIST, low) == MC_HERE_REMOVED);
  CHECK(state_of(&b, MC_PROFILE_ALBUM, other) == MC_HERE_UNAVAILABLE);
  CHECK(state_of(&b, MC_PROFILE_ALBUM, things) == MC_HERE_REMOVED);
  CHECK(state_of(&b, MC_PROFILE_ALBUM, kid) == MC_HERE_HELD);
  /* Back: Other returns at the next sync, Things stays removed. */
  CHECK(mc_sync_restore_here(b.sync, MC_PROFILE_ARTIST, low) == MC_OK);
  CHECK(mc_sync_restore_here(b.sync, MC_PROFILE_ARTIST, low) == MC_ERR_LOCKED);
  CHECK(items_of(&b, b.uid, "exclude_artist") == 0);
  CHECK(state_of(&b, MC_PROFILE_ALBUM, other) == MC_HERE_HELD);
  CHECK(confirmed(&b));
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(rb.files_received == 1 && files_of(&b, "Other") == 1 && files_of(&b, "Things") == 0);

  /* A selection: choosing Things under What to sync (on A, for B) drops its exclusion. */
  {
    const char *sel[] = {things, kid, NULL};
    select_albums(&a, b.uid, sel);
  }
  CHECK(items_of(&a, b.uid, "exclude_album") == 0);
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(files_of(&b, "Things") == 3 && files_of(&b, "Other") == 0 && files_of(&b, "Kid Album") == 1);
  CHECK(state_of(&b, MC_PROFILE_ALBUM, things) == MC_HERE_HELD);
  /* Removing it from the selection takes its row out (the editor shows it unticked)... */
  CHECK(remove_here(&b, MC_PROFILE_ALBUM, things, &files, &kept, &res) == MC_OK);
  CHECK(files == 3 && res.files_removed == 3 && files_of(&b, "Things") == 0);
  {
    mc_sync_profile *p = NULL;
    CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
    CHECK(p && !p->sync_all && p->count == 1 && !strcmp(p->items[0].ref_uid, kid));
    mc_sync_profile_free(p);
  }
  /* ...and Sync to this device puts it back in. */
  CHECK(mc_sync_restore_here(b.sync, MC_PROFILE_ALBUM, things) == MC_OK);
  {
    mc_sync_profile *p = NULL;
    CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
    CHECK(p && !p->sync_all && p->count == 2);
    mc_sync_profile_free(p);
  }
  CHECK(items_of(&b, b.uid, "exclude_album") == 0);
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(files_of(&b, "Things") == 3);

  /* An original imported on B stays until an archive holds it: here B's library changed
   * after A last verified itself, so A's report can't vouch for it. */
  snprintf(sql, sizeof sql,
           "UPDATE files SET source_peer_uid = NULL WHERE track_uid = '%s'", k1.uid);
  CHECK(mc_library_begin_write(b.lib) == MC_OK);
  CHECK(exec(mc_library_db(b.lib), sql) == SQLITE_OK);
  snprintf(sql, sizeof sql, "UPDATE tracks SET title = 'Kid Song!' WHERE uid = '%s'", k1.uid);
  CHECK(exec(mc_library_db(b.lib), sql) == SQLITE_OK);
  CHECK(mc_library_commit(b.lib) == MC_OK);
  CHECK(remove_here(&b, MC_PROFILE_ALBUM, kid, &files, &kept, &res) == MC_OK);
  CHECK(files == 0 && kept == 1 && res.files_removed == 0 && res.files_kept == 1);
  CHECK(files_of(&b, "Kid Album") == 1);
  CHECK(state_of(&b, MC_PROFILE_ALBUM, kid) == MC_HERE_REMOVED);

  dev_stop(&c);
  dev_stop(&b);
  dev_stop(&a);
  remove_tree(root);
  return TEST_RESULT();
}
