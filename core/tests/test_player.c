/* The player against the real audio device, with silent WAVs. Skipped (exit 77) when there
 * is no device. Checks the queue moves on by itself across sample rates, skips files it
 * can't play, stops at the end, seeks while paused, and counts plays. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "test.h"
#include "util/platform.h"

#define SKIP 77

static mc_mutex lock;
static int errors, events, played;

static void on_event(void *user, mc_player_event event) {
  (void)user;
  mc_mutex_lock(&lock);
  events++;
  if (event == MC_PLAYER_EVENT_ERROR) errors++;
  if (event == MC_PLAYER_EVENT_PLAYED) played++;
  mc_mutex_unlock(&lock);
}

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

/* Polls until the player is stopped or ms pass; records each queue item seen playing. */
static int watch(mc_player *player, int *seen, int max_seen, unsigned ms) {
  int n = 0;
  uint64_t end = mc_mono_ms() + ms;
  mc_player_status st;
  while (mc_mono_ms() < end) {
    mc_player_status_get(player, &st);
    if (st.state == MC_PLAYER_PLAYING && (n == 0 || seen[n - 1] != st.index) && n < max_seen) seen[n++] = st.index;
    if (st.state == MC_PLAYER_STOPPED && n) break;
    mc_sleep_ms(5);
  }
  return n;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  mc_mutex_init(&lock);
  char *a = join(dir, "player_a.wav"), *b = join(dir, "player_b.wav"), *bad = join(dir, "player_bad.ogg"),
       *missing = join(dir, "player_missing.wav"), *longer = join(dir, "player_long.wav");
  write_wav(a, 44100, 2, 300);
  write_wav(b, 48000, 1, 300);
  write_wav(longer, 22050, 2, 3000);
  FILE *f = mc_fopen(bad, "wb");
  if (f) fclose(f);

  mc_player *player = NULL;
  CHECK(mc_player_new(on_event, NULL, &player) == MC_OK);
  mc_player_status st;
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_STOPPED && st.index == -1 && st.count == 0);

  /* Loaded, a queue waits stopped on its item (no device needed), and the order runs on
   * from it: to the end in order, and shuffled, in rounds that each hold every item once,
   * the next not starting with the item the last ended on. */
  const char *loaded[] = {a, b, longer};
  int order[6];
  CHECK(mc_player_order(player, 0, 1, order) == MC_OK && order[0] == -1);
  CHECK(mc_player_load_queue(player, loaded, 3, 1) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_STOPPED && st.index == 1 && st.count == 3);
  CHECK(mc_player_order(player, -2, 5, order) == MC_OK);
  CHECK(order[0] == -1 && order[1] == 0 && order[2] == 1 && order[3] == 2 && order[4] == -1);
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  CHECK(mc_player_load_queue(player, loaded, 3, -1) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_STOPPED && st.index >= 0 && st.index < 3);
  CHECK(mc_player_order(player, 0, 6, order) == MC_OK);
  CHECK(order[0] == st.index && order[0] != order[1] && order[1] != order[2] && order[0] != order[2]);
  CHECK(order[3] != order[2] && order[3] != order[4] && order[4] != order[5] && order[3] != order[5]);
  CHECK(mc_player_order(player, -1, 1, order) == MC_OK && order[0] == -1);
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);

  /* Is there a device? The position only moves once it pulls audio; the state alone reads
   * PLAYING as soon as a start is asked for. Short of half the track, so no play counts. */
  const char *one[] = {longer};
  CHECK(mc_player_set_queue(player, one, 1, 0) == MC_OK);
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
  mc_mutex_lock(&lock);
  errors = events = played = 0;
  mc_mutex_unlock(&lock);

  /* a, two that can't be played, then b at another rate and channel count. */
  const char *queue[] = {a, bad, missing, b};
  CHECK(mc_player_set_queue(player, queue, 4, 0) == MC_OK);
  int seen[8];
  int n = watch(player, seen, 8, 5000);
  mc_player_status_get(player, &st);
  CHECK(n == 2 && seen[0] == 0 && seen[1] == 3);
  CHECK(st.state == MC_PLAYER_STOPPED && st.index == 0); /* played out: back at the start */
  mc_mutex_lock(&lock);
  CHECK(errors >= 1); /* the two skips may be one event */
  CHECK(events > 0);
  mc_mutex_unlock(&lock);

  /* Seeking while paused stays paused, at the new position. */
  CHECK(mc_player_set_queue(player, one, 1, 0) == MC_OK);
  mc_player_pause(player);
  CHECK(mc_player_seek_ms(player, 2000) == MC_OK);
  mc_sleep_ms(200);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PAUSED && st.index == 0);
  CHECK(st.position_ms == 2000);
  CHECK(st.duration_ms == 3000);

  /* Resuming carries on from there to the end. */
  mc_player_resume(player);
  mc_sleep_ms(300);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_PLAYING && st.position_ms > 2000 && st.position_ms <= 3000);
  n = watch(player, seen, 8, 3000);
  mc_player_status_get(player, &st);
  CHECK(st.state == MC_PLAYER_STOPPED);

  /* Plays count once half the track has been heard: both of these, in order. Replacing the
   * queue dropped the plays of the first one. */
  const char *twice[] = {a, a};
  int64_t before = (int64_t)mc_wall_ms();
  CHECK(mc_player_set_queue(player, twice, 2, 0) == MC_OK);
  n = watch(player, seen, 8, 3000);
  int index = -1;
  int64_t started = 0;
  CHECK(mc_player_take_played(player, &index, &started) == MC_OK && index == 0);
  CHECK(started >= before - 50 && started <= (int64_t)mc_wall_ms());
  int64_t first = started;
  CHECK(mc_player_take_played(player, &index, &started) == MC_OK && index == 1);
  CHECK(started >= first + 250); /* the second began after the first's 300 ms */
  CHECK(mc_player_take_played(player, &index, &started) == MC_ERR_NOT_FOUND);
  mc_mutex_lock(&lock);
  CHECK(played >= 1);
  mc_mutex_unlock(&lock);

  /* Seeking past half doesn't make a play count: it's what was heard that counts. */
  CHECK(mc_player_set_queue(player, one, 1, 0) == MC_OK);
  CHECK(mc_player_seek_ms(player, 2800) == MC_OK);
  n = watch(player, seen, 8, 3000);
  CHECK(mc_player_take_played(player, &index, &started) == MC_ERR_NOT_FOUND);

  /* Shuffled, the queue never ends: each round plays every item once, and the next round
   * doesn't start with the one just played. */
  const char *three[] = {a, b, a};
  CHECK(mc_player_set_shuffle(player, 1) == MC_OK);
  CHECK(mc_player_set_queue(player, three, 3, -1) == MC_OK);
  n = watch(player, seen, 8, 2500);
  mc_player_status_get(player, &st);
  CHECK(st.shuffle && st.state == MC_PLAYER_PLAYING);
  CHECK(n >= 6);
  if (n >= 6) {
    CHECK(seen[0] != seen[1] && seen[1] != seen[2] && seen[0] != seen[2]);
    CHECK(seen[3] != seen[4] && seen[4] != seen[5] && seen[3] != seen[5]);
  }
  /* Turned off, it plays on in order to the end of the queue, and stops there. (These are
   * short enough that the items after the current one are buffered already, so the order
   * changes after them.) */
  CHECK(mc_player_set_shuffle(player, 0) == MC_OK);
  n = watch(player, seen, 8, 3000);
  mc_player_status_get(player, &st);
  CHECK(!st.shuffle && st.state == MC_PLAYER_STOPPED);
  CHECK(n && seen[n - 1] == 2);

  /* Volume: clamped, and kept apart from muted. */
  CHECK(st.volume == 100 && !st.muted);
  CHECK(mc_player_set_volume(player, 40) == MC_OK);
  CHECK(mc_player_set_muted(player, 1) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.volume == 40 && st.muted);
  CHECK(mc_player_set_volume(player, -5) == MC_OK);
  CHECK(mc_player_set_muted(player, 0) == MC_OK);
  mc_player_status_get(player, &st);
  CHECK(st.volume == 0 && !st.muted);

  mc_player_free(player);
  mc_remove(a);
  mc_remove(b);
  mc_remove(bad);
  mc_remove(longer);
  free(a);
  free(b);
  free(bad);
  free(missing);
  free(longer);
  mc_mutex_destroy(&lock);
  return TEST_RESULT();
}
