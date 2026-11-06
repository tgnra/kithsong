/* Radio mixes and stations (musiccore/radio.h), from made-up analysis results, so no audio is
 * decoded. */
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
  mc_radio_options opts = {"a03", 10, 7, NOW_MS, 0};
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
  mc_radio_options opts = {"t0-0-0", 10, 3, NOW_MS, 0};
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
  mc_radio_options everything = {"t0-0-0", 40, 3, NOW_MS, 0};
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
  mc_radio_options opts = {"seed", 10, 1, NOW_MS, 0};
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
  mc_radio_options none = {NULL, 10, 1, NOW_MS, 0};
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
  mc_radio_options opts = {"seed", 2, 0, NOW_MS, 0};
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

/* Too long isn't a candidate, whoever asks; not knowing is no reason to leave a track out. */
static void test_max_length(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_length.d");
  if (!lib) return;
  const struct { const char *uid; int ms; } tracks[] = {
    {"seed", 200000}, {"eight", 480000}, {"over", 480001}, {"nine", 540000},
    {"zero", 0},      {"none", 200000},
  };
  int total = (int)(sizeof tracks / sizeof tracks[0]);
  for (int i = 0; i < total; i++) {
    char artist[32];
    snprintf(artist, sizeof artist, "artist-%s", tracks[i].uid);
    add_artist(lib, artist);
    add_track(lib, tracks[i].uid, tracks[i].uid, artist, NULL, tracks[i].ms, i * 0.1, 1, 1);
  }
  CHECK(mc_library_begin_write(lib) == MC_OK);
  exec_ok(lib, "UPDATE tracks SET duration_ms = NULL WHERE uid = 'none'");
  CHECK(mc_library_commit(lib) == MC_OK);

  /* 8 minutes unless it says, that long being fine. */
  mc_radio_pick picks[10];
  int n = 0;
  mc_radio_options opts = {"seed", 10, 1, NOW_MS, 0};
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == 4);
  CHECK(has(picks, n, "seed") && has(picks, n, "eight") && has(picks, n, "zero") && has(picks, n, "none"));

  opts.max_track_ms = 300000;
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == 3);
  CHECK(!has(picks, n, "eight"));
  CHECK(has(picks, n, "zero") && has(picks, n, "none"));

  opts.max_track_ms = -1;
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == total);

  /* A seed is held to it like any other track, a station's too. */
  opts.seed_track = "nine";
  opts.max_track_ms = 0;
  mc_radio_station *station = NULL;
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_ERR_NOT_FOUND);
  CHECK(mc_radio_seed_check(lib, &opts) == MC_ERR_NOT_FOUND);
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_ERR_NOT_FOUND);
  CHECK(!station);
  opts.max_track_ms = -1;
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(mc_radio_seed_check(lib, &opts) == MC_OK);
  opts.seed_track = "none";
  opts.max_track_ms = 0;
  CHECK(mc_radio_seed_check(lib, &opts) == MC_OK);
  opts.seed_track = "unheard-of";
  CHECK(mc_radio_seed_check(lib, &opts) == MC_ERR_NOT_FOUND);
  opts.seed_track = NULL;
  CHECK(mc_radio_seed_check(lib, &opts) == MC_ERR_INVALID_ARG);
  mc_library_close(lib);
}

/* Two clusters again. A station plays all of the seed's, then all of the other's, with no
 * repeats, and then goes on with tracks coming back. It reads the library once, so the library
 * can be closed by then. */
