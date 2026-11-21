/* Lost archives ("Taking over an archive" in docs/sync-protocol.md). Real devices over
 * loopback, discovery off. A is the archive that imported everything, B keeps originals of all
 * but one album, C holds nothing yet. Covers an archive too full to take what's new, files an
 * archive lost that only it had, an archive away for days and its row, the 30-day threshold
 * (last seen set back in sync.db), no device with room, the offer and taking over end to end (the lost device removed once this
 * one is checked, and what couldn't be had), and a reinstalled archive: recognised by its
 * library with its files, kind and name, never by its name alone. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "kithcore/i18n.h"
#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/archive.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 20000
#define MAX_EVENTS 64
#define DAY_MS ((int64_t)24 * 60 * 60 * 1000)

#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)


#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)

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
  uint32_t frames = 22050, bytes = frames * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 1);
  put32(f, 44100);
  put32(f, 88200);
  put16(f, 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++) put16(f, (unsigned)(uint16_t)(int16_t)((i * (int)pitch) % 20000));
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
  int codes, results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
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
                                        NULL, NULL, NULL, NULL, NULL, NULL, NULL};

static int dev_start(dev *d, const char *root, const char *label) {
  d->label = label;
  snprintf(d->config, sizeof d->config, "%s/%s/config", root, label);
  snprintf(d->libdir, sizeof d->libdir, "%s/%s/music", root, label);
  mc_mutex_init(&d->m);
  mc_cond_init(&d->c);
  CHECK(mc_library_open(d->libdir, &d->lib) == MC_OK);
  if (!d->lib) return 0;
  CHECK(mc_sync_new(d->lib, d->config, label, MC_DEVICE_DESKTOP, &k_events, d, &d->sync) == MC_OK);
  if (!d->sync) return 0;
  /* One process runs them all: each is given a host of its own. */
  snprintf(d->sync->host, sizeof d->sync->host, "host-%s", label);
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

/* Waits for a new session report on d about peer: its status, and the result in r. */
static mc_status wait_done(dev *d, const char *peer, int seen, mc_sync_result *r, int wait_ms) {
  uint64_t end = mc_mono_ms() + (uint64_t)wait_ms;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 50);
  mc_status st = MC_ERR_UNIMPLEMENTED;
  for (int i = d->ndone - 1; i >= 0 && count_done(d, peer) > seen; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      st = d->done[i].r.status;
      if (r) *r = d->done[i].r;
      break;
    }
  mc_mutex_unlock(&d->m);
  return st;
}

/* x's user presses Sync for y. */
static mc_status sync_now(dev *x, dev *y, mc_sync_result *rx) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx, rx, WAIT_MS);
  if (st == MC_OK) wait_done(y, x->uid, sy, NULL, WAIT_MS);
  return st;
}

static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  char code[MC_PAIR_CODE_LEN + 1] = "";
  mc_mutex_lock(&y->m);
  while (y->codes == codes && mc_mono_ms() < end) mc_cond_wait_ms(&y->c, &y->m, 50);
  memcpy(code, y->code, sizeof code);
  mc_mutex_unlock(&y->m);
  CHECK(mc_sync_pair_enter(x->sync, code) == MC_OK);
  mc_mutex_lock(&x->m);
  while (x->results == rx && mc_mono_ms() < end) mc_cond_wait_ms(&x->c, &x->m, 50);
  mc_pair_reason r = x->results > rx ? x->reason : (mc_pair_reason)-1;
  mc_mutex_unlock(&x->m);
  if (r == MC_PAIR_PAIRED) {
    CHECK(wait_done(x, y->uid, sx, NULL, WAIT_MS) == MC_OK);
    CHECK(wait_done(y, x->uid, sy, NULL, WAIT_MS) == MC_OK);
  }
  return r;
}

/* --- Looking inside --- */

static int exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n%s\n", sqlite3_errmsg(db), sql);
  return rc;
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

