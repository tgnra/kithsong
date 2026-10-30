/* Library rows written for accepted import items: albums, singles, tracks with no album,
 * re-imports, and two connections writing to one library. */
#include <stdio.h>
#include <string.h>

#include "library/library_internal.h"
#include "musiccore/musiccore.h"
#include "tagger/import_db.h"
#include "test.h"
#include "util/uid.h"

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

/* The first column of sql's first row, into out ("" if none). */
static void query_text(mc_library *lib, const char *sql, char *out, size_t cap) {
  sqlite3_stmt *stmt;
  out[0] = '\0';
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &stmt, NULL) != SQLITE_OK) return;
  if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
    snprintf(out, cap, "%s", (const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
}

/* uid's credit rows in table as "name|join;name|join", by position. */
static void credit_of(mc_library *lib, const char *table, const char *key, const char *uid, char *out,
                      size_t cap) {
  char sql[400];
  snprintf(sql, sizeof sql,
           "SELECT group_concat(name || '|' || joinphrase, ';') FROM (SELECT ar.name, c.joinphrase"
           " FROM %s c JOIN artists ar ON ar.uid = c.artist_uid WHERE c.%s = '%s' ORDER BY c.position)",
           table, key, uid);
  query_text(lib, sql, out, cap);
}

/* Multi-artist credits: the album's and each track's artists, the shown credit, rows past a
 * shorter credit deleted, and credits replaced in place. */
static void test_credits(const char *scratch) {
  char path[1024], db_file[1100];
  snprintf(path, sizeof path, "%s/test_import_db_credits.d", scratch);
  snprintf(db_file, sizeof db_file, "%s/" MC_LIBRARY_DB_NAME, path);
  remove_db(db_file);
  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  if (!lib) return;

  /* An album credited to two, a track that is the album's and one credited to three. */
  const mc_db_credit_part duo[] = {
    {{"Alpha", NULL, "a1b2c3d4-0000-4000-8000-0000000000a1"}, " & "},
    {{"Beta", "Beta, The", "a1b2c3d4-0000-4000-8000-0000000000b2"}, NULL},
  };
  const mc_db_credit_part trio[] = {
    {{"Alpha", NULL, "a1b2c3d4-0000-4000-8000-0000000000a1"}, " feat. "},
    {{"Beta", NULL, "a1b2c3d4-0000-4000-8000-0000000000b2"}, " & "},
    {{"Gamma", NULL, NULL}, NULL},
  };
  mc_db_album album = {0};
  album.title = "Together";
  album.artist = (mc_db_artist){"Alpha & Beta", NULL, NULL};
  album.credit = duo;
  album.credit_count = 2;
  album.mb_albumid = "a1b2c3d4-0000-4000-8000-0000000000cc";
  mc_db_track tracks[] = {track("c/01.flac", "One", 1), track("c/02.flac", "Two", 2)};
  tracks[1].artist = (mc_db_artist){"Alpha feat. Beta & Gamma", NULL, NULL};
  tracks[1].credit = trio;
  tracks[1].credit_count = 3;
  CHECK(mc_import_db_write(lib, &album, tracks, 2) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM artists") == 3); /* one row each, never the whole */
  CHECK(query_int(lib, "SELECT count(*) FROM artists WHERE name = 'Beta' AND sort_name = 'Beta, The'") == 1);

  char al[MC_UID_STRLEN], t1[MC_UID_STRLEN], t2[MC_UID_STRLEN], s[512];
  query_text(lib, "SELECT uid FROM albums", al, sizeof al);
  query_text(lib, "SELECT track_uid FROM files WHERE path = 'c/01.flac'", t1, sizeof t1);
  query_text(lib, "SELECT track_uid FROM files WHERE path = 'c/02.flac'", t2, sizeof t2);
  query_text(lib, "SELECT ar.name || '/' || a.artist_credit FROM albums a JOIN artists ar ON ar.uid = a.artist_uid",
             s, sizeof s);
  CHECK_STR(s, "Alpha/Alpha & Beta");
  credit_of(lib, "album_artists", "album_uid", al, s, sizeof s);
  CHECK_STR(s, "Alpha| & ;Beta|");
  credit_of(lib, "track_artists", "track_uid", t1, s, sizeof s);
  CHECK_STR(s, "Alpha| & ;Beta|"); /* the album's */
  credit_of(lib, "track_artists", "track_uid", t2, s, sizeof s);
  CHECK_STR(s, "Alpha| feat. ;Beta| & ;Gamma|");
  query_text(lib, "SELECT group_concat(artist_credit, ';') FROM (SELECT artist_credit FROM tracks ORDER BY track_no)",
             s, sizeof s);
  CHECK_STR(s, "Alpha & Beta;Alpha feat. Beta & Gamma");
  /* Row uids come from the owner and position; the rows are logged for sync. */
  char want[MC_UID_STRLEN], sql[256];
  CHECK(mc_uid_derive(t2, 2, want) == MC_OK);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM track_artists WHERE uid = '%s' AND position = 2", want);
  CHECK(query_int(lib, sql) == 1);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE entity = 'track_artists' AND row_uid = '%s'", want);
  CHECK(query_int(lib, sql) == 4); /* track_uid, artist_uid, position, joinphrase */
  CHECK(query_int(lib, "SELECT count(*) FROM changelog WHERE entity = 'tracks' AND field = 'artist_credit'") == 2);

  /* Shown as credited, and listed on each artist's page. */
  mc_track_list *list = NULL;
  CHECK(mc_library_list_album_tracks(lib, al, &list) == MC_OK && list && list->count == 2);
  if (list && list->count == 2) {
    CHECK_STR(list->items[1].artist, "Alpha feat. Beta & Gamma");
    CHECK_STR(list->items[0].artist, "Alpha & Beta");
  }
  mc_track_list_free(list);
  char gamma[MC_UID_STRLEN], beta[MC_UID_STRLEN];
  query_text(lib, "SELECT uid FROM artists WHERE name = 'Gamma'", gamma, sizeof gamma);
  query_text(lib, "SELECT uid FROM artists WHERE name = 'Beta'", beta, sizeof beta);
  list = NULL;
  CHECK(mc_library_list_artist_tracks(lib, gamma, &list) == MC_OK && list && list->count == 1);
  if (list && list->count == 1) CHECK_STR(list->items[0].title, "Two");
  mc_track_list_free(list);
  mc_album_list *albums = NULL;
  CHECK(mc_library_list_artist_albums(lib, beta, &albums) == MC_OK && albums && albums->count == 1);
  if (albums && albums->count == 1) {
    CHECK_STR(albums->items[0].artist, "Alpha & Beta");
    CHECK(albums->items[0].track_count == 2 && albums->items[0].credited_count == 2);
  }
  mc_album_list_free(albums);
  mc_credit_list *credit = NULL;
  CHECK(mc_library_list_credit(lib, t2, &credit) == MC_OK && credit && credit->count == 3);
  if (credit && credit->count == 3) {
    CHECK_STR(credit->items[0].name, "Alpha");
    CHECK_STR(credit->items[0].join, " feat. ");
    CHECK_STR(credit->items[2].name, "Gamma");
    CHECK_STR(credit->items[2].join, "");
    CHECK(strcmp(credit->items[2].uid, gamma) == 0);
  }
  mc_credit_list_free(credit);

  /* A re-import with a shorter credit: the rows past it go, logged, and the credit is the
   * artist's own name again. */
  const mc_db_credit_part solo[] = {{{"Alpha", NULL, "a1b2c3d4-0000-4000-8000-0000000000a1"}, NULL}};
  tracks[1].artist = (mc_db_artist){"Alpha", NULL, "a1b2c3d4-0000-4000-8000-0000000000a1"};
  tracks[1].credit = solo;
  tracks[1].credit_count = 1;
  CHECK(mc_import_db_write(lib, &album, &tracks[1], 1) == MC_OK);
  credit_of(lib, "track_artists", "track_uid", t2, s, sizeof s);
  CHECK_STR(s, "Alpha|");
  snprintf(sql, sizeof sql, "SELECT artist_credit IS NULL FROM tracks WHERE uid = '%s'", t2);
  CHECK(query_int(lib, sql) == 1);
  snprintf(sql, sizeof sql, "SELECT count(*) FROM changelog WHERE entity = 'track_artists' AND row_uid = '%s'"
                            " AND field = '_deleted'", want);
  CHECK(query_int(lib, sql) == 1);
  list = NULL;
  CHECK(mc_library_list_artist_tracks(lib, gamma, &list) == MC_OK && list && list->count == 0);
  mc_track_list_free(list);
  /* And back to three: the same rows, live again. */
  tracks[1].artist = (mc_db_artist){"Alpha feat. Beta & Gamma", NULL, NULL};
  tracks[1].credit = trio;
  tracks[1].credit_count = 3;
  CHECK(mc_import_db_write(lib, &album, &tracks[1], 1) == MC_OK);
  credit_of(lib, "track_artists", "track_uid", t2, s, sizeof s);
  CHECK_STR(s, "Alpha| feat. ;Beta| & ;Gamma|");

  /* No parts: the artist alone, as before credits. */
  mc_db_track lone = track("c/03.flac", "Three", 3);
  lone.artist = (mc_db_artist){"Delta", NULL, NULL};
  CHECK(mc_import_db_write(lib, &album, &lone, 1) == MC_OK);
  char t3[MC_UID_STRLEN];
  query_text(lib, "SELECT track_uid FROM files WHERE path = 'c/03.flac'", t3, sizeof t3);
  credit_of(lib, "track_artists", "track_uid", t3, s, sizeof s);
  CHECK_STR(s, "Delta|");
  snprintf(sql, sizeof sql, "SELECT artist_credit IS NULL FROM tracks WHERE uid = '%s'", t3);
  CHECK(query_int(lib, sql) == 1);

  /* Into an existing album kept as it is: a track with no artist takes its credit. */
  mc_db_album into = {0};
  into.title = "Together";
  into.artist = (mc_db_artist){"Someone", NULL, NULL};
  into.existing_uid = al;
  mc_db_track four = track("c/04.flac", "Four", 4);
  CHECK(mc_import_db_write(lib, &into, &four, 1) == MC_OK);
  char t4[MC_UID_STRLEN];
  query_text(lib, "SELECT track_uid FROM files WHERE path = 'c/04.flac'", t4, sizeof t4);
  credit_of(lib, "track_artists", "track_uid", t4, s, sizeof s);
  CHECK_STR(s, "Alpha| & ;Beta|");
  CHECK(query_int(lib, "SELECT count(*) FROM artists WHERE name = 'Someone'") == 0);

  /* A whole release swapped in updates a known track's credit (update_track). */
  mc_db_track again = track("c/04b.flac", "Four", 4);
  again.track_uid = t4;
  again.update_track = 1;
  again.artist = (mc_db_artist){"Gamma", NULL, NULL};
  CHECK(mc_import_db_write(lib, &into, &again, 1) == MC_OK);
  credit_of(lib, "track_artists", "track_uid", t4, s, sizeof s);
  CHECK_STR(s, "Gamma|");
  snprintf(sql, sizeof sql, "SELECT artist_uid = '%s' AND artist_credit IS NULL FROM tracks WHERE uid = '%s'",
           gamma, t4);
  CHECK(query_int(lib, sql) == 1);

  /* Replaced in place, as the refresh after upgrading does: the album to three, then back to
   * one artist alone. */
  mc_db_artist whole = {"Alpha, Beta & Gamma", NULL, NULL};
  const mc_db_credit_part three[] = {{trio[0].artist, ", "}, {trio[1].artist, " & "}, {trio[2].artist, NULL}};
  CHECK(mc_import_db_set_credit(lib, 1, al, &whole, three, 3) == MC_OK);
  credit_of(lib, "album_artists", "album_uid", al, s, sizeof s);
  CHECK_STR(s, "Alpha|, ;Beta| & ;Gamma|");
  snprintf(sql, sizeof sql, "SELECT artist_credit FROM albums WHERE uid = '%s'", al);
  query_text(lib, sql, s, sizeof s);
  CHECK_STR(s, "Alpha, Beta & Gamma");
  albums = NULL;
  CHECK(mc_library_list_artist_albums(lib, gamma, &albums) == MC_OK && albums && albums->count == 1);
  mc_album_list_free(albums);
  whole = (mc_db_artist){"Beta", NULL, NULL};
  CHECK(mc_import_db_set_credit(lib, 1, al, &whole, NULL, 0) == MC_OK);
  credit_of(lib, "album_artists", "album_uid", al, s, sizeof s);
  CHECK_STR(s, "Beta|");
  snprintf(sql, sizeof sql, "SELECT artist_uid = '%s' AND artist_credit IS NULL FROM albums WHERE uid = '%s'",
           beta, al);
  CHECK(query_int(lib, sql) == 1);
  /* Without parts or a name of its own, the credit is spelled from its parts. */
  whole = (mc_db_artist){NULL, NULL, NULL};
  CHECK(mc_import_db_set_credit(lib, 0, t3, &whole, duo, 2) == MC_OK);
  snprintf(sql, sizeof sql, "SELECT artist_credit FROM tracks WHERE uid = '%s'", t3);
  query_text(lib, sql, s, sizeof s);
  CHECK_STR(s, "Alpha & Beta");
  CHECK(mc_import_db_set_credit(lib, 0, "nope", &whole, duo, 2) == MC_ERR_NOT_FOUND);
  CHECK(query_int(lib, "SELECT count(*) FROM artists WHERE name = 'Delta'") == 1); /* left for the refresh */
  mc_library_close(lib);
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
  CHECK(query_int(lib, "SELECT count(*) FROM albums") == 1);
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

  /* Tracks with no album get none: no album row, matched to the existing artist by
   * MusicBrainz id even though the name differs in case. */
  mc_db_album loose = {0};
  loose.no_album = 1;
  loose.title = "Ignored";
  loose.artist = (mc_db_artist){"LOW", NULL, "a1b2c3d4-0000-4000-8000-000000000001"};
  mc_db_track s1 = track("/music/loose/just-like-christmas.mp3", "Just Like Christmas", 0);
  s1.year = 1999;
  mc_db_track s2 = track("/music/loose/canada.mp3", "Canada", 0);
  s2.year = 2002;
  CHECK(mc_import_db_write(lib, &loose, &s1, 1) == MC_OK);
  CHECK(mc_import_db_write(lib, &loose, &s2, 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM artists") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE album_uid IS NULL AND artist_uid IS NOT NULL") == 2);
  CHECK(query_int(lib, "SELECT year FROM tracks WHERE title = 'Canada'") == 2002);

  /* A single from a release is an album like any other, of one track. Nothing is ever a
   * 'singles' album. */
  mc_db_album single = {0};
  single.title = "Just Like Christmas";
  single.artist = album.artist;
  single.year = 1999;
  single.mb_albumid = "a1b2c3d4-0000-4000-8000-0000000000bb";
  mc_db_track c1 = track("/music/low/Just Like Christmas (1999)/01 Just Like Christmas.mp3",
                         "Just Like Christmas", 1);
  c1.mb_albumid = single.mb_albumid;
  CHECK(mc_import_db_write(lib, &single, &c1, 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE mb_albumid = 'a1b2c3d4-0000-4000-8000-0000000000bb'"
                       " AND year = 1999") == 1);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE kind <> 'album'") == 0);

  /* A featured artist gets their own row; with no album, the track is theirs. */
  mc_db_track feat = track("/music/loose/feat.mp3", "Collab", 0);
  feat.artist = (mc_db_artist){"Low feat. Someone", NULL, NULL};
  CHECK(mc_import_db_write(lib, &loose, &feat, 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums") == 2);
  CHECK(query_int(lib, "SELECT count(*) FROM artists") == 2);

  /* Re-importing an album's files with no album moves the tracks out of it, and the album is
   * deleted once it's empty. */
  CHECK(mc_import_db_write(lib, &loose, &tracks[0], 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE title = 'Things We Lost in the Fire'") == 1);
  CHECK(mc_import_db_write(lib, &loose, &tracks[1], 1) == MC_OK);
  CHECK(query_int(lib, "SELECT count(*) FROM albums WHERE title = 'Things We Lost in the Fire'") == 0);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks") == 6);
  CHECK(query_int(lib, "SELECT count(*) FROM files") == 6);
  CHECK(query_int(lib, "SELECT count(*) FROM tracks WHERE album_uid IS NULL") == 5);

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

  /* The album list for the grid: by artist, but no singles (albums of one track). X has two
   * tracks now; Just Like Christmas is a single, listed with Low's tracks with no album. */
  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  if (albums) {
    CHECK(albums->count == 1);
    if (albums->count == 1) {
      CHECK_STR(albums->items[0].title, "X");
      CHECK_STR(albums->items[0].artist, "Someone Else");
      CHECK(albums->items[0].folder != NULL && !albums->items[0].has_art && !albums->items[0].is_singles);
    }
    mc_album_list_free(albums);
  }
  char low[MC_UID_STRLEN] = "";
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib), "SELECT uid FROM artists WHERE name = 'Low'", -1, &stmt, NULL) ==
      SQLITE_OK) {
    if (sqlite3_step(stmt) == SQLITE_ROW) snprintf(low, sizeof low, "%s", (const char *)sqlite3_column_text(stmt, 0));
    sqlite3_finalize(stmt);
  }
  mc_track_list *singles = NULL;
  CHECK(mc_library_list_artist_singles(lib, low, &singles) == MC_OK && singles && singles->count == 5);
  int with_album = 0;
  for (int i = 0; singles && i < singles->count; i++) with_album += singles->items[i].album != NULL;
  CHECK(with_album == 1);
  mc_track_list_free(singles);

  mc_library_close(lib);
  test_credits(argc > 1 ? argv[1] : ".");
  return TEST_RESULT();
}
