/* Hash-ahead (sync/hashahead.h): the worker hashes only so far past the last file taken, a
 * file taken gets the hash of its bytes, taking one passes over those before it (unhashed
 * ones are never hashed), a file it can't read or one not queued is the caller's to hash, and
 * finish reports what was hashed but never taken. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kithcore/kithcore.h"
#include "sync/hashahead.h"
#include "tagger/dupes.h"
#include "test.h"
#include "util/platform.h"

#define WAIT_MS 60000
#define NFILES 10

static char g_paths[NFILES][1100];
static char g_keys[NFILES][8];
static char g_sha[NFILES][MC_SHA256_STRLEN];
static int g_unused[NFILES + 1];

static int write_noise(const char *path, int seed, size_t bytes) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t x = 2463534242u + (uint32_t)seed;
  for (size_t i = 0; i < bytes; i++) {
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    fputc((int)(x & 0xff), f);
  }
  return fclose(f) == 0;
}

/* Waits until the worker has hashed at least n files; returns how many it has. */
static int wait_hashed(mc_hash_ahead *ha, int n) {
  int hashed = 0, taken = 0;
  for (uint64_t end = mc_mono_ms() + WAIT_MS; mc_mono_ms() < end; mc_sleep_ms(5)) {
    mc_hash_ahead_stats(ha, &hashed, &taken);
    if (hashed >= n) break;
  }
  return hashed;
}

static void on_unused(void *user, const char *key, const char *path,
                      const mc_hash_ahead_result *r) {
  (void)user;
  int i = atoi(key + 1);
  if (i < 0 || i >= NFILES || strcmp(path, g_paths[i]) != 0 || strcmp(r->sha, g_sha[i]) != 0)
    i = NFILES; /* counted as wrong */
  g_unused[i]++;
}

int main(int argc, char **argv) {
  const char *scratch = argc > 1 ? argv[1] : ".";
  CHECK(mc_init() == MC_OK);
  char root[1024];
  snprintf(root, sizeof root, "%s/test_hash_ahead.d", scratch);
  mc_remove_tree(root);
  CHECK(mc_mkdirs(root) == MC_OK);
  for (int i = 0; i < NFILES; i++) {
    snprintf(g_paths[i], sizeof g_paths[i], "%s/%d.bin", root, i);
    snprintf(g_keys[i], sizeof g_keys[i], "k%d", i);
    if (i == 4) continue; /* never written: it can't be hashed */
    CHECK(write_noise(g_paths[i], i, 200000 + 1000 * i));
    CHECK(mc_sha256_file(g_paths[i], g_sha[i]) == MC_OK);
  }

  mc_hash_ahead *ha = NULL;
  CHECK(mc_hash_ahead_new(2, &ha) == MC_OK);
  if (!ha) return TEST_RESULT();
  for (int i = 0; i < NFILES; i++) CHECK(mc_hash_ahead_queue(ha, g_keys[i], g_paths[i]) == MC_OK);

  /* Two hashed before anything is taken, and no more. */
  CHECK(wait_hashed(ha, 2) == 2);
  mc_sleep_ms(100);
  int hashed, taken;
  mc_hash_ahead_stats(ha, &hashed, &taken);
  CHECK(hashed == 2 && taken == 0);

  /* Taken: its hash, size and mtime. The worker moves on to 2. */
  mc_hash_ahead_result r;
  int64_t size = 0, mtime = 0;
  CHECK(mc_file_info(g_paths[0], &size, &mtime) == MC_OK);
  CHECK(mc_hash_ahead_take(ha, "k0", &r) == 1);
  CHECK(!strcmp(r.sha, g_sha[0]) && r.size == size && r.mtime == mtime);
  CHECK(wait_hashed(ha, 3) == 3);
  /* Not again, and not one never queued. */
  CHECK(mc_hash_ahead_take(ha, "k0", &r) == 0);
  CHECK(mc_hash_ahead_take(ha, "nope", &r) == 0);

  /* 5, past the window: the worker hadn't started on it, so it's the caller's. 1 and 2 were
   * hashed and are passed over; 3 and 4 never are. Then 6 and 7 are hashed. */
  CHECK(mc_hash_ahead_take(ha, "k5", &r) == 0);
  CHECK(wait_hashed(ha, 5) == 5);
  CHECK(mc_hash_ahead_take(ha, "k6", &r) == 1 && !strcmp(r.sha, g_sha[6]));
  /* 8 is hashed now too (7 already was); 9 next. */
  CHECK(wait_hashed(ha, 6) == 6);
  CHECK(mc_hash_ahead_take(ha, "k1", &r) == 0); /* passed over */
  mc_hash_ahead_stats(ha, &hashed, &taken);
  CHECK(taken == 2);

  /* Unreadable: the caller's. Hashes nothing past its window meanwhile. */
  {
    mc_hash_ahead *hb = NULL;
    CHECK(mc_hash_ahead_new(1, &hb) == MC_OK);
    if (hb) {
      CHECK(mc_hash_ahead_queue(hb, "k4", g_paths[4]) == MC_OK);
      CHECK(mc_hash_ahead_queue(hb, "k3", g_paths[3]) == MC_OK);
      mc_sleep_ms(100);
      CHECK(mc_hash_ahead_take(hb, "k4", &r) == 0);
      CHECK(wait_hashed(hb, 1) == 1);
      CHECK(mc_hash_ahead_take(hb, "k3", &r) == 1 && !strcmp(r.sha, g_sha[3]));
      mc_hash_ahead_free(hb);
    }
  }

  /* Hashed but never taken: 1, 2, 7 and 8 (9 is past the window once 8 is done). */
  mc_hash_ahead_finish(ha, on_unused, NULL);
  for (int i = 0; i <= NFILES; i++)
    CHECK(g_unused[i] == (i == 1 || i == 2 || i == 7 || i == 8));
  CHECK(mc_hash_ahead_take(ha, "k9", &r) == 0);
  mc_hash_ahead_free(ha);
  mc_hash_ahead_free(NULL);

  mc_remove_tree(root);
  return TEST_RESULT();
}