static void test_station_goes_on(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_station.d");
  if (!lib) return;
  char id[32], artist[32], album[32];
  enum { POOL = 40, RUN = 200 };
  for (int i = 0; i < POOL; i++) {
    snprintf(id, sizeof id, "%c%02d", i < POOL / 2 ? 'a' : 'b', i);
    snprintf(artist, sizeof artist, "artist%d", i);
    snprintf(album, sizeof album, "album%d", i);
    add_artist(lib, artist);
    add_album(lib, album, artist, 0);
    add_track(lib, id, id, artist, album, 200000, (i < POOL / 2 ? 0.0 : 50.0) + (i % 20) * 0.1, 1, 1);
  }
  mc_radio_options opts = {"a03", 0, 7, NOW_MS, 0};
  mc_radio_station *station = NULL, *again = NULL;
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_OK);
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &again) == MC_OK);
  char dir[1024];
  snprintf(dir, sizeof dir, "%s", mc_library_dir(lib));
  mc_library_close(lib);
  if (!station || !again) return;

  /* In batches of any size, as a player would ask. */
  static mc_radio_pick picks[RUN];
  for (int at = 0, size = 1; at < RUN; at += size, size = size % 7 + 1) {
    if (at + size > RUN) size = RUN - at;
    CHECK(mc_radio_station_next(station, picks + at, size) == MC_OK);
  }
  CHECK_STR(picks[0].track_uid, "a03");
  CHECK(picks[0].distance == 0.0);
  /* The two sounds are two kinds of music, and it never leaves the seed's. */
  for (int i = 0; i < RUN; i++) CHECK(picks[i].track_uid[0] == 'a');
  for (int i = 0; i < POOL / 2; i++)
    for (int j = i + 1; j < POOL / 2; j++) CHECK(strcmp(picks[i].track_uid, picks[j].track_uid));
  /* What comes back keeps away from when it last played: half a pool at the very least, as
   * the last round's latest stay out. */
  for (int i = 0; i < RUN; i++)
    for (int j = i + 1; j < RUN && j < i + POOL / 4; j++) CHECK(strcmp(picks[i].track_uid, picks[j].track_uid));
  for (int i = 0; i < RUN; i++) {
    char end[64];
    snprintf(end, sizeof end, "%s.wav", picks[i].track_uid);
    CHECK(picks[i].path && !strncmp(picks[i].path, dir, strlen(dir)));
    CHECK(picks[i].path && strstr(picks[i].path, end));
  }

  /* The same inputs give the same station, whatever the batches. */
  mc_radio_pick same_run[60];
  CHECK(mc_radio_station_next(again, same_run, 60) == MC_OK);
  for (int i = 0; i < 60; i++) CHECK_STR(same_run[i].track_uid, picks[i].track_uid);
  mc_radio_station_close(again);

  CHECK(mc_radio_station_next(station, picks, 0) == MC_ERR_INVALID_ARG);
  CHECK(mc_radio_station_next(NULL, picks, 1) == MC_ERR_INVALID_ARG);
  mc_radio_station_close(station);
  mc_radio_station_close(NULL);
}

/* The gaps hold on a station as in a mix, but an album only stays out for so long, or a small
 * library would soon lose the artist gap with it. */
static void test_station_keeps_apart(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_station_apart.d");
  if (!lib) return;
  char id[32], artist[32], album[32];
  for (int a = 0; a < 6; a++) {
    snprintf(artist, sizeof artist, "artist%d", a);
    add_artist(lib, artist);
    snprintf(album, sizeof album, "album%d", a);
    add_album(lib, album, artist, 0);
    for (int t = 0; t < 8; t++) {
      snprintf(id, sizeof id, "t%d-%d", a, t);
      add_track(lib, id, id, artist, album, 200000, a * 0.01 + t * 0.0001, 1, 1);
    }
  }
  mc_radio_options opts = {"t0-0", 0, 3, NOW_MS, 0};
  mc_radio_station *station = NULL;
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_OK);
  mc_library_close(lib);
  if (!station) return;
  mc_radio_pick picks[30];
  CHECK(mc_radio_station_next(station, picks, 30) == MC_OK);
  /* uids are t<artist>-<track>. Six albums, so they had to give way, and artists didn't. */
  for (int i = 0; i < 30; i++)
    for (int j = i + 1; j < 30 && j <= i + 3; j++) CHECK(picks[i].track_uid[1] != picks[j].track_uid[1]);
  mc_radio_station_close(station);
}

/* However little there is, it doesn't end: one track, and two copies of one recording. */
static void test_station_small(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_station_small.d");
  if (!lib) return;
  add_artist(lib, "a1");
  add_album(lib, "album1", "a1", 0);
  add_album(lib, "album2", "a1", 0);
  add_track(lib, "only", "Only", "a1", "album1", 200000, 0.0, 1, 1);
  mc_radio_options opts = {"only", 0, 1, NOW_MS, 0};
  mc_radio_station *station = NULL;
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_OK);
  mc_radio_pick picks[8];
  if (station) {
    CHECK(mc_radio_station_next(station, picks, 8) == MC_OK);
    for (int i = 0; i < 8; i++) CHECK_STR(picks[i].track_uid, "only");
    mc_radio_station_close(station);
  }
  add_track(lib, "copy", "ONLY", "a1", "album2", 200000, 0.0, 1, 1);
  station = NULL;
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_OK);
  if (station) {
    CHECK(mc_radio_station_next(station, picks, 8) == MC_OK);
    for (int i = 0; i < 8; i++) CHECK(!strcmp(picks[i].track_uid, "only") || !strcmp(picks[i].track_uid, "copy"));
    mc_radio_station_close(station);
  }
  mc_library_close(lib);
}

