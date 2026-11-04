/* mc_library_read_stats(): an empty library, then one with a bit of everything that counts
 * differently, and that its numbers agree with the listings frontends show. */
#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "test.h"

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal", MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

/* A new library has nothing in it but the example smart lists. */
static void test_empty(mc_library *lib) {
  mc_library_stats s;
  memset(&s, 0xff, sizeof s);
  CHECK(mc_library_read_stats(lib, &s) == MC_OK);
  mc_library_tally none;
  memset(&none, 0, sizeof none);
  CHECK(memcmp(&s.all, &none, sizeof none) == 0);
  CHECK(memcmp(&s.here, &none, sizeof none) == 0);
  CHECK(s.files == 0 && s.size_bytes == 0);
  CHECK(s.playlists == 0 && s.album_groups == 0);
  CHECK(s.smart_playlists == 3 && s.smart_album_groups == 2);
  CHECK(s.plays == 0 && s.tracks_played == 0 && s.listened_ms == 0);
}

/* - Album al1, two tracks, only t1 here, on two files (an original and a transcode).
 * - Album al2, none of it here, with a featured artist on the album (ar5).
 * - Album al3, one track: a single, here.
 * - Album al4, one long track: a mix, so an album, here.
 * - Album al5, one track: a single, not here.
 * - t7, on no album, here, with a featured artist on the track (ar6); t8 on no album, not here.
 * - t9 has no artist of its own, nor a known length; it's al6's, with a featured artist (ar7).
 * - ar8 has nothing: an artist left behind. */
static void populate(sqlite3 *db) {
  static const char sql[] =
    "INSERT INTO artists (uid, name) VALUES ('ar1', 'One'), ('ar2', 'Two'), ('ar3', 'Three'),"
    " ('ar4', 'Four'), ('ar5', 'Five'), ('ar6', 'Six'), ('ar7', 'Seven'), ('ar8', 'Eight');"
    "INSERT INTO albums (uid, title, artist_uid) VALUES ('al1', 'A', 'ar1'), ('al2', 'B', 'ar2'),"
    " ('al3', 'C', 'ar3'), ('al4', 'D', 'ar3'), ('al5', 'E', 'ar4'), ('al6', 'F', 'ar1');"
    "INSERT INTO album_artists (uid, album_uid, artist_uid, position, joinphrase) VALUES"
    " ('aa1', 'al2', 'ar5', 1, ''), ('aa2', 'al6', 'ar7', 1, '');"
    "INSERT INTO tracks (uid, album_uid, artist_uid, title, duration_ms, genre) VALUES"
    " ('t1', 'al1', 'ar1', 't1', 60000, 'Rock'),"
    " ('t2', 'al1', 'ar1', 't2', 120000, ' rock '),"
    " ('t3', 'al2', 'ar2', 't3', 180000, 'Jazz'),"
    " ('t4', 'al2', 'ar2', 't4', 240000, ''),"
    " ('t5', 'al3', 'ar3', 't5', 300000, 'Pop'),"
    " ('t6', 'al4', 'ar3', 't6', 3600000, NULL),"
    " ('t10', 'al5', 'ar4', 't10', 1000, 'Folk'),"
    " ('t7', NULL, 'ar3', 't7', 2000, 'POP'),"
    " ('t8', NULL, 'ar4', 't8', 4000, NULL),"
    " ('t9', 'al6', NULL, 't9', NULL, NULL),"
    " ('t11', 'al6', 'ar1', 't11', 8000, NULL);"
    "INSERT INTO track_artists (uid, track_uid, artist_uid, position, joinphrase) VALUES"
    " ('ta1', 't7', 'ar6', 1, '');"
    "INSERT INTO files (uid, track_uid, path, format, size_bytes, source_file_uid) VALUES"
    " ('f1', 't1', 'a/1.flac', 'flac', 1000, NULL), ('f2', 't1', 'a/1.opus', 'opus', 100, 'x'),"
    " ('f5', 't5', 'c/5.flac', 'flac', 2000, NULL), ('f6', 't6', 'd/6.flac', 'flac', 4000, NULL),"
    " ('f7', 't7', 'g/7.flac', 'flac', 8000, NULL), ('f9', 't9', 'f/9.flac', 'flac', NULL, NULL);"
    "INSERT INTO playlists (uid, name) VALUES ('pl1', 'Mine'), ('pl2', 'Yours');"
    "INSERT INTO album_groups (uid, name) VALUES ('g1', 'Group');"
    "INSERT INTO track_plays (uid, track_uid, node_uid, play_count) VALUES"
    " ('p1', 't1', 'nodeA', 5), ('p2', 't1', 'nodeB', 2), ('p3', 't3', 'nodeA', 1),"
    " ('p4', 't5', 'nodeA', 0);"
    "INSERT INTO play_events (uid, track_uid, node_uid, started_ms, listened_ms, ended, context)"
    " VALUES ('e1', 't1', 'nodeA', 1, 30000, 'finished', 'album'),"
    " ('e2', 't1', 'nodeB', 2, 500, 'skipped', 'album'), ('e3', 't3', 'nodeA', 3, 0, 'failed', 'manual');";
  CHECK(exec(db, sql) == SQLITE_OK);
}

