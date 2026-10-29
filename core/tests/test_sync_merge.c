/* Pairing two devices whose libraries both have music: the larger library is kept and the
 * other device's music is merged into it (see "Merging libraries" in docs/sync-protocol.md).
 * Several devices in one process over loopback, discovery off, as in test_session.c:
 *  - the merged device asks to pair and confirms; both have a Last.fm account, so its own is
 *    signed out after one (failed) try to send its queued plays;
 *  - the merged device is asked, is in a group of its own (forgotten), and confirms next to
 *    the code; only it has an account, which becomes the group's;
 *  - the merged device declines;
 *  - the other device runs a version without merging: library_mismatch as before. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "scrobble/lastfm.h"
#include "sync/libmerge.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000
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

/* Whether dir holds a file whose name starts with prefix. */
typedef struct find_ctx {
  const char *prefix;
  int found;
} find_ctx;

static void find_prefix(void *user, const char *name, int is_dir) {
  find_ctx *f = user;
  if (!is_dir && !strncmp(name, f->prefix, strlen(f->prefix))) f->found = 1;
}

static int has_backup(const char *dir) {
  find_ctx f = {MC_LIBMERGE_PREFIX, 0};
  mc_list_dir(dir, find_prefix, &f);
  return f.found;
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
  int answer; /* how on_pair_merge answers when it needs to: 1, -1, or 0 to leave it */

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  int codes;
  int results;
  mc_pair_reason reason;
  int merged, kept_here;
  char signed_out[128];
  /* The last on_pair_merge. */
  int merges;
  mc_pair_merge pm;
  char pm_user[128], pm_forget[128];
  /* The last on_merge_done. */
  int merge_dones;
  mc_merge_result mr;
  char mr_signed_out[128];
  int progress;
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
  d->merged = r->merged;
  d->kept_here = r->kept_here;
  snprintf(d->signed_out, sizeof d->signed_out, "%s",
           r->scrobble_signed_out ? r->scrobble_signed_out : "?");
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

static void on_pair_merge(void *user, const mc_pair_merge *m) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->pm = *m;
  snprintf(d->pm_user, sizeof d->pm_user, "%s", m->scrobble_user ? m->scrobble_user : "?");
  d->pm_forget[0] = '\0';
  for (int i = 0; i < m->forget_count; i++) {
    size_t used = strlen(d->pm_forget);
    snprintf(d->pm_forget + used, sizeof d->pm_forget - used, "%s%s", i ? "," : "",
             m->forget_names[i]);
  }
  d->pm.name = d->pm.device_uid = d->pm.scrobble_user = NULL;
  d->pm.forget_names = NULL;
  d->merges++;
  int answer = m->needs_confirm ? d->answer : 0;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
  /* A callback may call the pairing functions. */
  if (answer) CHECK(mc_sync_pair_merge_confirm(d->sync, answer > 0) == MC_OK);
}

static void on_merge_progress(void *user, const mc_merge_progress *p) {
  dev *d = user;
  (void)p;
  mc_mutex_lock(&d->m);
  d->progress++;
  mc_mutex_unlock(&d->m);
}

static void on_merge_done(void *user, const mc_merge_result *r) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->mr = *r;
  snprintf(d->mr_signed_out, sizeof d->mr_signed_out, "%s",
           r->scrobble_signed_out ? r->scrobble_signed_out : "?");
  d->mr.backup_path = d->mr.scrobble_signed_out = NULL;
  d->merge_dones++;
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {NULL,         on_pair_code,      on_pair_result,
                                        NULL,         on_sync_done,      NULL,
                                        on_pair_merge, on_merge_progress, on_merge_done};

static void dev_paths(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
}

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
  if (!d->label || !d->lib) return;
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

/* Waits until *counter (under d->m) passes seen. */
static int wait_count(dev *d, const int *counter, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (*counter <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  int ok = *counter > seen;
  mc_mutex_unlock(&d->m);
  return ok;
}

static mc_status sync_now(dev *x, dev *y) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx);
  if (st == MC_OK) wait_done(y, x->uid, sy);
  return st;
}

