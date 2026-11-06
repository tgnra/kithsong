/* The seeds of an album's or artist's radio (kithcore/radio_seeds.h), from made-up analysis
 * results, so no audio is decoded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"
#include "kithcore/analysis.h"
#include "kithcore/kithcore.h"
#include "kithcore/radio.h"
#include "kithcore/radio_seeds.h"
#include "library/library_internal.h"
#include "test.h"
#include "util/platform.h"
#include "util/uid.h"

#define TRACK_MS 200000
#define LIMIT_MS (8 * 60 * 1000)

static void write_sql(mc_library *lib, const char *sql) {
  char *err = NULL;
  CHECK(mc_library_begin_write(lib) == MC_OK);
  if (sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
    test_failures++;
  }
  sqlite3_free(err);
  CHECK(mc_library_commit(lib) == MC_OK);
}

static mc_library *open_library(const char *scratch, const char *name) {
  char dir[1024], path[1100];
  snprintf(dir, sizeof dir, "%s/%s", scratch, name);
  mc_mkdirs(dir);
  snprintf(path, sizeof path, "%s/library.db", dir);
  remove(path);
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  return lib;
}

static void add_artist(mc_library *lib, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', '%s')", uid, uid);
  write_sql(lib, sql);
}

static void add_album(mc_library *lib, const char *uid, const char *artist, int mix) {
  char sql[256];
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid, mix) VALUES ('%s', '%s', '%s', %d)", uid, uid,
           artist, mix);
  write_sql(lib, sql);
}

/* How it sounds: tracks sound apart differ by that much in every timbre coefficient. */
static void result_of(double sound, mc_analysis *a) {
  memset(a, 0, sizeof *a);
  a->version = MC_ANALYSIS_VERSION;
  for (int i = 1; i < MC_ANALYSIS_MFCCS; i++) a->mfcc_mean[i] = (float)(sound * 5.0);
  a->centroid_mean = 1000.0f;
  a->lufs = -14.0;
  a->lra = 6.0;
  a->true_peak = -1.0;
}

static void add_track_of(mc_library *lib, const char *uid, const char *artist, const char *album, int duration_ms,
                         double sound, int analysed, int has_file) {
  char sql[512], file[MC_UID_STRLEN];
  snprintf(sql, sizeof sql, "INSERT INTO tracks (uid, title, artist_uid, album_uid, duration_ms) VALUES ('%s', '%s', %s%s%s, '%s', %d)",
           uid, uid, artist ? "'" : "", artist ? artist : "NULL", artist ? "'" : "", album, duration_ms);
  write_sql(lib, sql);
  if (has_file) {
    mc_uid_new(file);
    snprintf(sql, sizeof sql,
             "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms) VALUES ('%s', '%s', '%s/%s.wav', 'wav', 1000, 2000)",
             file, uid, album, uid);
    write_sql(lib, sql);
  }
  if (!analysed) return;
  mc_analysis a;
  result_of(sound, &a);
  CHECK(mc_analysis_store(lib, uid, NULL, MC_ANALYSIS_ORIGINAL, &a) == MC_OK);
}

static void add_track(mc_library *lib, const char *uid, const char *artist, const char *album, double sound) {
  add_track_of(lib, uid, artist, album, TRACK_MS, sound, 1, 1);
}

/* One row per device, as the library keeps them. */
static void played(mc_library *lib, const char *track, const char *node, int plays) {
  char sql[256], uid[MC_UID_STRLEN];
  mc_uid_new(uid);
  snprintf(sql, sizeof sql, "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES ('%s', '%s', '%s', %d, 1000)",
           uid, track, node, plays);
  write_sql(lib, sql);
}

static int album_seed_is(mc_library *lib, const char *album, const char *want) {
  char seed[MC_UID_STRLEN];
  return mc_radio_seed_for_album(lib, album, seed) == MC_OK && !strcmp(seed, want);
}

