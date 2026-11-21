/* The player's output tap against the real audio device. What it gives back is what plays,
 * as 16-bit stereo at the device's rate, before the volume and mute; nothing comes while
 * paused; and it can be started and stopped while the audio thread writes to it. The checks
 * that need no device run first; the rest are skipped (exit 77) without one. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "test.h"
#include "util/platform.h"

#define SKIP 77
#define LEVEL 8192 /* the WAV's every sample: a quarter of full scale */

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A 16-bit stereo PCM WAV whose every sample is LEVEL: a flat line, which comes through the
 * device's rate unchanged whatever it is. */
static void write_wav(const char *path, unsigned rate, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return;
  uint32_t frames = rate * ms / 1000, bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 2);
  put32(f, rate);
  put32(f, rate * 4);
  put16(f, 4);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames * 2; i++) put16(f, LEVEL);
  fclose(f);
}

/* Reads the tap for ms, counting the frames and how many samples were near LEVEL. */
static int drain(mc_player *player, unsigned ms, int *rate, int *level) {
  static int16_t buf[2 * 4096];
  int total = 0;
  *level = 0;
  for (uint64_t end = mc_mono_ms() + ms; mc_mono_ms() < end; mc_sleep_ms(10)) {
    int n = 0;
    if (mc_player_tap_read(player, buf, 4096, &n, rate) != MC_OK) return -1;
    for (int i = 0; i < n * 2; i++) *level += buf[i] > LEVEL - 400 && buf[i] < LEVEL + 400;
    total += n;
  }
  return total;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";
  size_t len = strlen(dir) + 32;
  char *path = malloc(len);
  snprintf(path, len, "%s/tap.wav", dir);
  write_wav(path, 44100, 4000);

  mc_player *player = NULL;
  CHECK(mc_player_new(NULL, NULL, &player) == MC_OK);
  int16_t buf[64];
  int n = -1, rate = -1;

  /* Not started, there's nothing to read; started, nothing until something plays. */
  CHECK(mc_player_tap_read(player, buf, 32, &n, &rate) == MC_ERR_INVALID_ARG && n == 0 && rate == 0);
  CHECK(mc_player_tap_stop(player) == MC_OK);
  CHECK(mc_player_tap_start(player) == MC_OK);
  CHECK(mc_player_tap_read(player, buf, 32, &n, &rate) == MC_OK && n == 0 && rate == 0);
  CHECK(mc_player_tap_read(player, buf, -1, &n, &rate) == MC_ERR_INVALID_ARG);
  CHECK(mc_player_tap_read(player, NULL, 32, &n, &rate) == MC_ERR_INVALID_ARG);
  CHECK(mc_player_tap_start(player) == MC_OK); /* again: empties it */

  /* Played quiet and muted, the tap still has the sound at its own level. */
  CHECK(mc_player_set_volume(player, 30) == MC_OK);
  CHECK(mc_player_set_muted(player, 1) == MC_OK);
  const char *one[] = {path};
  CHECK(mc_player_set_queue(player, one, 1, 0, NULL) == MC_OK);
  mc_player_status st;
  int has_device = 0;
  for (uint64_t end = mc_mono_ms() + 3000; !has_device && mc_mono_ms() < end; mc_sleep_ms(10)) {
    mc_player_status_get(player, &st);
    has_device = st.state == MC_PLAYER_PLAYING && st.position_ms > 0;
  }
  if (!has_device) {
    fprintf(stderr, "no audio device; skipping\n");
    mc_player_free(player); /* with the tap still started: freed with the player */
    remove(path);
    free(path);
    return test_failures ? 1 : SKIP;
  }

  int level = 0;
  int got = drain(player, 1000, &rate, &level);
  CHECK(rate >= 8000 && rate <= 384000);
  /* About a second's worth, as fast as the device plays it; the start may have been read already. */
  CHECK(got > rate / 2 && got < rate * 2);
  CHECK(level > got); /* most samples of both channels */

  /* Paused, nothing more comes once what was played is read. */
  CHECK(mc_player_pause(player) == MC_OK);
  mc_sleep_ms(200);
  drain(player, 50, &rate, &level);
  CHECK(drain(player, 300, &rate, &level) == 0);

  /* Started and stopped over and over while it plays: the audio thread never writes into a
   * buffer that's gone (run it under a sanitizer to see). */
  CHECK(mc_player_resume(player) == MC_OK);
  for (int i = 0; i < 200; i++) {
    CHECK(mc_player_tap_start(player) == MC_OK);
    mc_player_tap_read(player, buf, 32, &n, &rate);
    if (i % 2) CHECK(mc_player_tap_stop(player) == MC_OK);
    mc_sleep_ms(1);
  }
  CHECK(mc_player_tap_stop(player) == MC_OK);
  CHECK(mc_player_tap_read(player, buf, 32, &n, &rate) == MC_ERR_INVALID_ARG);

  mc_player_free(player);
  remove(path);
  free(path);
  return TEST_RESULT();
}
