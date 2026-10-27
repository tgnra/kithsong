/* Device names: mc_sync_set_device_name(), the name a group gives a device through its
 * profile, the settings.json copy and the local address list, in test_session.c style:
 * devices in one process over loopback with discovery off. (Discovery's re-announced name
 * is checked in test_discovery.) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "musiccore/settings.h"
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
  mc_library *lib; /* the frontend's handle */
  mc_sync *sync;
  char uid[MC_UID_STRLEN];

  mc_mutex m;
  mc_cond c;
  char code[MC_PAIR_CODE_LEN + 1];
  char requester[MC_SYNC_NAME_MAX];
  int codes;
  int results;
  mc_pair_reason reason;
  done_event done[MAX_EVENTS];
  int ndone;
} dev;

static void on_pair_code(void *user, const char *code, const char *name, int attempts) {
  dev *d = user;
  (void)attempts;
  mc_mutex_lock(&d->m);
  snprintf(d->code, sizeof d->code, "%s", code);
  snprintf(d->requester, sizeof d->requester, "%s", name ? name : "");
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

/* x pairs with y by address; y shows a code and x types it. Returns how it ended on x; on
 * success, waits for the first session on both. */
static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results, ry = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_pair_request_address(x->sync, "127.0.0.1", mc_sync_port(y->sync)) == MC_OK);
  uint64_t end = mc_mono_ms() + WAIT_MS;
  int refused = 0;
  while (mc_mono_ms() < end) {
    mc_mutex_lock(&x->m);
    refused = x->results > rx;
    mc_mutex_unlock(&x->m);
    if (refused || wait_codes(y, codes)) break;
  }
  if (refused) return wait_result(x, rx);
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

static void group_text(dev *d, const char *sql, char *out, size_t cap) {
  mc_sync_db *sdb = NULL;
  out[0] = '\0';
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return;
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_sync_db_handle(sdb), sql, -1, &stmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) {
      const char *v = (const char *)sqlite3_column_text(stmt, 0);
      snprintf(out, cap, "%s", v ? v : "(null)");
    }
    sqlite3_finalize(stmt);
  }
  mc_sync_db_close(sdb);
}

/* A logged write to d's sync.db, as if made through the API. */
static void group_write(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  CHECK(mc_sync_db_open(d->config, &sdb) == MC_OK);
  if (!sdb) return;
  CHECK(mc_sync_db_begin_write(sdb) == MC_OK);
  CHECK(exec(mc_sync_db_handle(sdb), sql) == SQLITE_OK);
  CHECK(mc_sync_db_commit(sdb) == MC_OK);
  mc_sync_db_close(sdb);
}

/* The name d's own column says dev's profile has; "(null)" if none. */
static void profile_name(dev *d, dev *of, char *out, size_t cap) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT name FROM profiles WHERE device_uid = '%s'", of->uid);
  group_text(d, sql, out, cap);
}

/* The name d's mc_sync_list_devices() gives of. */
static void listed_name(dev *d, dev *of, char *out, size_t cap) {
  out[0] = '\0';
  mc_sync_device_list *list = NULL;
  CHECK(mc_sync_list_devices(d->sync, &list) == MC_OK);
  for (int i = 0; list && i < list->count; i++)
    if (!strcmp(list->items[i].uid, of->uid))
      snprintf(out, cap, "%s", list->items[i].name ? list->items[i].name : "(null)");
  mc_sync_device_list_free(list);
}

static void current_name(dev *d, char *out, size_t cap) {
  out[0] = '\0';
  CHECK(mc_sync_get_device_name(d->sync, out, cap) == MC_OK);
}

