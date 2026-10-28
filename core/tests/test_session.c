/* Pairing and sessions between several devices in one process, over loopback, with
 * discovery off: peers are reached through injected addresses, pairing by address, and
 * address hints. Each device has its own config dir (sync.db) and library folder. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "sync/rows.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000
#define MAX_EVENTS 64

/* CHECK_STR, for arrays (which -Waddress says are never NULL). */
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

/* --- A device --- */

typedef struct done_event {
  char peer[MC_UID_STRLEN];
  mc_status status;
  int library_synced;
} done_event;

typedef struct dev {
  const char *label;
  char config[1024], libdir[1024];
  mc_library *lib; /* the frontend's handle */
  mc_sync *sync;
  char uid[MC_UID_STRLEN];

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes;
  int code_attempts;
  int results;
  mc_pair_reason reason;
  int attempts_left;
  char result_peer[MC_UID_STRLEN];
  done_event done[MAX_EVENTS];
  int ndone;
} dev;

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)name;
  mc_mutex_lock(&d->m);
  snprintf(d->code, sizeof d->code, "%s", code);
  d->code_attempts = attempts;
  d->codes++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static void on_pair_result(void *user, const mc_pair_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->reason = r->reason;
  d->attempts_left = r->attempts_left;
  snprintf(d->result_peer, sizeof d->result_peer, "%s", r->device_uid);
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
    e->library_synced = r->library_synced;
  }
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL, on_pair_code, on_pair_result, NULL, on_sync_done};

static void dev_paths(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
}

/* Opens the library and sync, and starts listening on an ephemeral port, discovery off,
 * with 127.0.0.1 as its only address hint. */
static int dev_start(dev *d) {
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
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

static void dev_stop(dev *d) {
  if (!d->label || !d->lib) return; /* never started */
  mc_sync_free(d->sync);
  d->sync = NULL;
  mc_library_close(d->lib);
  d->lib = NULL;
  mc_cond_destroy(&d->c);
  mc_mutex_destroy(&d->m);
}

/* Tells x where y listens, as discovery would. */
static void introduce(dev *x, dev *y) {
  mc_sync_test_peer(x->sync, y->uid, y->label, "desktop", "", "127.0.0.1", mc_sync_port(y->sync),
                    1);
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

static int count_done(dev *d, const char *peer, int ok_only) {
  int n = 0;
  for (int i = 0; i < d->ndone; i++)
    if (!strcmp(d->done[i].peer, peer) && (!ok_only || d->done[i].status == MC_OK)) n++;
  return n;
}

/* Waits for a new session report on d about peer; returns its status. */
static mc_status wait_done(dev *d, const char *peer, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer, 0) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_status st = MC_ERR_UNIMPLEMENTED;
  for (int i = d->ndone - 1; i >= 0; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      if (count_done(d, peer, 0) > seen) st = d->done[i].status;
      break;
    }
  mc_mutex_unlock(&d->m);
  return st;
}

static int done_seen(dev *d, const char *peer) {
  mc_mutex_lock(&d->m);
  int n = count_done(d, peer, 0);
  mc_mutex_unlock(&d->m);
  return n;
}

/* x syncs with y now; waits for both sides to finish. */
static mc_status sync_now(dev *x, dev *y) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx);
  if (st == MC_OK) wait_done(y, x->uid, sy);
  return st;
}

/* x asks to pair with y, given y's address; y shows a code and x types it, wrong first if
 * asked. Returns how it ended on x; on success, waits for the first session on both. */
