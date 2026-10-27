/* Sync profiles: mc_sync_get_profile() / mc_sync_set_profile() between devices of a group,
 * in test_session.c style. Two devices pair over loopback with discovery off; A has the
 * library, so it is the group's archive. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
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

/* Waits for a new session report on d about peer; returns its status. */
static mc_status wait_done(dev *d, const char *peer, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_status st = MC_ERR_UNIMPLEMENTED;
  for (int i = d->ndone - 1; i >= 0; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      if (count_done(d, peer) > seen) st = d->done[i].status;
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

/* x syncs with y now; waits for both sides to finish. */
static mc_status sync_now(dev *x, dev *y) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx);
  if (st == MC_OK) wait_done(y, x->uid, sy);
  return st;
}

/* x pairs with y by address, typing the code y shows; waits for the first session on both. */
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
    CHECK(wait_done(x, y->uid, sx) == MC_OK);
    CHECK(wait_done(y, x->uid, sy) == MC_OK);
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

/* Runs sql on a fresh connection to d's sync.db in a write; logged = 0 writes it as if it
 * came from a peer, so the triggers don't log it. */
static void group_edit(dev *d, const char *sql, int logged) {
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(d->config, &sdb) == MC_OK);
  if (!sdb) return;
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  sdb->applying_remote = !logged;
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  sdb->applying_remote = 0;
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  mc_sync_db_close(sdb);
}

/* Changelog rows d logged itself about profiles (field NULL: any field). */
static int logged(dev *d, const char *entity, const char *field) {
  char sql[512];
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM changelog WHERE origin_uid = '%s' AND entity = '%s'%s%s%s",
           d->uid, entity, field ? " AND field = '" : "", field ? field : "", field ? "'" : "");
  return group_int(d, sql);
}

static int count_items(dev *d, const char *owner, const char *kind, const char *ref) {
  char sql[512];
  snprintf(sql, sizeof sql,
           "SELECT count(*) FROM profile_items WHERE device_uid = '%s' AND kind = '%s'"
           " AND ref_uid = '%s'",
           owner, kind, ref);
  return group_int(d, sql);
}

static void lib_uid(dev *d, const char *sql, char out[MC_UID_STRLEN]) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(d->lib), sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, MC_UID_STRLEN, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
}

static void fill_library(dev *d) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], t[MC_UID_STRLEN], sql[512];
  mc_uid_new(ar);
  mc_uid_new(al);
  mc_uid_new(t);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', 'Things', '%s', 1994)",
           al, ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', 'Song', 1, 180000)",
           t, al, ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* --- Profiles --- */

static int has_item(const mc_sync_profile *p, mc_profile_item_kind kind, const char *ref) {
  int n = 0;
  for (int i = 0; i < p->count; i++)
    if (p->items[i].kind == kind && !strcmp(p->items[i].ref_uid, ref)) n++;
  return n;
}

static mc_profile_item item(mc_profile_item_kind kind, const char *ref) {
  mc_profile_item it;
  memset(&it, 0, sizeof it);
  it.kind = kind;
  snprintf(it.ref_uid, sizeof it.ref_uid, "%s", ref);
  return it;
}

static mc_sync_profile *get(dev *d, const char *uid) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(d->sync, uid, &p) == MC_OK);
  return p;
}