/* The penalty counts on a station, and for its own picks once they come round again. */
static void test_station_lately(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_station_lately.d");
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
  int before = 0, after = 0, runs = 40;
  mc_radio_options opts = {"seed", 0, 0, NOW_MS, 0};
  mc_radio_pick picks[2];
  for (int pass = 0; pass < 2; pass++) {
    for (uint32_t r = 1; r <= (uint32_t)runs; r++) {
      opts.random = r;
      mc_radio_station *station = NULL;
      CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_OK);
      if (!station) continue;
      CHECK(mc_radio_station_next(station, picks, 2) == MC_OK);
      *(pass ? &after : &before) += !strcmp(picks[1].track_uid, "t00");
      mc_radio_station_close(station);
    }
    if (!pass) played(lib, "t00", NOW_MS - 1000);
  }
  CHECK(before > runs / 2);
  CHECK(after < before);
  mc_library_close(lib);
}

static int asked;

static int give_up(void *user) {
  (void)user;
  asked++;
  return 1;
}

/* Reading the candidates can be stopped part way, which is how a player lets go of one. */
static void test_station_cancelled(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_radio_station_cancel.d");
  if (!lib) return;
  char id[32];
  add_artist(lib, "a1");
  for (int i = 0; i < 300; i++) {
    snprintf(id, sizeof id, "t%03d", i);
    add_track(lib, id, id, "a1", NULL, 200000, i * 0.1, 1, 1);
  }
  mc_radio_options opts = {NULL, 0, 1, NOW_MS, 0};
  mc_radio_station *station = NULL;
  CHECK(mc_radio_station_open(lib, &opts, give_up, NULL, &station) == MC_ERR_IN_USE);
  CHECK(!station);
  CHECK(asked == 1);
  CHECK(mc_radio_station_open(NULL, &opts, NULL, NULL, &station) == MC_ERR_INVALID_ARG);
  mc_library_close(lib);
}

/* --- Skips ---
 *
 * A seed between two groups that sound alike within and unlike each other, one each side of it
 * and about as far, so a mix goes either way until a skip says otherwise. A near seed sits by
 * it, by the same artist so neither is the other's first pick, and a far seed has a group of its
 * own, too unlike these for a skip here to matter. */
#define DAY_MS (24 * 3600 * 1000LL)
enum { SKIP_RUNS = 300, SKIP_MIX = 12, SIDE = 4 };

static const char *const k_ups[SIDE] = {"up0", "up1", "up2", "up3"};
static const char *const k_downs[SIDE] = {"down0", "down1", "down2", "down3"};

static mc_library *skip_library(const char *scratch, const char *name) {
  mc_library *lib = open_library(scratch, name);
  if (!lib) return NULL;
  char id[32], artist[32];
  int made = 0;
  const struct { const char *uid; double sound; } near[] = {{"seed", 30.0}, {"nearseed", 32.0}};
  for (int i = 0; i < 2 + 3 * SIDE; i++) {
    double sound;
    if (i < 2) {
      snprintf(id, sizeof id, "%s", near[i].uid);
      sound = near[i].sound;
    } else if (i < 2 + SIDE) {
      snprintf(id, sizeof id, "%s", k_ups[i - 2]);
      sound = 37.0 + (i - 2) * 0.01;
    } else if (i < 2 + 2 * SIDE) {
      snprintf(id, sizeof id, "%s", k_downs[i - 2 - SIDE]);
      sound = 23.0 - (i - 2 - SIDE) * 0.01;
    } else {
      snprintf(id, sizeof id, "far%d", i - 2 - 2 * SIDE);
      sound = 60.0 + (i - 2 - 2 * SIDE) * 0.5;
    }
    snprintf(artist, sizeof artist, "artist%d", i < 2 ? 0 : made);
    if (i != 1) add_artist(lib, artist);
    made++;
    add_track(lib, id, id, artist, NULL, 200000, sound, 1, 1);
  }
  return lib;
}