static mc_pair_reason pair(dev *x, dev *y, int by_address, int wrong_first) {
  int codes = y->codes, rx = x->results, ry = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  if (by_address) {
    CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  } else {
    introduce(x, y);
    CHECK(mc_sync_pair_request(x->sync, y->uid) == MC_OK);
  }
  /* y shows a code, or x hears it refused. */
  uint64_t end = mc_mono_ms() + WAIT_MS;
  int refused = 0;
  while (mc_mono_ms() < end) {
    mc_mutex_lock(&x->m);
    refused = x->results > rx;
    mc_mutex_unlock(&x->m);
    if (refused || wait_codes(y, codes)) break;
  }
  if (refused) {
    mc_pair_reason r = wait_result(x, rx);
    wait_result(y, ry); /* y says so too */
    return r;
  }
  char code[MC_PAIR_CODE_LEN + 1];
  mc_mutex_lock(&y->m);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(y->code_attempts == 3);
  if (wrong_first) {
    char wrong[MC_PAIR_CODE_LEN + 1];
    memcpy(wrong, code, sizeof wrong);
    wrong[0] = (char)('0' + (wrong[0] - '0' + 1) % 10);
    CHECK(mc_sync_pair_enter(x->sync, wrong) == MC_OK);
    CHECK(wait_result(x, rx) == MC_PAIR_WRONG_CODE);
    CHECK(x->attempts_left == 2);
    CHECK(wait_result(y, ry) == MC_PAIR_WRONG_CODE);
    rx = x->results;
    ry = y->results;
  }
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_pair_reason r = wait_result(x, rx);
  CHECK(wait_result(y, ry) == r);
  if (r == MC_PAIR_PAIRED) {
    CHECK_TEXT(x->result_peer, y->uid);
    CHECK(wait_done(x, y->uid, sx) == MC_OK);
    CHECK(wait_done(y, x->uid, sy) == MC_OK);
  }
  return r;
}

/* --- Looking inside --- */

static int query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
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

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* Runs fn on a fresh connection to d's sync.db. */
static int group_int(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return -1;
  int n = query_int(mc_sync_db_handle(sdb), sql);
  mc_sync_db_close(sdb);
  return n;
}

static void group_text(dev *d, const char *sql, char *out, size_t cap) {
  mc_sync_db *sdb = NULL;
  out[0] = '\0';
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return;
  query_text(mc_sync_db_handle(sdb), sql, out, cap);
  mc_sync_db_close(sdb);
}

static void group_digest(dev *d, char out[MC_DIGEST_STRLEN]) {
  mc_sync_db *sdb = NULL;
  out[0] = '\0';
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return;
  mc_rows_vv_digest(mc_sync_db_handle(sdb), out);
  mc_sync_db_close(sdb);
}

static void library_digest(dev *d, char out[MC_DIGEST_STRLEN]) {
  out[0] = '\0';
  mc_rows_vv_digest(mc_library_db(d->lib), out);
}

static void lib_text(dev *d, const char *sql, char *out, size_t cap) {
  query_text(mc_library_db(d->lib), sql, out, cap);
}

static int track_count(dev *d) {
  return query_int(mc_library_db(d->lib), "SELECT count(*) FROM tracks");
}

static void fill_library(dev *d, int tracks) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], pl[MC_UID_STRLEN], sql[512];
  mc_uid_new(ar);
  mc_uid_new(al);
  mc_uid_new(pl);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', 'Things', '%s', 1994)",
           al, ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql, "INSERT INTO playlists (uid, name) VALUES ('%s', 'Mix')", pl);
  CHECK(exec(db, sql) == SQLITE_OK);
  for (int i = 0; i < tracks; i++) {
    char t[MC_UID_STRLEN], e[MC_UID_STRLEN];
    mc_uid_new(t);
    mc_uid_new(e);
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
             " VALUES ('%s', '%s', '%s', 'Song %02d', %d, 180000)",
             t, al, ar, i + 1, i + 1);
    CHECK(exec(db, sql) == SQLITE_OK);
    snprintf(sql, sizeof sql,
             "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position)"
             " VALUES ('%s', '%s', '%s', 'a%02d')",
             e, pl, t, i);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

