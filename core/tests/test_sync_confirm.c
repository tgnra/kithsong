/* Confirming profiles: a device fetches and deletes files only for a profile its user has
 * confirmed by pressing Sync (mc_sync_confirm_now()). First the fingerprint, then two
 * devices over loopback in test_sync_files.c style: A holds the library and is the group's
 * archive, B joins it. Pairing moves no files; neither does an automatic sync until B's
 * profile is confirmed, or after it changes (on A or on B) until the next press. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/sync_db.h"
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
  void *more = realloc(n->items, (size_t)(n->count + 1) * sizeof *n->items);
  if (!more) return;
  n->items = more;
  snprintf(n->items[n->count], 512, "%c%s", is_dir ? 'd' : 'f', name);
  n->count++;
}

static void remove_tree(const char *dir) {
  names n = {0};
  mc_list_dir(dir, collect, &n);
  for (int i = 0; i < n.count; i++) {
    char p[2048];
    snprintf(p, sizeof p, "%s/%s", dir, n.items[i] + 1);
    if (n.items[i][0] == 'd')
      remove_tree(p);
    else
      remove(p);
  }
  free(n.items);
  mc_rmdir(dir);
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

/* --- Fingerprint --- */

static void fingerprint(mc_sync_db *sdb, const char *uid, char out[MC_PROFILE_FP_STRLEN]) {
  CHECK(mc_profile_fingerprint(mc_sync_db_handle(sdb), uid, out) == MC_OK);
  CHECK(strlen(out) == MC_PROFILE_FP_STRLEN - 1);
}

static void check_fingerprint(const char *root) {
  char dir[1100];
  snprintf(dir, sizeof dir, "%s/fp", root);
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(dir, &sdb) == MC_OK);
  if (!sdb) return;
  sqlite3 *db = mc_sync_db_handle(sdb);
  const char *d = "0192f1e4-7c3a-7b21-9d4e-000000000001";
  const char *al = "0192f1e4-7c3a-7b21-9d4e-0000000000a1";
  char none[MC_PROFILE_FP_STRLEN], fp[MC_PROFILE_FP_STRLEN], base[MC_PROFILE_FP_STRLEN];
  char sql[512];

  fingerprint(sdb, d, none); /* no profiles row */
  snprintf(sql, sizeof sql, "INSERT INTO profiles (device_uid) VALUES ('%s')", d);
  CHECK(exec(db, sql) == SQLITE_OK);
  fingerprint(sdb, d, base);
  CHECK(strcmp(none, base) != 0);

  /* What doesn't change what it holds: its name, a selection while it syncs everything, a
   * bitrate with originals, a switched-off budget's size. */
  snprintf(sql, sizeof sql,
           "UPDATE profiles SET name = 'Phone', bitrate_kbps = 128, budget_bytes = 1000"
           " WHERE device_uid = '%s';"
           "INSERT INTO profile_items (uid, device_uid, kind, ref_uid)"
           " VALUES ('0192f1e4-7c3a-7b21-9d4e-0000000000c1', '%s', 'album', '%s')",
           d, d, al);
  CHECK(exec(db, sql) == SQLITE_OK);
  fingerprint(sdb, d, fp);
  CHECK(!strcmp(fp, base));

  /* What does: each of mode, format, bitrate with Opus, budget, and the selection. */
  char seen[6][MC_PROFILE_FP_STRLEN];
  const char *edits[] = {
    "UPDATE profiles SET mode = 'selected' WHERE device_uid = '%s'",
    "UPDATE profiles SET format = 'opus' WHERE device_uid = '%s'",
    "UPDATE profiles SET bitrate_kbps = 64 WHERE device_uid = '%s'",
    "UPDATE profiles SET budget_enabled = 1 WHERE device_uid = '%s'",
    "UPDATE profiles SET budget_bytes = 2000 WHERE device_uid = '%s'",
    "DELETE FROM profile_items WHERE device_uid = '%s'",
  };
  for (int i = 0; i < 6; i++) {
    snprintf(sql, sizeof sql, edits[i], d);
    CHECK(exec(db, sql) == SQLITE_OK);
    fingerprint(sdb, d, seen[i]);
    CHECK(strcmp(seen[i], base) != 0);
    for (int j = 0; j < i; j++) CHECK(strcmp(seen[i], seen[j]) != 0);
  }

  /* An archive holds everything whatever its row says. */
  snprintf(sql, sizeof sql, "UPDATE profiles SET archive = 1 WHERE device_uid = '%s'", d);
  CHECK(exec(db, sql) == SQLITE_OK);
  fingerprint(sdb, d, base);
  snprintf(sql, sizeof sql, "UPDATE profiles SET mode = 'all', format = 'original' WHERE device_uid = '%s'", d);
  CHECK(exec(db, sql) == SQLITE_OK);
  fingerprint(sdb, d, fp);
  CHECK(!strcmp(fp, base));

  /* The state: unconfirmed with no devices row or no confirmation, then confirmed until the
   * profile changes. */
  CHECK(mc_profile_confirm_state(db, d) == MC_SYNC_UNCONFIRMED);
  snprintf(sql, sizeof sql, "INSERT INTO devices (uid, name) VALUES ('%s', 'Phone')", d);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_profile_confirm_state(db, d) == MC_SYNC_UNCONFIRMED);
  snprintf(sql, sizeof sql, "UPDATE devices SET confirmed_profile = '%s' WHERE uid = '%s'", fp, d);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_profile_confirm_state(db, d) == MC_SYNC_CONFIRMED);
  /* (Clearing archive here would be the group's last archive's void demotion, which still
   * counts as one, so it's the confirmation that differs instead.) */
  snprintf(sql, sizeof sql, "UPDATE devices SET confirmed_profile = '%s' WHERE uid = '%s'",
           seen[0], d);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_profile_confirm_state(db, d) == MC_SYNC_PROFILE_CHANGED);
  /* Not in a group: there's nothing to confirm. */
  CHECK(mc_profile_confirm(sdb, NULL) == MC_ERR_NOT_FOUND);
  mc_sync_db_close(sdb);
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

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done, NULL};

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
  mc_library_close(d->lib);
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