/* A selection of the given items, all else default. */
static mc_sync_profile selection(mc_profile_item *items, int count) {
  mc_sync_profile p;
  memset(&p, 0, sizeof p);
  p.sync_all = 1;
  p.items = items;
  p.count = count;
  return p;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mc_sync_profile_free(NULL); /* NULL is a no-op */
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_profile.d", scratch);
  remove_tree(root);

  static dev a, b;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  if (!dev_start(&a) || !dev_start(&b)) return TEST_RESULT();
  fill_library(&a);

  char x[MC_UID_STRLEN], y[MC_UID_STRLEN], z[MC_UID_STRLEN], w[MC_UID_STRLEN];
  char sql[1024];
  mc_uid_new(x);
  mc_uid_new(y);
  mc_uid_new(z);
  mc_uid_new(w);
  mc_sync_profile *p = NULL;

  /* Not in a group yet. */
  CHECK(mc_sync_get_profile(a.sync, a.uid, &p) == MC_ERR_NOT_FOUND);
  CHECK(p == NULL);
  {
    mc_sync_profile s = selection(NULL, 0);
    CHECK(mc_sync_set_profile(a.sync, a.uid, &s) == MC_ERR_NOT_FOUND);
  }
  CHECK(mc_sync_get_profile(a.sync, "nope", &p) == MC_ERR_INVALID_ARG);

  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);

  /* Unknown and removed devices. A member with no profiles row gets the defaults. */
  char fake[MC_UID_STRLEN], gone[MC_UID_STRLEN];
  mc_uid_new(fake);
  mc_uid_new(gone);
  CHECK(mc_sync_get_profile(a.sync, fake, &p) == MC_ERR_NOT_FOUND);
  snprintf(sql, sizeof sql,
           "INSERT INTO devices (uid, name, removed) VALUES ('%s', 'Gone', 1), ('%s', 'New', 0)",
           gone, fake);
  group_edit(&a, sql, 0);
  CHECK(mc_sync_get_profile(a.sync, gone, &p) == MC_ERR_NOT_FOUND);
  {
    mc_sync_profile s = selection(NULL, 0);
    CHECK(mc_sync_set_profile(a.sync, gone, &s) == MC_ERR_NOT_FOUND);
  }
  p = get(&a, fake);
  if (p) {
    CHECK(p->name == NULL && !p->archive && p->sync_all);
    CHECK(p->format.format == MC_FORMAT_ORIGINAL && p->format.bitrate_kbps == 0);
    CHECK(!p->budget_enabled && p->budget_bytes == 0);
    CHECK(p->count == 0 && p->items == NULL);
  }
  mc_sync_profile_free(p);
  /* Setting the defaults on it writes nothing. */
  {
    int before = logged(&a, "profiles", NULL);
    mc_sync_profile s = selection(NULL, 0);
    CHECK(mc_sync_set_profile(a.sync, fake, &s) == MC_OK);
    CHECK(logged(&a, "profiles", NULL) == before);
    snprintf(sql, sizeof sql, "SELECT count(*) FROM profiles WHERE device_uid = '%s'", fake);
    CHECK(group_int(&a, sql) == 0);
  }

  /* Bad arguments. */
  {
    mc_profile_item bad = item(MC_PROFILE_ALBUM, x);
    bad.kind = (mc_profile_item_kind)7;
    mc_sync_profile s = selection(&bad, 1);
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_ERR_INVALID_ARG);
    bad = item(MC_PROFILE_ALBUM, "short");
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_ERR_INVALID_ARG);
    s = selection(NULL, 0);
    s.format.format = MC_FORMAT_OPUS;
    s.format.bitrate_kbps = 100;
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_ERR_INVALID_ARG);
    s.format.bitrate_kbps = 0;
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_ERR_INVALID_ARG);
    s = selection(NULL, 0);
    s.budget_bytes = -1;
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_ERR_INVALID_ARG);
    CHECK(mc_sync_set_profile(a.sync, b.uid, NULL) == MC_ERR_INVALID_ARG);
  }

  /* A round trip of every field and all four kinds. */
  {
    mc_profile_item items[] = {item(MC_PROFILE_ALBUM, x), item(MC_PROFILE_ARTIST, y),
                               item(MC_PROFILE_PLAYLIST, z), item(MC_PROFILE_ALBUM_GROUP, w)};
    mc_sync_profile s = selection(items, 4);
    s.name = "Kitchen";
    s.sync_all = 0;
    s.format.format = MC_FORMAT_OPUS;
    s.format.bitrate_kbps = 128;
    s.budget_enabled = 1;
    s.budget_bytes = 32000000000LL;
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_OK);
    p = get(&a, b.uid);
    if (p) {
      CHECK_STR(p->name, "Kitchen");
      CHECK(!p->archive && !p->sync_all);
      CHECK(p->format.format == MC_FORMAT_OPUS && p->format.bitrate_kbps == 128);
      CHECK(p->budget_enabled && p->budget_bytes == 32000000000LL);
      CHECK(p->count == 4);
      CHECK(has_item(p, MC_PROFILE_ALBUM, x) == 1 && has_item(p, MC_PROFILE_ARTIST, y) == 1);
      CHECK(has_item(p, MC_PROFILE_PLAYLIST, z) == 1);
      CHECK(has_item(p, MC_PROFILE_ALBUM_GROUP, w) == 1);
    }
    mc_sync_profile_free(p);
    CHECK(count_items(&a, b.uid, "album", x) == 1 && count_items(&a, b.uid, "artist", y) == 1);
    CHECK(count_items(&a, b.uid, "playlist", z) == 1);
    CHECK(count_items(&a, b.uid, "album_group", w) == 1);
    snprintf(sql, sizeof sql, "SELECT bitrate_kbps FROM profiles WHERE device_uid = '%s'", b.uid);
    CHECK(group_int(&a, sql) == 128);

    /* The same profile again logs nothing. */
    int fields = logged(&a, "profiles", NULL), rows = logged(&a, "profile_items", NULL);
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_OK);
    CHECK(logged(&a, "profiles", NULL) == fields);
    CHECK(logged(&a, "profile_items", NULL) == rows);

    /* One field: one change. Original stores no bitrate; an empty name is no name. */
    s.format.format = MC_FORMAT_ORIGINAL;
    s.format.bitrate_kbps = 0;
    s.name = "";
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_OK);
    CHECK(logged(&a, "profiles", NULL) == fields + 3); /* format, bitrate_kbps, name */
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM profiles WHERE device_uid = '%s' AND bitrate_kbps IS NULL"
             " AND name IS NULL",
             b.uid);
    CHECK(group_int(&a, sql) == 1);
    fields = logged(&a, "profiles", NULL);

    /* Swapping one item for another: one delete and one insert; the others keep their uids. */
    char keep[MC_UID_STRLEN], v[MC_UID_STRLEN];
    mc_uid_new(v);
    snprintf(sql, sizeof sql,
             "SELECT uid FROM profile_items WHERE device_uid = '%s' AND kind = 'artist'", b.uid);
    {
      mc_sync_db *sdb = NULL;
      keep[0] = '\0';
      if (mc_sync_db_open(a.config, &sdb) == MC_OK) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), sql, -1, &stmt, NULL) == SQLITE_OK) {
          if (sqlite3_step(stmt) == SQLITE_ROW)
            snprintf(keep, sizeof keep, "%s", (const char *)sqlite3_column_text(stmt, 0));
          sqlite3_finalize(stmt);
        }
        mc_sync_db_close(sdb);
      }
    }
    CHECK(strlen(keep) == 36);
    items[0] = item(MC_PROFILE_ALBUM, v);
    int dels = logged(&a, "profile_items", "_deleted"), kinds = logged(&a, "profile_items", "kind");
    rows = logged(&a, "profile_items", NULL);
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_OK);
    CHECK(logged(&a, "profile_items", "_deleted") == dels + 1);
    CHECK(logged(&a, "profile_items", "kind") == kinds + 1);
    CHECK(logged(&a, "profile_items", NULL) == rows + 4); /* _deleted + 3 fields */
    CHECK(logged(&a, "profiles", NULL) == fields);
    CHECK(count_items(&a, b.uid, "album", x) == 0 && count_items(&a, b.uid, "album", v) == 1);
    snprintf(sql, sizeof sql,
             "SELECT count(*) FROM profile_items WHERE uid = '%s' AND kind = 'artist'", keep);
    CHECK(group_int(&a, sql) == 1);

    /* Duplicates come back once, and removing the item deletes every copy. An item of a kind
     * this version doesn't know is left out, and kept. */
    char d1[MC_UID_STRLEN], d2[MC_UID_STRLEN], u[MC_UID_STRLEN];
    mc_uid_new(d1);
    mc_uid_new(d2);
    mc_uid_new(u);
    snprintf(sql, sizeof sql,
             "INSERT INTO profile_items (uid, device_uid, kind, ref_uid) VALUES"
             " ('%s', '%s', 'album', '%s'), ('%s', '%s', 'album', '%s'),"
             " ('%s', '%s', 'genre', '%s')",
             d1, b.uid, x, d2, b.uid, x, u, b.uid, x);
    group_edit(&a, sql, 1);
    p = get(&a, b.uid);
    if (p) {
      CHECK(p->count == 5);
      CHECK(has_item(p, MC_PROFILE_ALBUM, x) == 1);
    }
    mc_sync_profile_free(p);
    dels = logged(&a, "profile_items", "_deleted");
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_OK); /* x isn't in s */
    CHECK(logged(&a, "profile_items", "_deleted") == dels + 2);
    CHECK(count_items(&a, b.uid, "album", x) == 0);
    CHECK(count_items(&a, b.uid, "genre", x) == 1);
    p = get(&a, b.uid);
    if (p) CHECK(p->count == 4 && !has_item(p, MC_PROFILE_ALBUM, x));
    mc_sync_profile_free(p);
    /* Setting duplicates stores one. */
    mc_profile_item twice[] = {item(MC_PROFILE_ARTIST, y), item(MC_PROFILE_ARTIST, y)};
    s = selection(twice, 2);
    CHECK(mc_sync_set_profile(a.sync, b.uid, &s) == MC_OK);
    CHECK(count_items(&a, b.uid, "artist", y) == 1);
    CHECK(count_items(&a, b.uid, "genre", x) == 1);
    p = get(&a, b.uid);
    if (p) CHECK(p->count == 1 && p->sync_all);
    mc_sync_profile_free(p);
  }

  /* A is the archive: locked to everything, originals, no budget; its name stays editable. */
  p = get(&a, a.uid);
  if (p) {
    CHECK(p->archive && p->sync_all && p->format.format == MC_FORMAT_ORIGINAL);
    p->sync_all = 0;
    CHECK(mc_sync_set_profile(a.sync, a.uid, p) == MC_ERR_LOCKED);
    p->sync_all = 1;
    p->format.format = MC_FORMAT_OPUS;
    p->format.bitrate_kbps = 160;
    CHECK(mc_sync_set_profile(b.sync, a.uid, p) == MC_ERR_LOCKED);
    p->format.format = MC_FORMAT_ORIGINAL;
    p->format.bitrate_kbps = 0;
    p->budget_enabled = 1;
    p->budget_bytes = 1000;
    CHECK(mc_sync_set_profile(a.sync, a.uid, p) == MC_ERR_LOCKED);
    p->budget_enabled = 0;
    p->budget_bytes = 0;
    CHECK(mc_sync_set_profile(a.sync, a.uid, p) == MC_OK);
    free(p->name);
    p->name = strdup("Vault");
    CHECK(mc_sync_set_profile(a.sync, a.uid, p) == MC_OK);
  }
  mc_sync_profile_free(p);
  p = get(&a, a.uid);
  if (p) CHECK_STR(p->name, "Vault");
  if (p) CHECK(p->archive && p->sync_all);
  mc_sync_profile_free(p);

  /* Concurrent edits to B's profile on A and on B both survive a session: A selects mode and
   * adds an album, B adds an artist and picks Opus. */
  {
    char album[MC_UID_STRLEN], artist[MC_UID_STRLEN];
    lib_uid(&a, "SELECT uid FROM albums", album);
    lib_uid(&a, "SELECT uid FROM artists", artist);
    CHECK(strlen(album) == 36 && strlen(artist) == 36);
    CHECK(sync_now(&a, &b) == MC_OK); /* B starts from what A has */

    mc_sync_profile *pa = get(&a, b.uid), *pb = get(&b, b.uid);
    if (pa && pb) {
      mc_profile_item *more = realloc(pa->items, (size_t)(pa->count + 1) * sizeof *more);
      CHECK(more != NULL);
      if (more) {
        pa->items = more;
        pa->items[pa->count++] = item(MC_PROFILE_ALBUM, album);
      }
      pa->sync_all = 0;
      CHECK(mc_sync_set_profile(a.sync, b.uid, pa) == MC_OK);
      more = realloc(pb->items, (size_t)(pb->count + 1) * sizeof *more);
      CHECK(more != NULL);
      if (more) {
        pb->items = more;
        pb->items[pb->count++] = item(MC_PROFILE_ARTIST, artist);
      }
      pb->format.format = MC_FORMAT_OPUS;
      pb->format.bitrate_kbps = 96;
      CHECK(mc_sync_set_profile(b.sync, b.uid, pb) == MC_OK);
    }
    mc_sync_profile_free(pa);
    mc_sync_profile_free(pb);
    CHECK(sync_now(&a, &b) == MC_OK);
    dev *both[] = {&a, &b};
    for (int i = 0; i < 2; i++) {
      p = get(both[i], b.uid);
      if (p) {
        CHECK(has_item(p, MC_PROFILE_ALBUM, album) == 1);
        CHECK(has_item(p, MC_PROFILE_ARTIST, artist) == 1);
        CHECK(has_item(p, MC_PROFILE_ARTIST, y) == 1);
        CHECK(p->count == 3);
        CHECK(!p->sync_all);
        CHECK(p->format.format == MC_FORMAT_OPUS && p->format.bitrate_kbps == 96);
      }
      mc_sync_profile_free(p);
    }
    CHECK(count_items(&b, b.uid, "genre", x) == 1); /* the unknown kind reached B as well */
  }

  dev_stop(&a);
  dev_stop(&b);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
