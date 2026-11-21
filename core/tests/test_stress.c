/* Stress test libraries and the profiler (kithcore/stress.h, perf.h): what the generator makes,
 * that it can't touch anything else, and that such a library neither syncs nor scrobbles. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "library/library_internal.h"
#include "test.h"
#include "util/platform.h"

static char *join(const char *a, const char *b) {
  size_t n = strlen(a) + strlen(b) + 2;
  char *p = malloc(n);
  snprintf(p, n, "%s%c%s", a, MC_PATH_SEP, b);
  return p;
}

static void wait_for(mc_stress_job *job, mc_status *st) {
  while (!mc_stress_job_done(job, st)) mc_sleep_ms(5);
}

static int64_t count(mc_library *lib, const char *sql) {
  sqlite3_stmt *s = NULL;
  int64_t n = -1;
  if (sqlite3_prepare_v2(mc_library_db(lib), sql, -1, &s, NULL) == SQLITE_OK && sqlite3_step(s) == SQLITE_ROW)
    n = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return n;
}

static void test_generate(const char *base) {
  char *config = join(base, "stress-config");
  mc_remove_tree(config);
  mc_mkdirs(config);
  char *dir = mc_stress_dir(config, MC_STRESS_1K);
  CHECK(dir != NULL);
  CHECK(!mc_stress_is_generated(dir));

  mc_stress_job *job = NULL;
  mc_status st = MC_ERR_IO;
  CHECK(mc_stress_start(dir, MC_STRESS_1K, &job) == MC_OK);
  wait_for(job, &st);
  mc_stress_job_free(job);
  CHECK(st == MC_OK);
  CHECK(mc_stress_is_generated(dir));
  CHECK(mc_stress_is_complete(dir));
  CHECK(mc_stress_count(config) == 1);

  /* Finished, it's not made over. */
  CHECK(mc_stress_start(dir, MC_STRESS_1K, &job) == MC_ERR_EXISTS);

  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  CHECK(mc_library_is_stress(lib));
  int64_t n = 0;
  CHECK(mc_library_count(lib, MC_ENTITY_TRACKS, &n) == MC_OK && n == 1000);
  CHECK(mc_library_count(lib, MC_ENTITY_ALBUMS, &n) == MC_OK && n > 40);
  CHECK(mc_library_count(lib, MC_ENTITY_ARTISTS, &n) == MC_OK && n >= 10);
  CHECK(mc_library_count(lib, MC_ENTITY_PLAYLISTS, &n) == MC_OK && n >= 4);
  CHECK(count(lib, "SELECT count(*) FROM changelog") == 0);
  CHECK(count(lib, "SELECT count(*) FROM files") == 1000);
  CHECK(count(lib, "SELECT max(length(title)) FROM tracks") >= 200);
  CHECK(count(lib, "SELECT count(*) FROM tracks WHERE title GLOB '*[^ -~]*'") > 10);
  CHECK(count(lib, "SELECT count(*) FROM track_artists WHERE position = 1") > 10);
  CHECK(count(lib, "PRAGMA foreign_key_check") == -1); /* no rows */

  /* What the frontends read works on it, and the first track of an album is really there. */
  mc_album_list *albums = NULL;
  CHECK(mc_library_list_albums(lib, &albums) == MC_OK);
  CHECK(albums && albums->count > 20);
  int covers = 0;
  for (int i = 0; albums && i < albums->count; i++) {
    char *cover = albums->items[i].folder ? mc_coverart_find(albums->items[i].folder) : NULL;
    CHECK(!!cover == !!albums->items[i].has_art);
    covers += cover != NULL;
    mc_free(cover);
  }
  CHECK(covers > 10);
  mc_track_list *tracks = NULL;
  CHECK(mc_library_list_album_tracks(lib, albums->items[0].uid, &tracks) == MC_OK);
  CHECK(tracks && tracks->count > 0 && tracks->items[0].path && mc_file_exists(tracks->items[0].path));
  mc_track_list_free(tracks);
  mc_album_list_free(albums);

  /* Neither sync nor scrobbling takes it, whatever the device's sync.db holds. */
  mc_sync *sync = (mc_sync *)1;
  CHECK(mc_sync_new(lib, config, "test", MC_DEVICE_DESKTOP, NULL, NULL, &sync) == MC_ERR_STRESS_LIBRARY);
  CHECK(sync == NULL);
  mc_scrobbler *sc = (mc_scrobbler *)1;
  CHECK(mc_scrobbler_new(lib, config, &sc) == MC_ERR_STRESS_LIBRARY);
  CHECK(sc == NULL);
  char *syncdb = join(config, "sync.db");
  CHECK(!mc_file_exists(syncdb)); /* refused before opening anything */
  free(syncdb);

  /* Open, it can't be deleted. */
  CHECK(mc_stress_delete(dir, dir) == MC_ERR_IN_USE);
  int deleted = -1;
  CHECK(mc_stress_delete_all(config, dir, &deleted) == MC_OK && deleted == 0);
  CHECK(mc_stress_is_complete(dir));
  mc_library_close(lib);

  CHECK(mc_stress_delete_all(config, NULL, &deleted) == MC_OK && deleted == 1);
  CHECK(!mc_file_exists(dir));
  mc_free(dir);
  mc_remove_tree(config);
  free(config);
}