/* x syncs with y, automatically (user = 0: as the scheduler or background work would) or as
 * x's user pressing Sync. Waits for both sides; rx and ry (may be NULL) get the results. */
static mc_status sync_with(dev *x, dev *y, int user, mc_sync_result *rx, mc_sync_result *ry) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK((user ? mc_sync_confirm_now(x->sync, y->uid) : mc_sync_now(x->sync, y->uid)) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx, rx);
  if (st == MC_OK) st = wait_done(y, x->uid, sy, ry);
  return st;
}

static mc_pair_reason pair(dev *x, dev *y, mc_sync_result *ry) {
  int codes = y->codes, rx = x->results, ry_seen = y->results;
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
  while (y->results <= ry_seen && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 100);
  mc_mutex_unlock(&y->m);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx, NULL) == MC_OK);
    CHECK(wait_done(y, x->uid, sy, ry) == MC_OK);
  }
  return r;
}

/* uid's confirm state as d's device list shows it; -1 if it isn't listed. */
static int confirm_of(dev *d, const char *uid) {
  mc_sync_device_list *list = NULL;
  CHECK(mc_sync_list_devices(d->sync, &list) == MC_OK);
  int state = -1;
  for (int i = 0; list && i < list->count; i++)
    if (!strcmp(list->items[i].uid, uid)) state = (int)list->items[i].confirm;
  mc_sync_device_list_free(list);
  return state;
}

static int64_t files_of(dev *d) {
  return query_int(mc_library_db(d->lib), "SELECT count(*) FROM files");
}

/* Adds a track with an "original" at its organised path. Its bytes needn't be audio: sync
 * sends originals as they are. */
