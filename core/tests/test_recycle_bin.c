/* The recycle bin (musiccore/delete.h, mc_sync_bin_restore() in musiccore/sync_here.h), in
 * test_sync_here.c style: A holds the library as WAV originals and is the group's archive; B,
 * as a phone, syncs everything and deletes into the bin. Checked: where the bin is offered;
 * that a delete in the bin goes nowhere until B's user presses Sync (B refuses A's session,
 * and its own automatic ones are held); that taking one back removes the delete from the
 * changelog, puts its rows back (plays included) and turns it into an exclusion, so the
 * music stays gone from B and stays on A; that a later delete the taken one needs comes back
 * with it; that a track's delete becomes an exclude_track; that a sync empties the bin and
 * sends the deletes; and that once a sync has begun sending, nothing in the bin can come
 * back, however the sync ends. */
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
                                        NULL, NULL, NULL, NULL, NULL};

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


/* editor sets device's profile to everything. */
static void select_albums_all(dev *editor, const char *device) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, device, &p) == MC_OK);
  if (!p) return;
  p->sync_all = 1;
  CHECK(mc_sync_set_profile(editor->sync, device, p) == MC_OK);
  mc_sync_profile_free(p);
}

/* --- The bin --- */

/* Deletes uid (an album or track) from d into the bin; the result into res. */
static mc_status bin_delete(dev *d, mc_delete_kind kind, const char *uid, mc_delete_result *res) {
  const char *uids[] = {uid};
  mc_delete_plan *plan = NULL;
  mc_status st = mc_delete_plan_new(d->lib, kind, uids, 1, &plan);
  if (st == MC_OK) st = mc_delete_run_to_bin(d->lib, plan, res);
  mc_delete_plan_free(plan);
  return st;
}

/* The id of the bin's newest entry, or -1; its other fields into *e (title and artist left
 * out). */
static int64_t newest_entry(dev *d, mc_bin_entry *e) {
  mc_bin_list *list = NULL;
  int64_t id = -1;
  if (mc_delete_bin_list(d->lib, &list) == MC_OK && list && list->count) {
    id = list->items[0].id;
    if (e) {
      *e = list->items[0];
      e->title = e->artist = NULL;
    }
  }
  mc_delete_bin_list_free(list);
  return id;
}

/* _deleted changelog rows in d's library for row uid. */
static int64_t deletes_of(dev *d, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM changelog WHERE field = '_deleted' AND row_uid = '%s'", uid);
  return lib_int(d, sql);
}

static int64_t track_exists(dev *d, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM tracks WHERE uid = '%s'", uid);
  return lib_int(d, sql);
}

/* Whether a search of d's library for query finds an album called title, waiting a while for
 * the index to catch up (it's rebuilt in the background). want: what's waited for. */
