/* Radio mixes (musiccore/radio.h), from made-up analysis results, so no audio is decoded. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"
#include "library/library_internal.h"
#include "musiccore/analysis.h"
#include "musiccore/musiccore.h"
#include "musiccore/radio.h"
#include "test.h"
#include "util/platform.h"
#include "util/uid.h"

#define NOW_MS 2000000000000LL

static void exec_ok(mc_library *lib, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
    test_failures++;
  }
  sqlite3_free(err);
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
  CHECK(mc_library_begin_write(lib) == MC_OK);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
}

static void add_album(mc_library *lib, const char *uid, const char *artist, int mix) {
  char sql[256];
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid, mix) VALUES ('%s', '%s', '%s', %d)",
           uid, uid, artist, mix);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
}

/* A track whose sound is one number: the further apart two are, the less alike they sound.
 * analysed 0 leaves it without a result, and has_file 0 without a file here. */
static void add_track(mc_library *lib, const char *uid, const char *title, const char *artist,
                      const char *album, int duration_ms, double sound, int analysed, int has_file) {
  char sql[512];
  CHECK(mc_library_begin_write(lib) == MC_OK);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, title, artist_uid, album_uid, duration_ms)"
           " VALUES ('%s', '%s', '%s', %s%s%s, %d)",
           uid, title, artist, album ? "'" : "", album ? album : "NULL", album ? "'" : "", duration_ms);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
  if (has_file) {
    char file[MC_UID_STRLEN];
    mc_uid_new(file);
    snprintf(sql, sizeof sql,
             "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms)"
             " VALUES ('%s', '%s', '%s.wav', 'wav', 1000, 2000)",
             file, uid, uid);
    exec_ok(lib, sql);
  }
  if (!analysed) return;
  mc_analysis a;
  memset(&a, 0, sizeof a);
  a.version = MC_ANALYSIS_VERSION;
  for (int i = 1; i < MC_ANALYSIS_MFCCS; i++) a.mfcc_mean[i] = (float)(sound * (i % 3 + 1));
  a.centroid_mean = (float)(1000.0 + 100.0 * sound);
  a.lufs = -14.0;
  a.lra = 6.0;
  a.true_peak = -1.0;
  CHECK(mc_analysis_store(lib, uid, NULL, MC_ANALYSIS_ORIGINAL, &a) == MC_OK);
}

static void played(mc_library *lib, const char *track, int64_t when_ms) {
  char sql[256], uid[MC_UID_STRLEN];
  mc_uid_new(uid);
  snprintf(sql, sizeof sql,
           "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms)"
           " VALUES ('%s', '%s', 'node', 1, %lld)",
           uid, track, (long long)when_ms);
  CHECK(mc_library_begin_write(lib) == MC_OK);
  exec_ok(lib, sql);
  CHECK(mc_library_commit(lib) == MC_OK);
}

static int has(const mc_radio_pick *picks, int n, const char *uid) {
  for (int i = 0; i < n; i++)
    if (!strcmp(picks[i].track_uid, uid)) return 1;
  return 0;
}

/* Two clusters, one artist and album per track in each. A mix stays in the seed's, and starts
 * with the seed. */
static void test_stays_near_the_seed(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_near.d");
  if (!lib) return;
  char id[32], artist[32], album[32];
  for (int i = 0; i < 40; i++) {
    snprintf(id, sizeof id, "%c%02d", i < 20 ? 'a' : 'b', i);
    snprintf(artist, sizeof artist, "artist%d", i);
    snprintf(album, sizeof album, "album%d", i);
    add_artist(lib, artist);
    add_album(lib, album, artist, 0);
    add_track(lib, id, id, artist, album, 200000, (i < 20 ? 0.0 : 50.0) + (i % 20) * 0.1, 1, 1);
  }
  mc_radio_pick picks[10];
  int n = 0;
  mc_radio_options opts = {"a03", 10, 7, NOW_MS};
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == 10);
  CHECK_STR(picks[0].track_uid, "a03");
  CHECK(picks[0].distance == 0.0);
  for (int i = 0; i < n; i++) CHECK(picks[i].track_uid[0] == 'a');
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) CHECK(strcmp(picks[i].track_uid, picks[j].track_uid));

  /* The same inputs give the same mix; another draw gives another. */
  mc_radio_pick again[10], other[10];
  int m = 0;
  CHECK(mc_radio_make(lib, &opts, again, &m) == MC_OK);
  CHECK(m == n);
  for (int i = 0; i < n; i++) CHECK_STR(again[i].track_uid, picks[i].track_uid);
  opts.random = 8;
  CHECK(mc_radio_make(lib, &opts, other, &m) == MC_OK);
  int differ = 0;
  for (int i = 0; i < n; i++) differ += strcmp(other[i].track_uid, picks[i].track_uid) != 0;
  CHECK(differ > 0);

  /* No seed given: one is drawn, and the mix is still full. */
  opts.seed_track = NULL;
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == 10);
  mc_library_close(lib);
}

