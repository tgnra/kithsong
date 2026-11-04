/* The radio stations a library offers (musiccore/radio_stations.h), and the Home row they fill,
 * from made-up analysis results, so no audio is decoded. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "analysis/analysis_internal.h"
#include "library/library_internal.h"
#include "musiccore/analysis.h"
#include "musiccore/home_picks.h"
#include "musiccore/musiccore.h"
#include "musiccore/radio.h"
#include "musiccore/radio_stations.h"
#include "test.h"
#include "util/platform.h"
#include "util/uid.h"

#define DAY_MS (24 * 60 * 60 * 1000LL)
#define TRACK_MS 200000

static void exec_ok(mc_library *lib, const char *sql) {
  char *err = NULL;
  if (sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
    test_failures++;
  }
  sqlite3_free(err);
}

static void write_sql(mc_library *lib, const char *sql) {
  CHECK(mc_library_begin_write(lib) == MC_OK);
  exec_ok(lib, sql);
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

/* Noon today, local: hours either side of it are the same day wherever the test runs. */
static int64_t noon_ms(void) {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  tm->tm_hour = 12;
  tm->tm_min = tm->tm_sec = 0;
  tm->tm_isdst = -1;
  return (int64_t)mktime(tm) * 1000;
}

static void add_artist(mc_library *lib, const char *uid) {
  char sql[256];
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('%s', '%s')", uid, uid);
  write_sql(lib, sql);
}

static void add_album(mc_library *lib, const char *uid, const char *artist, int mix) {
  char sql[256];
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title, artist_uid, mix, art_hash) VALUES ('%s', '%s', '%s', %d, 'h')",
           uid, uid, artist, mix);
  write_sql(lib, sql);
}

/* What a track of a library sounds like: sound apart on the fixed scales' terms, so sounds 2
 * apart are well over what makes two stations, with variant a small difference inside one. */
static void result_of(double sound, int variant, mc_analysis *a) {
  static const float spread[MC_ANALYSIS_MFCCS] = {0, 25, 12, 8, 6, 5, 4, 4, 3, 3, 3, 3, 3};
  memset(a, 0, sizeof *a);
  a->version = MC_ANALYSIS_VERSION;
  for (int i = 1; i < MC_ANALYSIS_MFCCS; i++)
    a->mfcc_mean[i] = (float)sound * spread[i] + (float)((variant * (i + 3)) % 11) * 0.02f * spread[i];
  a->centroid_mean = 1000.0f;
  a->lufs = -14.0;
  a->lra = 6.0;
  a->true_peak = -1.0;
}

static void add_track(mc_library *lib, const char *uid, const char *title, const char *artist, const char *album,
                      int duration_ms, double sound, int variant, int analysed, int has_file) {
  char sql[512], file[MC_UID_STRLEN];
  snprintf(sql, sizeof sql, "INSERT INTO tracks (uid, title, artist_uid, album_uid, duration_ms) VALUES ('%s', '%s', %s%s%s, '%s', %d)",
           uid, title, artist ? "'" : "", artist ? artist : "NULL", artist ? "'" : "", album, duration_ms);
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
  result_of(sound, variant, &a);
  CHECK(mc_analysis_store(lib, uid, NULL, MC_ANALYSIS_ORIGINAL, &a) == MC_OK);
}

/* clusters sounds, each of per tracks by an artist of its own: c0-3 is the fourth of the first. */
static void add_clusters(mc_library *lib, int clusters, int per) {
  char artist[32], album[32], uid[32];
  for (int c = 0; c < clusters; c++) {
    snprintf(artist, sizeof artist, "Artist%d", c);
    snprintf(album, sizeof album, "album%d", c);
    add_artist(lib, artist);
    add_album(lib, album, artist, 0);
    for (int i = 0; i < per; i++) {
      snprintf(uid, sizeof uid, "c%d-%d", c, i);
      add_track(lib, uid, uid, artist, album, TRACK_MS, c * 2.0, i, 1, 1);
    }
  }
}