static int artist_seed_is(mc_library *lib, const char *artist, const char *want) {
  char seed[MC_UID_STRLEN];
  return mc_radio_seed_for_artist(lib, artist, seed) == MC_OK && !strcmp(seed, want);
}

/* The middle of the album, however much the rest have been played. */
static void test_middle(const char *scratch) {
  mc_library *lib = open_library(scratch, "middle");
  add_artist(lib, "Artist");
  add_album(lib, "album", "Artist", 0);
  add_track(lib, "a-low", "Artist", "album", 0.0);
  add_track(lib, "b-near", "Artist", "album", 1.0);
  add_track(lib, "c-mid", "Artist", "album", 1.2);
  add_track(lib, "d-high", "Artist", "album", 2.0);
  add_track(lib, "e-far", "Artist", "album", 9.0);
  CHECK(album_seed_is(lib, "album", "c-mid"));
  CHECK(artist_seed_is(lib, "Artist", "c-mid"));
  played(lib, "a-low", "n1", 500);
  played(lib, "b-near", "n1", 500);
  played(lib, "e-far", "n1", 500);
  CHECK(album_seed_is(lib, "album", "c-mid"));
  mc_library_close(lib);
}

/* Level tracks go to the more played, counted over every device, and otherwise to the uid. */
static void test_tie_break(const char *scratch) {
  mc_library *lib = open_library(scratch, "tie");
  add_artist(lib, "Artist");
  add_album(lib, "two", "Artist", 0);
  add_track(lib, "a", "Artist", "two", 0.0);
  add_track(lib, "b", "Artist", "two", 3.0);
  CHECK(album_seed_is(lib, "two", "a"));
  played(lib, "b", "n1", 2);
  CHECK(album_seed_is(lib, "two", "b"));
  played(lib, "a", "n1", 3);
  CHECK(album_seed_is(lib, "two", "a"));
  played(lib, "b", "n2", 2); /* 4 in all, on two devices */
  CHECK(album_seed_is(lib, "two", "b"));
  mc_library_close(lib);
}

/* Only what a station would take, and the seed is always one that it would. */
static void test_eligible(const char *scratch) {
  mc_library *lib = open_library(scratch, "eligible");
  char seed[MC_UID_STRLEN];
  add_artist(lib, "Artist");
  add_album(lib, "album", "Artist", 0);
  add_track(lib, "fine", "Artist", "album", 0.0);
  add_track(lib, "fine2", "Artist", "album", 9.0);
  /* These would be the middle of the two, if they counted. */
  add_track_of(lib, "no-result", "Artist", "album", TRACK_MS, 4.5, 0, 1);
  add_track_of(lib, "no-file", "Artist", "album", TRACK_MS, 4.5, 1, 0);
  add_track_of(lib, "too-short", "Artist", "album", 30000, 4.5, 1, 1);
  add_track_of(lib, "too-long", "Artist", "album", LIMIT_MS + 1, 4.5, 1, 1);
  CHECK(mc_radio_seed_for_album(lib, "album", seed) == MC_OK);
  CHECK(!strcmp(seed, "fine"));
  mc_radio_options opts = {.seed_track = seed};
  CHECK(mc_radio_seed_check(lib, &opts) == MC_OK);

  /* Exactly the limit is still in. */
  add_album(lib, "limit", "Artist", 0);
  add_track_of(lib, "at-limit", "Artist", "limit", LIMIT_MS, 0.0, 1, 1);
  CHECK(album_seed_is(lib, "limit", "at-limit"));

  add_artist(lib, "Nobody");
  add_album(lib, "nothing", "Nobody", 0);
  add_track_of(lib, "n1", "Nobody", "nothing", TRACK_MS, 0, 0, 1);
  add_track_of(lib, "n2", "Nobody", "nothing", TRACK_MS, 0, 1, 0);
  add_track_of(lib, "n3", "Nobody", "nothing", LIMIT_MS + 60000, 0, 1, 1);
  CHECK(mc_radio_seed_for_album(lib, "nothing", seed) == MC_ERR_NOT_FOUND);
  CHECK(!seed[0]);
  CHECK(mc_radio_seed_for_artist(lib, "Nobody", seed) == MC_ERR_NOT_FOUND);
  CHECK(!seed[0]);

  add_artist(lib, "Dj");
  add_album(lib, "mix", "Dj", 1);
  add_track(lib, "m1", "Dj", "mix", 0.0);
  add_track(lib, "m2", "Dj", "mix", 0.0);
  CHECK(mc_radio_seed_for_album(lib, "mix", seed) == MC_ERR_NOT_FOUND);
  CHECK(mc_radio_seed_for_artist(lib, "Dj", seed) == MC_ERR_NOT_FOUND);

  CHECK(mc_radio_seed_for_album(lib, "no such album", seed) == MC_ERR_NOT_FOUND);
  CHECK(mc_radio_seed_for_artist(lib, "no such artist", seed) == MC_ERR_NOT_FOUND);
  CHECK(mc_radio_seed_for_album(NULL, "album", seed) == MC_ERR_INVALID_ARG);
  CHECK(mc_radio_seed_for_album(lib, NULL, seed) == MC_ERR_INVALID_ARG);
  CHECK(mc_radio_seed_for_artist(lib, NULL, seed) == MC_ERR_INVALID_ARG);
  CHECK(mc_radio_seed_for_artist(lib, "Dj", NULL) == MC_ERR_INVALID_ARG);
  mc_library_close(lib);
}

