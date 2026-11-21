/* A group of one goes ("Group membership" in docs/sync-protocol.md): devices pair over
 * loopback with discovery off, as in test_session.c, and the one left alone in a group
 * forgets it, keeping its library, keys and scrobbling account, and pairs again afterwards. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000
#define MAX_EVENTS 64

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

/* --- A device --- */

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
  char done_peer[MAX_EVENTS][MC_UID_STRLEN];
  int ndone;
  int dissolved;
  char dissolved_name[128];
} dev;

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)name, (void)attempts;
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
  if (d->ndone < MAX_EVENTS) snprintf(d->done_peer[d->ndone++], MC_UID_STRLEN, "%s", r->device_uid);
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_group_dissolved(void *user, const char *name) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->dissolved++;
  snprintf(d->dissolved_name, sizeof d->dissolved_name, "%s", name ? name : "");
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {.on_pair_code = on_pair_code,
                                        .on_pair_result = on_pair_result,
                                        .on_sync_done = on_sync_done,
                                        .on_group_dissolved = on_group_dissolved};

static void dev_paths(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
}

static int dev_start(dev *d) {
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  d->dissolved = 0;
  if (!d->lib) CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
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

/* Stops sync; the library stays open unless close_lib. */
static void dev_stop(dev *d, int close_lib) {
  if (!d->label || !d->sync) return;
  mc_sync_free(d->sync);
  d->sync = NULL;
  if (close_lib) {
    mc_library_close(d->lib);
    d->lib = NULL;
  }
  mc_cond_destroy(&d->c);
  mc_mutex_destroy(&d->m);
}

static int count_done(dev *d, const char *peer) {
  int n = 0;
  for (int i = 0; i < d->ndone; i++)
    if (!strcmp(d->done_peer[i], peer)) n++;
  return n;
}

static int done_seen(dev *d, const char *peer) {
  mc_mutex_lock(&d->m);
  int n = count_done(d, peer);
  mc_mutex_unlock(&d->m);
  return n;
}

static int wait_done(dev *d, const char *peer, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  int ok = count_done(d, peer) > seen;
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

/* Waits up to ms for d's dissolved count to pass seen. */
static int wait_dissolved(dev *d, int seen, uint64_t ms) {
  uint64_t end = mc_mono_ms() + ms;
  mc_mutex_lock(&d->m);
  while (d->dissolved <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  int n = d->dissolved;
  mc_mutex_unlock(&d->m);
  return n > seen;
}

/* x asks y to pair by address, and types y's code. On success, waits for the first session
 * on both. */
static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results, ry = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  int got = 0, refused = 0;
  while (!got && !refused && mc_mono_ms() < end) {
    mc_mutex_lock(&y->m);
    got = y->codes > codes;
    if (!got) mc_cond_wait_ms(&y->c, &y->m, 50);
    mc_mutex_unlock(&y->m);
    mc_mutex_lock(&x->m);
    refused = x->results > rx;
    mc_mutex_unlock(&x->m);
  }
  if (!got) return wait_result(x, rx);
  char code[MC_PAIR_CODE_LEN + 1];
  mc_mutex_lock(&y->m);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_pair_reason r = wait_result(x, rx);
  CHECK(wait_result(y, ry) == r);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx));
    CHECK(wait_done(y, x->uid, sy));
  }
  return r;
}

/* --- Looking inside --- */

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int group_int(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return -1;
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), sql, -1, &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  mc_sync_db_close(sdb);
  return n;
}

static int in_group(dev *d) {
  return group_int(d, "SELECT count(*) FROM local WHERE group_uid IS NOT NULL");
}

static int wait_out_of_group(dev *d) {
  for (uint64_t end = mc_mono_ms() + WAIT_MS; mc_mono_ms() < end; mc_sleep_ms(100))
    if (in_group(d) == 0) return 1;
  return 0;
}

/* Writes through sync.db's logging, as the app would. */
static void group_edit(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(d->config, &sdb) == MC_OK);
  if (!sdb) return;
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  mc_sync_db_close(sdb);
}

static int track_count(dev *d) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(d->lib), "SELECT count(*) FROM tracks", -1, &stmt, NULL) ==
      SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
  }
  return n;
}