static void played(mc_library *lib, const char *track, int plays, int64_t when_ms) {
  char sql[256], uid[MC_UID_STRLEN];
  mc_uid_new(uid);
  snprintf(sql, sizeof sql, "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES ('%s', '%s', 'n', %d, %lld)",
           uid, track, plays, (long long)when_ms);
  write_sql(lib, sql);
  for (int i = 0; i < plays; i++) {
    mc_uid_new(uid);
    snprintf(sql, sizeof sql,
             "INSERT INTO play_events (uid, track_uid, node_uid, started_ms, listened_ms, ended, context)"
             " VALUES ('%s', '%s', 'n', %lld, 180000, 'finished', 'album')",
             uid, track, (long long)when_ms);
    write_sql(lib, sql);
  }
}

static mc_radio_station_list *stations(mc_library *lib, int64_t now_ms) {
  mc_radio_station_list *list = NULL;
  CHECK(mc_radio_stations_list(lib, now_ms, &list) == MC_OK);
  return list;
}

static int same_seeds(const mc_radio_station_list *a, const mc_radio_station_list *b) {
  if (a->count != b->count) return 0;
  for (int i = 0; i < a->count; i++)
    if (strcmp(a->items[i].seed_track, b->items[i].seed_track)) return 0;
  return 1;
}

static void test_limit(void) {
  CHECK(mc_radio_station_limit(0) == 0);
  CHECK(mc_radio_station_limit(24) == 0);
  CHECK(mc_radio_station_limit(25) == 1);
  CHECK(mc_radio_station_limit(49) == 1);
  CHECK(mc_radio_station_limit(50) == 2);
  CHECK(mc_radio_station_limit(100) == 3);
  CHECK(mc_radio_station_limit(799) == 5);
  CHECK(mc_radio_station_limit(800) == 6);
  CHECK(mc_radio_station_limit(3199) == 7);
  CHECK(mc_radio_station_limit(3200) == MC_RADIO_STATIONS_MAX);
  CHECK(mc_radio_station_limit(1000000) == MC_RADIO_STATIONS_MAX);
}

static void test_arguments(void) {
  mc_radio_station_list *list = NULL;
  CHECK(mc_radio_stations_list(NULL, 0, &list) == MC_ERR_INVALID_ARG);
  mc_radio_station_list_free(NULL);
}

/* One seed in each sound, the seeds usable by a mix, and no more stations than its size allows. */
static void test_distinct(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_distinct.d");
  if (!lib) return;
  add_clusters(lib, 4, 50);
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 4);
  int seen[4] = {0};
  for (int i = 0; list && i < list->count; i++) {
    const mc_radio_station_info *s = &list->items[i];
    CHECK(s->seed_track[0] == 'c' && s->seed_track[1] >= '0' && s->seed_track[1] <= '3');
    seen[s->seed_track[1] - '0']++;
    char name[64];
    snprintf(name, sizeof name, "Artist%c Radio", s->seed_track[1]);
    CHECK(!strcmp(s->name, name));
    CHECK_STR(s->sub, "Station");
    CHECK_STR(s->seed_title, s->seed_track);
    CHECK(s->cover_dir && strstr(s->cover_dir, "album"));
    mc_radio_pick picks[5];
    int n = 0;
    mc_radio_options opts = {s->seed_track, 5, 1, 0};
    CHECK(mc_radio_make(lib, &opts, picks, &n) == MC_OK);
    CHECK(n > 0);
  }
  for (int c = 0; c < 4; c++) CHECK(seen[c] == 1);
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* The size allows four, but there are two sounds to tell apart, or one. */
static void test_alike(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_two.d");
  if (lib) {
    add_clusters(lib, 2, 100);
    mc_radio_station_list *list = stations(lib, noon_ms());
    CHECK(list && list->count == 2);
    mc_radio_station_list_free(list);
    mc_library_close(lib);
  }
  lib = open_library(scratch, "test_stations_one.d");
  if (lib) {
    add_clusters(lib, 1, 200);
    mc_radio_station_list *list = stations(lib, noon_ms());
    CHECK(list && list->count == 1);
    mc_radio_station_list_free(list);
    mc_library_close(lib);
  }
}