/* An artist's tracks are the ones their page has: those they're featured on, and those with no
 * artist of their own on the artist's album. */
static void test_artist_tracks(const char *scratch) {
  mc_library *lib = open_library(scratch, "artist");
  add_artist(lib, "Host");
  add_artist(lib, "Guest");
  add_album(lib, "hosts", "Host", 0);
  add_track(lib, "h1", "Host", "hosts", 0.0);
  add_track(lib, "with-guest", "Host", "hosts", 0.0);
  write_sql(lib, "INSERT INTO track_artists (uid, track_uid, artist_uid, position) VALUES ('c', 'with-guest', 'Guest', 1)");
  CHECK(artist_seed_is(lib, "Guest", "with-guest"));

  add_artist(lib, "Plain");
  add_album(lib, "plain", "Plain", 0);
  add_track(lib, "no-artist", NULL, "plain", 0.0);
  CHECK(artist_seed_is(lib, "Plain", "no-artist"));
  mc_library_close(lib);
}

/* More tracks than are compared, nearly all of them no use: the one that is still gets found,
 * though the first stride over the list goes past it. */
static void test_long_list(const char *scratch) {
  mc_library *lib = open_library(scratch, "long");
  add_artist(lib, "Prolific");
  add_album(lib, "box", "Prolific", 0);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  for (int i = 0; i < 700; i++) {
    char sql[256];
    snprintf(sql, sizeof sql,
             "INSERT INTO tracks (uid, title, artist_uid, album_uid, duration_ms)"
             " VALUES ('t%04d', 't%04d', 'Prolific', 'box', %d)",
             i, i, TRACK_MS);
    CHECK(sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, NULL) == SQLITE_OK);
  }
  CHECK(mc_library_commit(lib) == MC_OK);
  char seed[MC_UID_STRLEN];
  CHECK(mc_radio_seed_for_artist(lib, "Prolific", seed) == MC_ERR_NOT_FOUND);
  /* Last by title: 700 of 701, with a stride of 11. */
  add_track(lib, "z-only", "Prolific", "box", 0.0);
  CHECK(artist_seed_is(lib, "Prolific", "z-only"));
  CHECK(album_seed_is(lib, "box", "z-only"));
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_middle(scratch);
  test_tie_break(scratch);
  test_eligible(scratch);
  test_artist_tracks(scratch);
  test_long_list(scratch);
  mc_shutdown();
  return TEST_RESULT();
}