/* A row as the player would leave it. */
static void logged(mc_library *lib, const char *track, mc_play_end end, mc_play_context context,
                   const char *context_uid, int64_t started_ms, int64_t listened_ms) {
  mc_play_event e;
  memset(&e, 0, sizeof e);
  snprintf(e.track_uid, sizeof e.track_uid, "%s", track);
  e.started_ms = started_ms;
  e.listened_ms = listened_ms;
  e.end = end;
  e.context = context;
  snprintf(e.context_uid, sizeof e.context_uid, "%s", context_uid);
  CHECK(mc_library_log_play(lib, &e) == MC_OK);
}

static void radio_skip(mc_library *lib, const char *track, const char *seed, int64_t started_ms) {
  logged(lib, track, MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, seed, started_ms, 10000);
}

/* How many of a number of mixes from seed go first to one of the tracks, the rest of the mix
 * being no help: everything in reach gets into a mix of a few sooner or later. */
static int first_to(mc_library *lib, const char *seed, int64_t now_ms, const char *const *tracks, int n) {
  int hits = 0;
  mc_radio_options opts = {seed, 2, 0, now_ms, 0};
  for (uint32_t r = 1; r <= SKIP_RUNS; r++) {
    mc_radio_pick picks[2];
    int got = 0;
    opts.random = r;
    CHECK(mc_radio_make(lib, &opts, picks, &got) == MC_OK);
    CHECK(got == 2);
    for (int i = 0; i < n; i++) hits += !strcmp(picks[1].track_uid, tracks[i]);
  }
  return hits;
}

/* Every mix from a seed, as a fingerprint of what it picks. */
typedef struct mix_print {
  char uids[32][SKIP_MIX][MC_UID_STRLEN];
} mix_print;

static void mix_prints(mc_library *lib, const char *seed, int64_t now_ms, mix_print *out) {
  mc_radio_options opts = {seed, SKIP_MIX, 0, now_ms, 0};
  memset(out, 0, sizeof *out);
  for (uint32_t r = 0; r < 32; r++) {
    mc_radio_pick picks[SKIP_MIX];
    int got = 0;
    opts.random = r + 1;
    CHECK(mc_radio_make(lib, &opts, picks, &got) == MC_OK);
    CHECK(got == SKIP_MIX);
    for (int i = 0; i < got; i++) memcpy(out->uids[r][i], picks[i].track_uid, MC_UID_STRLEN);
  }
}

static int same_mixes(mc_library *lib, const char *seed, int64_t now_ms, const mix_print *before) {
  static mix_print now;
  mix_prints(lib, seed, now_ms, &now);
  return !memcmp(&now, before, sizeof now);
}

/* The fixture is what it says: on the scales a station uses, near what's near and past the
 * reach of what isn't. */
static void test_skip_fixture(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_fixture.d");
  if (!lib) return;
  mc_analysis_stats stats;
  CHECK(mc_analysis_stats_load(lib, &stats) == MC_OK);
  mc_analysis seed, nearseed, up, up_last, down, far;
  CHECK(mc_analysis_get(lib, "seed", &seed) == MC_OK);
  CHECK(mc_analysis_get(lib, "nearseed", &nearseed) == MC_OK);
  CHECK(mc_analysis_get(lib, "up0", &up) == MC_OK);
  CHECK(mc_analysis_get(lib, "up3", &up_last) == MC_OK);
  CHECK(mc_analysis_get(lib, "down0", &down) == MC_OK);
  CHECK(mc_analysis_get(lib, "far0", &far) == MC_OK);
  double reach = 0.6; /* SKIP_REACH */
  CHECK(mc_analysis_distance(&seed, &nearseed, &stats) < reach / 2);
  CHECK(mc_analysis_distance(&up, &up_last, &stats) < reach / 4);
  CHECK(mc_analysis_distance(&up, &down, &stats) > reach);
  CHECK(mc_analysis_distance(&seed, &far, &stats) > reach);
  CHECK(mc_analysis_distance(&nearseed, &far, &stats) > reach);
  mc_library_close(lib);
}

/* A skip lowers the track and what sounds like it on that station and those of seeds like it.
 * The other side gets what they lose. */
