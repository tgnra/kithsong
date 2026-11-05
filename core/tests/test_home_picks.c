/* What goes in the Home page's rows (musiccore/home_picks.h). */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "library/library_internal.h"
#include "musiccore/home_picks.h"
#include "musiccore/musiccore.h"
#include "util/platform.h"
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

/* A row in the play log, of the track'th track of album i, days ago. */
static void log_play(mc_library *lib, int album, int track, int days, int64_t listened_ms, mc_play_end end,
                     mc_play_context context) {
  mc_play_event e;
  memset(&e, 0, sizeof e);
  snprintf(e.track_uid, sizeof e.track_uid, "t%02d%d", album, track);
  e.started_ms = (int64_t)time(NULL) * 1000 - days * DAY_MS;
  e.listened_ms = listened_ms;
  e.end = end;
  e.context = context;
  if (context == MC_PLAY_CONTEXT_ALBUM) snprintf(e.context_uid, sizeof e.context_uid, "al%02d", album);
  if (context == MC_PLAY_CONTEXT_RADIO) snprintf(e.context_uid, sizeof e.context_uid, "t%02d%d", album, track);
  CHECK(mc_library_log_play(lib, &e) == MC_OK);
}

/* times real plays of an album's first track, days ago. */
static void log_real(mc_library *lib, int album, int times, int days) {
  for (int i = 0; i < times; i++) log_play(lib, album, 0, days, 200000, MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_ALBUM);
}

/* A log that reaches past both windows: one skip, of no album a test looks at. */
static void log_long_ago(mc_library *lib) {
  log_play(lib, ALBUMS - 1, 0, 400, 1000, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_ALBUM);
}

static int row_at(const mc_home_picks *p, mc_home_row_id row, const char *uid) {
  for (int i = 0; i < p->counts[row]; i++)
    if (!strcmp(p->rows[row][i].uid, uid)) return i;
  return -1;
}

/* An album on repeat beats one track played once after years of silence, which looked the same
 * to track_plays, and it's plays in the window that are counted. */
static void test_repeat_or_once(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_repeat.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  log_long_ago(lib);
  log_real(lib, 1, 6, 2);   /* one track on repeat */
  log_real(lib, 2, 4, 3);
  play(db, 3, 1, 100, 1);   /* a hundred plays over the years, the last of them just now */
  log_real(lib, 3, 1, 1);
  log_real(lib, 4, 5, 30);  /* on repeat, but before the window */

  mc_home_picks p;
  mc_home_picks_load(lib, 7, &p);
  CHECK(p.counts[MC_HOME_ROW_PLAYED_LATELY] == 4);
  if (p.counts[MC_HOME_ROW_PLAYED_LATELY] == 4) {
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][0].uid, "al01"); /* the most plays */
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][1].uid, "al02");
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][2].uid, "al03"); /* only while the row is short */
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][3].uid, "al04");
  }
  mc_home_picks_free(&p);

  /* With the row full of what's on repeat, one play doesn't get in. */
  for (int i = 5; i < 12; i++) log_real(lib, i, 3, 4);
  mc_home_picks_load(lib, 7, &p);
  CHECK(p.counts[MC_HOME_ROW_PLAYED_LATELY] == 9);
  CHECK(row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al11"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al03") && !row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al04"));
  mc_home_picks_free(&p);
  mc_library_close(lib);
}

/* Skips, failures and seeks to the end are in the log but aren't plays, so they neither make an
 * album played lately nor stop it being one not played in a while. */