static void fill_library(dev *d, int tracks) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[512];
  mc_uid_new(ar);
  mc_uid_new(al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid) VALUES ('%s', 'Things', '%s')",
           al, ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  for (int i = 0; i < tracks; i++) {
    char t[MC_UID_STRLEN];
    mc_uid_new(t);
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
             " VALUES ('%s', '%s', '%s', 'Song %02d', %d, 180000)",
             t, al, ar, i + 1, i + 1);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* --- The scenarios --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], sql[512];
  snprintf(root, sizeof root, "%s/test_sync_dissolve.d", scratch);
  remove_tree(root);

  static dev a, b, c;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  dev_paths(&c, root, "c");
  if (!dev_start(&a) || !dev_start(&b) || !dev_start(&c)) return TEST_RESULT();
  fill_library(&a, 5);

  /* A, the archive, signs in to Last.fm and has plays waiting to be scrobbled. */
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  snprintf(sql, sizeof sql,
           "INSERT INTO scrobble_account (uid, session_key, username) VALUES ('%s', 'sk-1', 'tania')",
           MC_SCROBBLE_ACCOUNT_UID);
  group_edit(&a, sql);
  group_edit(&a, "INSERT INTO scrobble_queue (played_at_s, artist, track) VALUES (1700000000, 'Low', 'Words')");

  /* A removes B, the last other member: B hears of it, and A's group goes. */
  CHECK(mc_sync_remove_device(a.sync, b.uid) == MC_OK);
  CHECK(wait_dissolved(&a, 0, WAIT_MS));
  CHECK(!strcmp(a.dissolved_name, "a"));
  CHECK(in_group(&a) == 0);
  CHECK(wait_out_of_group(&b));
  CHECK(b.dissolved == 0); /* B was removed: no group of one there */
  CHECK(group_int(&a, "SELECT count(*) FROM devices") == 0);
  CHECK(group_int(&a, "SELECT count(*) FROM sync_group") == 0);
  CHECK(group_int(&a, "SELECT count(*) FROM profiles") == 0);
  /* Kept: the library, this device's identity, the account and its unsent plays. */
  CHECK(track_count(&a) == 5);
  CHECK(strcmp(mc_sync_device_uid(a.sync), a.uid) == 0);
  CHECK(group_int(&a, "SELECT count(*) FROM local WHERE length(public_key) = 32") == 1);
  CHECK(group_int(&a, "SELECT count(*) FROM scrobble_account WHERE session_key = 'sk-1'"
                      " AND username = 'tania'") == 1);
  CHECK(group_int(&a, "SELECT count(*) FROM changelog WHERE entity = 'scrobble_account'") > 0);
  CHECK(group_int(&a, "SELECT count(*) FROM scrobble_queue") == 1);
  {
    mc_sync_device_list *list = NULL;
    CHECK(mc_sync_list_devices(a.sync, &list) == MC_OK && list && list->count == 0);
    mc_sync_device_list_free(list);
    mc_sync_lib_state s = MC_SYNC_LIBRARY_GROUP;
    CHECK(mc_sync_library_state(a.sync, &s) == MC_OK && s == MC_SYNC_LIBRARY_NO_GROUP);
  }
  /* Told once. */
  mc_sleep_ms(2500);
  CHECK(a.dissolved == 1);

  /* Pairing works afterwards, as for a device that never paired: A and B make a new group,
   * and the account goes into it. */
  CHECK(pair(&b, &a) == MC_PAIR_PAIRED);
  CHECK(in_group(&a) == 1 && in_group(&b) == 1);
  CHECK(group_int(&b, "SELECT count(*) FROM scrobble_account WHERE session_key = 'sk-1'") == 1);
  CHECK(track_count(&b) == 5);

  /* B leaves: A dissolves once it hears, through the session leaving makes. */
  CHECK(mc_sync_leave_group(b.sync) == MC_OK);
  CHECK(in_group(&b) == 0);
  CHECK(wait_dissolved(&a, 1, WAIT_MS));
  CHECK(in_group(&a) == 0);
  CHECK(b.dissolved == 0); /* leaving says nothing */
  CHECK(group_int(&a, "SELECT count(*) FROM scrobble_queue") == 1);

  /* A group of three losing one stays. */
  CHECK(pair(&b, &a) == MC_PAIR_PAIRED);
  CHECK(pair(&c, &a) == MC_PAIR_PAIRED);
  CHECK(mc_sync_remove_device(a.sync, c.uid) == MC_OK);
  CHECK(!wait_dissolved(&a, 2, 3000));
  CHECK(in_group(&a) == 1);
  CHECK(b.dissolved == 0);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM devices WHERE uid = '%s' AND removed = 0", b.uid);
  CHECK(group_int(&a, sql) == 1);

  /* Already alone at start: A stops, B's removal arrives meanwhile (as written here), and A
   * forgets the group as soon as it starts. */
  dev_stop(&a, 0);
  snprintf(sql, sizeof sql, "UPDATE devices SET removed = 1 WHERE uid = '%s'", b.uid);
  group_edit(&a, sql);
  CHECK(in_group(&a) == 1);
  if (dev_start(&a)) {
    CHECK(wait_dissolved(&a, 0, WAIT_MS));
    CHECK(in_group(&a) == 0);
    CHECK(track_count(&a) == 5);
    CHECK(group_int(&a, "SELECT count(*) FROM scrobble_account WHERE session_key = 'sk-1'") == 1);
  }

  dev *all[] = {&a, &b, &c};
  for (int k = 0; k < 3; k++) dev_stop(all[k], 1);
  mc_shutdown();
  if (!test_failures) remove_tree(root);
  return TEST_RESULT();
}
