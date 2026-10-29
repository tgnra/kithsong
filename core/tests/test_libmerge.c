/* Merging libraries, the library side (sync/libmerge.c), without the network: which library
 * is kept, setting a library aside, and merging the set-aside copy's current state into the
 * kept library: the duplicates check against the kept device's originals, playlists (one of
 * the same name), album groups, smart lists, play counts and recents mapped onto the kept
 * library's tracks, and none of the copy's changelog. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "sync/libmerge.h"
#include "tagger/dupes.h"
#include "test.h"
#include "util/platform.h"

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

/* --- Files and rows --- */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A short 16-bit stereo PCM WAV whose samples depend on seed, so each file differs. */
static int write_wav(const char *path, unsigned seed) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t frames = 4410, bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 2);
  put32(f, 44100);
  put32(f, 44100 * 4);
  put16(f, 4);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  uint32_t x = seed * 2654435761u + 1;
  for (uint32_t i = 0; i < frames * 2; i++) {
    x = x * 1103515245u + 12345u;
    put16(f, (x >> 16) & 0x3fff);
  }
  return fclose(f) == 0;
}

static int exec(sqlite3 *db, const char *sql) {
  char *err = NULL;
  int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
  if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n  %s\n", err ? err : "?", sql);
  sqlite3_free(err);
  return rc;
}

static int64_t query_int(sqlite3 *db, const char *sql) {
  sqlite3_stmt *stmt;
  int64_t v = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
    fprintf(stderr, "prepare failed: %s\n  %s\n", sqlite3_errmsg(db), sql);
    return -1;
  }
  if (sqlite3_step(stmt) == SQLITE_ROW) v = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return v;
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

/* One track of a test library. */
typedef struct track {
  const char *artist, *album, *album_mbid, *title, *rtid; /* rtid: mb_releasetrackid */
  int no;
  int with_file; /* a WAV in the library folder */
  char uid[MC_UID_STRLEN], album_uid[MC_UID_STRLEN], rel[256];
} track;