/* x asks y to pair by address and types y's code. If y is the device to answer the merge
 * (y->answer set and it needs to), the answer comes from on_pair_merge before the code is
 * typed. Returns how it ended on x; on success waits for the first session on both. */
static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results, ry = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  int refused = 0, shown = 0;
  while (mc_mono_ms() < end && !refused && !shown) {
    mc_mutex_lock(&x->m);
    refused = x->results > rx;
    mc_mutex_unlock(&x->m);
    mc_mutex_lock(&y->m);
    shown = y->codes > codes;
    mc_mutex_unlock(&y->m);
    if (!refused && !shown) mc_sleep_ms(20);
  }
  if (refused) {
    mc_pair_reason r = x->reason;
    wait_count(y, &y->results, ry);
    return r;
  }
  char code[MC_PAIR_CODE_LEN + 1];
  mc_mutex_lock(&y->m);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  mc_sync_pair_enter(x->sync, code); /* x may already have given up (a declined merge) */
  CHECK(wait_count(x, &x->results, rx));
  CHECK(wait_count(y, &y->results, ry));
  mc_pair_reason r = x->reason;
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx) == MC_OK);
    CHECK(wait_done(y, x->uid, sy) == MC_OK);
  }
  return r;
}

/* --- Data --- */

static int exec(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n  %s\n", err ? err : "?", sql);
  sqlite3_free(err);
  return rc;
}

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

static int lib_int(dev *d, const char *sql) {
  return query_int(mc_library_db(d->lib), sql);
}

/* A signed-in scrobbling account in d's sync.db (logged, as a sign-in is), and plays
 * queued under it. Before dev_start(). */
static void sign_in(dev *d, const char *user, int queued) {
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(d->config, &sdb) == MC_OK);
  if (!sdb) return;
  char sql[512];
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO scrobble_account (uid, session_key, username)"
           " VALUES ('" MC_SCROBBLE_ACCOUNT_UID "', 'key-%s', '%s')",
           user, user);
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  for (int i = 0; i < queued; i++) {
    snprintf(sql, sizeof sql,
             "INSERT INTO scrobble_queue (played_at_s, artist, track) VALUES (%lld, 'Low', 'T%d')",
             (long long)(mc_wall_ms() / 1000) - 60 * (i + 1), i);
    CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  }
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  mc_sync_db_close(sdb);
}

/* n tracks of one album (with WAV-ish files, which merging needs on disk), logged. */
static void fill_library(dev *d, const char *album, int n) {
  mc_library *lib = NULL;
  CHECK(mc_library_open(d->libdir, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[1024];
  mc_uid_new(ar);
  mc_uid_new(al);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Artist %s')", ar,
           album);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid) VALUES ('%s', '%s', '%s')",
           al, album, ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  char dir[1100];
  snprintf(dir, sizeof dir, "%s/%s", d->libdir, album);
  CHECK(mc_mkdirs(dir) == MC_OK);
  for (int i = 0; i < n; i++) {
    char t[MC_UID_STRLEN], f[MC_UID_STRLEN], rel[256], abs[1400];
    mc_uid_new(t);
    mc_uid_new(f);
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
             " VALUES ('%s', '%s', '%s', '%s %d', %d, 200000)",
             t, al, ar, album, i + 1, i + 1);
    CHECK(exec(db, sql) == SQLITE_OK);
    snprintf(rel, sizeof rel, "%s/%02d.mp3", album, i + 1);
    snprintf(abs, sizeof abs, "%s/%s", d->libdir, rel);
    FILE *fp = mc_fopen(abs, "wb");
    CHECK(fp != NULL);
    if (fp) {
      fprintf(fp, "not really audio: %s %s %d", d->label, album, i);
      fclose(fp);
    }
    int64_t size = 0, mtime = 0;
    mc_file_info(abs, &size, &mtime);
    snprintf(sql, sizeof sql,
             "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
             " VALUES ('%s', '%s', '%s', 'mp3', 192, %lld, %lld)",
             f, t, rel, (long long)size, (long long)mtime);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  CHECK(mc_library_commit(lib) == MC_OK);
  mc_library_close(lib);
}