/* Artists stay apart and albums don't repeat, while the library allows. */
static void test_keeps_apart(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_apart.d");
  if (!lib) return;
  char id[32], artist[32], album[32];
  for (int a = 0; a < 5; a++) {
    snprintf(artist, sizeof artist, "artist%d", a);
    add_artist(lib, artist);
    for (int b = 0; b < 2; b++) {
      snprintf(album, sizeof album, "album%d-%d", a, b);
      add_album(lib, album, artist, 0);
      for (int t = 0; t < 3; t++) {
        snprintf(id, sizeof id, "t%d-%d-%d", a, b, t);
        add_track(lib, id, id, artist, album, 200000, a * 0.01 + b * 0.001 + t * 0.0001, 1, 1);
      }
    }
  }
  mc_radio_pick picks[10];
  int n = 0;
  mc_radio_options opts = {"t0-0-0", 10, 3, NOW_MS};
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == 10);
  for (int i = 0; i < n; i++) {
    for (int j = i + 1; j < n; j++) {
      /* uids are t<artist>-<album>-<track> */
      CHECK(strncmp(picks[i].track_uid, picks[j].track_uid, 4) != 0); /* no album twice */
      if (j - i <= 3) CHECK(picks[i].track_uid[1] != picks[j].track_uid[1]);
    }
  }

  /* Ask for more than the rules can give: they give way, and tracks never repeat. */
  mc_radio_pick all[40];
  mc_radio_options everything = {"t0-0-0", 40, 3, NOW_MS};
  CHECK(mc_radio_make(lib, &everything, all, &n) == MC_OK);
  CHECK(n == 30);
  for (int i = 0; i < n; i++)
    for (int j = i + 1; j < n; j++) CHECK(strcmp(all[i].track_uid, all[j].track_uid));
  mc_library_close(lib);
}

/* What a mix leaves out, and one recording on two albums. */
static void test_leaves_out(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_out.d");
  if (!lib) return;
  add_artist(lib, "a1");
  add_artist(lib, "a2");
  add_album(lib, "album1", "a1", 0);
  add_album(lib, "album2", "a1", 0);
  add_album(lib, "djmix", "a2", 1);
  add_track(lib, "seed", "Seed", "a1", "album1", 200000, 0.0, 1, 1);
  add_track(lib, "good", "Good", "a2", NULL, 200000, 0.1, 1, 1);
  add_track(lib, "short", "Short", "a2", NULL, 30000, 0.1, 1, 1);
  add_track(lib, "nofile", "No File", "a2", NULL, 200000, 0.1, 1, 0);
  add_track(lib, "noresult", "No Result", "a2", NULL, 200000, 0.1, 0, 1);
  add_track(lib, "inmix", "In Mix", "a2", "djmix", 200000, 0.1, 1, 1);
  add_track(lib, "again", "SEED", "a1", "album2", 200000, 0.0, 1, 1);

  mc_radio_pick picks[10];
  int n = 0;
  mc_radio_options opts = {"seed", 10, 1, NOW_MS};
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == 2);
  CHECK(has(picks, n, "seed"));
  CHECK(has(picks, n, "good"));

  opts.seed_track = "nofile";
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_ERR_NOT_FOUND);
  opts.seed_track = "unheard-of";
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_ERR_NOT_FOUND);
  mc_library_close(lib);

  mc_library *empty = open_library(scratch, "test_radio_empty.d");
  if (!empty) return;
  mc_radio_options none = {NULL, 10, 1, NOW_MS};
  CHECK(mc_radio_make(empty, &none, picks, &n) == MC_ERR_NOT_FOUND);
  none.count = 0;
  CHECK(mc_radio_make(empty, &none, picks, &n) == MC_ERR_INVALID_ARG);
  mc_library_close(empty);
}

/* A track played just now gives way to one that sounds a little less alike. */
static void test_plays_down_what_played_lately(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_lately.d");
  if (!lib) return;
  char id[32], artist[32];
  add_artist(lib, "seedartist");
  add_track(lib, "seed", "Seed", "seedartist", NULL, 200000, 0.0, 1, 1);
  for (int i = 0; i < 12; i++) {
    snprintf(id, sizeof id, "t%02d", i);
    snprintf(artist, sizeof artist, "artist%d", i);
    add_artist(lib, artist);
    add_track(lib, id, id, artist, NULL, 200000, 0.5 + i * 2.0, 1, 1);
  }
  mc_radio_pick picks[2];
  int n = 0, nearest = 0, runs = 40;
  mc_radio_options opts = {"seed", 2, 0, NOW_MS};
  for (uint32_t r = 1; r <= (uint32_t)runs; r++) {
    opts.random = r;
    CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
    nearest += !strcmp(picks[1].track_uid, "t00");
  }
  CHECK(nearest > runs / 2);

  played(lib, "t00", NOW_MS - 1000);
  int after = 0;
  for (uint32_t r = 1; r <= (uint32_t)runs; r++) {
    opts.random = r;
    CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
    after += !strcmp(picks[1].track_uid, "t00");
  }
  CHECK(after < nearest);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_stays_near_the_seed(scratch);
  test_keeps_apart(scratch);
  test_leaves_out(scratch);
  test_plays_down_what_played_lately(scratch);
  return TEST_RESULT();
}