static void test_skips_ignored(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_skips.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  log_long_ago(lib);
  for (int i = 0; i < 10; i++) log_play(lib, 1, 0, 1, 2000, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_ALBUM);
  for (int i = 0; i < 5; i++) log_play(lib, 2, 0, 1, 0, MC_PLAY_END_FAILED, MC_PLAY_CONTEXT_ALBUM);
  log_play(lib, 3, 0, 1, 40000, MC_PLAY_END_FAILED, MC_PLAY_CONTEXT_ALBUM); /* whatever time it gives */
  play(db, 4, 2, 3, 200);
  for (int i = 0; i < 4; i++) log_play(lib, 4, 0, 1, 2000, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_ALBUM);

  /* A good listen that was skipped in the end is a play, and half of a short track is enough
   * for one, but 12 seconds of a long one aren't, even if that took it to the end. */
  log_play(lib, 5, 0, 1, 31000, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_ALBUM);
  CHECK(exec(db, "UPDATE tracks SET duration_ms = 20000 WHERE uid = 't060'") == SQLITE_OK);
  CHECK(exec(db, "UPDATE tracks SET duration_ms = 300000 WHERE uid = 't070'") == SQLITE_OK);
  log_play(lib, 6, 0, 1, 12000, MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_ALBUM);
  log_play(lib, 7, 0, 1, 12000, MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_ALBUM);

  mc_home_picks p;
  mc_home_picks_load(lib, 7, &p);
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al01"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al02"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al03"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al04"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al07"));
  CHECK(row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al05"));
  CHECK(row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al06"));
  CHECK(row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al04"));
  CHECK(!row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al01")); /* never really played */
  mc_home_picks_free(&p);
  mc_library_close(lib);
}

/* A station's plays are no reason for Played a Lot Recently, however many, and track_plays, which
 * has them too, doesn't let them in by the back door. But they were heard. */
static void test_radio(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_radio.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  log_play(lib, 1, 0, 5, 200000, MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO); /* the log's start */
  play(db, 1, 3, 2, 1);
  for (int t = 0; t < 3; t++)
    for (int i = 0; i < 2; i++) log_play(lib, 1, t, 1, 200000, MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO);
  play(db, 2, 2, 4, 200); /* long unplayed, but for a station last week */
  log_play(lib, 2, 0, 7, 200000, MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO);
  play(db, 3, 2, 4, 200); /* a station's skip is a skip */
  log_play(lib, 3, 0, 7, 3000, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO);

  mc_home_picks p;
  mc_home_picks_load(lib, 7, &p);
  CHECK(p.has_plays);
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al01"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al02"));
  CHECK(!row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al01"));
  CHECK(!row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al02"));
  CHECK(row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al03"));
  mc_home_picks_free(&p);
  mc_library_close(lib);
}

/* A log begun days ago: what it has comes first, and the guess fills in from before it, but only
 * from before it. test_played has no log at all. */
static void test_short_log(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_short.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  log_real(lib, 1, 3, 3);   /* the log begins */
  play(db, 2, 3, 2, 8);     /* played a lot before it, in the window */
  play(db, 3, 1, 1, 9);     /* a stray track before it */
  play(db, 4, 1, 1, 40);    /* longer ago, for a quiet row */
  play(db, 5, 3, 2, 1);     /* since the log began, and the log has no real plays of it */
  log_play(lib, 5, 0, 1, 1000, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_ALBUM);
  play(db, 6, 2, 2, 300);

  mc_home_picks p;
  mc_home_picks_load(lib, 7, &p);
  CHECK(p.counts[MC_HOME_ROW_PLAYED_LATELY] == 4);
  if (p.counts[MC_HOME_ROW_PLAYED_LATELY] == 4) {
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][0].uid, "al01");
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][1].uid, "al02");
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][2].uid, "al03");
    CHECK_STR(p.rows[MC_HOME_ROW_PLAYED_LATELY][3].uid, "al04");
  }
  CHECK(row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al06"));
  mc_home_picks_free(&p);
  mc_library_close(lib);
}

/* A log older than the windows leaves nothing to guess: track_plays alone can't put an album in
 * Played a Lot Recently, only say when it was last played. */