static mc_status offline_post(const char *url, const char *form, mc_buffer *body,
                              long *http_status) {
  (void)url;
  (void)form;
  (void)body;
  *http_status = 0;
  return MC_ERR_NET;
}

/* --- Tests --- */

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  char base[1024];
  snprintf(base, sizeof base, "%s/sync-merge-test", root);
  remove_tree(base);
  mc_lastfm_post = offline_post;

  dev a = {0}, b = {0}, c = {0}, d = {0}, e = {0}, f = {0}, g = {0}, h = {0};
  dev_paths(&a, base, "a");
  dev_paths(&b, base, "b");
  dev_paths(&c, base, "c");
  dev_paths(&d, base, "d");
  dev_paths(&e, base, "e");
  dev_paths(&f, base, "f");
  dev_paths(&g, base, "g");
  dev_paths(&h, base, "h");

  /* 1. A (1 track) asks B (3 tracks): B's library is kept, A confirms. Both are signed in;
   * A's two queued plays can't be sent (offline), so they're dropped with its account. */
  fill_library(&a, "Alpha", 1);
  fill_library(&b, "Beta", 3);
  sign_in(&a, "alice", 2);
  sign_in(&b, "bob", 0);
  a.answer = 1;
  if (dev_start(&a) && dev_start(&b)) {
    char b_lib[MC_UID_STRLEN];
    snprintf(b_lib, sizeof b_lib, "%s", mc_library_uid(b.lib));
    CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
    CHECK(b.reason == MC_PAIR_PAIRED);
    CHECK(a.merges == 1 && b.merges == 1);
    CHECK(!a.pm.kept_here && a.pm.needs_confirm && a.pm.initiator);
    CHECK(b.pm.kept_here && !b.pm.needs_confirm && !b.pm.initiator);
    CHECK(a.pm.tracks_here == 1 && a.pm.tracks_there == 3);
    CHECK(b.pm.tracks_here == 3 && b.pm.tracks_there == 1);
    CHECK(a.pm.scrobble_sign_out && b.pm.scrobble_sign_out);
    CHECK(!strcmp(a.pm_user, "alice") && !strcmp(b.pm_user, ""));
    CHECK(a.pm.forget_count == 0);
    CHECK(a.merged && b.merged && !a.kept_here && b.kept_here);
    CHECK(!strcmp(a.signed_out, "alice") && !strcmp(b.signed_out, "alice"));
    /* The merge ran after the first session. */
    CHECK(wait_count(&a, &a.merge_dones, 0));
    CHECK(a.mr.status == MC_OK && a.mr.tracks_added == 1);
    CHECK(!strcmp(a.mr_signed_out, "alice"));
    CHECK(a.mr.scrobbles_sent == 0 && a.mr.scrobbles_dropped == 2);
    CHECK(a.progress > 0);
    CHECK(b.merge_dones == 0);
    CHECK(!mc_sync_merge_pending(a.sync));
    char uid[64];
    query_text(mc_library_db(a.lib), "SELECT value FROM meta WHERE key = 'library_uid'", uid,
               sizeof uid);
    CHECK(!strcmp(uid, b_lib));
    CHECK(lib_int(&a, "SELECT count(*) FROM tracks") == 4);
    CHECK(lib_int(&a, "SELECT count(*) FROM files") == 1); /* its own; B's arrive later */
    CHECK(has_backup(a.libdir));
    /* A's account went; B's arrived in the first session. Its queue is empty. */
    char user[64];
    group_text(&a, "SELECT username FROM scrobble_account", user, sizeof user);
    CHECK(!strcmp(user, "bob"));
    CHECK(group_int(&a, "SELECT count(*) FROM scrobble_queue") == 0);
    /* A's music reaches B. */
    CHECK(sync_now(&a, &b) == MC_OK);
    CHECK(lib_int(&b, "SELECT count(*) FROM tracks") == 4);
    CHECK(lib_int(&b, "SELECT count(*) FROM tracks WHERE title = 'Alpha 1'") == 1);
    group_text(&b, "SELECT username FROM scrobble_account", user, sizeof user);
    CHECK(!strcmp(user, "bob"));
  }
  dev_stop(&a);
  dev_stop(&b);

  /* 2. D (2 tracks) is in a group with E. C (5 tracks) asks D: C's library is kept, so D
   * must confirm, which it does next to the code; it forgets E. Only D is signed in: its
   * account becomes the group's. */
  fill_library(&c, "Gamma", 5);
  fill_library(&d, "Delta", 2);
  sign_in(&d, "dora", 1);
  if (dev_start(&c) && dev_start(&d) && dev_start(&e)) {
    CHECK(pair(&e, &d) == MC_PAIR_PAIRED); /* E's library is empty: it joins */
    CHECK(c.merges == 0 && d.merges == 0 && e.merges == 0);
    d.answer = 1;
    CHECK(pair(&c, &d) == MC_PAIR_PAIRED);
    CHECK(c.pm.kept_here && !c.pm.needs_confirm);
    CHECK(!d.pm.kept_here && d.pm.needs_confirm && !d.pm.initiator);
    CHECK(!c.pm.scrobble_sign_out && !d.pm.scrobble_sign_out);
    CHECK(d.pm.forget_count == 1 && !strcmp(d.pm_forget, "e"));
    CHECK(c.merged && d.merged && c.kept_here && !d.kept_here);
    CHECK(!strcmp(c.signed_out, "") && !strcmp(d.signed_out, ""));
    CHECK(wait_count(&d, &d.merge_dones, 0));
    CHECK(d.mr.status == MC_OK && d.mr.tracks_added == 2);
    CHECK(lib_int(&d, "SELECT count(*) FROM tracks") == 7);
    /* D forgot E: only C and D are in its group now. */
    char sql[256];
    snprintf(sql, sizeof sql, "SELECT count(*) FROM devices WHERE uid = '%s'", e.uid);
    CHECK(group_int(&d, sql) == 0);
    CHECK(group_int(&d, "SELECT count(*) FROM devices WHERE removed = 0") == 2);
    /* D's account (and its queued play) stay, and become C's. */
    CHECK(group_int(&d, "SELECT count(*) FROM scrobble_queue") == 1);
    CHECK(sync_now(&d, &c) == MC_OK);
    char user[64];
    group_text(&c, "SELECT username FROM scrobble_account", user, sizeof user);
    CHECK(!strcmp(user, "dora"));
    CHECK(lib_int(&c, "SELECT count(*) FROM tracks") == 7);
  }
  dev_stop(&c);
  dev_stop(&d);
  dev_stop(&e);

  /* 3. F (1 track) asks G (2 tracks) and declines: nothing changes. */
  fill_library(&f, "Phi", 1);
  fill_library(&g, "Gee", 2);
  fill_library(&h, "Eta", 1);
  f.answer = -1;
  if (dev_start(&f) && dev_start(&g)) {
    char f_lib[MC_UID_STRLEN];
    snprintf(f_lib, sizeof f_lib, "%s", mc_library_uid(f.lib));
    CHECK(pair(&f, &g) == MC_PAIR_MERGE_DECLINED);
    CHECK(wait_count(&g, &g.results, 0));
    CHECK(g.reason == MC_PAIR_CANCELLED);
    CHECK(lib_int(&f, "SELECT count(*) FROM tracks") == 1);
    CHECK(!has_backup(f.libdir));
    CHECK(group_int(&f, "SELECT count(*) FROM local WHERE group_uid IS NULL") == 1);
    CHECK(group_int(&g, "SELECT count(*) FROM local WHERE group_uid IS NULL") == 1);

    /* 4. G runs a version without merging: H (1 track) is refused as before. */
    mc_sync_set_test_no_merge(g.sync, 1);
    int merges = g.merges;
    if (dev_start(&h)) {
      h.answer = 1;
      CHECK(pair(&h, &g) == MC_PAIR_LIBRARY_MISMATCH);
      CHECK(h.merges == 0 && g.merges == merges);
      CHECK(lib_int(&h, "SELECT count(*) FROM tracks") == 1);
      CHECK(!has_backup(h.libdir));
    }
  }
  dev_stop(&f);
  dev_stop(&g);
  dev_stop(&h);

  remove_tree(base);
  return TEST_RESULT();
}