/* Writes t's artist, album and track (and its file if with_file) into lib, logged. */
static void add_track(mc_library *lib, track *t, unsigned seed) {
  sqlite3 *db = mc_library_db(lib);
  char ar[MC_UID_STRLEN], sql[1536];
  snprintf(sql, sizeof sql, "SELECT uid FROM artists WHERE name = '%s'", t->artist);
  query_text(db, sql, ar, sizeof ar);
  snprintf(sql, sizeof sql, "SELECT uid FROM albums WHERE title = '%s'", t->album);
  query_text(db, sql, t->album_uid, sizeof t->album_uid);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  if (!ar[0]) {
    mc_uid_new(ar);
    snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', '%s')", ar, t->artist);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  if (!t->album_uid[0]) {
    mc_uid_new(t->album_uid);
    snprintf(sql, sizeof sql,
             "INSERT INTO albums (uid, title, artist_uid, mb_albumid) VALUES ('%s', '%s', '%s', %s%s%s)",
             t->album_uid, t->album, ar, t->album_mbid ? "'" : "",
             t->album_mbid ? t->album_mbid : "NULL", t->album_mbid ? "'" : "");
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  mc_uid_new(t->uid);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, duration_ms,"
           " mb_releasetrackid, added_ms) VALUES ('%s', '%s', '%s', '%s', %d, 1000, %s%s%s, 1000)",
           t->uid, t->album_uid, ar, t->title, t->no, t->rtid ? "'" : "",
           t->rtid ? t->rtid : "NULL", t->rtid ? "'" : "");
  CHECK(exec(db, sql) == SQLITE_OK);
  if (t->with_file) {
    snprintf(t->rel, sizeof t->rel, "%s/%s/%02d %s.wav", t->artist, t->album, t->no, t->title);
    char dir[1024], *abs = mc_library_abspath(lib, t->rel);
    snprintf(dir, sizeof dir, "%s/%s/%s", mc_library_dir(lib), t->artist, t->album);
    CHECK(mc_mkdirs(dir) == MC_OK);
    CHECK(abs && write_wav(abs, seed));
    int64_t size = 0, mtime = 0;
    CHECK(mc_file_info(abs, &size, &mtime) == MC_OK);
    free(abs);
    char fu[MC_UID_STRLEN];
    mc_uid_new(fu);
    snprintf(sql, sizeof sql,
             "INSERT INTO files (uid, track_uid, path, format, bitrate_kbps, size_bytes, mtime_ms)"
             " VALUES ('%s', '%s', '%s', 'wav', 1411, %lld, %lld)",
             fu, t->uid, t->rel, (long long)size, (long long)mtime);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
  CHECK(mc_library_commit(lib) == MC_OK);
}

static void edit(mc_library *lib, const char *sql) {
  CHECK(mc_library_begin_write(lib) == MC_OK);
  CHECK(exec(mc_library_db(lib), sql) == SQLITE_OK);
  CHECK(mc_library_commit(lib) == MC_OK);
}

/* Trashed files, as mc_trash would see them: removed, and counted. */
static int trashed;
static mc_status fake_trash(const char *path) {
  trashed++;
  return mc_remove(path);
}

/* --- Which library is kept --- */

static void test_kept_choice(void) {
  const char *lo = "01900000-0000-7000-8000-000000000001";
  const char *hi = "01900000-0000-7000-8000-000000000002";
  /* More tracks wins, whichever side it is on. */
  CHECK(mc_libmerge_keeps_first(10, 0, hi, 3, 999, lo) == 1);
  CHECK(mc_libmerge_keeps_first(3, 999, lo, 10, 0, hi) == 0);
  /* Same tracks: more bytes. */
  CHECK(mc_libmerge_keeps_first(5, 200, hi, 5, 100, lo) == 1);
  CHECK(mc_libmerge_keeps_first(5, 100, lo, 5, 200, hi) == 0);
  /* Same tracks and bytes: the older library, by uid. */
  CHECK(mc_libmerge_keeps_first(5, 100, lo, 5, 100, hi) == 1);
  CHECK(mc_libmerge_keeps_first(5, 100, hi, 5, 100, lo) == 0);
  /* Both devices ask with the sides swapped and get opposite answers: they agree. */
  for (int i = 0; i < 4; i++) {
    int64_t ta = i & 1 ? 7 : 5, ba = i & 2 ? 50 : 60;
    CHECK(mc_libmerge_keeps_first(ta, ba, lo, 5, 60, hi) !=
          mc_libmerge_keeps_first(5, 60, hi, ta, ba, lo));
  }
}

/* --- Setting aside and merging --- */

static void test_merge(const char *root) {
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/libmerge-test", root);
  remove_tree(dir);
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  char old_node[MC_UID_STRLEN], library_uid[MC_UID_STRLEN];
  snprintf(old_node, sizeof old_node, "%s", mc_library_node_uid(lib));
  snprintf(library_uid, sizeof library_uid, "%s", mc_library_uid(lib));

  /* The library that will be merged (the smaller one's, on this device). */
  track better = {"Low", "Things", "rel-things", "Better", "rt-better", 1, 1, "", "", ""};
  track same = {"Low", "Others", "rel-others", "Same", "rt-same", 1, 1, "", "", ""};
  track nocopy = {"Low", "Third", "rel-third", "Unknown", "rt-third", 1, 1, "", "", ""};
  track fresh = {"Home", "Tapes", NULL, "Fresh", NULL, 1, 1, "", "", ""};
  track mapped = {"Low", "Things", "rel-things", "Elsewhere", "rt-elsewhere", 2, 0, "", "", ""};
  track lost = {"Home", "Tapes", NULL, "Lost", NULL, 2, 0, "", "", ""};
  add_track(lib, &better, 1);
  add_track(lib, &same, 2);
  add_track(lib, &nocopy, 3);
  add_track(lib, &fresh, 4);
  add_track(lib, &mapped, 0);
  add_track(lib, &lost, 0);
  char sql[4096], pl_road[MC_UID_STRLEN], pl_solo[MC_UID_STRLEN], grp[MC_UID_STRLEN],
    smart[MC_UID_STRLEN];
  mc_uid_new(pl_road);
  mc_uid_new(pl_solo);
  mc_uid_new(grp);
  mc_uid_new(smart);
  snprintf(sql, sizeof sql,
           "INSERT INTO playlists (uid, name) VALUES ('%s', 'Road'), ('%s', 'Solo');"
           "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position) VALUES"
           " ('e1000000-0000-7000-8000-000000000001', '%s', '%s', 'a0'),"
           " ('e1000000-0000-7000-8000-000000000002', '%s', '%s', 'a1'),"
           " ('e1000000-0000-7000-8000-000000000003', '%s', '%s', 'a2'),"
           " ('e1000000-0000-7000-8000-000000000004', '%s', '%s', 'a3'),"
           " ('e1000000-0000-7000-8000-000000000005', '%s', '%s', 'a0');"
           "INSERT INTO album_groups (uid, name) VALUES ('%s', 'Faves');"
           "INSERT INTO album_group_entries (uid, group_uid, album_uid, position) VALUES"
           " ('e2000000-0000-7000-8000-000000000001', '%s', '%s', 'a0'),"
           " ('e2000000-0000-7000-8000-000000000002', '%s', '%s', 'a1');"
           "INSERT INTO smart_lists (uid, name, subject, rule) VALUES ('%s', 'In Solo', 'tracks',"
           " '{\"v\":1,\"match\":{\"field\":\"playlist\",\"op\":\"is\",\"value\":\"%s\"}}');"
           "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES"
           " ('e3000000-0000-7000-8000-000000000001', '%s', 'n1', 3, 5000),"
           " ('e3000000-0000-7000-8000-000000000002', '%s', 'n2', 2, 9000),"
           " ('e3000000-0000-7000-8000-000000000003', '%s', 'n1', 4, 7000);"
           "INSERT INTO recents (uid, kind, opened_ms) VALUES ('%s', 'album', 100),"
           " ('%s', 'playlist', 200), ('%s', 'playlist', 300);",
           pl_road, pl_solo, pl_road, fresh.uid, pl_road, better.uid, pl_road, same.uid, pl_road,
           lost.uid, pl_solo, mapped.uid, grp, grp, fresh.album_uid, grp, better.album_uid, smart,
           pl_solo, better.uid, better.uid, fresh.uid, fresh.album_uid, pl_solo,
           "5e1ec7ed-0000-4000-8000-000000000002");
  edit(lib, sql);
  /* The examples are renamed here; the kept library's own state of them wins. */
  edit(lib, "UPDATE smart_lists SET name = 'Mine' WHERE uid = '5e1ec7ed-0000-4000-8000-000000000001'");
  CHECK(query_int(db, "SELECT count(*) FROM changelog") > 20);

  /* Set aside: a copy, then an empty library under a new node uid with the same uid. */
  char *backup = NULL;
  CHECK(mc_libmerge_set_aside(lib, &backup) == MC_OK);
  CHECK(backup && mc_file_exists(backup));
  CHECK(backup && strstr(backup, MC_LIBMERGE_PREFIX) != NULL);
  CHECK(mc_libmerge_pending(lib, NULL) == 1);
  CHECK(query_int(db, "SELECT count(*) FROM tracks") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM files") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM playlists") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM changelog") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM smart_lists") == 5); /* the examples */
  CHECK(query_int(db, "SELECT count(*) FROM smart_lists WHERE name = 'Mine'") == 0);
  CHECK(strcmp(mc_library_node_uid(lib), old_node) != 0);
  CHECK(strcmp(mc_library_uid(lib), library_uid) == 0);
  /* The files are still there, waiting. */
  {
    char *abs = mc_library_abspath(lib, fresh.rel);
    CHECK(abs && mc_file_exists(abs));
    free(abs);
  }
  /* A second copy the same day gets another name. */
  {
    char *again = NULL;
    mc_library *other = NULL;
    char odir[1100];
    snprintf(odir, sizeof odir, "%s-other", dir);
    remove_tree(odir);
    CHECK(mc_library_open(odir, &other) == MC_OK);
    CHECK(mc_libmerge_set_aside(other, &again) == MC_OK);
    char *third = NULL;
    CHECK(mc_libmerge_set_aside(other, &third) == MC_OK);
    CHECK(again && third && strcmp(again, third) != 0);
    free(again);
    free(third);
    mc_library_close(other);
    remove_tree(odir);
  }

  /* The kept library arrives (as the first session would bring it, unlogged here: it's
   * the other device's history). It has three of the albums, by release id, and a
   * playlist called Road. */
  {
    char k_ar[] = "a0000000-0000-7000-8000-00000000000a";
    char sql2[2048];
    snprintf(sql2, sizeof sql2,
             "INSERT INTO artists (uid, name) VALUES ('%s', 'Low');"
             "INSERT INTO albums (uid, title, artist_uid, mb_albumid) VALUES"
             " ('a1000000-0000-7000-8000-000000000001', 'Things', '%s', 'rel-things'),"
             " ('a1000000-0000-7000-8000-000000000002', 'Others', '%s', 'rel-others'),"
             " ('a1000000-0000-7000-8000-000000000003', 'Third', '%s', 'rel-third');"
             "INSERT INTO tracks (uid, album_uid, artist_uid, title, track_no, mb_releasetrackid) VALUES"
             " ('b1000000-0000-7000-8000-000000000001', 'a1000000-0000-7000-8000-000000000001', '%s',"
             "  'Better', 1, 'rt-better'),"
             " ('b1000000-0000-7000-8000-000000000002', 'a1000000-0000-7000-8000-000000000001', '%s',"
             "  'Elsewhere', 2, 'rt-elsewhere'),"
             " ('b1000000-0000-7000-8000-000000000003', 'a1000000-0000-7000-8000-000000000002', '%s',"
             "  'Same', 1, 'rt-same'),"
             " ('b1000000-0000-7000-8000-000000000004', 'a1000000-0000-7000-8000-000000000003', '%s',"
             "  'Unknown', 1, 'rt-third');"
             "INSERT INTO playlists (uid, name) VALUES ('c1000000-0000-7000-8000-000000000001', 'road');",
             k_ar, k_ar, k_ar, k_ar, k_ar, k_ar, k_ar, k_ar);
    lib->applying_remote = 1;
    CHECK(exec(db, sql2) == SQLITE_OK);
    lib->applying_remote = 0;
  }
  /* The kept device's originals: Better as a 128 kbps MP3 (worse than this WAV), Same as
   * a FLAC (as good), and nothing known of Unknown. */
  mc_kept_file kept[2];
  memset(kept, 0, sizeof kept);
  snprintf(kept[0].track, sizeof kept[0].track, "b1000000-0000-7000-8000-000000000003");
  snprintf(kept[0].format, sizeof kept[0].format, "flac");
  snprintf(kept[1].track, sizeof kept[1].track, "b1000000-0000-7000-8000-000000000001");
  snprintf(kept[1].format, sizeof kept[1].format, "mp3");
  kept[1].kbps = 128;
  mc_libmerge_sort_kept(kept, 2);

  trashed = 0;
  mc_status (*was)(const char *) = mc_dupes_trash; /* safe_trash.c's */
  mc_dupes_trash = fake_trash;
  mc_merge_result r;
  CHECK(mc_libmerge_run(lib, kept, 2, NULL, NULL, &r) == MC_OK);
  mc_dupes_trash = was;
  CHECK(r.status == MC_OK);
  CHECK(r.tracks_added == 1);    /* Fresh */
  CHECK(r.tracks_better == 1);   /* Better: this WAV beats the kept MP3 */
  CHECK(r.tracks_skipped == 1);  /* Same: the kept FLAC is as good */
  CHECK(r.tracks_attached == 1); /* Unknown: the kept copy's quality isn't known */
  CHECK(r.tracks_mapped == 1);   /* Elsewhere: no file here, but the kept library has it */
  CHECK(r.tracks_left_out == 1); /* Lost: no file, no id */
  CHECK(r.tracks_failed == 0);
  CHECK(trashed == 1);
  CHECK(mc_libmerge_pending(lib, NULL) == 0);

  /* Tracks: the kept four plus Fresh, which keeps its date added. Better and Unknown have
   * this device's files, attached to the kept tracks; Same's file went to the trash. */
  CHECK(query_int(db, "SELECT count(*) FROM tracks") == 5);
  CHECK(query_int(db, "SELECT count(*) FROM tracks WHERE title = 'Fresh' AND added_ms = 1000") == 1);
  CHECK(query_int(db, "SELECT count(*) FROM files WHERE track_uid IN"
                      " ('b1000000-0000-7000-8000-000000000001',"
                      "  'b1000000-0000-7000-8000-000000000004')"
                      " AND source_peer_uid IS NULL") == 2);
  CHECK(query_int(db, "SELECT count(*) FROM files WHERE track_uid ="
                      " 'b1000000-0000-7000-8000-000000000003'") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM albums") == 4);
  {
    char *abs = mc_library_abspath(lib, same.rel);
    CHECK(abs && !mc_file_exists(abs));
    free(abs);
  }
  /* Untagged music came in as it was tagged. */
  CHECK(query_int(db, "SELECT count(*) FROM tracks t JOIN albums a ON a.uid = t.album_uid"
                      " JOIN artists ar ON ar.uid = a.artist_uid"
                      " WHERE t.title = 'Fresh' AND a.title = 'Tapes' AND ar.name = 'Home'") == 1);

  /* Playlists: Road came in as "Road (2)" beside the kept "road", with the entries whose
   * tracks came along, in order; Solo as it was. */
  CHECK(query_int(db, "SELECT count(*) FROM playlists") == 3);
  CHECK(query_int(db, "SELECT count(*) FROM playlists WHERE name = 'Road (2)'") == 1);
  CHECK(query_int(db, "SELECT count(*) FROM playlists WHERE name = 'Solo'") == 1);
  CHECK(r.playlists == 2 && r.playlists_renamed == 1);
  char order[256];
  query_text(db,
             "SELECT group_concat(title, ',') FROM (SELECT t.title FROM playlist_entries e"
             " JOIN playlists p ON p.uid = e.playlist_uid JOIN tracks t ON t.uid = e.track_uid"
             " WHERE p.name = 'Road (2)' ORDER BY e.position, e.uid)",
             order, sizeof order);
  CHECK(strcmp(order, "Fresh,Better,Same") == 0);
  CHECK(query_int(db, "SELECT count(*) FROM playlist_entries e JOIN playlists p"
                      " ON p.uid = e.playlist_uid WHERE p.name = 'Solo'"
                      " AND e.track_uid = 'b1000000-0000-7000-8000-000000000002'") == 1);
  /* The album group, on the kept albums. */
  CHECK(r.album_groups == 1);
  CHECK(query_int(db, "SELECT count(*) FROM album_group_entries e JOIN album_groups g"
                      " ON g.uid = e.group_uid WHERE g.name = 'Faves'") == 2);
  CHECK(query_int(db, "SELECT count(*) FROM album_group_entries"
                      " WHERE album_uid = 'a1000000-0000-7000-8000-000000000001'") == 1);
  /* The smart list names the carried-over Solo. */
  CHECK(r.smart_lists == 1);
  char solo[MC_UID_STRLEN], rule[512];
  query_text(db, "SELECT uid FROM playlists WHERE name = 'Solo'", solo, sizeof solo);
  query_text(db, "SELECT rule FROM smart_lists WHERE name = 'In Solo'", rule, sizeof rule);
  CHECK(solo[0] && strstr(rule, solo) != NULL && strstr(rule, pl_solo) == NULL);
  CHECK(query_int(db, "SELECT count(*) FROM smart_lists") == 6);
  /* Play counts, summed over the old devices onto this device's row. */
  CHECK(r.plays == 2);
  snprintf(sql, sizeof sql,
           "SELECT play_count FROM track_plays WHERE track_uid ="
           " 'b1000000-0000-7000-8000-000000000001' AND node_uid = '%s'",
           mc_library_node_uid(lib));
  CHECK(query_int(db, sql) == 5);
  CHECK(query_int(db, "SELECT last_played_ms FROM track_plays WHERE track_uid ="
                      " 'b1000000-0000-7000-8000-000000000001'") == 9000);
  /* Recents: the album, the playlist and an example list. */
  CHECK(r.recents == 3);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM recents WHERE uid = '%s' AND opened_ms = 200",
           solo);
  CHECK(query_int(db, sql) == 1);
  CHECK(query_int(db, "SELECT count(*) FROM recents WHERE uid ="
                      " '5e1ec7ed-0000-4000-8000-000000000002'") == 1);

  /* No history came along: every change logged is this device's, new, and new rows only;
   * the old node uid appears nowhere. */
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE origin_uid <> '%s'",
           mc_library_node_uid(lib));
  CHECK(query_int(db, sql) == 0);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE origin_uid = '%s'", old_node);
  CHECK(query_int(db, sql) == 0);
  CHECK(query_int(db, "SELECT count(*) FROM changelog") > 0);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM tracks WHERE uid IN ('%s', '%s', '%s')",
           better.uid, same.uid, mapped.uid);
  CHECK(query_int(db, sql) == 0);

  /* Nothing pending: running again does nothing. */
  CHECK(mc_libmerge_run(lib, NULL, 0, NULL, NULL, &r) == MC_ERR_NOT_FOUND);
  /* The set-aside copy is still whole. */
  sqlite3 *old = NULL;
  CHECK(sqlite3_open_v2(backup, &old, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
  CHECK(query_int(old, "SELECT count(*) FROM tracks") == 6);
  sqlite3_close(old);
  free(backup);
  mc_library_close(lib);
  remove_tree(dir);
}

int main(int argc, char **argv) {
  const char *root = argc > 1 ? argv[1] : ".";
  test_kept_choice();
  test_merge(root);
  return TEST_RESULT();
}