static void test_skip_lowers(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_lowers.d");
  if (!lib) return;
  static mix_print far_before;
  mix_prints(lib, "far0", NOW_MS, &far_before);
  const char *const *ups = k_ups, *const *downs = k_downs;
  const char *const skipped[] = {"up0"}, *const alike[] = {"up1", "up2", "up3"};
  int track = first_to(lib, "seed", NOW_MS, skipped, 1), others = first_to(lib, "seed", NOW_MS, alike, 3);
  int down = first_to(lib, "seed", NOW_MS, downs, SIDE);
  int near_up = first_to(lib, "nearseed", NOW_MS, ups, SIDE);
  CHECK(track > SKIP_RUNS / 16 && others > SKIP_RUNS / 8);

  radio_skip(lib, "up0", "seed", NOW_MS - 1000);
  int track_after = first_to(lib, "seed", NOW_MS, skipped, 1);
  int others_after = first_to(lib, "seed", NOW_MS, alike, 3);
  CHECK(track_after * 3 < track * 2);
  CHECK(others_after * 3 < others * 2);
  CHECK(first_to(lib, "seed", NOW_MS, downs, SIDE) > down);
  CHECK(first_to(lib, "nearseed", NOW_MS, ups, SIDE) < near_up); /* a seed like it, less so */
  CHECK(same_mixes(lib, "far0", NOW_MS, &far_before));
  mc_library_close(lib);
}

/* What doesn't teach anything leaves every pick as it was, to the last. */
static void test_skip_teaches_nothing(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_nothing.d");
  if (!lib) return;
  static mix_print before, near_before;
  mix_prints(lib, "seed", NOW_MS, &before);
  mix_prints(lib, "nearseed", NOW_MS, &near_before);

  /* Another context, the seed's uid or not. */
  const mc_play_context others[] = {MC_PLAY_CONTEXT_UNKNOWN, MC_PLAY_CONTEXT_MANUAL, MC_PLAY_CONTEXT_ALBUM,
                                    MC_PLAY_CONTEXT_PLAYLIST, MC_PLAY_CONTEXT_ALBUM_GROUP,
                                    MC_PLAY_CONTEXT_ARTIST, MC_PLAY_CONTEXT_SHUFFLE};
  for (size_t i = 0; i < sizeof others / sizeof others[0]; i++) {
    logged(lib, "up0", MC_PLAY_END_SKIPPED, others[i], "seed", NOW_MS - 1000, 10000);
    logged(lib, "up0", MC_PLAY_END_SKIPPED, others[i], "", NOW_MS - 1000, 10000);
  }
  CHECK(same_mixes(lib, "seed", NOW_MS, &before));

  /* Late, and on either side of it; too soon to have heard anything. */
  logged(lib, "up0", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000, 30000);
  logged(lib, "up0", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000, 190000);
  logged(lib, "up0", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000, 2999);
  logged(lib, "up0", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000, 0);
  CHECK(same_mixes(lib, "seed", NOW_MS, &before));

  /* Ended some other way: only someone moving on chose to. */
  const mc_play_end ends[] = {MC_PLAY_END_STOPPED, MC_PLAY_END_REPLACED, MC_PLAY_END_FAILED, MC_PLAY_END_FINISHED};
  for (size_t i = 0; i < sizeof ends / sizeof ends[0]; i++)
    logged(lib, "up0", ends[i], MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000, 10000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &before));

  /* A station's seed skipped says nothing about what's built on it, there or on a near seed,
   * nor does this seed skipped on a near seed's station say anything here. */
  radio_skip(lib, "seed", "seed", NOW_MS - 1000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &before));
  CHECK(same_mixes(lib, "nearseed", NOW_MS, &near_before));
  radio_skip(lib, "seed", "nearseed", NOW_MS - 1000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &before));

  /* A device with its clock ahead, and a seed the library no longer knows. */
  radio_skip(lib, "up0", "seed", NOW_MS + 1000);
  radio_skip(lib, "up0", "gone-since", NOW_MS - 1000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &before));

  /* With all that, the first real one tells. */
  radio_skip(lib, "up0", "seed", NOW_MS - 1000);
  CHECK(!same_mixes(lib, "seed", NOW_MS, &before));
  mc_library_close(lib);
}

