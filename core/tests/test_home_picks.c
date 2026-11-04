/* What goes in the Home page's rows (musiccore/home_picks.h). */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "library/library_internal.h"
#include "musiccore/home_picks.h"
#include "musiccore/musiccore.h"
#include "test.h"

#define DAY_MS (24 * 60 * 60 * 1000LL)

enum { ALBUMS = 30, TRACKS = 3 };

static void remove_db(const char *dir) {
  const char *const names[] = {MC_LIBRARY_DB_NAME, MC_LIBRARY_DB_NAME "-wal", MC_LIBRARY_DB_NAME "-shm"};
  char file[1100];
  for (int i = 0; i < 3; i++) {
    snprintf(file, sizeof file, "%s/%s", dir, names[i]);
    remove(file);
  }
}

static int exec(sqlite3 *db, const char *sql) {
  return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static mc_library *open_fresh(const char *scratch, const char *name) {
  char path[1024];
  snprintf(path, sizeof path, "%s/%s", scratch, name);
  remove_db(path);
  mc_library *lib = NULL;
  CHECK(mc_library_open(path, &lib) == MC_OK);
  return lib;
}

/* ALBUMS albums of TRACKS tracks, al00 on, with covers and files here, shared by two artists:
 * Anna has the even ones, Bo the odd. */
static void seed_albums(sqlite3 *db) {
  CHECK(exec(db, "INSERT INTO artists (uid, name) VALUES ('A', 'Anna'), ('B', 'Bo');") == SQLITE_OK);
  for (int i = 0; i < ALBUMS; i++) {
    char sql[256];
    const char *artist = i % 2 ? "B" : "A";
    snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid, art_hash) VALUES ('al%02d', 'Album %d', '%s', 'h');",
             i, i, artist);
    CHECK(exec(db, sql) == SQLITE_OK);
    for (int t = 0; t < TRACKS; t++) {
      snprintf(sql, sizeof sql, "INSERT INTO tracks (uid, album_uid, artist_uid, title) VALUES ('t%02d%d', 'al%02d', '%s', 'T');",
               i, t, i, artist);
      CHECK(exec(db, sql) == SQLITE_OK);
      snprintf(sql, sizeof sql, "INSERT INTO files (uid, track_uid, path, format) VALUES ('f%02d%d', 't%02d%d', 'm/%02d/%d.flac', 'flac');",
               i, t, i, t, i, t);
      CHECK(exec(db, sql) == SQLITE_OK);
    }
  }
}

/* Marks the first tracks of album i played, plays times each, days ago. */
static void play(sqlite3 *db, int album, int tracks, int plays, int days) {
  int64_t when = (int64_t)time(NULL) * 1000 - days * DAY_MS;
  for (int t = 0; t < tracks; t++) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES"
             " ('p%02d%d', 't%02d%d', 'n1', %d, %lld);",
             album, t, album, t, plays, (long long)when);
    CHECK(exec(db, sql) == SQLITE_OK);
  }
}

static int row_has(const mc_home_picks *p, mc_home_row_id row, const char *uid) {
  for (int i = 0; i < p->counts[row]; i++)
    if (!strcmp(p->rows[row][i].uid, uid)) return 1;
  return 0;
}

static void test_nothing(const char *scratch) {
  mc_home_picks picks;
  mc_home_picks_load(NULL, 1, &picks);
  CHECK(!picks.has_music && !picks.has_plays);
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) CHECK(picks.counts[i] == 0 && !picks.rows[i]);
  mc_home_picks_free(&picks);

  mc_library *lib = open_fresh(scratch, "test_home_picks_empty.d");
  if (!lib) return;
  mc_home_picks_load(lib, 1, &picks);
  CHECK(!picks.has_music && !picks.has_plays);
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) CHECK(picks.counts[i] == 0);
  mc_home_picks_free(&picks);
  mc_library_close(lib);
}

/* A library not played yet: Random, with its choice held by its seed. With nothing analysed
 * there are no stations, and no placeholders for them. */
