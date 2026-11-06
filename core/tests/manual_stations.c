/* Prints the radio stations a library offers (musiccore/radio_stations.h) and how long they took,
 * for judging them by eye, and times them on a made-up library of any size.
 *
 *   manual_stations <library folder> [--days <n>] [--runs <n>] [--migrate]
 *   manual_stations <library folder> --open [--now <ms>] [--runs <n>]
 *   manual_stations <empty folder> --make <tracks> [--skips <rows>]
 *
 * --days also shows the next days' stations, and --runs repeats the timing. Timing covers
 * mc_radio_stations_list() and the whole of mc_home_picks_load(), which it's part of.
 *
 * --open opens a station from a few seeds instead, printing how long each took and its first
 * picks. With --now they're the same on every run, so two builds can be compared.
 *
 * --make fills a new library with tracks of 30 made-up sounds and a few strays, and plays for
 * some of them, so the first form has something big to time. Nothing is decoded. --skips adds
 * that many rows of stations' plays over the last 90 days, most of them early skips.
 *
 * A library of an older schema is refused without --migrate, as an older app can't open one
 * that's been migrated. The artist refresh that opening normally starts stays off, so this
 * makes no network calls. Reading a library leaves its data as it was. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "analysis/analysis_internal.h"
#include "library/library_internal.h"
#include "musiccore/analysis.h"
#include "musiccore/home_picks.h"
#include "musiccore/musiccore.h"
#include "musiccore/radio.h"
#include "musiccore/radio_stations.h"
#include "radio/groups.h"
#include "util/platform.h"

#define DAY_MS (24 * 60 * 60 * 1000LL)

enum { SOUNDS = 30, TRACKS_PER_ALBUM = 12, ALBUMS_PER_ARTIST = 4 };

static int schema_on_disk(const char *dir) {
  char path[2048];
  snprintf(path, sizeof path, "%s/library.db", dir);
  sqlite3 *db = NULL;
  int version = -1;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK &&
      sqlite3_prepare_v2(db, "PRAGMA user_version", -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW)
    version = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return version;
}

/* xorshift32, then a rough normal from a sum of uniforms: this only needs to look like clusters. */
static uint32_t rng = 2463534242u;

static double unit(void) {
  rng ^= rng << 13;
  rng ^= rng >> 17;
  rng ^= rng << 5;
  return (double)(rng >> 8) / (double)(1u << 24);
}

static double normal(void) {
  double sum = 0;
  for (int i = 0; i < 12; i++) sum += unit();
  return sum - 6.0;
}

static void result_for(int sound, mc_analysis *a) {
  /* The sounds are spread by the sound's number, so they're the same on every run. */
  uint32_t kept = rng;
  mc_analysis centre;
  memset(&centre, 0, sizeof centre);
  rng = 2654435761u * (uint32_t)(sound + 1) + 1u;
  for (int i = 0; i < MC_ANALYSIS_MFCCS; i++) {
    centre.mfcc_mean[i] = (float)(normal() * 12.0 / (1 + i / 3));
    centre.mfcc_var[i] = (float)(20.0 + 15.0 * unit());
  }
  centre.centroid_mean = (float)(800.0 + 3000.0 * unit());
  centre.centroid_var = 200000.0f;
  centre.lufs = -20.0 + 12.0 * unit();
  centre.lra = 3.0 + 10.0 * unit();
  rng = kept;

  double stray = sound < 0 ? 6.0 : 1.0;
  *a = centre;
  a->version = MC_ANALYSIS_VERSION;
  for (int i = 0; i < MC_ANALYSIS_MFCCS; i++) {
    a->mfcc_mean[i] += (float)(normal() * stray * 3.0 / (1 + i / 3));
    a->mfcc_var[i] *= (float)(1.0 + 0.1 * normal());
  }
  a->centroid_mean *= (float)exp2(0.15 * normal());
  a->lufs += normal();
  a->lra += normal();
  a->true_peak = -1.0;
}