static void test_long_log(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_long.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  log_long_ago(lib);
  play(db, 1, 3, 2, 2);
  log_real(lib, 2, 2, 120); /* real plays too old to read */
  play(db, 2, 1, 2, 120);
  log_real(lib, 3, 2, 40);  /* a quiet fortnight */
  /* Heard for 40 seconds of a long track, so in the log but not in track_plays. */
  log_play(lib, 4, 0, 2, 40000, MC_PLAY_END_STOPPED, MC_PLAY_CONTEXT_SHUFFLE);

  CHECK(exec(db, "INSERT INTO playlists (uid, name) VALUES ('pl', 'Mix');"
                 "INSERT INTO playlist_entries (uid, playlist_uid, track_uid, position) VALUES"
                 " ('e1', 'pl', 't100', 'a'), ('e2', 'pl', 't100', 'b'), ('e3', 'pl', 't110', 'c');") == SQLITE_OK);
  log_real(lib, 10, 2, 1); /* however they were played; twice in the list is still two plays */
  log_real(lib, 11, 1, 1);

  mc_home_picks p;
  mc_home_picks_load(lib, 7, &p);
  CHECK(p.has_plays);
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al01"));
  CHECK(!row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al01"));
  CHECK(!row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al02"));
  CHECK(row_has(&p, MC_HOME_ROW_NOT_PLAYED_LATELY, "al02"));
  CHECK(row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al03"));
  CHECK(row_has(&p, MC_HOME_ROW_PLAYED_LATELY, "al04"));
  CHECK(row_at(&p, MC_HOME_ROW_PLAYED_LATELY, "pl") == 0);
  CHECK(row_at(&p, MC_HOME_ROW_PLAYED_LATELY, "al03") > row_at(&p, MC_HOME_ROW_PLAYED_LATELY, "al04"));
  mc_home_picks_free(&p);
  mc_library_close(lib);
}

/* A station in Recents is the same tile as one in the Radio row: marked, with the seed to start
 * from and the seed's cover. A recent's uid is the seed, the tile's its artist. */
static void test_recent_station(const char *scratch) {
  mc_library *lib = open_fresh(scratch, "test_home_picks_station.d");
  if (!lib) return;
  sqlite3 *db = mc_library_db(lib);
  seed_albums(db);
  CHECK(mc_library_record_opened(lib, MC_RECENT_STATION, "t010") == MC_OK);
  mc_sleep_ms(2);
  CHECK(mc_library_record_opened(lib, MC_RECENT_STATION, "t020") == MC_OK);
  mc_sleep_ms(2);
  CHECK(mc_library_record_opened(lib, MC_RECENT_ALBUM, "al03") == MC_OK);

  mc_home_picks p;
  mc_home_picks_load(lib, 1, &p);
  CHECK(p.counts[MC_HOME_ROW_RECENTS] == 3);
  if (p.counts[MC_HOME_ROW_RECENTS] == 3) {
    const mc_home_pick *album = &p.rows[MC_HOME_ROW_RECENTS][0], *st = &p.rows[MC_HOME_ROW_RECENTS][1];
    CHECK(!album->station && album->kind == MC_RECENT_ALBUM);
    CHECK(st->station && st->kind == MC_RECENT_ARTIST && !st->smart);
    CHECK_STR(st->uid, "A");
    CHECK_STR(st->seed_uid, "t020");
    CHECK_STR(st->title, "Anna Radio");
    CHECK_STR(st->sub, "Like T");
    CHECK_STR(st->seed_title, "T");
    CHECK(st->cover_dir && strstr(st->cover_dir, "02"));
    CHECK_STR(p.rows[MC_HOME_ROW_RECENTS][2].seed_uid, "t010");
    CHECK_STR(p.rows[MC_HOME_ROW_RECENTS][2].title, "Bo Radio");
  }
  mc_home_picks_free(&p);

  /* Gone from the row with its seed, or with the seed's file. */
  CHECK(exec(db, "DELETE FROM tracks WHERE uid = 't020'; DELETE FROM files WHERE track_uid = 't010'") == SQLITE_OK);
  mc_home_picks_load(lib, 1, &p);
  CHECK(p.counts[MC_HOME_ROW_RECENTS] == 1);
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
  test_repeat_or_once(scratch);
  test_skips_ignored(scratch);
  test_radio(scratch);
  test_short_log(scratch);
  test_long_log(scratch);
  test_recent_station(scratch);
  test_titles();
  mc_shutdown();
  return TEST_RESULT();
}