/* sql on d's sync.db through its API connection, unlogged, as if a peer had sent it. */
static void group_sql(dev *d, const char *sql) {
  mc_mutex_lock(&d->sync->api_lock);
  mc_sync_db *sdb = d->sync->api.sdb;
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  sdb->applying_remote = 1;
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  sdb->applying_remote = 0;
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  mc_mutex_unlock(&d->sync->api_lock);
}

static void group_text(dev *d, const char *sql, char *out, size_t cap) {
  mc_mutex_lock(&d->sync->api_lock);
  query_text(mc_sync_db_handle(d->sync->api.sdb), sql, out, cap);
  mc_mutex_unlock(&d->sync->api_lock);
}

typedef struct track_ref {
  char uid[MC_UID_STRLEN];
  char rel[256];
} track_ref;

/* A track with a WAV original at its organised path, imported on d. */
static void add_track(dev *d, const char *album, const char *title, int no, track_ref *out) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[1024], file[MC_UID_STRLEN];
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
  mc_uid_new(out->uid);
  mc_uid_new(file);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
           " VALUES ('%s', '%s', '%s', '%s', %d, 500)",
           out->uid, al, ar, title, no);
  CHECK(exec(db, sql) == SQLITE_OK);
  char dir[2048], p[2400];
  snprintf(out->rel, sizeof out->rel, "Low/%s (1994)/%02d %s.wav", album, no, title);
  snprintf(dir, sizeof dir, "%s/Low/%s (1994)", d->libdir, album);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(p, sizeof p, "%s/%s", d->libdir, out->rel);
  CHECK(write_wav(p, 100 + 37 * no));
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(p, &size, &mtime) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s', 'wav', 705, %lld, %lld)",
           file, out->uid, out->rel, (long long)size, (long long)mtime);
  CHECK(exec(db, sql) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

/* B keeps everything as originals, chosen by its user. */
static void choose_originals(dev *d) {
  mc_sync_profile *p = NULL;
  CHECK(mc_sync_get_profile(d->sync, d->uid, &p) == MC_OK);
  if (!p) return;
  p->format.format = MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = 0;
  p->sync_all = 1;
  p->budget_enabled = 0;
  CHECK(mc_sync_set_profile(d->sync, d->uid, p) == MC_OK);
  mc_sync_profile_free(p);
  mc_mutex_lock(&d->sync->api_lock);
  CHECK(mc_profile_choose(d->sync->api.sdb, NULL) == MC_OK);
  mc_mutex_unlock(&d->sync->api_lock);
}

/* The issue of kind about uid (NULL: any device) in p, NULL if none. */
static const mc_sync_issue *find_issue(const mc_sync_problems *p, mc_sync_issue_kind kind,
                                       const char *uid) {
  for (int i = 0; p && i < p->issue_count; i++)
    if (p->issues[i].kind == kind && (!uid || !strcmp(p->issues[i].device_uid, uid)))
      return &p->issues[i];
  return NULL;
}

/* Waits until d's issues have (want 1) or lack (0) one of kind about uid; then *out (may be
 * NULL) is a copy of the problems, to free. */
static int wait_issue(dev *d, mc_sync_issue_kind kind, const char *uid, int want,
                      mc_sync_problems **out) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  for (;;) {
    mc_sync_problems *p = NULL;
    CHECK(mc_sync_get_problems(d->sync, &p) == MC_OK);
    int has = find_issue(p, kind, uid) != NULL;
    if (has == want || mc_mono_ms() >= end) {
      if (has != want) {
        fprintf(stderr, "%s: issue %d %s, wanted %s; has:", d->label, kind,
                has ? "there" : "missing", want ? "there" : "gone");
        for (int i = 0; p && i < p->issue_count; i++) fprintf(stderr, " %d", p->issues[i].kind);
        fprintf(stderr, "\n");
      }
      if (out)
        *out = p;
      else
        mc_sync_problems_free(p);
      return has == want;
    }
    mc_sync_problems_free(p);
    mc_sync_waiting_changed(d->sync);
    mc_sleep_ms(50);
  }
}