/* Stations' plays of random tracks, from 200 seeds, as if listened to all the time. */
static void make_skips(sqlite3 *db, int tracks, int rows, int64_t now) {
  sqlite3_stmt *event;
  sqlite3_prepare_v2(db, "INSERT INTO play_events (uid, track_uid, node_uid, started_ms, listened_ms, ended, context, context_uid)"
                         " VALUES (?1, ?2, 'n', ?3, ?4, ?5, 'radio', ?6)", -1, &event, NULL);
  char uid[64], track[64], seed[64];
  for (int i = 0; i < rows; i++) {
    int skipped = unit() < 0.7;
    snprintf(uid, sizeof uid, "radio-%d", i);
    snprintf(track, sizeof track, "track%07d", (int)(unit() * tracks));
    snprintf(seed, sizeof seed, "track%07d", (int)(unit() * 200) * (tracks / 200));
    sqlite3_bind_text(event, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(event, 2, track, -1, SQLITE_STATIC);
    sqlite3_bind_int64(event, 3, now - (int64_t)(unit() * 90 * DAY_MS));
    sqlite3_bind_int64(event, 4, skipped ? 10000 : 200000);
    sqlite3_bind_text(event, 5, skipped ? "skipped" : "finished", -1, SQLITE_STATIC);
    sqlite3_bind_text(event, 6, seed, -1, SQLITE_STATIC);
    sqlite3_step(event);
    sqlite3_reset(event);
  }
  sqlite3_finalize(event);
}

static int make_library(const char *dir, int tracks, int skips) {
  mc_mkdirs(dir);
  mc_library *lib = NULL;
  if (mc_library_open_internal(dir, &lib) != MC_OK) {
    fprintf(stderr, "Can't open %s\n", dir);
    return 1;
  }
  sqlite3 *db = mc_library_db(lib);
  /* What logs each row calls back into the app; a made-up library needs no changelog. */
  static const char *const drop_logs =
    "SELECT 'DROP TRIGGER ' || name || ';' FROM sqlite_master WHERE type = 'trigger' AND name LIKE 'mc_log_%'";
  sqlite3_stmt *list;
  if (sqlite3_prepare_v2(db, drop_logs, -1, &list, NULL) == SQLITE_OK) {
    while (sqlite3_step(list) == SQLITE_ROW) {
      char *sql = mc_strdup((const char *)sqlite3_column_text(list, 0));
      sqlite3_exec(db, sql, NULL, NULL, NULL);
      mc_free(sql);
    }
    sqlite3_finalize(list);
  }
  sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
  sqlite3_stmt *artist, *album, *track, *file, *analysis, *plays, *event;
  sqlite3_prepare_v2(db, "INSERT INTO artists (uid, name) VALUES (?1, ?1)", -1, &artist, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO albums (uid, title, artist_uid, art_hash) VALUES (?1, ?1, ?2, 'h')", -1, &album, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO tracks (uid, title, artist_uid, album_uid, duration_ms) VALUES (?1, ?1, ?2, ?3, 200000)", -1, &track, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms) VALUES (?1, ?1, ?2, 'flac', 1000, 2000)", -1, &file, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO track_analysis (uid, track_uid, source, analysis_version, features, lufs, lra, true_peak)"
                         " VALUES (?1, ?1, 0, ?2, ?3, ?4, ?5, ?6)", -1, &analysis, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO track_plays (uid, track_uid, node_uid, play_count, last_played_ms) VALUES (?1, ?1, 'n', ?2, ?3)", -1, &plays, NULL);
  sqlite3_prepare_v2(db, "INSERT INTO play_events (uid, track_uid, node_uid, started_ms, listened_ms, ended, context) VALUES (?1, ?2, 'n', ?3, 200000, 'finished', 'album')", -1, &event, NULL);
  int64_t now = (int64_t)mc_wall_ms();
  char uid[64], albumu[64], artistu[64], path[128], euid[80];
  int sound = 0;
  for (int i = 0; i < tracks; i++) {
    int album_no = i / TRACKS_PER_ALBUM, artist_no = album_no / ALBUMS_PER_ARTIST;
    snprintf(artistu, sizeof artistu, "Artist %d", artist_no);
    snprintf(albumu, sizeof albumu, "Album %d", album_no);
    snprintf(uid, sizeof uid, "track%07d", i);
    if (i % (TRACKS_PER_ALBUM * ALBUMS_PER_ARTIST) == 0) {
      sqlite3_bind_text(artist, 1, artistu, -1, SQLITE_STATIC);
      sqlite3_step(artist);
      sqlite3_reset(artist);
      sound = (int)(unit() * SOUNDS);
    }
    if (i % TRACKS_PER_ALBUM == 0) {
      sqlite3_bind_text(album, 1, albumu, -1, SQLITE_STATIC);
      sqlite3_bind_text(album, 2, artistu, -1, SQLITE_STATIC);
      sqlite3_step(album);
      sqlite3_reset(album);
    }
    sqlite3_bind_text(track, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(track, 2, artistu, -1, SQLITE_STATIC);
    sqlite3_bind_text(track, 3, albumu, -1, SQLITE_STATIC);
    sqlite3_step(track);
    sqlite3_reset(track);
    snprintf(path, sizeof path, "%s/%s/%d.flac", artistu, albumu, i);
    sqlite3_bind_text(file, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_text(file, 2, path, -1, SQLITE_STATIC);
    sqlite3_step(file);
    sqlite3_reset(file);
    mc_analysis a;
    result_for(unit() < 0.01 ? -1 - (int)(unit() * 100) : sound, &a);
    unsigned char blob[MC_ANALYSIS_BLOB_SIZE];
    mc_analysis_pack(&a, blob);
    sqlite3_bind_text(analysis, 1, uid, -1, SQLITE_STATIC);
    sqlite3_bind_int(analysis, 2, MC_ANALYSIS_VERSION);
    sqlite3_bind_blob(analysis, 3, blob, sizeof blob, SQLITE_STATIC);
    sqlite3_bind_double(analysis, 4, a.lufs);
    sqlite3_bind_double(analysis, 5, a.lra);
    sqlite3_bind_double(analysis, 6, a.true_peak);
    sqlite3_step(analysis);
    sqlite3_reset(analysis);
    if (unit() < 0.3) { /* a third played, with a log to go with it */
      int count = 1 + (int)(unit() * unit() * 30);
      sqlite3_bind_text(plays, 1, uid, -1, SQLITE_STATIC);
      sqlite3_bind_int(plays, 2, count);
      sqlite3_bind_int64(plays, 3, now - (int64_t)(unit() * 400) * DAY_MS);
      sqlite3_step(plays);
      sqlite3_reset(plays);
      for (int e = 0; e < count; e++) {
        snprintf(euid, sizeof euid, "%s-%d", uid, e);
        sqlite3_bind_text(event, 1, euid, -1, SQLITE_STATIC);
        sqlite3_bind_text(event, 2, uid, -1, SQLITE_STATIC);
        sqlite3_bind_int64(event, 3, now - (int64_t)(unit() * 400) * DAY_MS);
        sqlite3_step(event);
        sqlite3_reset(event);
      }
    }
  }
  make_skips(db, tracks, skips, now);
  sqlite3_stmt *all[] = {artist, album, track, file, analysis, plays, event};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) sqlite3_finalize(all[i]);
  int rc = sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
  mc_library_close(lib);
  printf("Made %d tracks in %s: %s\n", tracks, dir, rc == SQLITE_OK ? "ok" : "failed");
  return rc != SQLITE_OK;
}

/* What the library divides into, and how clearly each count did. */
static void print_groups(mc_library *lib) {
  mc_radio_groups groups;
  uint64_t start = mc_wall_ms();
  if (mc_radio_groups_load(lib, NULL, NULL, &groups) != MC_OK) return;
  printf("Groups: %d, in %llu ms, of", groups.count, (unsigned long long)(mc_wall_ms() - start));
  for (int g = 0; g < groups.count; g++) printf(" %d", groups.tracks[g]);
  printf(" tracks. Silhouettes:");
  for (int k = 2; k <= MC_RADIO_STATIONS_MAX; k++) printf(" %d: %.3f", k, groups.silhouette[k]);
  printf("\n");
}

static void print_stations(const mc_radio_station_list *list) {
  for (int i = 0; i < list->count; i++) {
    const mc_radio_station_info *s = &list->items[i];
    printf("%d. %s | %s | seed: %s | %s | cover: %s\n", i + 1, s->name, s->sub, s->seed_title, s->seed_track,
           s->cover_dir ? s->cover_dir : "none");
  }
}

static void print_apart(mc_library *lib, const mc_radio_station_list *list) {
  mc_analysis_stats stats;
  if (mc_analysis_stats_load(lib, &stats) != MC_OK) return;
  double least = -1, least_fixed = -1;
  for (int i = 0; i < list->count; i++) {
    for (int k = 0; k < i; k++) {
      mc_analysis a, b;
      if (mc_analysis_get(lib, list->items[i].seed_track, &a) != MC_OK ||
          mc_analysis_get(lib, list->items[k].seed_track, &b) != MC_OK)
        continue;
      double d = mc_analysis_distance(&a, &b, &stats), fixed = mc_analysis_distance(&a, &b, NULL);
      if (least < 0 || d < least) least = d;
      if (least_fixed < 0 || fixed < least_fixed) least_fixed = fixed;
    }
  }
  if (least >= 0) printf("Closest two seeds: %.2f on the library's scales, %.2f on the fixed ones\n", least, least_fixed);
}

/* Seeds spread over the library, each opened in turn. */
static void open_stations(mc_library *lib, int64_t now, int runs) {
  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(mc_library_db(lib),
                         "SELECT uid FROM tracks ORDER BY uid LIMIT 1"
                         " OFFSET ?1 * (SELECT count(*) FROM tracks) / 5", -1, &stmt, NULL) != SQLITE_OK)
    return;
  for (int s = 0; s < 5; s++) {
    sqlite3_reset(stmt);
    sqlite3_bind_int(stmt, 1, s);
    if (sqlite3_step(stmt) != SQLITE_ROW) break;
    char seed[MC_UID_STRLEN];
    snprintf(seed, sizeof seed, "%s", (const char *)sqlite3_column_text(stmt, 0));
    mc_radio_options opts = {seed, 0, 7, now, 0, NULL};
    for (int r = 0; r < runs; r++) {
      mc_radio_station *station = NULL;
      uint64_t start = mc_wall_ms();
      mc_status st = mc_radio_station_open(lib, &opts, NULL, NULL, &station);
      uint64_t opened = mc_wall_ms();
      if (st != MC_OK) {
        printf("%s: failed, %d\n", seed, (int)st);
        break;
      }
      mc_radio_pick picks[12];
      mc_radio_station_next(station, picks, 12);
      printf("%s: opened in %llu ms, 12 picks in %llu ms:", seed, (unsigned long long)(opened - start),
             (unsigned long long)(mc_wall_ms() - opened));
      for (int i = 1; i < 12; i++) printf(" %s", picks[i].track_uid + 5);
      printf("\n");
      mc_radio_station_close(station);
    }
  }
  sqlite3_finalize(stmt);
}

int main(int argc, char **argv) {
  if (argc < 2 || argv[1][0] == '-') {
    fprintf(stderr, "usage: %s <library folder> [--days <n>] [--runs <n>] [--migrate] [--open] [--now <ms>] | --make <tracks> [--skips <rows>]\n", argv[0]);
    return 2;
  }
  const char *dir = argv[1];
  int days = 1, runs = 3, migrate = 0, make = 0, skips = 0, open_them = 0;
  int64_t fixed_now = 0;
  for (int i = 2; i < argc; i++) {
    int more = i + 1 < argc;
    if (!strcmp(argv[i], "--days") && more) days = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--runs") && more) runs = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--make") && more) make = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--skips") && more) skips = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--now") && more) fixed_now = atoll(argv[++i]);
    else if (!strcmp(argv[i], "--open")) open_them = 1;
    else if (!strcmp(argv[i], "--migrate")) migrate = 1;
    else return 2;
  }
  if (mc_init() != MC_OK) return 1;
  if (make > 0) return make_library(dir, make, skips);

  int version = schema_on_disk(dir);
  if (version <= 0) {
    fprintf(stderr, "No library in %s\n", dir);
    return 1;
  }
  if (version < mc_library_schema_latest() && !migrate) {
    fprintf(stderr, "%s is at schema v%d and this build would migrate it to v%d. Work on a copy, or pass --migrate.\n",
            dir, version, mc_library_schema_latest());
    return 1;
  }
  mc_library *lib = NULL;
  if (mc_library_open_internal(dir, &lib) != MC_OK) {
    fprintf(stderr, "Can't open the library\n");
    return 1;
  }

  int64_t now = fixed_now ? fixed_now : (int64_t)mc_wall_ms();
  if (open_them) {
    open_stations(lib, now, runs);
    mc_library_close(lib);
    return 0;
  }
  print_groups(lib);
  for (int d = 0; d < days; d++) {
    mc_radio_station_list *list = NULL;
    mc_status st = mc_radio_stations_list(lib, now + d * DAY_MS, &list);
    if (st != MC_OK) {
      fprintf(stderr, "Failed: %d\n", (int)st);
      mc_library_close(lib);
      return 1;
    }
    printf("--- Day %d: %d stations\n", d, list->count);
    print_stations(list);
    print_apart(lib, list);
    mc_radio_station_list_free(list);
  }

  for (int r = 0; r < runs; r++) {
    uint64_t start = mc_wall_ms();
    mc_radio_station_list *list = NULL;
    mc_radio_stations_list(lib, now, &list);
    uint64_t stations = mc_wall_ms();
    mc_radio_station_list_free(list);
    mc_home_picks picks;
    uint64_t home = mc_wall_ms();
    mc_home_picks_load(lib, 1, &picks);
    uint64_t done = mc_wall_ms();
    printf("Run %d: stations %llu ms, the whole of Home %llu ms (%d in the row)\n", r + 1,
           (unsigned long long)(stations - start), (unsigned long long)(done - home),
           picks.counts[MC_HOME_ROW_RADIO]);
    mc_home_picks_free(&picks);
  }
  mc_library_close(lib);
  return 0;
}