/* None until there's enough to go on, then one. */
static void test_too_few(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_few.d");
  if (!lib) return;
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 0);
  mc_radio_station_list_free(list);

  add_clusters(lib, 1, MC_RADIO_STATION_MIN_TRACKS - 1);
  list = stations(lib, noon_ms());
  CHECK(list && list->count == 0);
  mc_radio_station_list_free(list);

  add_track(lib, "last", "last", "Artist0", "album0", TRACK_MS, 0, 99, 1, 1);
  list = stations(lib, noon_ms());
  CHECK(list && list->count == 1);
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* Each of these would make the library big enough, and stands apart enough to be chosen. */
static void test_eligibility(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_rules.d");
  if (!lib) return;
  add_clusters(lib, 1, MC_RADIO_STATION_MIN_TRACKS - 1);
  add_artist(lib, "Other");
  add_album(lib, "other", "Other", 0);
  add_album(lib, "mixes", "Other", 1);
  add_album(lib, "more", "Other", 0);
  add_track(lib, "short", "short", "Other", "other", 59999, 4, 0, 1, 1);
  add_track(lib, "long", "long", "Other", "other", 480001, 4, 1, 1, 1);
  add_track(lib, "mixed", "mixed", "Other", "mixes", TRACK_MS, 4, 2, 1, 1);
  add_track(lib, "nofile", "nofile", "Other", "other", TRACK_MS, 4, 3, 1, 0);
  add_track(lib, "noresult", "noresult", "Other", "other", TRACK_MS, 4, 4, 0, 1);
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 0);
  mc_radio_station_list_free(list);

  /* Exactly the longest and shortest are allowed. */
  add_track(lib, "eight", "eight", "Other", "more", 480000, 0, 50, 1, 1);
  list = stations(lib, noon_ms());
  CHECK(list && list->count == 1);
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* Far from everything is what farthest-point selection likes, and no use for a station. */
static void test_lonely(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_lonely.d");
  if (!lib) return;
  add_clusters(lib, 3, 40);
  add_artist(lib, "Noise");
  add_album(lib, "noise", "Noise", 0);
  add_track(lib, "stray1", "stray1", "Noise", "noise", TRACK_MS, 30, 0, 1, 1);
  add_track(lib, "stray2", "stray2", "Noise", "noise", TRACK_MS, -30, 0, 1, 1);
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count >= 1);
  for (int i = 0; list && i < list->count; i++) CHECK(strncmp(list->items[i].seed_track, "stray", 5));
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* The same all day, plays of today included; then a seed gives way to what's been played. */
static void test_stable_and_played(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_stable.d");
  if (!lib) return;
  add_clusters(lib, 4, 50);
  int64_t noon = noon_ms();
  mc_radio_station_list *first = stations(lib, noon);
  mc_radio_station_list *later = stations(lib, noon + 3 * 3600 * 1000LL);
  mc_radio_station_list *earlier = stations(lib, noon - 3 * 3600 * 1000LL);
  CHECK(first && later && earlier && first->count == 4);
  CHECK(same_seeds(first, later) && same_seeds(first, earlier));
  mc_radio_station_list_free(later);
  mc_radio_station_list_free(earlier);

  /* A track of the first cluster that isn't its seed. */
  char pick[32] = "";
  for (int i = 0; i < 50 && !pick[0]; i++) {
    char uid[32];
    snprintf(uid, sizeof uid, "c0-%d", i);
    int seed = 0;
    for (int k = 0; first && k < first->count; k++) seed |= !strcmp(first->items[k].seed_track, uid);
    if (!seed) strcpy(pick, uid);
  }
  played(lib, pick, 20, noon - 3600 * 1000LL);
  later = stations(lib, noon);
  CHECK(later && same_seeds(first, later));
  mc_radio_station_list_free(later);

  /* Tomorrow those plays are history. */
  mc_radio_station_list *tomorrow = stations(lib, noon + DAY_MS);
  CHECK(tomorrow && tomorrow->count == 4);
  if (tomorrow && tomorrow->count) CHECK(!strcmp(tomorrow->items[0].seed_track, pick)); /* the most played leads */
  mc_radio_station_list_free(tomorrow);
  mc_radio_station_list_free(first);
  mc_library_close(lib);
}

