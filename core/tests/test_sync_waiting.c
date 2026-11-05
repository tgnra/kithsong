/* Tracks waiting for a device (sync/waiting.h): worked out from one device's databases, with
 * no network. This device (S) wants Opus at 128 kbps; the group's other members are made up
 * in its sync.db, and who imported which track in its library changelog:
 *
 *   - Desk ("Tania's desktop") imported every track, and holds nothing more by its profile.
 *   - Phone wants album 2 as Opus 128: the very copy S wants.
 *   - Tablet wants album 3 as Opus 256: no source for S's 128 (never encoded twice).
 *   - Laptop wants album 4 as originals.
 *   - New, never confirmed, wants everything as originals: it holds nothing yet.
 *
 * Track 5 is held here already, as a 128 kbps copy. Each check sets who is reachable. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/i18n.h"
#include "musiccore/musiccore.h"
#include "sync/libmerge.h"
#include "sync/sync_db.h"
#include "sync/sync_internal.h"
#include "sync/waiting.h"
#include "test.h"
#include "util/platform.h"

#define NTRACKS 5

/* CHECK_STR, for arrays (which -Waddress says are never NULL). */
#define CHECK_TEXT(a, b) CHECK(strcmp((a), (b)) == 0)

enum { DESK, PHONE, TABLET, LAPTOP, NEW, NMEMBERS };

static const char *const k_names[NMEMBERS] = {"Tania's desktop", "Phone", "Tablet", "Laptop",
                                              "New"};

typedef struct world {
  mc_sync_db *sdb;
  mc_library *lib;
  char self[MC_UID_STRLEN];
  char uid[NMEMBERS][MC_UID_STRLEN];
  char node[NMEMBERS][MC_UID_STRLEN];
  char album[NTRACKS][MC_UID_STRLEN];
  char track[NTRACKS][MC_UID_STRLEN];
  int reachable[NMEMBERS];
} world;

static int reach(void *user, const char *uid) {
  world *w = user;
  for (int i = 0; i < NMEMBERS; i++)
    if (!strcmp(w->uid[i], uid)) return w->reachable[i];
  return 0;
}

static int exec(sqlite3 *db, const char *sql) {
  int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
  if (rc != SQLITE_OK) fprintf(stderr, "%s: %s\n", sql, sqlite3_errmsg(db));
  return rc;
}

/* sql on sync.db as if a peer had sent it: unlogged. */
static void group_sql(world *w, const char *sql) {
  CHECK(mc_sync_db_begin_write(w->sdb) == MC_OK);
  w->sdb->applying_remote = 1;
  CHECK(exec(mc_sync_db_handle(w->sdb), sql) == SQLITE_OK);
  w->sdb->applying_remote = 0;
  CHECK(mc_sync_db_commit(w->sdb) == MC_OK);
}

static void meta(mc_library *lib, const char *key, char out[MC_UID_STRLEN]) {
  out[0] = '\0';
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT value FROM meta WHERE key = ?1", -1, &stmt,
                         NULL) != SQLITE_OK)
    return;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *v = (const char *)sqlite3_column_text(stmt, 0);
    snprintf(out, MC_UID_STRLEN, "%s", v ? v : "");
  }
  sqlite3_finalize(stmt);
}

/* Records uid's profile as its user confirmed it. */
static void confirm(world *w, const char *uid) {
  char fp[MC_PROFILE_FP_STRLEN], sql[512];
  CHECK(mc_profile_fingerprint(mc_sync_db_handle(w->sdb), uid, fp) == MC_OK);
  snprintf(sql, sizeof sql, "UPDATE devices SET confirmed_profile = '%s' WHERE uid = '%s'", fp,
           uid);
  group_sql(w, sql);
}

static void add_member(world *w, const char *uid, const char *node, const char *name,
                       const char *kind, const char *profile) {
  char *sql = sqlite3_mprintf(
      "INSERT INTO devices (uid, name, kind, node_uid) VALUES ('%q', '%q', '%q', '%q');"
      " INSERT INTO profiles (device_uid, mode, format, bitrate_kbps) VALUES ('%q', %s)",
      uid, name, kind, node, uid, profile);
  CHECK(sql != NULL);
  if (sql) group_sql(w, sql);
  sqlite3_free(sql);
}