/* d's row for uid in its device list, copied into out; 0 if not listed. Free out->name and
 * the rest with free_row(). */
static int row_of(dev *d, const char *uid, mc_sync_device_list **list, const mc_sync_device **out) {
  *out = NULL;
  CHECK(mc_sync_list_devices(d->sync, list) == MC_OK);
  for (int i = 0; *list && i < (*list)->count; i++)
    if (!strcmp((*list)->items[i].uid, uid)) *out = &(*list)->items[i];
  return *out != NULL;
}

/* --- Free space, as a test sets it --- */

static char g_small_dir[4][1024];
static int64_t g_small_free[4];

static void set_small(int i, const char *dir, int64_t free_bytes) {
  snprintf(g_small_dir[i], sizeof g_small_dir[i], "%s", dir ? dir : "");
  g_small_free[i] = free_bytes;
}

static mc_status test_free(const char *dir, int64_t *out) {
  for (int i = 0; i < 4; i++)
    if (g_small_dir[i][0] && !strcmp(dir, g_small_dir[i])) {
      *out = g_small_free[i];
      return MC_OK;
    }
  return mc_free_space(dir, out);
}

/* d last saw uid at ms, and uid last said it was up then: the test clock for "away". */
static void set_seen(dev *d, const char *uid, int64_t ms) {
  char sql[1024];
  snprintf(sql, sizeof sql,
           "INSERT INTO peer_contact (device_uid, last_sync_ms, seen_ms) VALUES ('%s', %lld, %lld)"
           " ON CONFLICT (device_uid) DO UPDATE SET last_sync_ms = excluded.last_sync_ms,"
           " seen_ms = excluded.seen_ms;"
           "UPDATE capabilities SET state = json_set(coalesce(state, '{}'), '$.seen', %lld)"
           " WHERE device_uid = '%s';",
           uid, (long long)ms, (long long)ms, (long long)ms, uid);
  group_sql(d, sql);
  mc_sync_waiting_changed(d->sync);
}

/* d tries a session with gone, which fails: it no longer counts it online. */
static void lose(dev *d, const char *gone) {
  int seen = done_seen(d, gone);
  CHECK(mc_sync_now(d->sync, gone) == MC_OK);
  CHECK(wait_done(d, gone, seen, NULL, WAIT_MS) != MC_OK);
}

static mc_sync_archive_offer *offer_of(dev *d) {
  mc_sync_archive_offer *o = NULL;
  CHECK(mc_sync_get_archive_offer(d->sync, &o) == MC_OK);
  return o;
}

static int wait_takeover(dev *d, mc_takeover_state want, mc_sync_takeover **out) {
  uint64_t end = mc_mono_ms() + 3 * WAIT_MS;
  for (;;) {
    mc_sync_takeover *t = NULL;
    CHECK(mc_sync_takeover_get(d->sync, &t) == MC_OK);
    if ((t && t->state == want) || mc_mono_ms() >= end || (t && t->state == MC_TAKEOVER_FAILED)) {
      int ok = t && t->state == want;
      if (!ok)
        fprintf(stderr, "%s: takeover state %d, wanted %d\n", d->label, t ? (int)t->state : -1,
                (int)want);
      *out = t;
      return ok;
    }
    mc_sync_takeover_free(t);
    mc_sleep_ms(50);
  }
}

static int removed(dev *d, const char *uid) {
  char sql[256], v[16];
  snprintf(sql, sizeof sql, "SELECT removed FROM devices WHERE uid = '%s'", uid);
  group_text(d, sql, v, sizeof v);
  return !strcmp(v, "1");
}

static void album_uid(dev *d, const char *title, char out[MC_UID_STRLEN]) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT uid FROM albums WHERE title = '%s'", title);
  query_text(mc_library_db(d->lib), sql, out, MC_UID_STRLEN);
}