/* One skip wears off, and is gone by 60 days. Fading is measured from opts.now_ms. */
static void test_skip_fades(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_fades.d");
  if (!lib) return;
  static mix_print before;
  int64_t late = NOW_MS + 60 * DAY_MS;
  mix_prints(lib, "seed", late, &before);
  const char *const *ups = k_ups;
  int untouched = first_to(lib, "seed", NOW_MS, ups, SIDE);
  radio_skip(lib, "up0", "seed", NOW_MS);
  int fresh = first_to(lib, "seed", NOW_MS, ups, SIDE);
  int month = first_to(lib, "seed", NOW_MS + 30 * DAY_MS, ups, SIDE);
  int nearly = first_to(lib, "seed", NOW_MS + 59 * DAY_MS, ups, SIDE);
  CHECK(fresh < month);
  CHECK(month < nearly);
  CHECK(nearly <= untouched);
  CHECK(untouched - nearly < (untouched - fresh) / 4);
  CHECK(same_mixes(lib, "seed", late, &before));

  /* What has faded has no say at all, not even against a skip that hasn't. */
  radio_skip(lib, "up0", "seed", late + 30 * DAY_MS);
  int alone = first_to(lib, "seed", late + 30 * DAY_MS, ups, SIDE);
  CHECK(alone == fresh);
  mc_library_close(lib);
}

/* Skips of a pairing add up, to a limit, and finishing it takes one back. */
static void test_skip_repeats(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_repeats.d");
  if (!lib) return;
  const char *const *ups = k_ups;
  static mix_print none;
  mix_prints(lib, "seed", NOW_MS, &none);
  int counts[4];
  counts[0] = first_to(lib, "seed", NOW_MS, ups, SIDE);
  for (int i = 1; i <= 3; i++) {
    radio_skip(lib, "up0", "seed", NOW_MS - 3000000 + i * 100000);
    counts[i] = first_to(lib, "seed", NOW_MS, ups, SIDE);
    CHECK(counts[i] < counts[i - 1]);
  }
  for (int i = 0; i < 5; i++) radio_skip(lib, "up0", "seed", NOW_MS - 2000000 + i * 1000);
  /* Those are a little fresher, but a pairing is already as strong as it gets. */
  CHECK(first_to(lib, "seed", NOW_MS, ups, SIDE) == counts[3]);
  CHECK(counts[3] > 0); /* still in the running */

  /* However many skips reach a track, it goes down no further. */
  const char *const rest[] = {"up1", "up2", "up3"};
  for (int i = 0; i < 3; i++)
    for (int k = 0; k < 3; k++) radio_skip(lib, rest[i], "seed", NOW_MS - 1000 - k);
  CHECK(first_to(lib, "seed", NOW_MS, ups, SIDE) * 2 > counts[3]);
  mc_library_close(lib);

  /* Heard out after a skip: the skip is off. Before one, or got to by seeking: it's not. */
  lib = skip_library(scratch, "test_radio_skip_offset.d");
  if (!lib) return;
  radio_skip(lib, "up0", "seed", NOW_MS - 2000000);
  logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000000, 200000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &none));
  logged(lib, "up0", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 900000, 10000);
  logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 800000, 5000);
  CHECK(!same_mixes(lib, "seed", NOW_MS, &none));
  logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_ALBUM, "seed", NOW_MS - 700000, 200000);
  logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO, "nearseed", NOW_MS - 700000, 200000);
  CHECK(!same_mixes(lib, "seed", NOW_MS, &none)); /* that pairing's own finish only */
  logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 600000, 200000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &none));

  /* A pairing holds no more than three, so three finishes clear it however many there were. */
  for (int i = 0; i < 8; i++) radio_skip(lib, "up0", "seed", NOW_MS - 500000 + i);
  for (int i = 0; i < 2; i++)
    logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 400000 + i, 200000);
  CHECK(!same_mixes(lib, "seed", NOW_MS, &none));
  logged(lib, "up0", MC_PLAY_END_FINISHED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 300000, 200000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &none));
  mc_library_close(lib);
}

/* A track started again, or gone back to, wasn't skipped, however the play after it ended. The
 * same track coming round later on is another matter. */