static void edit(dev *d, const char *sql) {
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  CHECK(exec(mc_library_db(d->lib), sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

static int digests_equal(dev **ds, int n) {
  char g0[MC_DIGEST_STRLEN], l0[MC_DIGEST_STRLEN], g[MC_DIGEST_STRLEN], l[MC_DIGEST_STRLEN];
  group_digest(ds[0], g0);
  library_digest(ds[0], l0);
  int same = g0[0] && l0[0];
  for (int i = 1; i < n; i++) {
    group_digest(ds[i], g);
    library_digest(ds[i], l);
    if (strcmp(g, g0) || strcmp(l, l0)) same = 0;
  }
  return same;
}

static mc_sync_lib_state lib_state(dev *d) {
  mc_sync_lib_state s = (mc_sync_lib_state)-1;
  CHECK(mc_sync_library_state(d->sync, &s) == MC_OK);
  return s;
}

/* --- The scenario --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_session.d", scratch);
  remove_tree(root);

  static dev a, b, c, d, e, f, g, h;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  dev_paths(&c, root, "c");
  dev_paths(&d, root, "d");
  dev_paths(&e, root, "e");
  dev_paths(&f, root, "f");
  dev_paths(&g, root, "g");
  dev_paths(&h, root, "h");
  if (!dev_start(&a) || !dev_start(&b) || !dev_start(&c)) return TEST_RESULT();
  fill_library(&a, 20);
  CHECK(lib_state(&a) == MC_SYNC_LIBRARY_NO_GROUP);

  /* A asks B. A's library has tracks and B's is empty, so the group syncs A's library and
   * A is its archive. A first types a wrong code. */
  CHECK(pair(&a, &b, 0, 1) == MC_PAIR_PAIRED);
  char group[MC_UID_STRLEN], sql[512], text[256];
  group_text(&a, "SELECT group_uid FROM local", group, sizeof group);
  CHECK(strlen(group) == 36);
  group_text(&b, "SELECT group_uid FROM local", text, sizeof text);
  CHECK_TEXT(text, group);
  group_text(&b, "SELECT library_uid FROM sync_group", text, sizeof text);
  CHECK_TEXT(text, mc_library_uid(a.lib));
  snprintf(sql, sizeof sql, "SELECT archive FROM profiles WHERE device_uid = '%s'", a.uid);
  CHECK(group_int(&b, sql) == 1);
  CHECK(group_int(&b, "SELECT count(*) FROM profiles WHERE archive = 1") == 1);
  CHECK(group_int(&b, "SELECT count(*) FROM devices WHERE length(public_key) = 32") == 2);
  CHECK(track_count(&b) == 20);
  CHECK(lib_state(&a) == MC_SYNC_LIBRARY_GROUP);
  CHECK(lib_state(&b) == MC_SYNC_LIBRARY_GROUP);
  {
    dev *ab[] = {&a, &b};
    /* Each side recorded its node uid after the group exchange: the next session carries it. */
    CHECK(sync_now(&a, &b) == MC_OK);
    CHECK(digests_equal(ab, 2));
  }

  /* C (empty) pairs with B by address and joins the group; A hears of C through B. */
  CHECK(pair(&c, &b, 1, 0) == MC_PAIR_PAIRED);
  CHECK(track_count(&c) == 20);
  CHECK(group_int(&c, "SELECT count(*) FROM devices") == 3);
  CHECK(group_int(&a, "SELECT count(*) FROM devices") == 2);
  CHECK(sync_now(&a, &b) == MC_OK);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM devices WHERE uid = '%s' AND removed = 0",
           c.uid);
  CHECK(group_int(&a, sql) == 1);
  {
    mc_sync_device_list *list = NULL;
    CHECK(mc_sync_list_devices(a.sync, &list) == MC_OK);
    if (list) {
      CHECK(list->count == 3);
      int self = 0, archives = 0;
      for (int i = 0; i < list->count; i++) {
        self += list->items[i].is_self;
        archives += list->items[i].archive;
        CHECK(list->items[i].name != NULL);
      }
      CHECK(self == 1 && archives == 1);
    }
    mc_sync_device_list_free(list);
  }

  /* Edits on two devices to different fields of one track both survive; the same field
   * edited on two devices ends the same everywhere. */
  char t1[MC_UID_STRLEN], t2[MC_UID_STRLEN];
  lib_text(&a, "SELECT uid FROM tracks WHERE title = 'Song 01'", t1, sizeof t1);
  lib_text(&a, "SELECT uid FROM tracks WHERE title = 'Song 02'", t2, sizeof t2);
  snprintf(sql, sizeof sql, "UPDATE tracks SET title = 'Song One' WHERE uid = '%s'", t1);
  edit(&a, sql);
  snprintf(sql, sizeof sql, "UPDATE tracks SET genre = 'Slowcore' WHERE uid = '%s'", t1);
  edit(&c, sql);
  snprintf(sql, sizeof sql, "UPDATE tracks SET title = 'From A' WHERE uid = '%s'", t2);
  edit(&a, sql);
  snprintf(sql, sizeof sql, "UPDATE tracks SET title = 'From B' WHERE uid = '%s'", t2);
  edit(&b, sql);
  snprintf(sql, sizeof sql, "UPDATE tracks SET title = 'From C' WHERE uid = '%s'", t2);
  edit(&c, sql);
  /* An album group made on C, with A's album in it, reaches everyone. */
  {
    char ag[MC_UID_STRLEN], ge[MC_UID_STRLEN], al[MC_UID_STRLEN];
    mc_uid_new(ag);
    mc_uid_new(ge);
    lib_text(&c, "SELECT uid FROM albums", al, sizeof al);
    CHECK(mc_library_begin_write(c.lib) == MC_OK);
    snprintf(sql, sizeof sql, "INSERT INTO album_groups (uid, name) VALUES ('%s', 'Late')", ag);
    CHECK(exec(mc_library_db(c.lib), sql) == SQLITE_OK);
    snprintf(sql, sizeof sql,
             "INSERT INTO album_group_entries (uid, group_uid, album_uid, position)"
             " VALUES ('%s', '%s', '%s', 'a0')",
             ge, ag, al);
    CHECK(exec(mc_library_db(c.lib), sql) == SQLITE_OK);
    CHECK(mc_library_commit(c.lib) == MC_OK);
  }
  /* Recents: A and then C open the album, so everyone keeps C's later open of it. A's artist
   * arrives everywhere, and the playlist C opened goes from everyone's recents because B
   * removed it after that. */
  char al[MC_UID_STRLEN], ar[MC_UID_STRLEN], pl[MC_UID_STRLEN], opened[32];
  lib_text(&a, "SELECT uid FROM albums", al, sizeof al);
  lib_text(&a, "SELECT uid FROM artists", ar, sizeof ar);
  lib_text(&a, "SELECT uid FROM playlists", pl, sizeof pl);
  CHECK(mc_library_record_opened(a.lib, MC_RECENT_ALBUM, al) == MC_OK);
  mc_sleep_ms(5);
  CHECK(mc_library_record_opened(c.lib, MC_RECENT_ALBUM, al) == MC_OK);
  mc_sleep_ms(5);
  CHECK(mc_library_record_opened(c.lib, MC_RECENT_PLAYLIST, pl) == MC_OK);
  mc_sleep_ms(5);
  CHECK(mc_library_record_opened(a.lib, MC_RECENT_ARTIST, ar) == MC_OK);
  CHECK(mc_library_record_opened(b.lib, MC_RECENT_PLAYLIST, pl) == MC_OK);
  CHECK(mc_library_remove_recent(b.lib, pl) == MC_OK);
  lib_text(&c, "SELECT opened_ms FROM recents WHERE kind = 'album'", opened, sizeof opened);
  dev *abc[] = {&a, &b, &c};
  for (int round = 0; round < 3 && !digests_equal(abc, 3); round++) {
    CHECK(sync_now(&a, &b) == MC_OK);
    CHECK(sync_now(&c, &b) == MC_OK);
  }
  CHECK(digests_equal(abc, 3));
  char want[256];
  snprintf(sql, sizeof sql, "SELECT title FROM tracks WHERE uid = '%s'", t2);
  lib_text(&a, sql, want, sizeof want);
  CHECK(!strcmp(want, "From A") || !strcmp(want, "From B") || !strcmp(want, "From C"));
  for (int i = 0; i < 3; i++) {
    snprintf(sql, sizeof sql, "SELECT title || '/' || genre FROM tracks WHERE uid = '%s'", t1);
    lib_text(abc[i], sql, text, sizeof text);
    CHECK_TEXT(text, "Song One/Slowcore");
    snprintf(sql, sizeof sql, "SELECT title FROM tracks WHERE uid = '%s'", t2);
    lib_text(abc[i], sql, text, sizeof text);
    CHECK_TEXT(text, want);
    CHECK(track_count(abc[i]) == 20);
    CHECK(query_int(mc_library_db(abc[i]->lib), "SELECT count(*) FROM playlist_entries") == 20);
    CHECK(query_int(mc_library_db(abc[i]->lib),
                    "SELECT count(*) FROM album_group_entries e JOIN album_groups g"
                    " ON g.uid = e.group_uid JOIN albums a ON a.uid = e.album_uid"
                    " WHERE g.name = 'Late'") == 1);
    mc_recent_list *recents = NULL;
    CHECK(mc_library_list_recents(abc[i]->lib, 0, &recents) == MC_OK && recents &&
          recents->count == 2);
    if (recents && recents->count == 2) {
      CHECK(recents->items[0].kind == MC_RECENT_ARTIST && !strcmp(recents->items[0].uid, ar));
      CHECK(recents->items[1].kind == MC_RECENT_ALBUM && !strcmp(recents->items[1].uid, al));
      snprintf(text, sizeof text, "%lld", (long long)recents->items[1].opened_ms);
      CHECK_TEXT(text, opened);
    }
    mc_recent_list_free(recents);
  }

  /* Refusals. D and E form a group of their own: A can't pair with D (in_group). F has a
   * library of its own, so it can't join A's group (library_mismatch). */
  if (dev_start(&d) && dev_start(&e) && dev_start(&f)) {
    CHECK(pair(&d, &e, 1, 0) == MC_PAIR_PAIRED);
    CHECK(pair(&a, &d, 1, 0) == MC_PAIR_IN_GROUP);
    CHECK(pair(&d, &e, 1, 0) == MC_PAIR_ALREADY_PAIRED);
    fill_library(&f, 2);
    CHECK(pair(&f, &a, 1, 0) == MC_PAIR_LIBRARY_MISMATCH);
    CHECK(group_int(&f, "SELECT count(*) FROM local WHERE group_uid IS NULL") == 1);
  }

  /* The only archive can't be removed, or leave. */
  CHECK(mc_sync_remove_device(b.sync, a.uid) == MC_ERR_LOCKED);
  CHECK(mc_sync_leave_group(a.sync) == MC_ERR_LOCKED);

  /* A copy of A's library folder on a new device: same library, so G may join; its node
   * uid is A's, so G takes a new one. */
  CHECK(exec(mc_library_db(a.lib), "PRAGMA wal_checkpoint(TRUNCATE)") == SQLITE_OK);
  CHECK(mc_mkdirs(g.libdir) == MC_OK);
  char from[1100], to[1100];
  snprintf(from, sizeof from, "%s/" MC_LIBRARY_DB_NAME, a.libdir);
  snprintf(to, sizeof to, "%s/" MC_LIBRARY_DB_NAME, g.libdir);
  CHECK(mc_copy_file(from, to, 1) == MC_OK);
  snprintf(from, sizeof from, "%s/" MC_LIBRARY_DB_NAME "-wal", a.libdir);
  snprintf(to, sizeof to, "%s/" MC_LIBRARY_DB_NAME "-wal", g.libdir);
  if (mc_file_exists(from)) mc_copy_file(from, to, 1);
  if (dev_start(&g)) {
    char before[MC_UID_STRLEN];
    snprintf(before, sizeof before, "%s", mc_library_node_uid(g.lib));
    CHECK_TEXT(before, mc_library_node_uid(a.lib));
    CHECK(pair(&g, &b, 1, 0) == MC_PAIR_PAIRED);
    lib_text(&g, "SELECT value FROM meta WHERE key = 'node_uid'", text, sizeof text);
    CHECK(strlen(text) == 36 && strcmp(text, before) != 0);
    CHECK(sync_now(&g, &b) == MC_OK); /* G's devices row now records its new uid */
    CHECK(sync_now(&a, &b) == MC_OK);
    CHECK(group_int(&a, "SELECT count(*) FROM devices WHERE node_uid IS NOT NULL") == 4);
    CHECK(group_int(&a, "SELECT count(DISTINCT node_uid) FROM devices"
                        " WHERE node_uid IS NOT NULL") == 4);
    snprintf(sql, sizeof sql, "UPDATE tracks SET year = 2001 WHERE uid = '%s'", t1);
    edit(&g, sql);
    CHECK(sync_now(&g, &b) == MC_OK);
    snprintf(sql, sizeof sql, "SELECT year FROM tracks WHERE uid = '%s'", t1);
    lib_text(&b, sql, text, sizeof text);
    CHECK_TEXT(text, "2001");
  }

  /* H joins with an empty library, then opens another library: group data still syncs,
   * the library doesn't. Its fresh mc_sync knows no addresses and has no discovery, so it
   * reaches B through B's address hints alone. */
  if (dev_start(&h)) {
    CHECK(pair(&h, &b, 1, 0) == MC_PAIR_PAIRED);
    CHECK(track_count(&h) == 20);
    dev_stop(&h);
    char other[512];
    snprintf(other, sizeof other, "%.400s/h/other-music", root);
    snprintf(h.libdir, sizeof h.libdir, "%s", other);
    {
      mc_sync_db *sdb = NULL;
      CHECK(mc_sync_db_open(h.config, &sdb) == MC_OK);
      if (sdb) {
        CHECK(exec(mc_sync_db_handle(sdb), "DELETE FROM peer_contact") == SQLITE_OK);
        mc_sync_db_close(sdb);
      }
    }
    if (dev_start(&h)) {
      CHECK(lib_state(&h) == MC_SYNC_LIBRARY_OTHER);
      int seen = done_seen(&h, b.uid);
      CHECK(sync_now(&h, &b) == MC_OK);
      mc_mutex_lock(&h.m);
      CHECK(h.ndone > seen && !h.done[h.ndone - 1].library_synced);
      mc_mutex_unlock(&h.m);
      CHECK(track_count(&h) == 0);
      CHECK(group_int(&h, "SELECT count(*) FROM devices WHERE removed = 0") == 5);
    }
  }

  /* Both sides calling each other at once: one session wins, nothing breaks. */
  {
    int sa = done_seen(&a, b.uid), sb = done_seen(&b, a.uid);
    CHECK(mc_sync_now(a.sync, b.uid) == MC_OK);
    CHECK(mc_sync_now(b.sync, a.uid) == MC_OK);
    wait_done(&a, b.uid, sa);
    wait_done(&b, a.uid, sb);
    mc_sleep_ms(500);
    CHECK(sync_now(&a, &b) == MC_OK);
    dev *ab[] = {&a, &b};
    CHECK(digests_equal(ab, 2));
  }

  /* Removing C: members refuse it from then on, and C forgets the group when told. */
  CHECK(mc_sync_remove_device(a.sync, c.uid) == MC_OK);
  CHECK(sync_now(&a, &b) == MC_OK);
  snprintf(sql, sizeof sql, "SELECT removed FROM devices WHERE uid = '%s'", c.uid);
  CHECK(group_int(&b, sql) == 1);
  {
    int seen = done_seen(&c, b.uid);
    CHECK(mc_sync_now(c.sync, b.uid) == MC_OK);
    mc_status cst = wait_done(&c, b.uid, seen);
    CHECK(cst != MC_OK);
    CHECK(group_int(&c, "SELECT count(*) FROM local WHERE group_uid IS NULL") == 1);
    CHECK(group_int(&c, "SELECT count(*) FROM devices") == 0);
    CHECK(lib_state(&c) == MC_SYNC_LIBRARY_NO_GROUP);
  }

  /* With the scheduler on, an edit reaches the other side within seconds, once the rate
   * limit is off: A and B just synced, so it would hold the edit for an hour (see
   * test_sync_rate.c). */
  mc_sync_set_test_auto_interval(a.sync, 0);
  mc_sync_set_test_auto_interval(b.sync, 0);
  CHECK(sync_now(&a, &b) == MC_OK); /* clears the limit the last session set */
  mc_sync_set_test_manual(a.sync, 0);
  mc_sync_set_test_manual(b.sync, 0);
  snprintf(sql, sizeof sql, "UPDATE tracks SET disc_no = 9 WHERE uid = '%s'", t2);
  edit(&a, sql);
  mc_sync_notify_changed(a.sync);
  int arrived = 0;
  snprintf(sql, sizeof sql, "SELECT disc_no FROM tracks WHERE uid = '%s'", t2);
  for (uint64_t end = mc_mono_ms() + WAIT_MS; !arrived && mc_mono_ms() < end; mc_sleep_ms(100)) {
    lib_text(&b, sql, text, sizeof text);
    arrived = !strcmp(text, "9");
  }
  CHECK(arrived);

  dev *all[] = {&a, &b, &c, &d, &e, &f, &g, &h};
  for (int i = 0; i < 8; i++) dev_stop(all[i]);
  mc_shutdown();
  if (!test_failures) remove_tree(root);
  return TEST_RESULT();
}
