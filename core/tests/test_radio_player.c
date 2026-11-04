/* A station played by the player (mc_player_play_radio()), with silent WAVs far shorter than the
 * library says, so it gets through a lot of them quickly. The queue growing needs no audio
 * device; what's played does, and is skipped (exit 77) without one. */
#include <stdint.h>
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

#define SKIP 77

enum { TRACKS = 12, AHEAD = 5, WAV_MS = 200 };

static mc_mutex lock;
static int errors, queue_events;

static void on_event(void *user, mc_player_event event) {
  (void)user;
  mc_mutex_lock(&lock);
  if (event == MC_PLAYER_EVENT_ERROR) errors++;
  if (event == MC_PLAYER_EVENT_QUEUE) queue_events++;
  mc_mutex_unlock(&lock);
}

static void exec_ok(mc_library *lib, const char *sql) {
  char *err = NULL;
  CHECK(mc_library_begin_write(lib) == MC_OK);
  if (sqlite3_exec(mc_library_db(lib), sql, NULL, NULL, &err) != SQLITE_OK) {
    fprintf(stderr, "%s: %s\n", sql, err ? err : "?");
    test_failures++;
  }
  sqlite3_free(err);
  CHECK(mc_library_commit(lib) == MC_OK);
}

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A silent 16-bit stereo PCM WAV. */
static void write_wav(const char *path, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  unsigned rate = 44100, channels = 2;
  uint32_t frames = rate * ms / 1000, bytes = frames * channels * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, channels);
  put32(f, rate);
  put32(f, rate * channels * 2);
  put16(f, channels * 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < bytes; i++) fputc(0, f);
  fclose(f);
}

/* A track of its own artist, with a file on disk if has_wav, a made-up result if analysed. The
 * library says it's over three minutes, so it's a candidate. */