static void test_unplayed(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_unplayed.d");
  if (!lib) return;
  seed_albums(mc_library_db(lib));

  mc_home_picks a, b, c;
  mc_home_picks_load(lib, 7, &a);
  mc_home_picks_load(lib, 7, &b);
  mc_home_picks_load(lib, 8, &c);
  CHECK(a.has_music && !a.has_plays);
  CHECK(a.counts[MC_HOME_ROW_RECENTS] == 0);
  CHECK(a.counts[MC_HOME_ROW_PLAYED_LATELY] == 0 && a.counts[MC_HOME_ROW_NOT_PLAYED_LATELY] == 0);

  CHECK(a.counts[MC_HOME_ROW_RADIO] == 0 && !a.rows[MC_HOME_ROW_RADIO]);

  CHECK(a.counts[MC_HOME_ROW_RANDOM] == MC_HOME_ROW_MAX); /* more albums than fit */
  CHECK(b.counts[MC_HOME_ROW_RANDOM] == MC_HOME_ROW_MAX && c.counts[MC_HOME_ROW_RANDOM] == MC_HOME_ROW_MAX);
  int same = 1, other = 1;
  for (int i = 0; i < MC_HOME_ROW_MAX; i++) {
    same &= !strcmp(a.rows[MC_HOME_ROW_RANDOM][i].uid, b.rows[MC_HOME_ROW_RANDOM][i].uid);
    other &= !strcmp(a.rows[MC_HOME_ROW_RANDOM][i].uid, c.rows[MC_HOME_ROW_RANDOM][i].uid);
  }
  CHECK(same);
  CHECK(!other);
  const mc_home_pick *first = &a.rows[MC_HOME_ROW_RANDOM][0];
  CHECK(first->kind == MC_RECENT_ALBUM && !first->station && first->cover_dir);
  CHECK(!strncmp(first->title, "Album ", 6));
  CHECK(!strcmp(first->sub, "Anna") || !strcmp(first->sub, "Bo"));

  mc_home_picks_free(&a);
  mc_home_picks_free(&b);
  mc_home_picks_free(&c);
  mc_library_close(lib);
}

/* Each row takes what's played by when. */
static void test_played(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_played.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  play(db, 1, 3, 5, 1);     /* Bo's, all of it this week */
  play(db, 2, 2, 1, 200);   /* a while ago */
  play(db, 4, 1, 1, 400);   /* longer ago, but one stray track */
  CHECK(exec(db, "INSERT INTO playlists (uid, name) VALUES ('pl', 'Mix');"
                 "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position) VALUES"
                 " ('e1', 'pl', 't060', 'a');") == SQLITE_OK);
  CHECK(exec(db, "INSERT INTO recents (uid, kind, opened_ms) VALUES ('al06', 'album', 1), ('A', 'artist', 2),"
                 " ('pl', 'playlist', 3);") == SQLITE_OK);

  mc_home_picks p;
  mc_home_picks_load(lib, 7, &p);
  CHECK(p.has_music && p.has_plays);

  CHECK(p.counts[MC_HOME_ROW_RADIO] == 0); /* played, but not analysed */

  CHECK(p.counts[MC_HOME_ROW_RECENTS] == 3);
  if (p.counts[MC_HOME_ROW_RECENTS] == 3) {
    const mc_home_pick *r = p.rows[MC_HOME_ROW_RECENTS];
    CHECK(r[0].kind == MC_RECENT_PLAYLIST && !strcmp(r[0].uid, "pl") && !r[0].cover_dir);
    CHECK_STR(r[0].sub, "1 track");
    CHECK(r[1].kind == MC_RECENT_ARTIST && !r[1].station && r[1].cover_dir);
    CHECK_STR(r[1].sub, "15 albums");
    CHECK(r[2].kind == MC_RECENT_ALBUM && !strcmp(r[2].uid, "al06"));
  }

  CHECK(row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al01"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al02"));

  /* Two tracks played ahead of one stray track played longer ago. */
  CHECK(p.counts[MC_HOME_ROW_NOT_PLAYED_LATELY] == 2);
  if (p.counts[MC_HOME_ROW_NOT_PLAYED_LATELY] == 2) {
    CHECK_STR(p.rows[MC_HOME_ROW_NOT_PLAYED_LATELY][0].uid, "al02");
    CHECK_STR(p.rows[MC_HOME_ROW_NOT_PLAYED_LATELY][1].uid, "al04");
  }

  CHECK(!row_has(&p, MC_HOME_ROW_RANDOM, "al01") && !row_has(&p, MC_HOME_ROW_RANDOM, "al02"));
  CHECK(!row_has(&p, MC_HOME_ROW_RANDOM, "al04"));
  mc_home_picks_free(&p);
  mc_library_close(lib);
}

static void test_titles(void) {
  CHECK_STR(mc_str(mc_home_row_title(MC_HOME_ROW_RADIO)), "Radio");
  CHECK_STR(mc_str(mc_home_row_title(MC_HOME_ROW_RECENTS)), "Recents");
  CHECK_STR(mc_str(mc_home_row_title(MC_HOME_ROW_PLAYED_LATELY)), "Played a lot recently");
  CHECK_STR(mc_str(mc_home_row_title(MC_HOME_ROW_NOT_PLAYED_LATELY)), "Not played in a while");
  CHECK_STR(mc_str(mc_home_row_title(MC_HOME_ROW_RANDOM)), "Random");
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_nothing(scratch);
  test_unplayed(scratch);
  test_played(scratch);
  test_titles();
  mc_shutdown();
  return TEST_RESULT();
}
