/* The play log as the player keeps it, against the real audio device, with silent WAVs.
 * Skipped (exit 77) when there is no device. Each case plays something and checks the rows it
 * leaves: gapless moves, skips, seeks, pauses, a file that can't be played, a queue replaced,
 * the app closing mid-track, and starts that are never heard. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "test.h"
#include "util/platform.h"

#define SKIP 77

#define T_SHORT "aaaaaaaa-0000-7000-8000-000000000001"
#define T_OTHER "aaaaaaaa-0000-7000-8000-000000000002"
#define T_LONG "aaaaaaaa-0000-7000-8000-000000000003"
#define T_BAD "aaaaaaaa-0000-7000-8000-000000000004"
#define ALBUM "bbbbbbbb-0000-7000-8000-000000000001"
#define PLAYLIST "bbbbbbbb-0000-7000-8000-000000000002"

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A silent 16-bit PCM WAV. */
static void write_wav(const char *path, unsigned rate, unsigned channels, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
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

static char *join(const char *dir, const char *name) {
  size_t n = strlen(dir) + strlen(name) + 2;
  char *s = malloc(n);
  if (s) snprintf(s, n, "%s/%s", dir, name);
  return s;
}

static mc_player *player;

static void wait_position(int64_t ms) {
  mc_player_status st;
  for (uint64_t end = mc_mono_ms() + 5000; mc_mono_ms() < end; mc_sleep_ms(5)) {
    mc_player_status_get(player, &st);
    if (st.state == MC_PLAYER_PLAYING && st.position_ms >= ms) return;
  }
  CHECK(0 && "the position never got there");
}

static void wait_stopped(void) {
  mc_player_status st;
  for (uint64_t end = mc_mono_ms() + 8000; mc_mono_ms() < end; mc_sleep_ms(5)) {
    mc_player_status_get(player, &st);
    if (st.state == MC_PLAYER_STOPPED) return;
  }
  CHECK(0 && "it never stopped");
}

/* Takes the rows there are, waiting for want of them first. */
static int take(mc_play_event *rows, int want) {
  int n = 0;
  for (uint64_t end = mc_mono_ms() + 3000; n < want && mc_mono_ms() < end;) {
    if (mc_player_take_ended(player, &rows[n]) == MC_OK) n++;
    else mc_sleep_ms(5);
  }
  mc_sleep_ms(60); /* none extra should follow */
  mc_play_event extra;
  while (n < 16 && mc_player_take_ended(player, &extra) == MC_OK) rows[n++] = extra;
  return n;
}

static int same(const char *a, const char *b) {
  return strcmp(a, b) == 0;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  char *a = join(dir, "play_log_a.wav"), *b = join(dir, "play_log_b.wav"), *longer = join(dir, "play_log_long.wav"),
       *missing = join(dir, "play_log_missing.wav");
  write_wav(a, 44100, 2, 300);
  write_wav(b, 48000, 1, 300);
  write_wav(longer, 22050, 2, 4000);

  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);
  mc_play_event rows[16];
  mc_player_status st;

  /* Is there a device? The position only moves once it pulls audio. */
  const char *probe[] = {longer};
  CHECK(mc_player_set_queue(player, probe, 1, 0, NULL) == MC_OK);
  int has_device = 0;
  for (uint64_t end = mc_mono_ms() + 3000; !has_device && mc_mono_ms() < end; mc_sleep_ms(10)) {
    mc_player_status_get(player, &st);
    has_device = st.state == MC_PLAYER_PLAYING && st.position_ms > 0;
  }
  if (!has_device) {
    fprintf(stderr, "no audio device; skipping\n");
    mc_player_free(player);
    return SKIP;
  }

  /* A queue given no origin isn't logged, whatever becomes of it. */
  CHECK(mc_player_stop(player) == MC_OK);
  wait_stopped();
  CHECK(take(rows, 0) == 0);

  /* Gapless, across a change of rate: each item a row, played to its end, of what it was
   * started as; the second begins where the first ended. */
  {
    const char *paths[] = {a, b}, *uids[] = {T_SHORT, T_OTHER};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    uint64_t before = mc_wall_ms();
    CHECK(mc_player_set_queue(player, paths, 2, 0, &origin) == MC_OK);
    wait_stopped();
    CHECK(take(rows, 2) == 2);
    CHECK(same(rows[0].track_uid, T_SHORT) && same(rows[1].track_uid, T_OTHER));
    CHECK(rows[0].end == MC_PLAY_END_FINISHED && rows[1].end == MC_PLAY_END_FINISHED);
    CHECK(rows[0].context == MC_PLAY_CONTEXT_ALBUM && same(rows[0].context_uid, ALBUM));
    CHECK(rows[1].context == MC_PLAY_CONTEXT_ALBUM && same(rows[1].context_uid, ALBUM));
    CHECK(rows[0].listened_ms >= 250 && rows[0].listened_ms <= 350);
    CHECK(rows[1].listened_ms >= 250 && rows[1].listened_ms <= 350);
    CHECK(rows[0].started_ms >= (int64_t)before - 1000);
    CHECK(rows[1].started_ms >= rows[0].started_ms + 200);
  }

  /* A file that can't be played has a row saying so, and the queue goes on past it, also
   * when it's in the middle and found by decoding ahead. */
  {
    const char *paths[] = {missing, a, missing, b}, *uids[] = {T_BAD, T_SHORT, T_LONG, T_OTHER};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_PLAYLIST, PLAYLIST};
    CHECK(mc_player_set_queue(player, paths, 4, 0, &origin) == MC_OK);
    wait_stopped();
    CHECK(take(rows, 4) == 4);
    int failed = 0, finished = 0;
    for (int i = 0; i < 4; i++) {
      if (rows[i].end == MC_PLAY_END_FAILED) {
        failed++;
        CHECK(rows[i].listened_ms == 0 && rows[i].started_ms > 0);
        CHECK(same(rows[i].track_uid, T_BAD) || same(rows[i].track_uid, T_LONG));
      } else {
        finished += rows[i].end == MC_PLAY_END_FINISHED;
      }
    }
    CHECK(failed == 2 && finished == 2);
  }

  /* Nothing playable at all: each has its row, and it stops. */
  {
    const char *paths[] = {missing, missing}, *uids[] = {T_BAD, T_LONG};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_UNKNOWN, NULL};
    CHECK(mc_player_set_queue(player, paths, 2, 0, &origin) == MC_OK);
    CHECK(take(rows, 2) == 2);
    CHECK(rows[0].end == MC_PLAY_END_FAILED && rows[1].end == MC_PLAY_END_FAILED);
    CHECK(rows[0].context == MC_PLAY_CONTEXT_UNKNOWN && !rows[0].context_uid[0]);
    wait_stopped();
  }

  /* Skipped: what was heard, not the track's length; the next one starts a row. Next past the
   * last item is a skip too, though it stops. */
  {
    const char *paths[] = {longer, a}, *uids[] = {T_LONG, T_SHORT};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_set_queue(player, paths, 2, 0, &origin) == MC_OK);
    wait_position(300);
    CHECK(mc_player_next(player) == MC_OK);
    mc_sleep_ms(100);
    CHECK(mc_player_next(player) == MC_OK);
    wait_stopped();
    CHECK(take(rows, 2) == 2);
    CHECK(same(rows[0].track_uid, T_LONG) && rows[0].end == MC_PLAY_END_SKIPPED);
    CHECK(rows[0].listened_ms >= 250 && rows[0].listened_ms < 1000);
    CHECK(same(rows[1].track_uid, T_SHORT) && rows[1].end == MC_PLAY_END_SKIPPED);
    CHECK(rows[1].listened_ms < 300);
  }

  /* Seeks are one row. Forward doesn't add what it jumped over; back adds what's heard again,
   * so the time heard can pass the position. */
  {
    const char *paths[] = {longer}, *uids[] = {T_LONG};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_MANUAL, NULL};
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(200);
    CHECK(mc_player_seek_ms(player, 3700) == MC_OK);
    wait_stopped();
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].end == MC_PLAY_END_FINISHED && rows[0].context == MC_PLAY_CONTEXT_MANUAL);
    CHECK(rows[0].listened_ms >= 400 && rows[0].listened_ms < 1200);

    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(500);
    CHECK(mc_player_seek_ms(player, 50) == MC_OK);
    wait_position(250);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].end == MC_PLAY_END_STOPPED);
    CHECK(rows[0].listened_ms >= 600 && rows[0].listened_ms < 1500);

    /* Back to the start is starting it again, so a row of its own. */
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(200);
    CHECK(mc_player_seek_ms(player, 0) == MC_OK);
    mc_sleep_ms(100);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 2) == 2);
    CHECK(rows[0].end == MC_PLAY_END_SKIPPED && rows[1].end == MC_PLAY_END_STOPPED);
    CHECK(rows[1].started_ms >= rows[0].started_ms);
  }

  /* Paused time isn't heard. */
  {
    const char *paths[] = {longer}, *uids[] = {T_LONG};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(200);
    CHECK(mc_player_pause(player) == MC_OK);
    mc_sleep_ms(1000);
    CHECK(mc_player_resume(player) == MC_OK);
    wait_position(350);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].listened_ms >= 300 && rows[0].listened_ms < 900);
  }

  /* Replaced: the old row is the old queue's, track and context, though it's only closed
   * once the new queue has been given. An empty queue ends a play the same way. */
  {
    const char *paths[] = {longer}, *uids[] = {T_LONG};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(200);
    const char *next_paths[] = {a}, *next_uids[] = {T_SHORT};
    mc_play_origin next = {next_uids, MC_PLAY_CONTEXT_PLAYLIST, PLAYLIST};
    CHECK(mc_player_set_queue(player, next_paths, 1, 0, &next) == MC_OK);
    wait_stopped();
    CHECK(take(rows, 2) == 2);
    CHECK(same(rows[0].track_uid, T_LONG) && rows[0].end == MC_PLAY_END_REPLACED);
    CHECK(rows[0].context == MC_PLAY_CONTEXT_ALBUM && same(rows[0].context_uid, ALBUM));
    CHECK(rows[0].listened_ms >= 150);
    CHECK(same(rows[1].track_uid, T_SHORT) && rows[1].end == MC_PLAY_END_FINISHED);
    CHECK(rows[1].context == MC_PLAY_CONTEXT_PLAYLIST && same(rows[1].context_uid, PLAYLIST));

    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(100);
    CHECK(mc_player_load_queue(player, NULL, 0, 0, NULL) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].end == MC_PLAY_END_REPLACED);
  }

  /* A queue loaded but never played has no row; played from there, it has. */
  {
    const char *paths[] = {a}, *uids[] = {T_SHORT};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_load_queue(player, paths, 1, 0, &origin) == MC_OK);
    CHECK(take(rows, 0) == 0);
    CHECK(mc_player_resume(player) == MC_OK);
    wait_position(1);
    wait_stopped();
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].end == MC_PLAY_END_FINISHED && same(rows[0].context_uid, ALBUM));
  }

  /* Started and paused before a frame of it is heard, then given up on: still a row, which
   * says how little was heard. A stopped item played again is a new row. */
  {
    const char *paths[] = {longer}, *uids[] = {T_LONG};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    CHECK(mc_player_pause(player) == MC_OK);
    mc_sleep_ms(200);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].end == MC_PLAY_END_STOPPED && rows[0].listened_ms < 100 && rows[0].started_ms > 0);

    CHECK(mc_player_resume(player) == MC_OK);
    wait_position(100);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].end == MC_PLAY_END_STOPPED && rows[0].listened_ms >= 50);
  }

  /* Started shuffled with nothing picked is the shuffle context, still saying what of; with
   * an item picked, or shuffled later, it's what the frontend said. */
  {
    const char *paths[] = {a}, *uids[] = {T_SHORT};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
    CHECK(mc_player_set_queue(player, paths, 1, -1, &origin) == MC_OK);
    wait_position(1);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].context == MC_PLAY_CONTEXT_SHUFFLE && same(rows[0].context_uid, ALBUM));
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(1);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].context == MC_PLAY_CONTEXT_ALBUM);
    mc_play_origin radio = {uids, MC_PLAY_CONTEXT_RADIO, NULL};
    CHECK(mc_player_set_queue(player, paths, 1, -1, &radio) == MC_OK);
    wait_position(1);
    CHECK(mc_player_stop(player) == MC_OK);
    CHECK(take(rows, 1) == 1);
    CHECK(rows[0].context == MC_PLAY_CONTEXT_RADIO);
    CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  }

  /* An item with no uid has no row; the rest of its queue has. */
  {
    const char *paths[] = {a, b}, *uids[] = {"", T_OTHER};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_UNKNOWN, NULL};
    CHECK(mc_player_set_queue(player, paths, 2, 0, &origin) == MC_OK);
    wait_stopped();
    CHECK(take(rows, 1) == 1);
    CHECK(same(rows[0].track_uid, T_OTHER));
  }

  /* The app closing mid-track: there and then, with no event to wait for, and nothing more
   * once it has. If it plays on, what's heard after is a new row. */
  {
    const char *paths[] = {longer}, *uids[] = {T_LONG};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_ALBUM, ALBUM};
    CHECK(mc_player_set_queue(player, paths, 1, 0, &origin) == MC_OK);
    wait_position(300);
    CHECK(mc_player_end_play(player) == MC_OK);
    CHECK(mc_player_take_ended(player, &rows[0]) == MC_OK);
    CHECK(mc_player_take_ended(player, &rows[1]) == MC_ERR_NOT_FOUND);
    CHECK(rows[0].end == MC_PLAY_END_STOPPED && same(rows[0].track_uid, T_LONG));
    CHECK(rows[0].listened_ms >= 250 && rows[0].listened_ms < 1000);
    CHECK(mc_player_end_play(player) == MC_OK); /* twice is no more rows, or very nearly */
    mc_sleep_ms(200);
    CHECK(mc_player_stop(player) == MC_OK);
    int n = take(rows, 1);
    CHECK(n == 1 || n == 2);
    CHECK(rows[n - 1].end == MC_PLAY_END_STOPPED && rows[n - 1].listened_ms >= 100);

    CHECK(mc_player_end_play(player) == MC_OK); /* stopped: nothing under way */
    CHECK(mc_player_take_ended(player, &rows[0]) == MC_ERR_NOT_FOUND);
  }

  /* Nobody taking them: the newest are kept, and nothing is overrun. */
  {
    const char *paths[] = {missing, a}, *uids[] = {T_BAD, T_SHORT};
    mc_play_origin origin = {uids, MC_PLAY_CONTEXT_UNKNOWN, NULL};
    for (int i = 0; i < 70; i++) {
      CHECK(mc_player_load_queue(player, paths, 2, 0, &origin) == MC_OK);
      CHECK(mc_player_resume(player) == MC_OK);
      mc_sleep_ms(3);
    }
    CHECK(mc_player_stop(player) == MC_OK);
    wait_stopped();
    mc_sleep_ms(100);
    int n = 0;
    while (mc_player_take_ended(player, &rows[0]) == MC_OK) n++;
    CHECK(n > 0 && n <= 64);
  }

  CHECK(mc_player_end_play(NULL) == MC_ERR_INVALID_ARG);
  CHECK(mc_player_take_ended(NULL, &rows[0]) == MC_ERR_INVALID_ARG);
  CHECK(mc_player_take_ended(player, NULL) == MC_ERR_INVALID_ARG);

  mc_player_free(player);
  remove(a);
  remove(b);
  remove(longer);
  free(a);
  free(b);
  free(longer);
  free(missing);
  return TEST_RESULT();
}