static void add_track(mc_library *lib, const char *dir, const char *uid, double sound, int has_wav, int analysed) {
  char sql[512], file[MC_UID_STRLEN], path[1200];
  snprintf(sql, sizeof sql, "INSERT INTO artists (uid, name) VALUES ('ar-%s', 'ar-%s')", uid, uid);
  exec_ok(lib, sql);
  snprintf(sql, sizeof sql,
           "INSERT INTO tracks (uid, title, artist_uid, duration_ms) VALUES ('%s', '%s', 'ar-%s', 200000)", uid, uid,
           uid);
  exec_ok(lib, sql);
  mc_uid_new(file);
  snprintf(sql, sizeof sql,
           "INSERT INTO files (uid, track_uid, path, format, size_bytes, mtime_ms)"
           " VALUES ('%s', '%s', '%s.wav', 'wav', 1000, 2000)",
           file, uid, uid);
  exec_ok(lib, sql);
  snprintf(path, sizeof path, "%s/%s.wav", dir, uid);
  if (has_wav) write_wav(path, WAV_MS);
  else remove(path);
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

static mc_library *open_library(const char *scratch, const char *name, char *dir, size_t n) {
  char path[1200];
  snprintf(dir, n, "%s/%s", scratch, name);
  mc_mkdirs(dir);
  snprintf(path, sizeof path, "%s/library.db", dir);
  remove(path);
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  return lib;
}

static void seed_path(char *out, size_t n, const char *dir, const char *uid) {
  snprintf(out, n, "%s/%s.wav", dir, uid);
}

/* Waits until the queue holds count items; 0 if it doesn't in ms. */
static int wait_for_count(mc_player *player, int count, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  mc_player_status st;
  do {
    mc_player_status_get(player, &st);
    if (st.count >= count) return 1;
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  return 0;
}

static int wait_for_stopped(mc_player *player, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  mc_player_status st;
  do {
    mc_player_status_get(player, &st);
    if (st.state == MC_PLAYER_STOPPED) return 1;
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  return 0;
}

static int wait_for_no_radio(mc_player *player, unsigned ms) {
  uint64_t end = mc_mono_ms() + ms;
  mc_player_status st;
  do {
    mc_player_status_get(player, &st);
    if (!st.radio) return 1;
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  return 0;
}

static int queue_events_so_far(void) {
  mc_mutex_lock(&lock);
  int seen = queue_events;
  mc_mutex_unlock(&lock);
  return seen;
}

/* Waits for a queue event past the first after. It can't be expected to come with the items:
 * the feed gets them in while the player's thread is still opening the audio device, which took
 * from 60 ms to over 2 s here, and the event waits for that thread. So this is a limit for an
 * event that's never coming, not for how long one should take. */
static int wait_for_queue_event(int after) {
  uint64_t end = mc_mono_ms() + 30000;
  do {
    if (queue_events_so_far() > after) return 1;
    mc_sleep_ms(5);
  } while (mc_mono_ms() < end);
  return 0;
}

static int queue_count(mc_player *player) {
  mc_player_status st;
  mc_player_status_get(player, &st);
  return st.count;
}

static void take_all_ended(mc_player *player) {
  mc_play_event e;
  while (mc_player_take_ended(player, &e) == MC_OK) {}
}

/* The queue fills up to a few tracks ahead and no further, and needs no audio to do it. */
static void test_tops_up(mc_player *player, const char *dir) {
  char path[1200], uid[MC_UID_STRLEN], first[AHEAD + 1][MC_UID_STRLEN];
  seed_path(path, sizeof path, dir, "t00");
  int before = queue_events_so_far(); /* another player's may have come in already */
  /* Paused at once, so with a device too the position stays put and the count settles. */
  CHECK(mc_player_play_radio(player, dir, path, "t00") == MC_OK);
  CHECK(mc_player_pause(player) == MC_OK);
  CHECK(wait_for_count(player, AHEAD + 1, 10000));
  mc_sleep_ms(200);
  CHECK(queue_count(player) == AHEAD + 1);

  mc_player_status st;
  mc_player_status_get(player, &st);
  CHECK(st.radio);
  CHECK(mc_player_queue_uid(player, 0, uid) == MC_OK);
  CHECK(!strcmp(uid, "t00"));
  for (int i = 0; i <= AHEAD; i++) {
    CHECK(mc_player_queue_uid(player, i, first[i]) == MC_OK);
    for (int j = 0; j < i; j++) CHECK(strcmp(first[i], first[j]));
  }
  CHECK(mc_player_queue_uid(player, AHEAD + 1, uid) == MC_ERR_NOT_FOUND);
  CHECK(wait_for_queue_event(before));

  /* The thread is idle now, with the device open or given up on: the station ending wakes it,
   * and so does a new one's items, which is what a frontend waits for in the middle of a play. */
  before = queue_events_so_far();
  CHECK(mc_player_stop_radio(player) == MC_OK);
  CHECK(wait_for_queue_event(before));
  mc_player_status_get(player, &st);
  CHECK(!st.radio);
  before = queue_events_so_far();
  CHECK(mc_player_play_radio(player, dir, path, "t00") == MC_OK);
  CHECK(mc_player_pause(player) == MC_OK);
  CHECK(wait_for_count(player, AHEAD + 1, 10000));
  CHECK(wait_for_queue_event(before));

  /* Another queue ends it, and nothing's added to that one. */
  const char *const one[] = {path};
  CHECK(mc_player_set_queue(player, one, 1, 0, NULL) == MC_OK);
  mc_player_pause(player);
  mc_sleep_ms(200);
  mc_player_status_get(player, &st);
  CHECK(!st.radio);
  CHECK(st.count == 1);
}

/* A seed that can't be used plays by itself, and the station gives up without a fuss. */
static void test_bad_seed(mc_player *player, const char *dir) {
  char path[1200];
  seed_path(path, sizeof path, dir, "unanalysed");
  CHECK(mc_player_play_radio(player, dir, path, "unanalysed") == MC_OK);
  mc_player_pause(player);
  CHECK(wait_for_no_radio(player, 10000));
  CHECK(queue_count(player) == 1);
  CHECK(mc_player_play_radio(NULL, dir, path, "t00") == MC_ERR_INVALID_ARG);
  CHECK(mc_player_play_radio(player, NULL, path, "t00") == MC_ERR_INVALID_ARG);
  CHECK(mc_player_stop_radio(NULL) == MC_ERR_INVALID_ARG);
}

/* Freed while it's still reading the library, it neither hangs nor crashes. */
static void test_free_while_loading(const char *dir) {
  char path[1200];
  seed_path(path, sizeof path, dir, "t00");
  mc_player *player = NULL;
  CHECK(mc_player_new(on_event, NULL, &player) == MC_OK);
  CHECK(mc_player_play_radio(player, dir, path, "t00") == MC_OK);
  mc_player_free(player);
}

/* Plays on past the end of what the library holds, logging it all as the station's, in the
 * station's order though shuffle is on. */
static void test_plays_on(mc_player *player, const char *dir) {
  char path[1200];
  seed_path(path, sizeof path, dir, "t00");
  take_all_ended(player);
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  CHECK(mc_player_play_radio(player, dir, path, "t00") == MC_OK);
  /* Next straight away: the seed may well be all there is yet. */
  CHECK(mc_player_next(player) == MC_OK);
  int last = 0, moved = 0;
  uint64_t end = mc_mono_ms() + 30000;
  mc_player_status st;
  do {
    mc_player_status_get(player, &st);
    CHECK(st.index >= last); /* never back, as a shuffled order would */
    if (st.index > last) moved++;
    last = st.index;
    mc_sleep_ms(5);
  } while (st.index < TRACKS + 3 && mc_mono_ms() < end);
  CHECK(st.index >= TRACKS + 3);
  CHECK(moved > 0);
  CHECK(st.state == MC_PLAYER_PLAYING);
  CHECK(st.radio);

  /* Stopping the station leaves what it queued to play out, and then the player stops. */
  CHECK(mc_player_stop_radio(player) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(!st.radio);
  int count = st.count;
  CHECK(wait_for_stopped(player, 10000));
  CHECK(queue_count(player) == count);
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);

  /* What the player logs, handed on as a frontend does, puts the station in the recents once,
   * though this little library brings the seed round again. */
  mc_library *lib = NULL;
  CHECK(mc_library_open(dir, &lib) == MC_OK);
  mc_play_event e;
  int rows = 0, finished = 0, starts = 0;
  while (mc_player_take_ended(player, &e) == MC_OK) {
    rows++;
    finished += e.end == MC_PLAY_END_FINISHED;
    starts += mc_play_event_starts_station(&e);
    CHECK(e.context == MC_PLAY_CONTEXT_RADIO);
    CHECK_STR(e.context_uid, "t00");
    if (lib) CHECK(mc_library_log_play(lib, &e) == MC_OK);
  }
  CHECK(rows > TRACKS);
  CHECK(finished > 0);
  CHECK(starts > 1);
  mc_recent_list *recents = NULL;
  if (lib) CHECK(mc_library_list_recents(lib, 0, MC_LIST_STATIONS, &recents) == MC_OK && recents && recents->count == 1);
  if (recents && recents->count == 1) {
    CHECK(recents->items[0].kind == MC_RECENT_STATION);
    CHECK_STR(recents->items[0].uid, "t00");
  }
  mc_recent_list_free(recents);
  sqlite3_stmt *writes = NULL;
  if (lib) CHECK(sqlite3_prepare_v2(mc_library_db(lib), "SELECT count(*) FROM changelog WHERE entity = 'recents'"
                                    " AND field = 'opened_ms'", -1, &writes, NULL) == SQLITE_OK);
  if (writes) CHECK(sqlite3_step(writes) == SQLITE_ROW && sqlite3_column_int(writes, 0) == 1);
  sqlite3_finalize(writes);
  mc_library_close(lib);
}

/* With every file gone, it gives up rather than failing through the library for ever. */
static void test_files_gone(mc_player *player, const char *dir) {
  char path[1200];
  seed_path(path, sizeof path, dir, "t00");
  for (int i = 1; i < TRACKS; i++) {
    char gone[1200];
    snprintf(gone, sizeof gone, "%s/t%02d.wav", dir, i);
    remove(gone);
  }
  CHECK(mc_player_play_radio(player, dir, path, "t00") == MC_OK);
  CHECK(wait_for_no_radio(player, 20000));
  CHECK(wait_for_stopped(player, 10000));
  CHECK(queue_count(player) < 40);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  mc_mutex_init(&lock);
  char dir[1024];
  mc_library *lib = open_library(scratch, "test_radio_player.d", dir, sizeof dir);
  if (!lib) return TEST_RESULT();
  for (int i = 0; i < TRACKS; i++) {
    char uid[16];
    snprintf(uid, sizeof uid, "t%02d", i);
    add_track(lib, dir, uid, i * 0.1, 1, 1);
  }
  add_track(lib, dir, "unanalysed", 0.0, 1, 0);
  mc_library_close(lib);
  /* Longer than the buffer, so paused on the seed the player doesn't decode on into the queue. */
  char seed[1200];
  seed_path(seed, sizeof seed, dir, "t00");
  write_wav(seed, 3000);

  test_free_while_loading(dir);
  mc_player *player = NULL;
  CHECK(mc_player_new(on_event, NULL, &player) == MC_OK);
  if (!player) return TEST_RESULT();
  test_tops_up(player, dir);
  test_bad_seed(player, dir);

  /* Anything more needs sound to come out. */
  char path[1200];
  seed_path(path, sizeof path, dir, "t00");
  const char *const one[] = {path};
  mc_mutex_lock(&lock);
  errors = 0;
  mc_mutex_unlock(&lock);
  CHECK(mc_player_set_queue(player, one, 1, 0, NULL) == MC_OK);
  mc_sleep_ms(100);
  CHECK(wait_for_stopped(player, 5000));
  mc_sleep_ms(100); /* the error is reported just after the state it causes */
  mc_mutex_lock(&lock);
  int no_device = errors > 0;
  mc_mutex_unlock(&lock);
  if (no_device) {
    fprintf(stderr, "no audio device: skipping what needs one\n");
    mc_player_free(player);
    return test_failures ? 1 : SKIP;
  }
  test_plays_on(player, dir);
  test_files_gone(player, dir);
  mc_player_free(player);
  return TEST_RESULT();
}
