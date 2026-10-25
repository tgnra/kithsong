/* Library rows written for accepted import items: albums, singles, re-imports, and two
 * connections writing to one library. */
#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "tagger/import_db.h"
#include "test.h"

static int query_int(mc_library *lib, const char *sql) {
  sqlite3_stmt *stmt;
  int n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
  if (sqlite3_step(stmt) == SQLITE_ROW) n = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return n;
}

static mc_db_track track(const char *path, const char *title, int no) {
  mc_db_track t = {0};
  t.path = path;
  t.format = "flac";
  t.title = title;
  t.track_no = no;
  t.disc_no = 1;
  t.duration_ms = 200000;
  return t;
}

static void remove_db(const char *path) {
  char extra[1100];
  remove(path);
  snprintf(extra, sizeof extra, "%s-wal", path);
  remove(extra);
  snprintf(extra, sizeof extra, "%s-shm", path);
  remove(extra);
}

int main(int argc, char **argv) {
  char path[1024], db_file[1100];
  snprintf(path, sizeof path, "%s/test_import_db.d", argc > 1 ? argv[1] : ".");
  snprintf(db_file, sizeof db_file, "%s/" MC_LIBRARY_DB_NAME, path);
  remove_db(db_file);

  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return TEST_RESULT();

  /* An album. */
  mc_db_album album = {0};
  album.title = "Things We Lost in the Fire";
  album.artist = (mc_db_artist){"Low", "Low", "a1b2c3d4-0000-4000-8000-000000000001"};
  album.year = 2001;
  album.mb_albumid = "a1b2c3d4-0000-4000-8000-0000000000aa";
  mc_db_track tracks[] = {track("/music/low/01.flac", "Sunflower", 1),
                          track("/music/low/02.flac", "Whore", 2)};
  CHECK(mc_import_db_write(lib, &album, tracks, 2) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind = 'album'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 2);
  CHECK(query_int(lib, "SELECT count(*) FROM files") == 2);
  CHECK(query_int(lib, "SELECT count(*) FROM artists") == 1); /* tracks fall back to album artist */

  /* Importing the same album again updates it in place. */
  tracks[0].title = "Sunflower (corrected)";
  CHECK(mc_import_db_write(lib, &album, tracks, 2) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 2);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE title = 'Sunflower (corrected)'") == 1);
  /* Date added is set when the track is first imported, not on re-import. */
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE added_ms > 0") == 2);

  /* A file moved within the library keeps its track; only the path changes. */
  tracks[0].old_path = tracks[0].path;
  tracks[0].path = "/music/low (2001)/01.flac";
  CHECK(mc_import_db_write(lib, &album, tracks, 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 2);
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE path = '/music/low (2001)/01.flac'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM files WHERE path = '/music/low/01.flac'") == 0);
  CHECK(mc_import_db_is_track_at(lib, "/music/low (2001)/01.flac", NULL) == 0);
  tracks[0].old_path = NULL;

  tracks[1].mb_releasetrackid = "a1b2c3d4-0000-4000-8000-0000000000c2";
  CHECK(mc_import_db_write(lib, &album, &tracks[1], 1) == MC_OK);
  CHECK(mc_import_db_is_track_at(lib, "/music/low/02.flac", "a1b2c3d4-0000-4000-8000-0000000000c2"));
  CHECK(!mc_import_db_is_track_at(lib, "/music/low/02.flac", "a1b2c3d4-0000-4000-8000-0000000000c3"));

  /* Two singles by the same artist share one "<artist> Singles" album, matched to the
   * existing artist by MusicBrainz id even though the name differs in case. */
  mc_db_album singles = {0};
  singles.singles = 1;
  singles.artist = (mc_db_artist){"LOW", NULL, "a1b2c3d4-0000-4000-8000-000000000001"};
  mc_db_track s1 = track("/music/loose/just-like-christmas.mp3", "Just Like Christmas", 0);
  s1.year = 1999;
  s1.mb_albumid = "a1b2c3d4-0000-4000-8000-0000000000bb";
  mc_db_track s2 = track("/music/loose/canada.mp3", "Canada", 0);
  s2.year = 2002;
  CHECK(mc_import_db_write(lib, &singles, &s1, 1) == MC_OK);
  CHECK(mc_import_db_write(lib, &singles, &s2, 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind = 'singles'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind = 'singles' AND title = 'LOW Singles'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM artists") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks t JOIN albums a ON a.uid = t.album_uid"
                       " WHERE a.kind = 'singles'") == 2);
  /* Each single keeps its own year and release. */
  CHECK(query_int(lib, "SELECT year FROM tracks WHERE title = 'Canada'") == 2002);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE title = 'Just Like Christmas'"
                       " AND mb_albumid = 'a1b2c3d4-0000-4000-8000-0000000000bb'") == 1);

  /* A featured artist gets their own row; the single still files under the main artist. */
  mc_db_track feat = track("/music/loose/feat.mp3", "Collab", 0);
  feat.artist = (mc_db_artist){"Low feat. Someone", NULL, NULL};
  CHECK(mc_import_db_write(lib, &singles, &feat, 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind = 'singles'") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM artists") == 2);

  /* Re-importing an album's files as singles moves the tracks, and the album is deleted
   * once it's empty. */
  CHECK(mc_import_db_write(lib, &singles, &tracks[0], 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind = 'album'") == 1);
  CHECK(mc_import_db_write(lib, &singles, &tracks[1], 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind = 'album'") == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 5);
  CHECK(query_int(lib, "SELECT count(*) FROM files") == 5);

  /* A second connection (as the import thread uses) interleaving writes with the first
   * must not reuse or go back on change-log timestamps. */
  mc_library *other = NULL;
  CHECK(mc_library_open(path, &other) == MC_OK);
  if (other) {
    mc_db_track a = track("/music/x/a.mp3", "A", 1), b = track("/music/x/b.mp3", "B", 2);
    mc_db_album x = {0};
    x.title = "X";
    x.artist.name = "Someone Else";
    CHECK(mc_import_db_write(other, &x, &a, 1) == MC_OK);
    CHECK(mc_import_db_write(lib, &x, &b, 1) == MC_OK);
    CHECK(mc_import_db_write(other, &x, &a, 1) == MC_OK);
    mc_library_close(other);
  }
  CHECK(query_int(lib, "SELECT count(*) FROM changelog c1 WHERE EXISTS (SELECT 1 FROM changelog c2"
                       " WHERE c2.seq > c1.seq AND c2.hlc <= c1.hlc)") == 0);

  /* The album list for the grid: every album, by artist, albums before singles. */
  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  if (albums) {
    CHECK(albums->count == query_int(lib, "SELECT count(*) FROM albums"));
    CHECK(albums->count >= 2);
    CHECK_STR(albums->items[0].artist, "Low");
    CHECK(albums->items[0].is_singles);
    CHECK_STR(albums->items[0].title, "LOW Singles");
    CHECK(albums->items[0].folder != NULL && !albums->items[0].has_art);
    CHECK_STR(albums->items[albums->count - 1].artist, "Someone Else");
    mc_album_list_free(albums);
  }

  mc_library_close(lib);
  return TEST_RESULT();
}