/* Two clear sounds in this library's terms, but too near each other to be told apart by anyone
 * else's: settling can't reach across, so it's the spread limit that has to stop the second. */
static void test_too_close(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_close.d");
  if (!lib) return;
  add_artist(lib, "Near");
  add_album(lib, "near", "Near", 0);
  char uid[32];
  for (int i = 0; i < 100; i++) {
    snprintf(uid, sizeof uid, "n%d", i);
    add_track(lib, uid, uid, "Near", "near", TRACK_MS, i % 2 ? 0.8 : 0.0, i / 2 % 3, 1, 1);
  }
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 1);
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* Spreading out only starts from what's played: the other seeds get there by settling. */
static void test_settles_on_played(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_settle.d");
  if (!lib) return;
  add_clusters(lib, 3, 40);
  int64_t yesterday = noon_ms() - DAY_MS;
  played(lib, "c0-7", 8, yesterday);
  played(lib, "c1-11", 4, yesterday);
  played(lib, "c2-23", 2, yesterday);
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 3);
  if (list && list->count == 3) { /* the most played first */
    CHECK(!strcmp(list->items[0].seed_track, "c0-7"));
    CHECK(!strcmp(list->items[1].seed_track, "c1-11"));
    CHECK(!strcmp(list->items[2].seed_track, "c2-23"));
  }
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* A name of its own beats plays: the second sound is nearly all the first's artist, and a played
 * track of theirs loses to an unplayed one by someone else. */
static void test_settles_on_new_artist(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_artist.d");
  if (!lib) return;
  add_artist(lib, "Main");
  add_artist(lib, "Guest");
  add_album(lib, "main", "Main", 0);
  add_album(lib, "guest", "Guest", 0);
  char uid[32];
  for (int i = 0; i < 60; i++) {
    snprintf(uid, sizeof uid, "a%d", i);
    add_track(lib, uid, uid, "Main", "main", TRACK_MS, 0, i, 1, 1);
    snprintf(uid, sizeof uid, "b%d", i);
    add_track(lib, uid, uid, i == 30 ? "Guest" : "Main", i == 30 ? "guest" : "main", TRACK_MS, 2, i, 1, 1);
  }
  int64_t yesterday = noon_ms() - DAY_MS;
  played(lib, "a3", 9, yesterday);
  played(lib, "b5", 5, yesterday);
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 2);
  if (list && list->count == 2) {
    CHECK(!strcmp(list->items[0].seed_track, "a3"));
    CHECK(!strcmp(list->items[1].seed_track, "b30"));
    CHECK(!strcmp(list->items[1].name, "Guest Radio"));
    CHECK(!strcmp(list->items[1].sub, "Station"));
  }
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* A library not played much moves on from day to day. */
static void test_days_differ(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_days.d");
  if (!lib) return;
  add_clusters(lib, 4, 50);
  int64_t noon = noon_ms();
  mc_radio_station_list *today = stations(lib, noon);
  int moved = 0;
  for (int d = 1; d <= 5; d++) {
    mc_radio_station_list *other = stations(lib, noon + d * DAY_MS);
    moved += other && today && !same_seeds(today, other);
    mc_radio_station_list_free(other);
  }
  CHECK(moved > 0);
  mc_radio_station_list_free(today);
  mc_library_close(lib);
}

/* One artist in several sounds: one name, told apart by the seeds' titles, then their albums,
 * and a station that reads just as another does goes. */