static int finds_album(dev *d, const char *query, const char *title, int want) {
  int found = !want;
  for (int tries = 0; tries < 50; tries++) {
    mc_search_results *r = NULL;
    found = 0;
    if (mc_library_search(d->lib, query, 0, &r) == MC_OK && r)
      for (int i = 0; i < r->count; i++)
        if (r->items[i].kind == MC_SEARCH_ALBUM && r->items[i].name && !strcmp(r->items[i].name, title))
          found = 1;
    mc_search_results_free(r);
    if (found == want) break;
    mc_sleep_ms(100);
  }
  return found;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  CHECK(mc_transfer_trash != mc_trash); /* safe_trash.c's stand-in, not the real trash */
  char root[1024], sql[1024];
  snprintf(root, sizeof root, "%s/test_recycle_bin.d", scratch);
  remove_tree(root);

  static dev a, b, c;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  dev_paths(&c, root, "c");
  if (!dev_start(&a) || !dev_start(&b) || !dev_start(&c)) return TEST_RESULT();
  track_ref t1, t2, t3, p1, p2, k1;
  add_track(&a, "Low", "Things", 1994, "Song One", 1, 440, &t1);
  add_track(&a, "Low", "Things", 1994, "Song Two", 2, 550, &t2);
  add_track(&a, "Low", "Things", 1994, "Song Three", 3, 660, &t3);
  add_track(&a, "Pear", "Pair", 1999, "Left", 1, 330, &p1);
  add_track(&a, "Pear", "Pair", 1999, "Right", 2, 350, &p2);
  add_track(&a, "Kid", "Kid Album", 2000, "Kid Song", 1, 770, &k1);
  char things[MC_UID_STRLEN], pair_album[MC_UID_STRLEN], kid[MC_UID_STRLEN];
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Things'", things,
             sizeof things);
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Pair'", pair_album,
             sizeof pair_album);
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Kid Album'", kid,
             sizeof kid);

  /* Outside a group there's nothing to take a delete back to. */
  CHECK(!mc_sync_bin_offered(a.sync) && !mc_sync_bin_offered(c.sync));
  CHECK(mc_delete_bin_count(a.lib) == 0);

  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  select_albums_all(&a, b.uid);
  mc_sync_result ra, rb;
  CHECK(sync_now(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 6);
  /* The archive keeps everything; B keeps a part. */
  CHECK(!mc_sync_bin_offered(a.sync));
  CHECK(mc_sync_bin_offered(b.sync));
  CHECK(mc_library_record_play(b.lib, t1.uid) == MC_OK);
  CHECK(finds_album(&b, "things", "Things", 1));

  /* B deletes Things into the bin: its files go at once, the delete is logged as ever. */
  mc_delete_result del;
  CHECK(bin_delete(&b, MC_DELETE_ALBUMS, things, &del) == MC_OK);
  CHECK(del.tracks_deleted == 3 && del.files_trashed >= 3 && del.tracks_kept == 0);
  CHECK(!lib_file_exists(&b, t1.rel) && !lib_file_exists(&b, "Low/Things (1994)"));
  CHECK(track_exists(&b, t1.uid) == 0 && deletes_of(&b, t1.uid) == 1 && deletes_of(&b, things) == 1);
  CHECK(mc_delete_bin_count(b.lib) == 1);
  mc_bin_entry e;
  int64_t things_id = newest_entry(&b, &e);
  CHECK(things_id > 0 && e.kind == MC_DELETE_ALBUMS && e.items == 1 && e.tracks == 3 && e.deleted_ms > 0);
  {
    mc_bin_list *list = NULL;
    CHECK(mc_delete_bin_list(b.lib, &list) == MC_OK);
    CHECK(list && list->count == 1);
    if (list && list->count) {
      CHECK_STR(list->items[0].title, "Things");
      CHECK_STR(list->items[0].artist, "Low");
    }
    mc_delete_bin_list_free(list);
  }
  CHECK(finds_album(&b, "things", "Things", 0) == 0);

  /* It goes nowhere without a press of Sync on B: A's press is refused (busy)... */
  {
    int sa = done_seen(&a, b.uid);
    CHECK(mc_sync_confirm_now(a.sync, b.uid) == MC_OK);
    mc_status st = wait_done(&a, b.uid, sa, NULL);
    CHECK(st != MC_OK && st != MC_ERR_UNIMPLEMENTED);
  }
  CHECK(track_exists(&a, t1.uid) == 1 && lib_file_exists(&a, t1.rel));
  CHECK(deletes_of(&a, t1.uid) == 0);
  CHECK(mc_delete_bin_count(b.lib) == 1);
  /* ...and so is B's own background sync. */
  {
    int sb = done_seen(&b, a.uid);
    CHECK(mc_sync_background(b.sync, a.uid) == MC_OK);
    mc_sleep_ms(2000);
    CHECK(done_seen(&b, a.uid) == sb);
  }
  CHECK(deletes_of(&a, t1.uid) == 0 && mc_delete_bin_count(b.lib) == 1);
  /* A session the user didn't ask for can't send it, whichever side it's on. */
  CHECK(mc_sync_bin_send_begin(b.sync, b.lib, 0) == MC_ERR_LOCKED);
  CHECK(mc_delete_bin_count(b.lib) == 1);

  /* Taken back: the delete is gone from the changelog, the rows are back with the play, and
   * B's profile now excludes the album, whose files stay gone. */
  int entries = 0;
  CHECK(mc_sync_bin_restore(b.sync, things_id, &entries) == MC_OK);
  CHECK(entries == 1);
  CHECK(mc_delete_bin_count(b.lib) == 0);
  CHECK(track_exists(&b, t1.uid) == 1 && track_exists(&b, t3.uid) == 1);
  CHECK(deletes_of(&b, t1.uid) == 0 && deletes_of(&b, things) == 0);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM track_plays WHERE track_uid = '%s'", t1.uid);
  CHECK(lib_int(&b, sql) == 1);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM albums WHERE uid = '%s'", things);
  CHECK(lib_int(&b, sql) == 1);
  CHECK(files_of(&b, "Things") == 0 && !lib_file_exists(&b, t1.rel));
  CHECK(items_of(&b, b.uid, "exclude_album") == 1);
  CHECK(finds_album(&b, "things", "Things", 1));
  {
    mc_here_state s = MC_HERE_UNAVAILABLE;
    CHECK(mc_sync_here_state(b.sync, MC_PROFILE_ALBUM, things, &s) == MC_OK && s == MC_HERE_REMOVED);
  }
  CHECK(mc_sync_bin_restore(b.sync, things_id, NULL) == MC_ERR_NOT_FOUND);

  /* A sync now: A keeps Things and hears of the exclusion; B fetches nothing of it. */
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(rb.files_received == 0 && rb.files_deleted == 0);
  CHECK(track_exists(&a, t1.uid) == 1 && lib_file_exists(&a, t1.rel) && files_of(&a, "Things") == 3);
  CHECK(deletes_of(&a, t1.uid) == 0 && deletes_of(&a, things) == 0);
  CHECK(items_of(&a, b.uid, "exclude_album") == 1);
  CHECK(files_of(&b, "Things") == 0);

  /* Two track deletes, the second taking the album with its last track: taking back the
   * first brings back the second too, as the first's track needs the album. Each becomes
   * an exclude_track. */
  CHECK(bin_delete(&b, MC_DELETE_TRACKS, p1.uid, &del) == MC_OK && del.tracks_deleted == 1);
  int64_t p1_id = newest_entry(&b, &e);
  CHECK(e.kind == MC_DELETE_TRACKS && e.tracks == 1);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM albums WHERE uid = '%s'", pair_album);
  CHECK(lib_int(&b, sql) == 1);
  CHECK(bin_delete(&b, MC_DELETE_TRACKS, p2.uid, &del) == MC_OK && del.tracks_deleted == 1);
  CHECK(lib_int(&b, sql) == 0 && deletes_of(&b, pair_album) == 1);
  CHECK(mc_delete_bin_count(b.lib) == 2);
  CHECK(mc_sync_bin_restore(b.sync, p1_id, &entries) == MC_OK);
  CHECK(entries == 2 && mc_delete_bin_count(b.lib) == 0);
  CHECK(track_exists(&b, p1.uid) == 1 && track_exists(&b, p2.uid) == 1 && lib_int(&b, sql) == 1);
  CHECK(deletes_of(&b, pair_album) == 0 && deletes_of(&b, p2.uid) == 0);
  CHECK(items_of(&b, b.uid, "exclude_track") == 2 && files_of(&b, "Pair") == 0);
  {
    mc_sync_profile *p = NULL;
    mc_sync_estimate est;
    CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
    CHECK(p && p->sync_all && p->count == 0); /* editors don't see exclusions */
    CHECK(p && mc_sync_estimate_profile(b.sync, b.uid, p, &est) == MC_OK && est.tracks == 1);
    mc_sync_profile_free(p);
  }
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(rb.files_received == 0 && files_of(&b, "Pair") == 0 && files_of(&a, "Pair") == 2);
  CHECK(items_of(&a, b.uid, "exclude_track") == 2);

  /* Once a sync begins sending, the bin is emptied first: a sync cut off halfway leaves the
   * deletes to go with the next, and none can be taken back. */
  CHECK(bin_delete(&b, MC_DELETE_ALBUMS, kid, &del) == MC_OK && del.tracks_deleted == 1);
  int64_t kid_id = newest_entry(&b, NULL);
  CHECK(mc_sync_bin_send_begin(b.sync, b.lib, 1) == MC_OK);
  CHECK(mc_delete_bin_count(b.lib) == 0);
  CHECK(mc_sync_bin_restore(b.sync, kid_id, NULL) == MC_ERR_LOCKED); /* still sending */
  mc_sync_bin_send_end(b.sync, b.lib);
  CHECK(mc_sync_bin_restore(b.sync, kid_id, NULL) == MC_ERR_NOT_FOUND);
  CHECK(deletes_of(&b, k1.uid) == 1);
  /* The delete goes with the next sync, which is automatic again with the bin empty. */
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(track_exists(&a, k1.uid) == 0 && deletes_of(&a, k1.uid) == 1);

  /* A press of Sync on B sends what's in the bin, and empties it. */
  CHECK(bin_delete(&b, MC_DELETE_TRACKS, t2.uid, &del) == MC_OK && del.tracks_deleted == 1);
  int64_t t2_id = newest_entry(&b, NULL);
  CHECK(mc_delete_bin_count(b.lib) == 1);
  CHECK(sync_now(&b, &a, &rb, &ra) == MC_OK);
  CHECK(mc_delete_bin_count(b.lib) == 0);
  CHECK(track_exists(&a, t2.uid) == 0 && deletes_of(&a, t2.uid) == 1);
  CHECK(mc_sync_bin_restore(b.sync, t2_id, NULL) == MC_ERR_NOT_FOUND);
  CHECK(track_exists(&a, t1.uid) == 1 && track_exists(&a, p1.uid) == 1);

  /* An archive's deletes don't go to the bin (the frontend asks mc_sync_bin_offered()); a
   * restore there is refused. */
  CHECK(mc_sync_bin_restore(a.sync, 1, NULL) == MC_ERR_LOCKED);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM recycle_bin");
  CHECK(lib_int(&a, sql) == 0);
  CHECK(group_int(&a, "SELECT count(*) FROM devices") >= 2);

  dev_stop(&c);
  dev_stop(&b);
  dev_stop(&a);
  remove_tree(root);
  return TEST_RESULT();
}