static void test_same_every_time(const char *base) {
  char *a = join(base, "stress-same-a"), *b = join(base, "stress-same-b");
  mc_remove_tree(a);
  mc_remove_tree(b);
  mc_stress_job *job;
  mc_status st;
  const char *dirs[] = {a, b};
  char first[2][MC_UID_STRLEN + 200];
  for (int i = 0; i < 2; i++) {
    CHECK(mc_stress_start(dirs[i], MC_STRESS_1K, &job) == MC_OK);
    wait_for(job, &st);
    mc_stress_job_free(job);
    CHECK(st == MC_OK);
    mc_library *lib = NULL;
    CHECK(mc_library_open(dirs[i], &lib) == MC_OK);
    sqlite3_stmt *s = NULL;
    sqlite3_prepare_v2(mc_library_db(lib), "SELECT uid, title FROM tracks ORDER BY uid DESC LIMIT 1", -1, &s, NULL);
    CHECK(s && sqlite3_step(s) == SQLITE_ROW);
    snprintf(first[i], sizeof first[i], "%s %.100s", sqlite3_column_text(s, 0), sqlite3_column_text(s, 1));
    sqlite3_finalize(s);
    mc_library_close(lib);
  }
  CHECK_STR(first[0], first[1]);
  CHECK(mc_stress_delete(a, NULL) == MC_OK);
  CHECK(mc_stress_delete(b, NULL) == MC_OK);
  free(a);
  free(b);
}

/* The folder and everything in it stay as they were. */
static void test_never_touches_other_folders(const char *base) {
  char *real = join(base, "stress-real");
  mc_remove_tree(real);
  mc_library *lib = NULL;
  CHECK(mc_library_open(real, &lib) == MC_OK); /* a user's library */
  CHECK(!mc_library_is_stress(lib));
  mc_library_close(lib);

  mc_stress_job *job = NULL;
  CHECK(mc_stress_start(real, MC_STRESS_1K, &job) == MC_ERR_EXISTS);
  CHECK(job == NULL);
  CHECK(mc_stress_delete(real, NULL) == MC_ERR_INVALID_ARG);
  char *db = join(real, MC_LIBRARY_DB_NAME);
  CHECK(mc_file_exists(db));

  /* mc_stress_delete_all() leaves what hasn't the marker, even under its own root. */
  char *config = join(base, "stress-config2"), *root = join(config, MC_STRESS_ROOT), *other = join(root, "mine");
  mc_remove_tree(config);
  mc_mkdirs(other);
  int deleted = -1;
  CHECK(mc_stress_count(config) == 0);
  CHECK(mc_stress_delete_all(config, NULL, &deleted) == MC_OK && deleted == 0);
  CHECK(mc_file_exists(other));
  mc_remove_tree(config);
  mc_remove_tree(real);
  free(other);
  free(root);
  free(config);
  free(db);
  free(real);
}

static void test_cancel(const char *base) {
  char *config = join(base, "stress-config3");
  mc_remove_tree(config);
  char *dir = mc_stress_dir(config, MC_STRESS_100K);
  mc_stress_job *job = NULL;
  CHECK(mc_stress_start(dir, MC_STRESS_100K, &job) == MC_OK);
  while (mc_stress_job_percent(job) < 1 && !mc_stress_job_done(job, NULL)) mc_sleep_ms(2);
  mc_stress_job_cancel(job);
  mc_status st = MC_OK;
  wait_for(job, &st);
  mc_stress_job_free(job);
  CHECK(st == MC_ERR_NOT_FOUND);
  CHECK(!mc_file_exists(dir)); /* nothing left behind */
  mc_free(dir);
  mc_remove_tree(config);
  free(config);
}