static void test_same_name(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_names.d");
  if (!lib) return;
  add_artist(lib, "Solo");
  char uid[32], album[32];
  for (int c = 0; c < 3; c++) {
    snprintf(album, sizeof album, "album%d", c);
    add_album(lib, album, "Solo", 0);
    for (int i = 0; i < 40; i++) {
      snprintf(uid, sizeof uid, "s%d-%d", c, i);
      add_track(lib, uid, uid, "Solo", album, TRACK_MS, c * 2.0, i, 1, 1);
    }
  }
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 3);
  for (int i = 0; list && i < list->count; i++) {
    const mc_radio_station_info *s = &list->items[i];
    char sub[64];
    snprintf(sub, sizeof sub, "Like %s", s->seed_title);
    CHECK_STR(s->name, "Solo Radio");
    CHECK(!strcmp(s->sub, sub));
  }
  mc_radio_station_list_free(list);

  /* Every track of an album titled alike, so only the albums tell the seeds apart. */
  write_sql(lib, "UPDATE tracks SET title = 'Same'");
  list = stations(lib, noon_ms());
  CHECK(list && list->count == 3);
  for (int i = 0; list && i < list->count; i++) {
    CHECK(!strncmp(list->items[i].sub, "Like Same, album", 16));
    for (int k = 0; k < i; k++) CHECK(strcmp(list->items[i].sub, list->items[k].sub));
  }
  mc_radio_station_list_free(list);

  /* And with one album as well there's nothing left to tell them by. */
  write_sql(lib, "UPDATE albums SET title = 'One'");
  list = stations(lib, noon_ms());
  CHECK(list && list->count == 1);
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* A seed with no artist is named after itself, and has its own name to keep. */
static void test_no_artist(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_noartist.d");
  if (!lib) return;
  char sql[128], uid[32];
  snprintf(sql, sizeof sql, "INSERT INTO albums (uid, title) VALUES ('loose', 'Loose')");
  write_sql(lib, sql);
  for (int i = 0; i < 30; i++) {
    snprintf(uid, sizeof uid, "n%d", i);
    add_track(lib, uid, uid, NULL, "loose", TRACK_MS, 0, i, 1, 1);
  }
  mc_radio_station_list *list = stations(lib, noon_ms());
  CHECK(list && list->count == 1);
  if (list && list->count) {
    char name[64];
    snprintf(name, sizeof name, "%s Radio", list->items[0].seed_title);
    CHECK(!strcmp(list->items[0].name, name));
    CHECK(!list->items[0].artist_uid[0]);
    CHECK(!list->items[0].cover_dir);
  }
  mc_radio_station_list_free(list);
  mc_library_close(lib);
}

/* What the Home row makes of them. */
static void test_home_row(const char *scratch) {
  mc_library *lib = open_library(scratch, "test_stations_home.d");
  if (!lib) return;
  add_clusters(lib, 3, 40);
  mc_home_picks a, b;
  mc_home_picks_load(lib, 1, &a);
  mc_home_picks_load(lib, 2, &b); /* Random's seed has nothing to do with it */
  CHECK(a.counts[MC_HOME_ROW_RADIO] == 3 && b.counts[MC_HOME_ROW_RADIO] == 3);
  for (int i = 0; i < a.counts[MC_HOME_ROW_RADIO]; i++) {
    const mc_home_pick *p = &a.rows[MC_HOME_ROW_RADIO][i];
    CHECK(p->station && p->kind == MC_RECENT_ARTIST && !p->smart);
    CHECK(p->seed_uid[0] == 'c');
    CHECK(!strncmp(p->uid, "Artist", 6));
    CHECK(p->title && strstr(p->title, " Radio"));
    CHECK_STR(p->sub, "Station");
    CHECK_STR(p->seed_title, p->seed_uid);
    CHECK(p->cover_dir != NULL);
    CHECK_STR(p->seed_uid, b.rows[MC_HOME_ROW_RADIO][i].seed_uid);
  }
  for (int i = 0; i < a.counts[MC_HOME_ROW_RANDOM]; i++) {
    CHECK(!a.rows[MC_HOME_ROW_RANDOM][i].seed_uid[0] && !a.rows[MC_HOME_ROW_RANDOM][i].seed_title);
  }
  mc_home_picks_free(&a);
  mc_home_picks_free(&b);
  mc_library_close(lib);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  test_limit();
  test_arguments();
  test_distinct(scratch);
  test_alike(scratch);
  test_too_close(scratch);
  test_too_few(scratch);
  test_eligibility(scratch);
  test_lonely(scratch);
  test_stable_and_played(scratch);
  test_settles_on_played(scratch);
  test_settles_on_new_artist(scratch);
  test_days_differ(scratch);
  test_same_name(scratch);
  test_no_artist(scratch);
  test_home_row(scratch);
  mc_shutdown();
  return TEST_RESULT();
}