/* Waits until d's issues have a notice of kind about uid; then *out holds the problems, to free. */
static const mc_sync_issue *wait_notice(dev *d, mc_sync_issue_kind kind, const char *uid,
                                        mc_sync_problems **out) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  for (;;) {
    mc_sync_problems *p = NULL;
    CHECK(mc_sync_get_problems(d->sync, &p) == MC_OK);
    for (int i = 0; p && i < p->issue_count; i++)
      if (p->issues[i].kind == kind && p->issues[i].notice && !strcmp(p->issues[i].device_uid, uid)) {
        *out = p;
        return &p->issues[i];
      }
    if (mc_mono_ms() >= end) {
      fprintf(stderr, "%s: no notice %d\n", d->label, kind);
      *out = p;
      return NULL;
    }
    mc_sync_problems_free(p);
    mc_sleep_ms(50);
  }
}

/* --- Tests --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mc_strings_set_locale("en");
  mc_sync_test_free_space = test_free;
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_takeover.d", scratch);
  mc_remove_tree(root);

  static dev a, b, c;
  if (!dev_start(&a, root, "a") || !dev_start(&b, root, "b") || !dev_start(&c, root, "c"))
    return TEST_RESULT();
  track_ref t[5];
  add_track(&a, "Kept", "One", 1, &t[0]);
  add_track(&a, "Kept", "Two", 2, &t[1]);
  add_track(&a, "Kept", "Three", 3, &t[2]);
  add_track(&a, "Gone", "Four", 1, &t[3]);
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  CHECK(pair(&b, &c) == MC_PAIR_PAIRED);
  CHECK(mc_archive_is(mc_sync_db_handle(b.sync->api.sdb), a.uid) == 1);

  /* B keeps everything as originals but the album "Gone", which only A has. */
  choose_originals(&b);
  {
    char gone[MC_UID_STRLEN];
    album_uid(&b, "Gone", gone);
    CHECK(mc_sync_exclude_items(b.sync, b.uid, MC_PROFILE_ALBUM,
                                (const char(*)[MC_UID_STRLEN])gone, 1, 1) == MC_OK);
    mc_sync_result r;
    CHECK(sync_now(&b, &a, &r) == MC_OK);
    CHECK(r.files_received == 3);
  }

  /* G: A has no room for a track B imports. A says so, and B shows it. */
  add_track(&b, "Kept", "New", 5, &t[4]);
  set_small(0, a.libdir, 10 << 20);
  CHECK(sync_now(&b, &a, NULL) == MC_OK);
  CHECK(sync_now(&b, &a, NULL) == MC_OK);
  {
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_FULL, a.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_ARCHIVE_FULL, a.uid);
    CHECK(is && is->tracks == 1 && is->bytes > 0);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "a is full: 1 track isn't backed up");
      CHECK(strstr(it.body, "a keeps the full copy of the library, but it needs") == it.body);
      CHECK_TEXT(it.fix, "Free some space on a, or make a device with more room the archive.");
    }
    mc_sync_problems_free(p);
  }
  /* With room again it takes the track, and the issue goes. */
  set_small(0, NULL, 0);
  CHECK(sync_now(&b, &a, NULL) == MC_OK);
  CHECK(sync_now(&b, &a, NULL) == MC_OK);
  CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_FULL, a.uid, 0, NULL));
  /* An archive that's up, the group's only one: nothing to say. */
  CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_AWAY, NULL, 0, NULL));

  /* B: A loses the files of One, which B has too, and Four, which nobody else has. B names
   * Four. */
  {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", a.libdir, t[0].rel);
    CHECK(remove(path) == 0);
    snprintf(path, sizeof path, "%s/%s", a.libdir, t[3].rel);
    CHECK(remove(path) == 0);
    mc_verify_result vr;
    CHECK(mc_sync_verify(a.sync, MC_VERIFY_FAST, &vr) == MC_OK);
    CHECK(vr.failed == 2);
    CHECK(sync_now(&b, &a, NULL) == MC_OK);
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_DAMAGED, a.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_ARCHIVE_DAMAGED, a.uid);
    CHECK(is && is->tracks == 1 && is->nexamples == 1);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "a has lost 1 track");
      CHECK_TEXT(it.body, "Files of “Four” are missing or damaged on a, which keeps the full copy"
                          " of the library. No other device has their original files.");
    }
    mc_sync_problems_free(p);
  }
  CHECK(sync_now(&a, &c, NULL) == MC_OK);
  CHECK(sync_now(&b, &c, NULL) == MC_OK);

  /* A goes away. */
  dev_stop(&a);
  lose(&b, a.uid);
  lose(&c, a.uid);
  int64_t now = (int64_t)mc_wall_ms();

  /* A: away 12 days, said on B's list and A's row. */
  set_seen(&b, a.uid, now - 12 * DAY_MS - 60000);
  {
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_AWAY, a.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_ARCHIVE_AWAY, a.uid);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "a last synced 12 days ago");
      CHECK_TEXT(it.body, "a keeps the only full copy of the library, and it last synced 12 days"
                          " ago. Music imported since isn't backed up there.");
    }
    CHECK(find_issue(p, MC_ISSUE_ARCHIVE_LOST, NULL) == NULL);
    mc_sync_problems_free(p);
    mc_sync_device_list *list = NULL;
    const mc_sync_device *row = NULL;
    CHECK(row_of(&b, a.uid, &list, &row));
    if (row) {
      CHECK(!row->online && row->archive);
      char warn[512];
      mc_sync_device_warning(b.sync, row, 0, warn, sizeof warn);
      CHECK_TEXT(warn, "Keeps the only full copy of the library and last synced 12 days ago.");
    }
    mc_sync_device_list_free(list);
    mc_sync_archive_offer *o = offer_of(&b);
    CHECK(o == NULL);
    mc_sync_archive_offer_free(o);
  }

  /* The threshold: 29 days is away, 31 lost. */
  set_seen(&b, a.uid, now - 29 * DAY_MS);
  CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_AWAY, a.uid, 1, NULL));
  CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_LOST, a.uid, 0, NULL));
  CHECK(mc_sync_take_over_archive(b.sync, a.uid) == MC_ERR_NOT_FOUND);
  set_seen(&b, a.uid, now - 31 * DAY_MS - 60000);
  set_seen(&c, a.uid, now - 31 * DAY_MS - 60000);
  CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_AWAY, a.uid, 0, NULL));
  CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_LOST, a.uid, 1, NULL));
  CHECK(wait_issue(&c, MC_ISSUE_ARCHIVE_LOST, a.uid, 1, NULL));

  /* D: no device has room for the library. */
  set_small(1, b.libdir, 10 << 20);
  {
    char sql[256];
    snprintf(sql, sizeof sql, "UPDATE capabilities SET free_bytes = 1000 WHERE device_uid = '%s'",
             c.uid);
    group_sql(&b, sql);
    mc_sync_archive_offer *o = offer_of(&b);
    CHECK(o && !o->can_take && o->nobody_can && !o->reinstall && o->bytes_needed > 0 &&
          o->bytes_free == (10 << 20));
    if (o) {
      mc_sync_offer_text ot;
      mc_sync_archive_offer_describe(o, MC_DEVICE_DESKTOP, &ot);
      CHECK_TEXT(ot.title, "Make this computer the archive?");
      CHECK(strstr(ot.body, "a hasn't synced for 31 days and keeps the only full copy of your"
                            " library.") == ot.body);
      CHECK(strstr(ot.body, "No device in the group has room for the ") != NULL);
      CHECK_TEXT(ot.button, "");
    }
    mc_sync_archive_offer_free(o);
    CHECK(mc_sync_take_over_archive(b.sync, a.uid) == MC_ERR_LOCKED);
    mc_sync_problems *p = NULL;
    const mc_sync_issue *is = NULL;
    for (uint64_t end = mc_mono_ms() + WAIT_MS;;) {
      mc_sync_waiting_changed(b.sync);
      mc_sleep_ms(100);
      CHECK(wait_issue(&b, MC_ISSUE_ARCHIVE_LOST, a.uid, 1, &p));
      is = find_issue(p, MC_ISSUE_ARCHIVE_LOST, a.uid);
      if ((is && is->count == 0) || mc_mono_ms() >= end) break;
      mc_sync_problems_free(p);
      p = NULL;
    }
    CHECK(is && is->count == 0);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "a hasn't synced for 31 days");
      CHECK(strstr(it.fix, "Turn a back on, or make a device with room for the whole library") ==
            it.fix);
    }
    mc_sync_problems_free(p);
    /* Still listed: nobody removed it. */
    CHECK(!removed(&b, a.uid));
  }

  /* C: B has room now. The offer names what only A had; then B takes over. */
  set_small(1, NULL, 0);
  {
    mc_sync_archive_offer *o = offer_of(&b);
    CHECK(o && o->can_take && !o->nobody_can && !o->asked && o->unrecoverable == 1 &&
          o->ntracks == 1);
    if (o && o->ntracks == 1) CHECK_TEXT(o->tracks[0].title, "Four");
    if (o) {
      mc_sync_offer_text ot;
      mc_sync_archive_offer_describe(o, MC_DEVICE_DESKTOP, &ot);
      CHECK(strstr(ot.body, "1 track can't be had in full quality any more, as no other device"
                            " has its original file: “Four”.") != NULL);
      CHECK_TEXT(ot.button, "Make This Device the Archive");
    }
    mc_sync_archive_offer_free(o);
    CHECK(mc_sync_archive_offer_seen(b.sync, a.uid) == MC_OK);
    o = offer_of(&b);
    CHECK(o && o->asked);
    mc_sync_archive_offer_free(o);
    /* C, which holds nothing, may take over too. */
    o = offer_of(&c);
    CHECK(o && o->can_take && o->unrecoverable == 1);
    mc_sync_archive_offer_free(o);

    CHECK(mc_sync_take_over_archive(b.sync, a.uid) == MC_OK);
    mc_status again = mc_sync_take_over_archive(b.sync, a.uid);
    CHECK(again == MC_ERR_LOCKED || again == MC_ERR_NOT_FOUND);
    mc_sync_takeover *tk = NULL;
    CHECK(wait_takeover(&b, MC_TAKEOVER_DONE, &tk));
    CHECK(tk && tk->unrecovered == 1 && tk->ntracks == 1 && !strcmp(tk->device_uid, a.uid));
    if (tk && tk->ntracks == 1) CHECK_TEXT(tk->tracks[0].title, "Four");
    mc_sync_takeover_free(tk);
    CHECK(removed(&b, a.uid));
    CHECK(mc_archive_is(mc_sync_db_handle(b.sync->api.sdb), b.uid) == 1);
    mc_sync_problems *p = NULL;
    const mc_sync_issue *is = wait_notice(&b, MC_ISSUE_TAKEOVER, a.uid, &p);
    CHECK(is && is->tracks == 1);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "This device has taken over from a");
      CHECK_TEXT(it.body, "a was removed from the group. 1 track couldn't be had as an original"
                          " file: “Four”.");
    }
    CHECK(find_issue(p, MC_ISSUE_ARCHIVE_LOST, NULL) == NULL);
    mc_sync_problems_free(p);
    mc_sync_dismiss_notices(b.sync);
    CHECK(wait_issue(&b, MC_ISSUE_TAKEOVER, a.uid, 0, NULL));

    /* C hears of it: its offer goes, and so does its chance. */
    CHECK(sync_now(&b, &c, NULL) == MC_OK);
    CHECK(wait_issue(&c, MC_ISSUE_ARCHIVE_LOST, a.uid, 0, NULL));
    o = offer_of(&c);
    CHECK(o == NULL);
    mc_sync_archive_offer_free(o);
    CHECK(mc_sync_take_over_archive(c.sync, a.uid) == MC_ERR_NOT_FOUND);
    CHECK(removed(&c, a.uid));
  }
  dev_stop(&b);
  dev_stop(&c);