static void test_skip_started_again(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_again.d");
  if (!lib) return;
  static mix_print none;
  mix_prints(lib, "seed", NOW_MS, &none);

  /* Previous 10 seconds in, and that play then stopped, replaced, skipped late, or skipped too
   * soon to say anything. */
  const mc_play_end ends[] = {MC_PLAY_END_STOPPED, MC_PLAY_END_REPLACED, MC_PLAY_END_SKIPPED};
  int64_t at = NOW_MS - 2000000;
  for (size_t i = 0; i < sizeof ends / sizeof ends[0]; i++, at += 100000) {
    radio_skip(lib, "up0", "seed", at);
    logged(lib, "up0", ends[i], MC_PLAY_CONTEXT_RADIO, "seed", at + 10000 + 200, 40000);
    CHECK(same_mixes(lib, "seed", NOW_MS, &none));
  }
  radio_skip(lib, "up0", "seed", at);
  logged(lib, "up0", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", at + 10000 + 200, 500);
  CHECK(same_mixes(lib, "seed", NOW_MS, &none));
  at += 100000;

  /* Next, a couple of seconds of what came after, and Previous back to it. */
  radio_skip(lib, "up0", "seed", at);
  logged(lib, "up1", MC_PLAY_END_SKIPPED, MC_PLAY_CONTEXT_RADIO, "seed", at + 10000, 2000);
  logged(lib, "up0", MC_PLAY_END_STOPPED, MC_PLAY_CONTEXT_RADIO, "seed", at + 12000 + 200, 40000);
  CHECK(same_mixes(lib, "seed", NOW_MS, &none));
  at += 100000;

  /* Started again, and then skipped for real: that one counts. */
  radio_skip(lib, "up0", "seed", at);
  radio_skip(lib, "up0", "seed", at + 10000 + 200);
  CHECK(!same_mixes(lib, "seed", NOW_MS, &none));
  mc_library_close(lib);

  /* Back round minutes later, it was a skip all along. */
  lib = skip_library(scratch, "test_radio_skip_again_later.d");
  if (!lib) return;
  radio_skip(lib, "up0", "seed", NOW_MS - 2000000);
  logged(lib, "up0", MC_PLAY_END_STOPPED, MC_PLAY_CONTEXT_RADIO, "seed", NOW_MS - 1000000, 40000);
  CHECK(!same_mixes(lib, "seed", NOW_MS, &none));
  mc_library_close(lib);
}

/* However much is skipped, a mix still takes everything and a station still plays everything,
 * the skipped included. */
static void test_skip_excludes_nothing(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_excludes.d");
  if (!lib) return;
  enum { ALL = 2 + 3 * SIDE };
  const char *uids[ALL];
  char far[SIDE][8];
  uids[0] = "seed";
  uids[1] = "nearseed";
  for (int i = 0; i < SIDE; i++) {
    snprintf(far[i], sizeof far[i], "far%d", i);
    uids[2 + i] = k_ups[i];
    uids[2 + SIDE + i] = k_downs[i];
    uids[2 + 2 * SIDE + i] = far[i];
  }
  for (int i = 1; i < ALL; i++)
    for (int k = 0; k < 6; k++) radio_skip(lib, uids[i], "seed", NOW_MS - 1000 - k);
  mc_radio_options opts = {"seed", ALL, 5, NOW_MS, 0};
  mc_radio_pick picks[ALL];
  int n = 0;
  CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
  CHECK(n == ALL);
  for (int i = 0; i < ALL; i++) CHECK(has(picks, n, uids[i]));
  mc_radio_station *station = NULL;
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &station) == MC_OK);
  if (station) {
    CHECK(mc_radio_station_next(station, picks, ALL) == MC_OK);
    CHECK_STR(picks[0].track_uid, "seed");
    for (int i = 0; i < ALL; i++) CHECK(has(picks, ALL, uids[i]));
    mc_radio_station_close(station);
  }
  mc_library_close(lib);
}

/* A track with no file here isn't a candidate, but says what sounds like it all the same. */
static void test_skip_elsewhere(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_elsewhere.d");
  if (!lib) return;
  const char *const *ups = k_ups;
  int before = first_to(lib, "seed", NOW_MS, ups, SIDE);
  add_artist(lib, "phone");
  add_track(lib, "phoneonly", "phoneonly", "phone", NULL, 200000, 37.0, 1, 0);
  radio_skip(lib, "phoneonly", "seed", NOW_MS - 1000);
  CHECK(first_to(lib, "seed", NOW_MS, ups, SIDE) < before);
  mc_library_close(lib);
}

static void station_picks(mc_radio_station *station, mc_radio_pick *picks, int n) {
  if (station) CHECK(mc_radio_station_next(station, picks, n) == MC_OK);
}

/* Told of a skip, a station goes by it from the next pick, just as it would if it had been in
 * the log on opening. Only early skips of that station count. */
