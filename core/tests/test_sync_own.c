/* Each device owns its profile (see "Profiles and what a device holds" in
 * docs/sync-protocol.md). Two devices over loopback in test_sync_files.c style: A holds the
 * library and is the group's archive, B joins it.
 *  - Pairing: B starts with its budget on from its free space, and fetches nothing until its
 *    user chooses, whoever syncs with it; a press of Sync on B chooses.
 *  - A remote edit: A can't set B's profile, and a change A logs to it anyway (as a device
 *    that ignored the rule would) is passed on but never applied: B's own profile stands, on
 *    B and, once B edits it, on A.
 *  - Archives: only a device sets its own role, and the group always keeps one. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define MAX_EVENTS 64
#define GIB ((int64_t)1 << 30)

static char g_b_music[64];

/* B has 10 GiB free, A what it really has. */
static mc_status stub_free_space(const char *dir, int64_t *out) {
  if (g_b_music[0] && strstr(dir, g_b_music)) {
    *out = 10 * GIB + 12345;
    return MC_OK;
  }
  return mc_free_space(dir, out);
}

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
                                        NULL, NULL, NULL, NULL, NULL, NULL, NULL};

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

/* x's user presses Sync for y. Waits for both sides; rx and ry (may be NULL) get the
 * results. */
static mc_status sync_with(dev *x, dev *y, mc_sync_result *rx, mc_sync_result *ry) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
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

/* editor sets uid's profile to everything (album_uid NULL) or that album only, originals, no
 * budget; want is what the setter should say. */
static void set_selection(dev *editor, const char *uid, const char *album_uid, mc_status want) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(editor->sync, uid, &p) == MC_OK);
  if (!p) return;
  mc_profile_item item;
  memset(&item, 0, sizeof item);
  mc_profile_item *saved = p->items;
  int saved_count = p->count;
  p->sync_all = album_uid == NULL;
  p->format.format = MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = 0;
  p->budget_enabled = 0;
  if (album_uid) {
    item.kind = MC_PROFILE_ALBUM;
    snprintf(item.ref_uid, sizeof item.ref_uid, "%s", album_uid);
    p->items = &item;
    p->count = 1;
  }
  CHECK(mc_sync_set_profile(editor->sync, uid, p) == want);
  p->items = saved;
  p->count = saved_count;
  mc_sync_profile_free(p);
}

static mc_sync_outlook outlook_of(dev *d) {
  mc_sync_outlook o;
  /* The scheduler works it out after each session; manual mode still runs that. */
  uint64_t end = mc_mono_ms() + 2000;
  do {
    CHECK(mc_sync_get_outlook(d->sync, &o) == MC_OK);
    mc_sleep_ms(50);
  } while (mc_mono_ms() < end && !o.chosen && !o.receive_tracks && !o.send_tracks);
  return o;
}

