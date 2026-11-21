/* Protocol versions between devices in one process, over loopback, with discovery off (see
 * "Versions" in docs/sync-protocol.md): devices of one version sync and negotiate optional
 * features; a peer that sends no version, or an old release's, and a peer above this
 * version's range are refused before any data moves, with the outdated side marked on both;
 * and pairing across versions is refused before anything is written. Old releases are played
 * by devices posing as them (mc_sync_set_test_version() with check 0): they send v 1 or none,
 * refuse nobody, and take the "version" refusals for unknown errors, as 0.9.4 does. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/i18n.h"
#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 30000
#define MAX_EVENTS 64

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
  mc_status pair_status;
  char result_peer[MC_UID_STRLEN];
  char result_name[128];
  mc_sync_version result_version;
  done_event done[MAX_EVENTS];
  int ndone;
  int versions; /* on_version calls */
  char version_peer[MC_UID_STRLEN];
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
  d->pair_status = r->status;
  snprintf(d->result_peer, sizeof d->result_peer, "%s", r->device_uid);
  snprintf(d->result_name, sizeof d->result_name, "%s", r->name);
  d->result_version = r->version;
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

static void on_version(void *user, const char *device_uid) {
  dev *d = user;
  mc_mutex_lock(&d->m);
  d->versions++;
  snprintf(d->version_peer, sizeof d->version_peer, "%s", device_uid);
  mc_cond_broadcast(&d->c);
  mc_mutex_unlock(&d->m);
}

static const mc_sync_events k_events = {.on_pair_code = on_pair_code,
                                        .on_pair_result = on_pair_result,
                                        .on_sync_done = on_sync_done,
                                        .on_version = on_version};

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

/* As this build, or as a release from before versions (v 1, nothing else, no checks). */
static void as_current(dev *d) {
  mc_sync_set_test_version(d->sync, MC_PROTOCOL_VERSION, MC_PROTOCOL_MIN_VERSION, mc_version(), 1,
                           MC_FEAT_DEFAULT);
}

static void as_old_release(dev *d) {
  mc_sync_set_test_version(d->sync, 1, -1, NULL, 0, 0);
}