static void fill_library(dev *d, int tracks) {
  sqlite3 *db = mc_library_db(d->lib);
  char ar[MC_UID_STRLEN], al[MC_UID_STRLEN], sql[512];
  mc_uid_new(ar);
  mc_uid_new(al);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", ar);
  CHECK(exec(db, sql) == SQLITE_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', 'Things', '%s', 1994)",
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

/* --- Settings --- */

static void test_settings(const char *root) {
  char dir[1024], path[1100];
  snprintf(dir, sizeof dir, "%s/settings", root);
  CHECK(mc_mkdirs(dir) == MC_OK);
  snprintf(path, sizeof path, "%s/settings.json", dir);
  FILE *f = mc_fopen(path, "wb");
  CHECK(f != NULL);
  if (f) {
    fputs("{\"library_dir\": \"/music\", \"theme\": \"dark\"}", f);
    fclose(f);
  }

  mc_settings *s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  if (!s) return;
  CHECK(mc_settings_device_name(s) == NULL);
  CHECK(mc_settings_set_device_name(NULL, "x") == MC_ERR_INVALID_ARG);
  CHECK(mc_settings_set_device_name(s, "Living room") == MC_OK);
  CHECK(mc_settings_set_device_name(s, "Kitchen \xc3\xa9") == MC_OK); /* replaces */
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);

  s = NULL;
  CHECK(mc_settings_load(dir, &s) == MC_OK);
  if (!s) return;
  CHECK_STR(mc_settings_device_name(s), "Kitchen \xc3\xa9");
  CHECK_STR(mc_settings_library_dir(s), "/music");
  CHECK(mc_settings_set_device_name(s, "") == MC_OK); /* clears */
  CHECK(mc_settings_device_name(s) == NULL);
  CHECK(mc_settings_set_device_name(s, "Den") == MC_OK);
  CHECK(mc_settings_set_device_name(s, NULL) == MC_OK);
  CHECK(mc_settings_device_name(s) == NULL);
  CHECK(mc_settings_save(s) == MC_OK);
  mc_settings_free(s);

  /* Keys this version doesn't know survive. */
  char text[1024] = "";
  f = mc_fopen(path, "rb");
  if (f) {
    size_t n = fread(text, 1, sizeof text - 1, f);
    text[n] = '\0';
    fclose(f);
  }
  CHECK(strstr(text, "\"theme\"") != NULL);
  CHECK(strstr(text, "device_name") == NULL);
}

/* -1 when discovery doesn't list uid as nearby, else its in_other_group. */
static int nearby_other_group(dev *d, const char *uid) {
  mc_sync_nearby_list *list = NULL;
  int found = -1;
  CHECK(mc_sync_list_nearby(d->sync, &list) == MC_OK);
  for (int i = 0; list && i < list->count; i++)
    if (!strcmp(list->items[i].uid, uid)) found = list->items[i].in_other_group;
  mc_sync_nearby_list_free(list);
  return found;
}

/* --- The scenario --- */

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_name.d", scratch);
  remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);

  test_settings(root);

  /* Local addresses: numeric, loopback left out. */
  {
    char addrs[8][MC_SYNC_ADDR_STRLEN];
    CHECK(mc_sync_local_addresses(addrs, 0) == 0);
    int n = mc_sync_local_addresses(addrs, 8);
    CHECK(n >= 0 && n <= 8);
    for (int i = 0; i < n; i++) {
      CHECK(addrs[i][0] != '\0');
      CHECK(strcmp(addrs[i], "127.0.0.1") != 0 && strcmp(addrs[i], "::1") != 0);
    }
    printf("local addresses: %d%s%s\n", n, n ? ", first " : "", n ? addrs[0] : "");
  }

  static dev a, b;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  if (!dev_start(&a) || !dev_start(&b)) return TEST_RESULT();
  fill_library(&a, 3);

  char text[MC_SYNC_NAME_MAX * 2], sql[512];
  current_name(&a, text, sizeof text);
  CHECK_TEXT(text, "a");

  /* Rejects names with nothing in them; trims the rest. */
  CHECK(mc_sync_set_device_name(a.sync, NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_sync_set_device_name(a.sync, "") == MC_ERR_INVALID_ARG);
  CHECK(mc_sync_set_device_name(a.sync, " \t\r\n ") == MC_ERR_INVALID_ARG);
  CHECK(mc_sync_set_device_name(NULL, "x") == MC_ERR_INVALID_ARG);
  current_name(&a, text, sizeof text);
  CHECK_TEXT(text, "a");
  CHECK(mc_sync_set_device_name(a.sync, "  Desk \t") == MC_OK);
  current_name(&a, text, sizeof text);
  CHECK_TEXT(text, "Desk");

  /* Too long: cut at MC_SYNC_DEVICE_NAME_MAX bytes, never inside a character. "ab" and 63
   * two-byte characters are 128 bytes; the last character goes whole. */
  {
    char longname[256] = "ab";
    for (int i = 0; i < 63; i++) strcat(longname, "\xc3\xa9");
    CHECK(strlen(longname) == 128);
    CHECK(mc_sync_set_device_name(a.sync, longname) == MC_OK);
    current_name(&a, text, sizeof text);
    CHECK(strlen(text) == 126);
    CHECK(strncmp(text, longname, 126) == 0);
    /* A small buffer cuts on a boundary too. */
    char small[4];
    CHECK(mc_sync_get_device_name(a.sync, small, sizeof small) == MC_OK);
    CHECK_TEXT(small, "ab");
    CHECK(mc_sync_get_device_name(a.sync, small, 0) == MC_ERR_INVALID_ARG);
  }

  /* A device in a group, seen by one in none: pairable (we'd join it), so not "in another
   * group". */
  static const char k_grouped[] = "00000000-0000-4000-8000-0000000000a1";
  static const char k_ungrouped[] = "00000000-0000-4000-8000-0000000000a2";
  mc_sync_test_peer(a.sync, k_grouped, "Grouped", "desktop", "0123456789abcdef", "127.0.0.1",
                    9, 1);
  mc_sync_test_peer(a.sync, k_ungrouped, "Loose", "desktop", "", "127.0.0.1", 9, 1);
  CHECK(nearby_other_group(&a, k_grouped) == 0);
  CHECK(nearby_other_group(&a, k_ungrouped) == 0);

  /* Renamed before pairing: B's code prompt names A by its new name. */
  CHECK(mc_sync_set_device_name(a.sync, "Studio") == MC_OK);
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  CHECK_TEXT(b.requester, "Studio");

  /* Now A is in a group too: the other group's device clashes, the loose one doesn't. */
  CHECK(nearby_other_group(&a, k_grouped) == 1);
  CHECK(nearby_other_group(&a, k_ungrouped) == 0);
  mc_sync_test_peer(a.sync, k_grouped, "Grouped", "desktop", "", "", 0, 0);
  mc_sync_test_peer(a.sync, k_ungrouped, "Loose", "desktop", "", "", 0, 0);
  CHECK(nearby_other_group(&a, k_grouped) == -1);

  /* Each device gave its own profile its name after the first session. */
  profile_name(&a, &a, text, sizeof text);
  CHECK_TEXT(text, "Studio");
  profile_name(&b, &b, text, sizeof text);
  CHECK_TEXT(text, "b");
  /* The archive row A's pairing made is intact. */
  snprintf(sql, sizeof sql, "SELECT archive FROM profiles WHERE device_uid = '%s'", a.uid);
  group_text(&b, sql, text, sizeof text);
  CHECK_TEXT(text, "1");
  CHECK(sync_now(&a, &b) == MC_OK);
  profile_name(&a, &b, text, sizeof text);
  CHECK_TEXT(text, "b");
  profile_name(&b, &a, text, sizeof text);
  CHECK_TEXT(text, "Studio");

  /* A renames B through B's profile; after a session B takes the name on, and both list
   * it. */
  snprintf(sql, sizeof sql, "UPDATE profiles SET name = 'Living room' WHERE device_uid = '%s'",
           b.uid);
  group_write(&a, sql);
  listed_name(&a, &b, text, sizeof text);
  CHECK_TEXT(text, "Living room");
  current_name(&b, text, sizeof text);
  CHECK_TEXT(text, "b");
  CHECK(sync_now(&a, &b) == MC_OK);
  current_name(&b, text, sizeof text);
  CHECK_TEXT(text, "Living room");
  listed_name(&b, &b, text, sizeof text);
  CHECK_TEXT(text, "Living room");
  snprintf(sql, sizeof sql, "SELECT name FROM devices WHERE uid = '%s'", b.uid);
  group_text(&b, sql, text, sizeof text);
  CHECK_TEXT(text, "Living room"); /* B wrote its own row */
  CHECK(sync_now(&a, &b) == MC_OK);
  group_text(&a, sql, text, sizeof text);
  CHECK_TEXT(text, "Living room");

  /* With no profile name, a device shows its own devices.name. */
  snprintf(sql, sizeof sql,
           "UPDATE profiles SET name = NULL WHERE device_uid = '%s';"
           "UPDATE devices SET name = 'Old B' WHERE uid = '%s'",
           b.uid, b.uid);
  group_write(&a, sql);
  listed_name(&a, &b, text, sizeof text);
  CHECK_TEXT(text, "Old B");
  snprintf(sql, sizeof sql, "UPDATE profiles SET name = '' WHERE device_uid = '%s'", b.uid);
  group_write(&a, sql);
  listed_name(&a, &b, text, sizeof text);
  CHECK_TEXT(text, "Old B");

  /* A device renaming itself in a group sets its profile name and its devices row. */
  CHECK(mc_sync_set_device_name(b.sync, " Kitchen ") == MC_OK);
  current_name(&b, text, sizeof text);
  CHECK_TEXT(text, "Kitchen");
  profile_name(&b, &b, text, sizeof text);
  CHECK_TEXT(text, "Kitchen");
  snprintf(sql, sizeof sql, "SELECT name FROM devices WHERE uid = '%s'", b.uid);
  group_text(&b, sql, text, sizeof text);
  CHECK_TEXT(text, "Kitchen");
  CHECK(sync_now(&b, &a) == MC_OK);
  listed_name(&a, &b, text, sizeof text);
  CHECK_TEXT(text, "Kitchen");
  profile_name(&a, &b, text, sizeof text);
  CHECK_TEXT(text, "Kitchen");

  /* Started again with its old settings name, B takes on the group's at once. */
  dev_stop(&b);
  if (dev_start(&b)) {
    current_name(&b, text, sizeof text);
    CHECK_TEXT(text, "Kitchen");
  }

  dev_stop(&a);
  dev_stop(&b);
  remove_tree(root);
  mc_shutdown();
  return TEST_RESULT();
}
