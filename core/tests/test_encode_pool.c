/* The encode pool (sync/encode.h): several encodes at once and each file once, a FILE_REQ's
 * encode joining the job queued ahead for it, an owner's queued jobs dropped (but for one a
 * PREPARE_REQ wants too), the cache trim held off while a file is held, and stop. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "sync/encode.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A 16-bit stereo 44.1 kHz WAV of a tone. */
static int write_wav(const char *path, unsigned ms, double pitch) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t frames = 44100 * ms / 1000, bytes = frames * 4;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, 2);
  put32(f, 44100);
  put32(f, 44100 * 4);
  put16(f, 4);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++) {
    double v = 0.4 * sin(2 * 3.14159265358979 * pitch * i / 44100);
    put16(f, (unsigned)(uint16_t)(int16_t)(v * 32767));
    put16(f, (unsigned)(uint16_t)(int16_t)(v * 0.8 * 32767));
  }
  return fclose(f) == 0;
}

/* Waits until the pool has encoded n files in all. */
static int wait_encoded(mc_encode_pool *p, int n) {
  mc_encode_stats st;
  for (int waited = 0; waited < WAIT_MS; waited += 10) {
    mc_encode_pool_stats(p, &st);
    if (st.encoded >= n) return 1;
    mc_sleep_ms(10);
  }
  return 0;
}

static char g_uids[8][MC_UID_STRLEN];
static char g_srcs[8][1100];

static void cached(const char *cache, int i, char *out, size_t cap) {
  snprintf(out, cap, "%s%c%s%copus-64.opus", cache, MC_PATH_SEP, g_uids[i], MC_PATH_SEP);
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024], cache[1100], path[1300];
  snprintf(root, sizeof root, "%s/test_encode_pool.d", scratch);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  for (int i = 0; i < 8; i++) {
    CHECK(mc_uid_new(g_uids[i]) == MC_OK);
    snprintf(g_srcs[i], sizeof g_srcs[i], "%s/%d.wav", root, i);
    CHECK(write_wav(g_srcs[i], i == 7 ? 20000 : 3000, 220 + 55 * i));
  }

  /* Three at once: six queued in the background, the last of them asked for now, which
   * joins its job instead of encoding it again. Each is encoded once. */
  {
    snprintf(cache, sizeof cache, "%s/cache1", root);
    mc_encode_pool *p = NULL;
    CHECK(mc_encode_pool_new(cache, INT64_MAX, &p) == MC_OK);
    if (!p) return TEST_RESULT();
    mc_encode_pool_set_size(p, 3);
    for (int i = 0; i < 6; i++)
      CHECK(mc_encode_pool_queue(p, g_uids[i], g_srcs[i], 64, MC_ENCODE_BACKGROUND, NULL) == MC_OK);
    /* Asked for again: still one job. */
    CHECK(mc_encode_pool_queue(p, g_uids[2], g_srcs[2], 64, MC_ENCODE_AHEAD, p) == MC_OK);
    mc_encode_pool_hold(p);
    char *out = NULL;
    CHECK(mc_encode_pool_encode(p, g_uids[5], g_srcs[5], 64, &out) == MC_OK);
    cached(cache, 5, path, sizeof path);
    CHECK(out && !strcmp(out, path) && mc_file_exists(out));
    free(out);
    mc_encode_pool_release(p);
    CHECK(wait_encoded(p, 6));
    mc_encode_stats st;
    mc_encode_pool_stats(p, &st);
    CHECK(st.encoded == 6 && st.unasked == 0);
    CHECK(st.peak >= 2 && st.peak <= 3);
    for (int i = 0; i < 6; i++) {
      cached(cache, i, path, sizeof path);
      CHECK(mc_file_exists(path));
    }
    /* Asked for once it's in the cache: no new encode. */
    CHECK(mc_encode_pool_encode(p, g_uids[0], g_srcs[0], 64, &out) == MC_OK);
    free(out);
    mc_encode_pool_stats(p, &st);
    CHECK(st.encoded == 6 && st.unasked == 1);
    mc_encode_pool_free(p);
  }

  /* Dropping an owner: one worker, busy with a long encode, and three queued ahead behind
   * it. The owner's turn ends: its jobs go, but for the one a PREPARE_REQ also wants. */
  {
    snprintf(cache, sizeof cache, "%s/cache2", root);
    mc_encode_pool *p = NULL;
    CHECK(mc_encode_pool_new(cache, INT64_MAX, &p) == MC_OK);
    if (!p) return TEST_RESULT();
    int owner;
    CHECK(mc_encode_pool_queue(p, g_uids[7], g_srcs[7], 64, MC_ENCODE_BACKGROUND, NULL) == MC_OK);
    mc_encode_stats st = {0};
    for (int waited = 0; !st.peak && waited < WAIT_MS; waited += 1) {
      mc_encode_pool_stats(p, &st); /* until the long one runs, so the rest queue */
      if (!st.peak) mc_sleep_ms(1);
    }
    for (int i = 0; i < 3; i++)
      CHECK(mc_encode_pool_queue(p, g_uids[i], g_srcs[i], 64, MC_ENCODE_AHEAD, &owner) == MC_OK);
    CHECK(mc_encode_pool_queue(p, g_uids[1], g_srcs[1], 64, MC_ENCODE_BACKGROUND, NULL) == MC_OK);
    mc_encode_pool_drop(p, &owner);
    CHECK(wait_encoded(p, 2));
    mc_encode_pool_stop(p); /* nothing else was queued: waits for nothing */
    mc_encode_pool_stats(p, &st);
    CHECK(st.encoded == 2);
    for (int i = 0; i < 3; i++) {
      cached(cache, i, path, sizeof path);
      CHECK(mc_file_exists(path) == (i == 1));
    }
    cached(cache, 7, path, sizeof path);
    CHECK(mc_file_exists(path));
    /* Stopped: jobs are refused until it resumes. */
    char *out = NULL;
    CHECK(mc_encode_pool_encode(p, g_uids[3], g_srcs[3], 64, &out) != MC_OK && !out);
    CHECK(mc_encode_pool_queue(p, g_uids[3], g_srcs[3], 64, MC_ENCODE_BACKGROUND, NULL) != MC_OK);
    mc_encode_pool_resume(p);
    CHECK(mc_encode_pool_encode(p, g_uids[3], g_srcs[3], 64, &out) == MC_OK && out);
    free(out);
    mc_encode_pool_free(p);
  }

  /* The trim (to nothing, here) waits while a file is held: the one just encoded stays
   * until it has been "sent". */
  {
    snprintf(cache, sizeof cache, "%s/cache3", root);
    mc_encode_pool *p = NULL;
    CHECK(mc_encode_pool_new(cache, 0, &p) == MC_OK);
    if (!p) return TEST_RESULT();
    mc_encode_pool_hold(p);
    char *out = NULL;
    CHECK(mc_encode_pool_encode(p, g_uids[4], g_srcs[4], 64, &out) == MC_OK);
    mc_sleep_ms(50);
    mc_encode_stats st;
    mc_encode_pool_stats(p, &st);
    CHECK(out && mc_file_exists(out) && st.trims == 0);
    mc_encode_pool_release(p);
    mc_encode_pool_stats(p, &st);
    CHECK(out && !mc_file_exists(out) && st.trims == 1);
    free(out);
    mc_encode_pool_free(p);
  }

  mc_remove_tree(root);
  return TEST_RESULT();
}