static void test_skip_in_session(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_session.d");
  if (!lib) return;
  mc_radio_options opts = {"seed", 0, 3, NOW_MS, 0};
  mc_radio_station *plain = NULL, *ignored = NULL, *told = NULL, *read_it = NULL;
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &plain) == MC_OK);
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &ignored) == MC_OK);
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &told) == MC_OK);
  radio_skip(lib, "up0", "seed", NOW_MS);
  CHECK(mc_radio_station_open(lib, &opts, NULL, NULL, &read_it) == MC_OK);
  mc_library_close(lib);
  if (!plain || !ignored || !told || !read_it) return;

  mc_play_event e;
  memset(&e, 0, sizeof e);
  strcpy(e.track_uid, "up0");
  strcpy(e.context_uid, "seed");
  e.context = MC_PLAY_CONTEXT_RADIO;
  e.end = MC_PLAY_END_SKIPPED;
  e.listened_ms = 10000;
  CHECK(mc_radio_station_heard(told, &e) == MC_OK);

  mc_play_event no = e;
  no.context = MC_PLAY_CONTEXT_ALBUM;
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  no = e;
  no.listened_ms = 30000;
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  no = e;
  no.listened_ms = 2999;
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  no = e;
  no.end = MC_PLAY_END_REPLACED;
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  no = e;
  strcpy(no.context_uid, "nearseed");
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  no = e;
  strcpy(no.track_uid, "seed");
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  no = e;
  strcpy(no.track_uid, "unheard-of");
  CHECK(mc_radio_station_heard(ignored, &no) == MC_OK);
  CHECK(mc_radio_station_heard(NULL, &e) == MC_ERR_INVALID_ARG);
  CHECK(mc_radio_station_heard(told, NULL) == MC_ERR_INVALID_ARG);

  enum { RUN = 14 };
  mc_radio_pick a[RUN], b[RUN], c[RUN], d[RUN];
  station_picks(plain, a, RUN);
  station_picks(ignored, b, RUN);
  station_picks(told, c, RUN);
  station_picks(read_it, d, RUN);
  int differ = 0;
  for (int i = 0; i < RUN; i++) {
    CHECK_STR(a[i].track_uid, b[i].track_uid);
    CHECK_STR(c[i].track_uid, d[i].track_uid);
    differ += strcmp(a[i].track_uid, c[i].track_uid) != 0;
  }
  CHECK(differ > 0);
  mc_radio_station_close(plain);
  mc_radio_station_close(ignored);
  mc_radio_station_close(told);
  mc_radio_station_close(read_it);
}

static int calls_left;

static int give_up_soon(void *user) {
  (void)user;
  return calls_left-- <= 0;
}

/* Reading the log can be stopped part way, like reading the candidates. */
static void test_skip_cancelled(const char *scratch) {
  mc_library *lib = skip_library(scratch, "test_radio_skip_cancel.d");
  if (!lib) return;
  for (int i = 0; i < 300; i++) radio_skip(lib, "up0", "seed", NOW_MS - 1000 - i);
  mc_radio_options opts = {"seed", 0, 1, NOW_MS, 0};
  mc_radio_station *station = NULL;
  calls_left = 0;
  CHECK(mc_radio_station_open(lib, &opts, give_up_soon, NULL, &station) == MC_ERR_IN_USE);
  CHECK(!station);
  calls_left = 1; /* the rows, then on to comparing */
  CHECK(mc_radio_station_open(lib, &opts, give_up_soon, NULL, &station) == MC_ERR_IN_USE);
  CHECK(!station);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_stays_near_the_seed(scratch);
  test_keeps_apart(scratch);
  test_leaves_out(scratch);
  test_plays_down_what_played_lately(scratch);
  test_max_length(scratch);
  test_station_goes_on(scratch);
  test_station_keeps_apart(scratch);
  test_station_small(scratch);
  test_station_lately(scratch);
  test_station_cancelled(scratch);
  test_skip_fixture(scratch);
  test_skip_lowers(scratch);
  test_skip_teaches_nothing(scratch);
  test_skip_fades(scratch);
  test_skip_repeats(scratch);
  test_skip_started_again(scratch);
  test_skip_excludes_nothing(scratch);
  test_skip_elsewhere(scratch);
  test_skip_in_session(scratch);
  test_skip_cancelled(scratch);
  return TEST_RESULT();
}