static int64_t b_profile_int(dev *d, const char *b_uid, const char *col) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT %s FROM profiles WHERE device_uid = '%s'", col, b_uid);
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(d->config, &sdb) == MC_OK);
  int64_t v = sdb ? query_int(mc_sync_db_handle(sdb), sql) : -1;
  mc_sync_db_close(sdb);
  return v;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_own.d", scratch);
  remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  snprintf(g_b_music, sizeof g_b_music, "/b/music");
  mc_sync_test_free_space = stub_free_space;

  static dev a, b;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b")) return TEST_RESULT();
  add_track(&a, "Things", "Song One", 1);
  add_track(&a, "Things", "Song Two", 2);
  add_track(&a, "Other", "Lone", 1);
  char things[MC_UID_STRLEN];
  query_text(mc_library_db(a.lib), "SELECT uid FROM albums WHERE title = 'Things'", things,
             sizeof things);

  /* Pairing runs a session: B gets the library's data and no files, and starts with a budget
   * of nine tenths of its free space, whole GiB. A is the archive, chosen by being one. */
  mc_sync_result ra, rb;
  memset(&rb, 0, sizeof rb);
  CHECK(pair(&a, &b, &rb) == MC_PAIR_PAIRED);
  CHECK(rb.library_synced && rb.files_received == 0);
  CHECK(files_of(&b) == 0);
  CHECK(query_int(mc_library_db(b.lib), "SELECT count(*) FROM tracks") == 3);
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
  CHECK(p && p->budget_enabled && p->format.format == MC_FORMAT_AUTO && p->sync_all);
  CHECK(p && p->budget_bytes == 9 * GIB);
  mc_sync_profile_free(p);
  CHECK(!outlook_of(&b).chosen);
  CHECK(outlook_of(&a).chosen);

  /* A presses Sync for B: still nothing moves to B, whose user hasn't chosen. */
  CHECK(sync_with(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.files_received == 0 && files_of(&b) == 0);
  /* The budget reached A as B's own edit. */
  CHECK(b_profile_int(&a, b.uid, "budget_enabled") == 1);

  /* A can't set B's profile or role. */
  set_selection(&a, b.uid, things, MC_ERR_LOCKED);
  CHECK(mc_sync_set_archive(a.sync, b.uid, 1) == MC_ERR_LOCKED);
  CHECK(mc_sync_exclude_items(a.sync, b.uid, MC_PROFILE_ALBUM,
                              (const char(*)[MC_UID_STRLEN])things, 1, 1) == MC_ERR_LOCKED);

  /* A logs a change to B's profile anyway, as a device ignoring the rule would: a selection
   * of Things, and B's budget off. B logs the rows and passes them on, but B's profile is as
   * B left it. */
  {
    mc_sync_db *sdb = NULL;
    CHECK(mc_sync_db_open(a.config, &sdb) == MC_OK);
    if (sdb) {
      char sql[512], item[MC_UID_STRLEN];
      mc_uid_new(item);
      CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
      snprintf(sql, sizeof sql,
               "UPDATE profiles SET mode = 'selected', budget_enabled = 0 WHERE device_uid = '%s';"
               "INSERT INTO profile_items (uid, device_uid, kind, ref_uid)"
               " VALUES ('%s', '%s', 'album', '%s')",
               b.uid, item, b.uid, things);
      CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
      CHECK(mc_sync_db_commit(sdb) == MC_OK);
      mc_sync_db_close(sdb);
    }
  }
  mc_sync_notify_changed(a.sync);
  CHECK(sync_with(&a, &b, &ra, &rb) == MC_OK);
  CHECK(rb.rows_received > 0);
  CHECK(mc_sync_result_did_something(&rb) && !mc_sync_result_changed(&rb));
  CHECK(b_profile_int(&b, b.uid, "budget_enabled") == 1);
  /* Straight after, there's nothing to do either way. */
  CHECK(sync_with(&a, &b, &ra, &rb) == MC_OK);
  CHECK(!mc_sync_result_did_something(&ra) && !mc_sync_result_did_something(&rb));
  {
    mc_sync_db *sdb = NULL;
    CHECK(mc_sync_db_open(b.config, &sdb) == MC_OK);
    if (sdb) {
      char sql[256];
      snprintf(sql, sizeof sql, "SELECT count(*) FROM profile_items WHERE device_uid = '%s'", b.uid);
      CHECK(query_int(mc_sync_db_handle(sdb), sql) == 0);
      snprintf(sql, sizeof sql,
               "SELECT count(*) FROM changelog WHERE entity = 'profiles' AND row_uid = '%s'"
               " AND origin_uid = '%s'",
               b.uid, a.uid);
      CHECK(query_int(mc_sync_db_handle(sdb), sql) == 2); /* logged, not applied */
      char mode[16];
      snprintf(sql, sizeof sql, "SELECT mode FROM profiles WHERE device_uid = '%s'", b.uid);
      query_text(mc_sync_db_handle(sdb), sql, mode, sizeof mode);
      CHECK(!strcmp(mode, "all"));
      mc_sync_db_close(sdb);
    }
  }

  /* B chooses: everything as originals, then Sync. Its edit wins on A too, over A's own
   * forged rows, which never count. */
  set_selection(&b, b.uid, NULL, MC_OK);
  CHECK(sync_with(&b, &a, &rb, &ra) == MC_OK);
  CHECK(rb.files_received == 3 && files_of(&b) == 3);
  CHECK(outlook_of(&b).chosen);
  CHECK(b_profile_int(&a, b.uid, "budget_enabled") == 0);
  p = NULL;
  CHECK(mc_sync_get_profile(b.sync, b.uid, &p) == MC_OK);
  CHECK(p && p->sync_all && !p->budget_enabled && p->count == 0);
  mc_sync_profile_free(p);

  /* B selects Things alone: A shows it as B has it. */
  set_selection(&b, b.uid, things, MC_OK);
  CHECK(sync_with(&b, &a, &rb, &ra) == MC_OK);
  CHECK(files_of(&b) == 2 && rb.files_deleted == 1);
  p = NULL;
  CHECK(mc_sync_get_profile(a.sync, b.uid, &p) == MC_OK);
  CHECK(p && !p->sync_all && p->count == 1 && !strcmp(p->items[0].ref_uid, things));
  mc_sync_profile_free(p);

  /* Archives. A, the only archive, can't step down; B makes itself one, and once its own
   * report shows it complete A may. The group is never left without one. */
  CHECK(mc_sync_set_archive(a.sync, a.uid, 0) == MC_ERR_LOCKED);
  CHECK(mc_sync_set_archive(b.sync, b.uid, 1) == MC_OK);
  CHECK(mc_sync_set_archive(a.sync, b.uid, 0) == MC_ERR_LOCKED);
  CHECK(sync_with(&b, &a, &rb, &ra) == MC_OK);
  CHECK(files_of(&b) == 3);
  CHECK(mc_sync_verify(b.sync, MC_VERIFY_FAST, NULL) == MC_OK);
  CHECK(sync_with(&b, &a, &rb, &ra) == MC_OK);
  CHECK(mc_sync_set_archive(a.sync, a.uid, 0) == MC_OK);
  CHECK(sync_with(&a, &b, &ra, &rb) == MC_OK);
  CHECK(mc_sync_set_archive(b.sync, b.uid, 0) == MC_ERR_LOCKED);
  CHECK(mc_sync_remove_device(a.sync, b.uid) == MC_ERR_LOCKED);
  /* Stepping down chose A's own profile, everything as originals: A keeps what it has. */
  CHECK(outlook_of(&a).chosen);

  dev_stop(&a);
  dev_stop(&b);
  mc_sync_test_free_space = NULL;
  remove_tree(root);
  return TEST_RESULT();
}