static void introduce(dev *x, dev *y) {
  mc_sync_test_peer(x->sync, y->uid, y->label, "desktop", "", "127.0.0.1", mc_sync_port(y->sync),
                    1);
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

/* Waits for a new session report on d about peer; returns its status. */
static mc_status wait_done(dev *d, const char *peer, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (count_done(d, peer) <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_status st = MC_ERR_UNIMPLEMENTED;
  for (int i = d->ndone - 1; i >= 0 && count_done(d, peer) > seen; i--)
    if (!strcmp(d->done[i].peer, peer)) {
      st = d->done[i].status;
      break;
    }
  mc_mutex_unlock(&d->m);
  return st;
}

/* x syncs with y now; *on_y gets y's report. */
static mc_status sync_now(dev *x, dev *y, mc_status *on_y) {
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  CHECK(mc_sync_now(x->sync, y->uid) == MC_OK);
  mc_status st = wait_done(x, y->uid, sx);
  mc_status sty = wait_done(y, x->uid, sy);
  if (on_y) *on_y = sty;
  return st;
}

static mc_pair_reason wait_result(dev *d, int seen) {
  uint64_t end = mc_mono_ms() + WAIT_MS;
  mc_mutex_lock(&d->m);
  while (d->results <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 100);
  mc_pair_reason r = d->results > seen ? d->reason : (mc_pair_reason)-1;
  mc_mutex_unlock(&d->m);
  return r;
}

static int wait_codes(dev *d, int seen, int ms) {
  uint64_t end = mc_mono_ms() + (uint64_t)ms;
  mc_mutex_lock(&d->m);
  while (d->codes <= seen && mc_mono_ms() < end) mc_cond_wait_ms(&d->c, &d->m, 50);
  int ok = d->codes > seen;
  mc_mutex_unlock(&d->m);
  return ok;
}

/* x asks y to pair, once x's previous pairing has wound up (it reports before it's over). */
static void request_pair(dev *x, dev *y) {
  introduce(x, y);
  mc_status st = MC_ERR_LOCKED;
  for (int i = 0; i < 100 && (st = mc_sync_pair_request(x->sync, y->uid)) == MC_ERR_LOCKED; i++) mc_sleep_ms(20);
  CHECK(st == MC_OK);
}

/* x and y pair with the right code: y shows it, x types it. */
static mc_pair_reason pair(dev *x, dev *y) {
  int codes = y->codes, rx = x->results, ry = y->results;
  int sx = done_seen(x, y->uid), sy = done_seen(y, x->uid);
  introduce(x, y);
  CHECK(mc_sync_pair_request(x->sync, y->uid) == MC_OK);
  if (!wait_codes(y, codes, WAIT_MS)) return wait_result(x, rx);
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

static int query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static int group_int(dev *d, const char *sql) {
  mc_sync_db *sdb = NULL;
  if (mc_sync_db_open(d->config, &sdb) != MC_OK) return -1;
  int n = query_int(mc_sync_db_handle(sdb), sql);
  mc_sync_db_close(sdb);
  return n;
}

static int lib_int(dev *d, const char *sql) {
  return query_int(mc_library_db(d->lib), sql);
}

/* Changelog rows d holds that came from (or were made by) origin. */
static int rows_from(dev *d, const char *origin, int library) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE origin_uid = '%s'", origin);
  return library ? lib_int(d, sql) : group_int(d, sql);
}

static void add_playlist(dev *d, const char *name) {
  char uid[MC_UID_STRLEN], sql[256];
  mc_uid_new(uid);
  snprintf(sql, sizeof sql, "INSERT INTO playlists (uid, name) VALUES ('%s', '%s')", uid, name);
  CHECK(mc_library_begin_write(d->lib) == MC_OK);
  CHECK(sqlite3_exec(mc_library_db(d->lib), sql, NULL, NULL, NULL) == SQLITE_OK);
  CHECK(mc_library_commit(d->lib) == MC_OK);
}

static int playlists_named(dev *d, const char *name) {
  char sql[256];
  snprintf(sql, sizeof sql, "SELECT count(*) FROM playlists WHERE name = '%s'", name);
  return lib_int(d, sql);
}

/* What d knows of peer's version, from the device list or else the nearby one. */
static mc_sync_version version_of(dev *d, const char *peer) {
  mc_sync_version v;
  memset(&v, 0xff, sizeof v);
  mc_sync_device_list *list = NULL;
  if (mc_sync_list_devices(d->sync, &list) == MC_OK) {
    for (int i = 0; i < list->count; i++)
      if (!strcmp(list->items[i].uid, peer)) v = list->items[i].version;
    mc_sync_device_list_free(list);
  }
  return v;
}

static mc_sync_version nearby_version(dev *d, const char *peer) {
  mc_sync_version v;
  memset(&v, 0xff, sizeof v);
  mc_sync_nearby_list *list = NULL;
  if (mc_sync_list_nearby(d->sync, &list) == MC_OK) {
    for (int i = 0; i < list->count; i++)
      if (!strcmp(list->items[i].uid, peer)) v = list->items[i].version;
    mc_sync_nearby_list_free(list);
  }
  return v;
}

static int versions_seen(dev *d) {
  mc_mutex_lock(&d->m);
  int n = d->versions;
  mc_mutex_unlock(&d->m);
  return n;
}

/* --- The scenarios --- */

/* The rule itself, and the notice's wording. */
static void test_rule(void) {
  mc_version_self me;
  mc_version_self_default(&me);
  CHECK(me.protocol == 2 && me.protocol_min == 2);
  mc_sync_version v = {MC_SYNC_VERSION_OK, 2, 2, "0.9.5"};
  CHECK(mc_version_judge(&me, &v) && v.outdated == MC_SYNC_VERSION_OK);
  v = (mc_sync_version){MC_SYNC_VERSION_OK, 0, 0, ""}; /* no version at all */
  CHECK(!mc_version_judge(&me, &v) && v.outdated == MC_SYNC_PEER_OUTDATED);
  v = (mc_sync_version){MC_SYNC_VERSION_OK, 1, 0, ""}; /* 0.9.4's HELLO says v 1 */
  CHECK(!mc_version_judge(&me, &v) && v.outdated == MC_SYNC_PEER_OUTDATED);
  v = (mc_sync_version){MC_SYNC_VERSION_OK, 3, 2, "1.0"}; /* newer, still takes us */
  CHECK(mc_version_judge(&me, &v) && v.outdated == MC_SYNC_VERSION_OK);
  v = (mc_sync_version){MC_SYNC_VERSION_OK, 4, 3, "1.1"}; /* newer, past us */
  CHECK(!mc_version_judge(&me, &v) && v.outdated == MC_SYNC_SELF_OUTDATED);

  char text[256];
  v = (mc_sync_version){MC_SYNC_PEER_OUTDATED, 3, 3, "0.9.3"};
  mc_sync_version_text("Tania's desktop", &v, text, sizeof text);
  CHECK_TEXT(text, "Tania's desktop runs Kithsong 0.9.3, which can't sync with this version."
                   " Update Kithsong on Tania's desktop.");
  v = (mc_sync_version){MC_SYNC_SELF_OUTDATED, 4, 3, "1.1"};
  mc_sync_version_text("Tania's desktop", &v, text, sizeof text);
  CHECK_TEXT(text, "Tania's desktop runs Kithsong 1.1, which can't sync with this version."
                   " Update Kithsong on this device.");
  v = (mc_sync_version){MC_SYNC_PEER_OUTDATED, 1, 0, ""};
  mc_sync_version_text(NULL, &v, text, sizeof text);
  CHECK_TEXT(text, "The other device runs an older version of Kithsong, which can't sync with"
                   " this version. Update Kithsong on the other device.");
  v.outdated = MC_SYNC_VERSION_OK;
  CHECK(mc_sync_version_text("x", &v, text, sizeof text) == 0 && !text[0]);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_rule();
  char root[1024];
  snprintf(root, sizeof root, "%s/test_sync_version.d", scratch);
  remove_tree(root);

  static dev a, b, c, d;
  dev_paths(&a, root, "a");
  dev_paths(&b, root, "b");
  dev_paths(&c, root, "c");
  dev_paths(&d, root, "d");
  if (!dev_start(&a) || !dev_start(&b) || !dev_start(&c) || !dev_start(&d)) return TEST_RESULT();
  add_playlist(&a, "Mix");

  /* Same version: they pair and sync, and nothing is marked. */
  CHECK(pair(&a, &b) == MC_PAIR_PAIRED);
  CHECK(playlists_named(&b, "Mix") == 1);
  mc_status on_b = MC_ERR_UNIMPLEMENTED;
  CHECK(sync_now(&a, &b, &on_b) == MC_OK);
  CHECK(on_b == MC_OK);
  CHECK(version_of(&a, b.uid).outdated == MC_SYNC_VERSION_OK);
  CHECK(version_of(&b, a.uid).outdated == MC_SYNC_VERSION_OK);

  /* A capability flag: used only when both HELLOs name it. */
  CHECK(mc_sync_test_last_features(a.sync) == 0);
  mc_sync_set_test_version(a.sync, MC_PROTOCOL_VERSION, MC_PROTOCOL_MIN_VERSION, mc_version(), 1,
                           MC_FEAT_DEFAULT | MC_FEAT_BIT(MC_FEAT_TEST));
  CHECK(sync_now(&a, &b, NULL) == MC_OK);
  CHECK(mc_sync_test_last_features(a.sync) == 0);
  CHECK(mc_sync_test_last_features(b.sync) == 0);
  mc_sync_set_test_version(b.sync, MC_PROTOCOL_VERSION, MC_PROTOCOL_MIN_VERSION, mc_version(), 1,
                           MC_FEAT_DEFAULT | MC_FEAT_BIT(MC_FEAT_TEST));
  CHECK(sync_now(&b, &a, NULL) == MC_OK);
  CHECK(mc_sync_test_last_features(a.sync) == MC_FEAT_BIT(MC_FEAT_TEST));
  CHECK(mc_sync_test_last_features(b.sync) == MC_FEAT_BIT(MC_FEAT_TEST));
  as_current(&a);
  as_current(&b);

  /* B as a release from before versions (its HELLO says v 1), with changes of its own and of
   * A's waiting. A refuses it after the HELLOs, either way round; B, which knows no such
   * refusal, ends its session as on any unknown error. Nothing moves either way. */
  as_old_release(&b);
  add_playlist(&b, "Only on B");
  add_playlist(&a, "Only on A");
  CHECK(mc_sync_set_device_name(b.sync, "B renamed") == MC_OK);
  int a_from_b = rows_from(&a, b.uid, 0), b_from_a = rows_from(&b, a.uid, 0);
  int a_lib = lib_int(&a, "SELECT count(*) FROM changelog");
  int b_lib = lib_int(&b, "SELECT count(*) FROM changelog");
  int va = versions_seen(&a);
  on_b = MC_ERR_UNIMPLEMENTED;
  CHECK(sync_now(&a, &b, &on_b) == MC_ERR_VERSION);
  CHECK(on_b == MC_ERR_FORMAT);
  CHECK(versions_seen(&a) == va + 1);
  CHECK_TEXT(a.version_peer, b.uid);
  mc_sync_version v = version_of(&a, b.uid);
  CHECK(v.outdated == MC_SYNC_PEER_OUTDATED);
  CHECK(v.protocol == 1);
  CHECK(v.protocol_min == 0);
  CHECK_TEXT(v.app, "");
  on_b = MC_ERR_UNIMPLEMENTED;
  CHECK(sync_now(&b, &a, &on_b) == MC_ERR_FORMAT);
  CHECK(on_b == MC_ERR_VERSION);
  CHECK(playlists_named(&a, "Only on B") == 0);
  CHECK(playlists_named(&b, "Only on A") == 0);
  CHECK(rows_from(&a, b.uid, 0) == a_from_b);
  CHECK(rows_from(&b, a.uid, 0) == b_from_a);
  CHECK(lib_int(&a, "SELECT count(*) FROM changelog") == a_lib);
  CHECK(lib_int(&b, "SELECT count(*) FROM changelog") == b_lib);
  /* The old release says nothing of its version on its side. */
  CHECK(version_of(&b, a.uid).outdated == MC_SYNC_VERSION_OK);

  /* A peer that sends no version at all is refused the same way. */
  mc_sync_set_test_version(b.sync, -1, -1, NULL, 0, 0);
  CHECK(sync_now(&a, &b, NULL) == MC_ERR_VERSION);
  v = version_of(&a, b.uid);
  CHECK(v.outdated == MC_SYNC_PEER_OUTDATED && v.protocol == 0);
  CHECK(playlists_named(&a, "Only on B") == 0);

  /* B as a newer version that no longer syncs with A's: refused both ways, and each side
   * knows which is outdated. A refuses as soon as it reads B's HELLO, and B refuses A's. */
  mc_sync_set_test_version(b.sync, MC_PROTOCOL_VERSION + 1, MC_PROTOCOL_VERSION + 1, "9.0.0", 1,
                           MC_FEAT_DEFAULT);
  on_b = MC_ERR_UNIMPLEMENTED;
  CHECK(sync_now(&a, &b, &on_b) == MC_ERR_VERSION);
  CHECK(on_b == MC_ERR_VERSION);
  v = version_of(&a, b.uid);
  CHECK(v.outdated == MC_SYNC_SELF_OUTDATED);
  CHECK(v.protocol == MC_PROTOCOL_VERSION + 1 && v.protocol_min == MC_PROTOCOL_VERSION + 1);
  CHECK_TEXT(v.app, "9.0.0");
  v = version_of(&b, a.uid);
  CHECK(v.outdated == MC_SYNC_PEER_OUTDATED);
  CHECK(v.protocol == MC_PROTOCOL_VERSION);
  CHECK_TEXT(v.app, mc_version());
  char text[256];
  v = version_of(&a, b.uid);
  mc_sync_version_text("b", &v, text, sizeof text);
  CHECK(strstr(text, "Update Kithsong on this device.") != NULL);
  CHECK(playlists_named(&a, "Only on B") == 0);
  CHECK(playlists_named(&b, "Only on A") == 0);

  /* Newer but still taking A's version: they sync. Back on one version, a session that
   * succeeds clears the marks on both. */
  mc_sync_set_test_version(b.sync, MC_PROTOCOL_VERSION + 1, MC_PROTOCOL_MIN_VERSION, "9.0.0", 1,
                           MC_FEAT_DEFAULT);
  va = versions_seen(&a);
  int vb = versions_seen(&b);
  CHECK(sync_now(&a, &b, NULL) == MC_OK);
  CHECK(version_of(&a, b.uid).outdated == MC_SYNC_VERSION_OK);
  CHECK(version_of(&b, a.uid).outdated == MC_SYNC_VERSION_OK);
  CHECK(versions_seen(&a) == va + 1 && versions_seen(&b) == vb + 1);
  CHECK(playlists_named(&a, "Only on B") == 1);
  CHECK(playlists_named(&b, "Only on A") == 1);
  as_current(&b);

  /* Pairing. Every case is refused before a code is drawn or anything is written. */
  int a_devices = group_int(&a, "SELECT count(*) FROM devices");
  int a_group_rows = group_int(&a, "SELECT count(*) FROM changelog");
  CHECK(a_devices == 2);

  /* An old release asks A (C, with an empty library, would join A's group): A refuses it
   * at its PAIR_HELLO and shows no code; C takes the refusal for a failure. */
  as_old_release(&c);
  int ra = a.results, rc = c.results, codes_a = a.codes;
  CHECK(mc_sync_pair_request_address(c.sync, "127.0.0.1", mc_sync_port(a.sync)) == MC_OK);
  CHECK(wait_result(&c, rc) == MC_PAIR_FAILED);
  CHECK(wait_result(&a, ra) == MC_PAIR_VERSION);
  CHECK(a.pair_status == MC_ERR_VERSION);
  CHECK_TEXT(a.result_peer, c.uid);
  /* 0.9.4's PAIR_HELLO says no version at all. */
  CHECK(a.result_version.outdated == MC_SYNC_PEER_OUTDATED && a.result_version.protocol == 0);
  CHECK(a.codes == codes_a);

  /* A asks the old release: C, which knows no versions, accepts and shows its code; A
   * refuses at its PAIR_READY, before J-PAKE, and C takes that for A giving up. */
  ra = a.results;
  rc = c.results;
  int codes_c = c.codes;
  request_pair(&a, &c);
  CHECK(wait_result(&a, ra) == MC_PAIR_VERSION);
  CHECK(a.pair_status == MC_ERR_VERSION);
  CHECK(a.result_version.outdated == MC_SYNC_PEER_OUTDATED);
  CHECK(wait_result(&c, rc) == MC_PAIR_CANCELLED);
  CHECK(c.codes == codes_c + 1);
  /* Not a member, so the nearby list says it. */
  CHECK(nearby_version(&a, c.uid).outdated == MC_SYNC_PEER_OUTDATED);

  /* A device past A's range asks A: refused at PAIR_HELLO, each side saying which is behind. */
  mc_sync_set_test_version(d.sync, MC_PROTOCOL_VERSION + 1, MC_PROTOCOL_VERSION + 1, "9.0.0", 1,
                           MC_FEAT_DEFAULT);
  ra = a.results;
  int rd = d.results;
  request_pair(&d, &a);
  CHECK(wait_result(&d, rd) == MC_PAIR_VERSION);
  CHECK(d.pair_status == MC_ERR_VERSION);
  CHECK_TEXT(d.result_peer, a.uid);
  CHECK_TEXT(d.result_name, "a");
  CHECK(d.result_version.outdated == MC_SYNC_PEER_OUTDATED);
  CHECK_TEXT(d.result_version.app, mc_version());
  CHECK(wait_result(&a, ra) == MC_PAIR_VERSION);
  CHECK(a.result_version.outdated == MC_SYNC_SELF_OUTDATED);
  CHECK_TEXT(a.result_version.app, "9.0.0");
  CHECK(a.codes == codes_a);

  /* And A asking it: D refuses at PAIR_HELLO. */
  ra = a.results;
  rd = d.results;
  request_pair(&a, &d);
  CHECK(wait_result(&a, ra) == MC_PAIR_VERSION);
  CHECK(a.result_version.outdated == MC_SYNC_SELF_OUTDATED);
  CHECK(wait_result(&d, rd) == MC_PAIR_VERSION);

  /* An old release that refuses by its join rules (B, already in A's group) says nothing of
   * its version, and gets "version" rather than what it said. */
  as_old_release(&b);
  ra = a.results;
  int rb = b.results;
  request_pair(&a, &b);
  CHECK(wait_result(&a, ra) == MC_PAIR_VERSION);
  CHECK(a.result_version.outdated == MC_SYNC_PEER_OUTDATED && a.result_version.protocol == 0);
  CHECK_TEXT(a.result_peer, b.uid);
  CHECK(wait_result(&b, rb) == MC_PAIR_ALREADY_PAIRED);
  as_current(&b);

  /* Nothing written anywhere. */
  CHECK(group_int(&a, "SELECT count(*) FROM devices") == a_devices);
  CHECK(group_int(&a, "SELECT count(*) FROM changelog") == a_group_rows);
  CHECK(group_int(&c, "SELECT count(*) FROM local WHERE group_uid IS NOT NULL") == 0);
  CHECK(group_int(&d, "SELECT count(*) FROM local WHERE group_uid IS NOT NULL") == 0);
  CHECK(group_int(&c, "SELECT count(*) FROM devices") <= 1);
  CHECK(group_int(&d, "SELECT count(*) FROM devices") <= 1);

  dev_stop(&a);
  dev_stop(&b);
  dev_stop(&c);
  dev_stop(&d);
  remove_tree(root);
  return TEST_RESULT();
}