static void select_album(world *w, int member, int album) {
  char item[MC_UID_STRLEN], sql[512];
  mc_uid_new(item);
  snprintf(sql, sizeof sql,
           "INSERT INTO profile_items (uid, device_uid, kind, ref_uid)"
           " VALUES ('%s', '%s', 'album', '%s')",
           item, w->uid[member], w->album[album]);
  group_sql(w, sql);
}

static void fill_library(world *w) {
  sqlite3 *db = mc_library_db(w->lib);
  char artist[MC_UID_STRLEN], sql[1024];
  mc_uid_new(artist);
  CHECK(mc_library_begin_write(w->lib) == MC_OK);
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', 'Low')", artist);
  CHECK(exec(db, sql) == SQLITE_OK);
  for (int i = 0; i < NTRACKS; i++) {
    mc_uid_new(w->album[i]);
    mc_uid_new(w->track[i]);
    snprintf(
        sql, sizeof sql,
        "INSERT INTO albums (uid, title, artist_uid, year) VALUES ('%s', 'Album %d', '%s', 1994);"
        " INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms)"
        " VALUES ('%s', '%s', '%s', 'Song %d', 1, 180000)",
        w->album[i], i + 1, artist, w->track[i], w->album[i], artist, i + 1);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  CHECK(mc_library_commit(w->lib) == MC_OK);
  /* Every track arrived from Desk, which imported it: its node logged the track's rows. The
   * rows this library logged itself are rewritten as Desk's. */
  snprintf(sql, sizeof sql, "UPDATE changelog SET origin_uid = '%s' WHERE entity = 'tracks'",
           w->node[DESK]);
  CHECK(exec(db, sql) == SQLITE_OK);
  /* Track 5 is here, a 128 kbps copy. */
  char file[MC_UID_STRLEN], source[MC_UID_STRLEN];
  mc_uid_new(file);
  mc_uid_new(source);
  snprintf(
      sql, sizeof sql,
      "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms,"
      " source_file_uid, source_peer_uid) VALUES ('%s', '%s', 'Low/Album 5 (1994)/01 Song 5.opus',"
      " 'opus', 128, 2000000, 1, '%s', '%s')",
      file, w->track[4], source, w->uid[DESK]);
  CHECK(exec(db, sql) == SQLITE_OK);
}

static void set_reach(world *w, int desk, int phone, int tablet, int laptop) {
  w->reachable[DESK] = desk;
  w->reachable[PHONE] = phone;
  w->reachable[TABLET] = tablet;
  w->reachable[LAPTOP] = laptop;
  w->reachable[NEW] = 0;
}

static mc_sync_waiting *compute(world *w) {
  mc_sync_waiting *out = NULL;
  CHECK(mc_waiting_compute(w->sdb, w->lib, reach, w, &out) == MC_OK);
  CHECK(out != NULL);
  return out;
}

static int tracks_of(const mc_sync_waiting *out, const char *uid) {
  for (int i = 0; out && i < out->count; i++)
    if (!strcmp(out->devices[i].uid, uid)) return out->devices[i].tracks;
  return 0;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mc_strings_set_locale("en");
  char root[1024], config[1100], libdir[1100], sql[2048];
  snprintf(root, sizeof root, "%s/test_sync_waiting.d", scratch);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  snprintf(config, sizeof config, "%s/config", root);
  snprintf(libdir, sizeof libdir, "%s/music", root);

  static world w;
  CHECK(mc_library_open(libdir, &w.lib) == MC_OK);
  CHECK(mc_sync_db_open(config, &w.sdb) == MC_OK);
  if (!w.lib || !w.sdb) return TEST_RESULT();
  snprintf(w.self, sizeof w.self, "%s", mc_sync_db_device_uid(w.sdb));
  for (int i = 0; i < NMEMBERS; i++) {
    mc_uid_new(w.uid[i]);
    mc_uid_new(w.node[i]);
  }
  fill_library(&w);

  /* Outside a group: nothing waits. */
  set_reach(&w, 0, 0, 0, 0);
  mc_sync_waiting *out = compute(&w);
  CHECK(out && out->tracks == 0 && out->count == 0);
  mc_sync_waiting_free(out);

  /* The group, syncing this library. */
  char group[MC_UID_STRLEN], library[MC_UID_STRLEN], node[MC_UID_STRLEN];
  mc_uid_new(group);
  meta(w.lib, "library_uid", library);
  meta(w.lib, "node_uid", node);
  snprintf(sql, sizeof sql,
           "UPDATE local SET group_uid = '%s';"
           " INSERT INTO sync_group (uid, library_uid) VALUES ('%s', '%s')",
           group, group, library);
  group_sql(&w, sql);
  add_member(&w, w.self, node, "Here", "phone", "'all', 'opus', 128");
  add_member(&w, w.uid[DESK], w.node[DESK], k_names[DESK], "desktop",
             "'selected', 'original', NULL");
  add_member(&w, w.uid[PHONE], w.node[PHONE], k_names[PHONE], "phone", "'selected', 'opus', 128");
  add_member(&w, w.uid[TABLET], w.node[TABLET], k_names[TABLET], "phone",
             "'selected', 'opus', 256");
  add_member(&w, w.uid[LAPTOP], w.node[LAPTOP], k_names[LAPTOP], "desktop",
             "'selected', 'original', NULL");
  add_member(&w, w.uid[NEW], w.node[NEW], k_names[NEW], "desktop", "'all', 'original', NULL");
  select_album(&w, PHONE, 1);
  select_album(&w, TABLET, 2);
  select_album(&w, LAPTOP, 3);

  /* This device's profile isn't confirmed yet: it fetches nothing, so nothing waits. */
  out = compute(&w);
  CHECK(out && out->tracks == 0);
  mc_sync_waiting_free(out);
  confirm(&w, w.self);
  for (int i = 0; i < NEW; i++) confirm(&w, w.uid[i]);

  /* Nobody reachable. Track 1: only Desk has its original. Track 2: Phone's 128 copy or Desk.
   * Track 3: only Desk (Tablet's 256 copy is no source). Track 4: Desk or Laptop. Track 5 is
   * here. New holds nothing yet. */
  out = compute(&w);
  CHECK(out && out->tracks == 4);
  CHECK(out && out->count == 3);
  /* They're named, in order: a frontend can show them. */
  for (int i = 0; out && out->track_uids && i < out->tracks; i++) {
    CHECK(strcmp(out->track_uids[i], w.track[4]) != 0);
    if (i) CHECK(strcmp(out->track_uids[i - 1], out->track_uids[i]) < 0);
  }
  CHECK(out && out->track_uids);
  CHECK(tracks_of(out, w.uid[DESK]) == 4);
  CHECK(tracks_of(out, w.uid[PHONE]) == 1);
  CHECK(tracks_of(out, w.uid[LAPTOP]) == 1);
  CHECK(tracks_of(out, w.uid[TABLET]) == 0);
  CHECK(tracks_of(out, w.uid[NEW]) == 0);
  if (out && out->count == 3) {
    /* Most first, then by name. */
    CHECK_TEXT(out->devices[0].uid, w.uid[DESK]);
    CHECK_STR(out->devices[0].name, k_names[DESK]);
    CHECK(out->devices[0].kind == MC_DEVICE_DESKTOP);
    CHECK_TEXT(out->devices[1].uid, w.uid[LAPTOP]);
    CHECK_TEXT(out->devices[2].uid, w.uid[PHONE]);
  }
  char text[256];
  mc_sync_waiting_text(out, text, sizeof text);
  CHECK_TEXT(text, "4 tracks are waiting for Tania's desktop and 2 other devices to come online.");

  /* Copies compare equal; a change doesn't. */
  mc_sync_waiting *copy = mc_waiting_copy(out);
  CHECK(mc_waiting_equal(out, copy));
  if (copy && copy->count) copy->track_uids[0][0] ^= 1;
  CHECK(!mc_waiting_equal(out, copy));
  if (copy && copy->count) {
    copy->track_uids[0][0] ^= 1;
    CHECK(mc_waiting_equal(out, copy));
    copy->devices[0].tracks--;
  }
  CHECK(!mc_waiting_equal(out, copy));
  CHECK(!mc_waiting_equal(out, NULL));
  mc_sync_waiting_free(copy);
  mc_sync_waiting_free(out);

  /* A merged library still to come in (sync/libmerge.h): nothing is fetched, so nothing
   * waits, until it's merged or the pairing is undone. */
  CHECK(mc_library_meta_set(w.lib, MC_LIBMERGE_META, MC_LIBMERGE_PREFIX "2026-11-02.db") == MC_OK);
  CHECK(mc_library_meta_set(w.lib, MC_LIBMERGE_WAIT_META, "run") == MC_OK);
  out = compute(&w);
  CHECK(out && out->tracks == 0 && out->count == 0);
  mc_sync_waiting_free(out);
  CHECK(mc_library_meta_set(w.lib, MC_LIBMERGE_WAIT_META, NULL) == MC_OK);
  CHECK(mc_library_meta_set(w.lib, MC_LIBMERGE_META, NULL) == MC_OK);
  out = compute(&w);
  CHECK(out && out->tracks == 4);
  mc_sync_waiting_free(out);

  /* Phone reachable: its 128 kbps copy gives track 2. Tablet reachable: its 256 copy still
   * doesn't give track 3. */
  set_reach(&w, 0, 1, 1, 0);
  out = compute(&w);
  CHECK(out && out->tracks == 3);
  CHECK(tracks_of(out, w.uid[DESK]) == 3);
  CHECK(tracks_of(out, w.uid[LAPTOP]) == 1);
  CHECK(tracks_of(out, w.uid[PHONE]) == 0);
  mc_sync_waiting_free(out);

  /* Laptop reachable too: track 4 comes from it; tracks 1 and 3 wait for Desk alone. */
  set_reach(&w, 0, 1, 1, 1);
  out = compute(&w);
  CHECK(out && out->tracks == 2 && out->count == 1);
  CHECK(tracks_of(out, w.uid[DESK]) == 2);
  mc_sync_waiting_text(out, text, sizeof text);
  CHECK_TEXT(text, "2 tracks are waiting for Tania's desktop to come online.");
  mc_sync_waiting_free(out);

  /* Track 1 arrives as a 128 kbps copy: one left, said in the singular. */
  char file[MC_UID_STRLEN], source[MC_UID_STRLEN];
  mc_uid_new(file);
  mc_uid_new(source);
  snprintf(
      sql, sizeof sql,
      "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms,"
      " source_file_uid, source_peer_uid) VALUES ('%s', '%s', 'Low/Album 1 (1994)/01 Song 1.opus',"
      " 'opus', 128, 2000000, 1, '%s', '%s')",
      file, w.track[0], source, w.uid[PHONE]);
  CHECK(exec(mc_library_db(w.lib), sql) == SQLITE_OK);
  out = compute(&w);
  CHECK(out && out->tracks == 1 && tracks_of(out, w.uid[DESK]) == 1);
  mc_sync_waiting_text(out, text, sizeof text);
  CHECK_TEXT(text, "1 track is waiting for Tania's desktop to come online.");
  mc_sync_waiting_free(out);

  /* Desk reachable: nothing waits for anyone. */
  set_reach(&w, 1, 0, 0, 0);
  out = compute(&w);
  CHECK(out && out->tracks == 0 && out->count == 0);
  mc_sync_waiting_text(out, text, sizeof text);
  CHECK_TEXT(text, "");
  mc_sync_waiting_free(out);

  /* A better file swapped in on Laptop: the newest file_rev change is Laptop's, so the
   * current original of track 3 is Laptop's alone, and Desk's copy no longer counts. */
  snprintf(sql, sizeof sql,
           "INSERT INTO changelog (hlc, origin_uid, entity, row_uid, field, value)"
           " VALUES ((SELECT max(hlc) + 1 FROM changelog), '%s', 'tracks', '%s', 'file_rev', '%s')",
           w.node[LAPTOP], w.track[2], source);
  CHECK(exec(mc_library_db(w.lib), sql) == SQLITE_OK);
  set_reach(&w, 1, 1, 1, 0);
  out = compute(&w);
  CHECK(out && out->tracks == 1 && out->count == 1);
  CHECK(tracks_of(out, w.uid[LAPTOP]) == 1);
  mc_sync_waiting_free(out);

  /* A removed member holds nothing for anyone. */
  snprintf(sql, sizeof sql, "UPDATE devices SET removed = 1 WHERE uid = '%s'", w.uid[LAPTOP]);
  group_sql(&w, sql);
  out = compute(&w);
  CHECK(out && out->tracks == 0);
  mc_sync_waiting_free(out);

  mc_sync_db_close(w.sdb);
  mc_library_close(w.lib);
  mc_remove_tree(root);
  return TEST_RESULT();
}