#ifndef _WIN32
  /* E: X, the archive, is reinstalled as Z: its library folder copied back, with the same
   * name. W has the same name and nothing else. */
  {
    static dev x, y, z, w;
    char root2[1100];
    snprintf(root2, sizeof root2, "%s/e", root);
    if (!dev_start(&x, root2, "x") || !dev_start(&y, root2, "y")) return TEST_RESULT();
    track_ref u[2];
    add_track(&x, "Kept", "One", 1, &u[0]);
    add_track(&x, "Kept", "Two", 2, &u[1]);
    CHECK(pair(&x, &y) == MC_PAIR_PAIRED);
    dev_stop(&x);
    lose(&y, x.uid);
    char cmd[4096];
    snprintf(cmd, sizeof cmd, "mkdir -p '%s/z' && cp -a '%s' '%s/z/music'", root2, x.libdir, root2);
    CHECK(system(cmd) == 0);
    if (!dev_start(&z, root2, "z")) return TEST_RESULT();
    CHECK(mc_sync_set_device_name(z.sync, "x") == MC_OK);
    CHECK(pair(&z, &y) == MC_PAIR_PAIRED);
    mc_sync_problems *p = NULL;
    CHECK(wait_issue(&z, MC_ISSUE_ARCHIVE_REINSTALL, x.uid, 1, &p));
    const mc_sync_issue *is = find_issue(p, MC_ISSUE_ARCHIVE_REINSTALL, x.uid);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.title, "This looks like x, reinstalled");
    }
    mc_sync_problems_free(p);
    mc_sync_archive_offer *o = offer_of(&z);
    CHECK(o && o->reinstall && o->can_take && o->unrecoverable == 0);
    if (o) {
      mc_sync_offer_text ot;
      mc_sync_archive_offer_describe(o, MC_DEVICE_DESKTOP, &ot);
      CHECK_TEXT(ot.button, "Take Over");
    }
    mc_sync_archive_offer_free(o);

    /* The name alone isn't enough. */
    if (!dev_start(&w, root2, "w")) return TEST_RESULT();
    CHECK(mc_sync_set_device_name(w.sync, "x") == MC_OK);
    CHECK(pair(&w, &y) == MC_PAIR_PAIRED);
    mc_sync_waiting_changed(w.sync);
    mc_sleep_ms(500);
    CHECK(wait_issue(&w, MC_ISSUE_ARCHIVE_REINSTALL, NULL, 0, NULL));
    o = offer_of(&w);
    CHECK(o == NULL);
    mc_sync_archive_offer_free(o);
    /* Nor is any other device on the group's library. */
    CHECK(wait_issue(&y, MC_ISSUE_ARCHIVE_REINSTALL, NULL, 0, NULL));

    /* Z takes over: its own files checked, nothing to fetch, X removed. */
    CHECK(mc_sync_take_over_archive(z.sync, x.uid) == MC_OK);
    mc_sync_takeover *tk = NULL;
    CHECK(wait_takeover(&z, MC_TAKEOVER_DONE, &tk));
    CHECK(tk && tk->unrecovered == 0 && tk->tracks_total == 2 && tk->tracks_done == 2);
    mc_sync_takeover_free(tk);
    CHECK(removed(&z, x.uid));
    is = wait_notice(&z, MC_ISSUE_TAKEOVER, x.uid, &p);
    CHECK(is != NULL);
    if (is) {
      mc_sync_issue_text it;
      mc_sync_issue_describe(is, 0, &it);
      CHECK_TEXT(it.body, "x was removed from the group, and every track is here in full quality.");
    }
    mc_sync_problems_free(p);
    dev_stop(&w);
    dev_stop(&z);
    dev_stop(&y);
  }
#endif

  mc_remove_tree(root);
  return TEST_RESULT();
}