static void test_perf(const char *base) {
  /* Off, feeding it does nothing. */
  mc_perf_set_enabled(0);
  mc_perf_mark("ignored");
  mc_perf_frame("ignored", 5);
  mc_perf_view_begin("ignored");
  CHECK(!mc_perf_view_pending());
  char *report = mc_perf_report();
  CHECK(report && !strstr(report, "ignored"));
  mc_free(report);

  mc_perf_set_enabled(1);
  CHECK(mc_perf_enabled());
  uint64_t t = mc_perf_now_us();
  mc_perf_mark_at("main_entered", t);
  mc_perf_mark("window_shown");
  mc_perf_mark("window_shown"); /* only the first counts */
  mc_perf_note_int("library_tracks", 10000);
  mc_perf_note_int("library_tracks", 20000);
  mc_perf_set_refresh_hz(100);
  for (int i = 0; i < 6; i++) {
    mc_perf_frame("albums", 1500);
    mc_sleep_ms(i == 4 ? 60 : 12);
  }
  mc_perf_frame_break("albums");
  mc_perf_view_begin("artists");
  CHECK(mc_perf_view_pending());
  mc_perf_view_end();
  CHECK(!mc_perf_view_pending());
  mc_perf_auto_begin();
  CHECK(mc_perf_auto_running());
  mc_perf_frame("albums", 0);
  mc_sleep_ms(5);
  mc_perf_frame("albums", 0);
  mc_perf_auto_end();

  /* Memory: a reading that grows when this process takes more. */
  mc_perf_memory m0, m1;
  CHECK(mc_perf_memory_now(&m0) == MC_OK && m0.resident > 0 && m0.peak >= m0.resident);
  mc_perf_memory_sample("before");
  size_t big = 64u << 20;
  char *hold = malloc(big);
  CHECK(hold != NULL);
  /* Each page touched, so it's resident: through volatile, or the compiler may drop the writes
   * or put them after the reading, as nothing else looks at them. */
  volatile char *touch = hold;
  for (size_t i = 0; hold && i < big; i += 4096) touch[i] = 1;
  CHECK(mc_perf_memory_now(&m1) == MC_OK && m1.private_bytes >= m0.private_bytes + big / 2);
  CHECK(m1.resident >= m0.resident + big / 2 && m1.peak >= m1.resident);
  mc_perf_memory_sample("after");
  free(hold);

  report = mc_perf_report();
  CHECK(report != NULL);
  if (report) {
    CHECK(strstr(report, "[startup]") && strstr(report, "main_entered ") && strstr(report, "window_shown "));
    CHECK(strstr(report, "library_tracks 20000\n") && !strstr(report, "library_tracks 10000"));
    CHECK(strstr(report, "[scroll albums]\nframes 5\n"));
    CHECK(strstr(report, "[scroll auto_albums]\nframes 1\n"));
    CHECK(strstr(report, "paint_median_ms 1.50\n"));
    CHECK(strstr(report, "[views]\n") && strstr(report, "artists 1 "));
    CHECK(strstr(report, "refresh_hz 100\n"));
    const char *mem = strstr(report, "[memory]\n");
    CHECK(mem && strstr(mem, "\nbefore ") && strstr(mem, "\nafter ") && strstr(mem, "\nbefore ") < strstr(mem, "\nafter "));
    /* At 100 Hz the 60 ms frame missed several refreshes. */
    const char *dropped = strstr(report, "dropped ");
    CHECK(dropped && atoi(dropped + 8) >= 4);
  }

  char *path = join(base, "perf-report.txt");
  CHECK(mc_perf_save(path) == MC_OK);
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(path, &size, &mtime) == MC_OK && report && size == (int64_t)strlen(report));
  mc_remove(path);
  free(path);
  mc_free(report);

  mc_perf_clear();
  report = mc_perf_report();
  CHECK(report && !strstr(report, "[scroll") && strstr(report, "window_shown ") && strstr(report, "[memory]"));
  mc_free(report);
  mc_perf_set_enabled(0);
}

int main(int argc, char **argv) {
  const char *base = argc > 1 ? argv[1] : ".";
  test_perf(base);
  test_generate(base);
  test_same_every_time(base);
  test_never_touches_other_folders(base);
  test_cancel(base);
  return TEST_RESULT();
}