static void add_track(dev *d, const char *album, const char *title, int no) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], tr[MC_UID_STRLEN], fi[MC_UID_STRLEN], sql[1024];
  query_text(db, "SELECT uid FROM artists WHERE name = 'Low'", ar, sizeof ar);
  snprintf(sql, sizeof sql, "SELECT uid FROM albums WHERE title = '%s'", album);
  query_text(db, sql, al, sizeof al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  if (!al[0]) {
    mc_uid_new(al);
    snprintf(sql, sizeof sql,
             "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', '%s', '%s', 1994)",
             al, album, ar);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  mc_uid_new(tr);
  mc_uid_new(fi);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, 1500)",
           tr, al, ar, title, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  char rel[512], dir[2048], p[2048];
  snprintf(rel, sizeof rel, "Low/%s (1994)/%02d %s.wav", album, no, title);
  snprintf(dir, sizeof dir, "%s/Low/%s (1994)", d->libdir, album);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(p, sizeof p, "%s/%s", d->libdir, rel);
  FILE *f = fopen(p, "wb");
  CHECK(f != NULL);
  if (f) {
    for (int i = 0; i < 4096; i++) fputc((i * 7 + no * 31 + (int)strlen(album)) & 0xff, f);
    fclose(f);
  }
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', 'wav', 1411, %lld, %lld)",
           fi, tr, rel, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* editor sets uid's profile to everything (album_uid NULL) or that album only, originals. */
static void set_selection(dev *editor, const char *uid, const char *album_uid) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  mc_profile_item item;
  memset(&item, 0, sizeof item);
  mc_profile_item *saved = p->items;
  int saved_count = p->count;
  p->sync_all = album_uid == NULL;
  if (album_uid) {
    item.kind = MC_PROFILE_ALBUM;
    snprintf(item.ref_uid, sizeof item.ref_uid, "%s", album_uid);
    p->items = &item;
    p->count = 1;
  }
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == MC_OK);
  p->items = saved;
  p->count = saved_count;
  mc_sync_profile_free(p);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_confirm.d", scratch);
  remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  check_fingerprint(root);

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  CHECK(mc_sync_confirm_now(a.sync, NULL) == MC_OK); /* no group yet: nothing to confirm */
  add_track(&a, "Things", "Song One", 1);
  add_track(&a, "Things", "Song Two", 2);
  add_track(&a, "Other", "Lone", 1);
  char things[MC_UID_STRLEN];
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Things'", things,
             sizeof things);

  /* Pairing runs a session, but B has never been confirmed: it gets the library's data and
   * no files, though its profile (everything) wants them all. */
  mc_sync_result ra, rb;
  memset(&rb, 0, sizeof rb);
  CHECK(pair(&a, &b, &rb) == MC_PAIR_PAIRED);
  CHECK(rb.library_synced && rb.files_paused && rb.files_received == 0);
  CHECK(files_of(&b) == 0);
  CHECK(query_int(mc_library_db(b.lib), "SELECT count(*) FROM tracks") == 3);
  CHECK(confirm_of(&a, b.uid) == MC_SYNC_UNCONFIRMED);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_UNCONFIRMED);
  CHECK(confirm_of(&a, a.uid) == MC_SYNC_UNCONFIRMED);

  /* Setting the profile up isn't enough, nor is an automatic sync. */
  set_selection(&a, b.uid, NULL);
  CHECK(sync_with(&a, &b, 0, &ra, &rb) == MC_OK);
  CHECK(rb.files_paused && rb.files_received == 0 && files_of(&b) == 0);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_UNCONFIRMED);

  /* A's user presses Sync for B: A's profile is confirmed at once, B's by the session, and B
   * fetches everything. */
  CHECK(sync_with(&a, &b, 1, &ra, &rb) == MC_OK);
  CHECK(!ra.files_paused && !rb.files_paused);
  CHECK(rb.files_received == 3 && files_of(&b) == 3);
  CHECK(confirm_of(&a, a.uid) == MC_SYNC_CONFIRMED);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_CONFIRMED);
  /* B's confirmation reaches A in the next session. */
  CHECK(sync_with(&a, &b, 0, &ra, &rb) == MC_OK);
  CHECK(!rb.files_paused);
  CHECK(confirm_of(&a, b.uid) == MC_SYNC_CONFIRMED);
  CHECK(confirm_of(&b, a.uid) == MC_SYNC_CONFIRMED);

  /* Unchanged since: automatic syncs move files on their own, and a rename changes nothing. */
  add_track(&a, "Third", "New", 1);
  CHECK(mc_sync_set_device_name(b.sync, "Bee") == MC_OK);
  CHECK(sync_with(&a, &b, 0, &ra, &rb) == MC_OK);
  CHECK(!rb.files_paused && rb.files_received == 1 && files_of(&b) == 4);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_CONFIRMED);

  /* A changes B's profile: A shows it paused at once, and B, once it hears of it, fetches and
   * deletes nothing in automatic syncs, from either side. */
  set_selection(&a, b.uid, things);
  CHECK(confirm_of(&a, b.uid) == MC_SYNC_PROFILE_CHANGED);
  CHECK(sync_with(&a, &b, 0, &ra, &rb) == MC_OK);
  CHECK(rb.files_paused && rb.files_deleted == 0 && files_of(&b) == 4);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_PROFILE_CHANGED);
  CHECK(sync_with(&b, &a, 0, &rb, &ra) == MC_OK);
  CHECK(rb.files_paused && rb.files_deleted == 0 && files_of(&b) == 4);
  CHECK(!ra.files_paused); /* A's own profile hasn't changed */

  /* Changed back, it's the confirmed one again; changed on B itself, it pauses too. */
  set_selection(&b, b.uid, NULL);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_CONFIRMED);
  set_selection(&b, b.uid, things);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_PROFILE_CHANGED);
  CHECK(sync_with(&b, &a, 0, &rb, &ra) == MC_OK);
  CHECK(rb.files_paused && files_of(&b) == 4);

  /* B's user presses Sync: the selection applies, Other and Third go. */
  CHECK(sync_with(&b, &a, 1, &rb, &ra) == MC_OK);
  CHECK(!rb.files_paused && rb.files_deleted == 2 && files_of(&b) == 2);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_CONFIRMED);

  /* A press on the other end confirms B's profile too, B being the responder: back to
   * everything. */
  set_selection(&a, b.uid, NULL);
  CHECK(sync_with(&a, &b, 0, &ra, &rb) == MC_OK);
  CHECK(rb.files_paused && files_of(&b) == 2);
  CHECK(sync_with(&a, &b, 1, &ra, &rb) == MC_OK);
  CHECK(!rb.files_paused && rb.files_received == 2 && files_of(&b) == 4);
  CHECK(confirm_of(&b, b.uid) == MC_SYNC_CONFIRMED);

  dev_stop(&a);
  dev_stop(&b);
  remove_tree(root);
  return TEST_RESULT();
}