static void test_counts(mc_library *lib) {
  mc_library_stats s;
  CHECK(mc_library_read_stats(lib, &s) == MC_OK);

  CHECK(s.all.tracks == 11);
  CHECK(s.here.tracks == 5); /* t1 once, with its two files; t5, t6, t7, t9 */
  CHECK(s.all.duration_ms == 60000 + 120000 + 180000 + 240000 + 300000 + 3600000 + 1000 + 2000 + 4000 + 8000);
  CHECK(s.here.duration_ms == 60000 + 300000 + 3600000 + 2000);

  CHECK(s.all.albums == 4);   /* al1, al2, the mix al4, al6 */
  CHECK(s.here.albums == 3);  /* al1, al4, al6 */
  CHECK(s.all.singles == 4);  /* al3, al5, t7, t8 */
  CHECK(s.here.singles == 2); /* al3, t7 */

  CHECK(s.all.artists == 7);  /* all but ar8 */
  CHECK(s.here.artists == 4); /* ar1, ar3, and ar6 and ar7, featured */

  CHECK(s.all.genres == 4); /* rock, jazz, pop, folk */
  CHECK(s.here.genres == 2); /* rock, pop */

  CHECK(s.files == 6);
  CHECK(s.size_bytes == 1000 + 100 + 2000 + 4000 + 8000);

  CHECK(s.playlists == 2 && s.album_groups == 1);
  CHECK(s.smart_playlists == 3 && s.smart_album_groups == 2);
  CHECK(s.plays == 8);
  CHECK(s.tracks_played == 2); /* t1 over two devices is one; t5's row has no plays */
  CHECK(s.listened_ms == 30500);
}

/* What the listings say, which is what a frontend shows next to the numbers. */
static void test_agrees_with_listings(mc_library *lib) {
  mc_library_stats s;
  CHECK(mc_library_read_stats(lib, &s) == MC_OK);

  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  if (albums) {
    int here = 0;
    for (int i = 0; i < albums->count; i++) here += albums->items[i].folder != NULL;
    CHECK(s.all.albums == albums->count);
    CHECK(s.here.albums == here);
    mc_album_list_free(albums);
  }

  mc_artist_list *artists = NULL;
  CHECK(mc_library_list_artists(lib, &artists) == MC_OK);
  if (artists) {
    CHECK(s.all.artists == artists->count);
    mc_artist_list_free(artists);
  }
  artists = NULL;
  CHECK(mc_library_list_artists_here(lib, &artists) == MC_OK);
  if (artists) {
    CHECK(s.here.artists == artists->count);
    mc_artist_list_free(artists);
  }

  int64_t tracks = 0, files = 0, playlists = 0;
  CHECK(mc_library_count(lib, MC_ENTITY_TRACKS, &tracks) == MC_OK);
  CHECK(mc_library_count(lib, MC_ENTITY_FILES, &files) == MC_OK);
  CHECK(mc_library_count(lib, MC_ENTITY_PLAYLISTS, &playlists) == MC_OK);
  CHECK(s.all.tracks == tracks && s.files == files && s.playlists == playlists);
}

/* A track's plays and files go with it. */
static void test_after_delete(mc_library *lib, sqlite3 *db) {
  CHECK(exec(db, "DELETE FROM tracks WHERE uid = 't1'") == SQLITE_OK);
  mc_library_stats s;
  CHECK(mc_library_read_stats(lib, &s) == MC_OK);
  CHECK(s.all.tracks == 10 && s.here.tracks == 4);
  CHECK(s.files == 4);
  CHECK(s.size_bytes == 2000 + 4000 + 8000);
  CHECK(s.plays == 1 && s.tracks_played == 1);
  CHECK(s.listened_ms == 0);
}

int main(int argc, char **argv) {
  if (argc < 2) return 1;
  char dir[1024];
  snprintf(dir, sizeof dir, "%s/test_stats_lib", argv[1]);
  remove_db(dir);

  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();

  mc_library_stats s;
  CHECK(mc_library_read_stats(NULL, &s) == MC_ERR_INVALID_ARG);
  CHECK(mc_library_read_stats(lib, NULL) == MC_ERR_INVALID_ARG);

  sqlite3 *db = mc_library_db(lib);
  test_empty(lib);
  populate(db);
  test_counts(lib);
  test_agrees_with_listings(lib);
  test_after_delete(lib, db);

  mc_library_close(lib);
  remove_db(dir);
  return TEST_RESULT();
}
