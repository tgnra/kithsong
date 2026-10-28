/* The shuffled play order (playback/shuffle.c): the Miller Shuffle gives each item once, and
 * the order goes on in rounds that never start with the item the last one ended with. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "playback/shuffle.h"
#include "test.h"

/* Whether mc_miller_shuffle() gives every place 0..n-1 exactly once for seed. */
static int is_permutation(uint32_t n, uint32_t seed) {
  unsigned char *seen = calloc(n, 1);
  int ok = seen != NULL;
  for (uint32_t i = 0; ok && i < n; i++) {
    uint32_t j = mc_miller_shuffle(i, seed, n);
    if (j >= n || seen[j]) ok = 0;
    else seen[j] = 1;
  }
  free(seen);
  return ok;
}

int main(void) {
  static const uint32_t seeds[] = {0, 1, 42, 0x9E3779B9u, 0xFFFFFFFFu, 123456789u};
  int nseeds = (int)(sizeof seeds / sizeof *seeds);

  /* Every size a playlist or album is likely to have, and sizes that trip the algorithm's
   * guards against a modulus sharing a factor with one of its primes. */
  int all = 1;
  for (uint32_t n = 1; n <= 600; n++)
    for (int s = 0; s < nseeds; s++) all &= is_permutation(n, seeds[s]);
  CHECK(all);
  static const uint32_t large[] = {65536, 65537, 93251, 98323, 186501, 186502, 294967, 1000003};
  for (size_t i = 0; i < sizeof large / sizeof *large; i++) {
    CHECK(is_permutation(large[i], seeds[2]));
    CHECK(is_permutation(large[i], seeds[3]));
  }

  /* It does shuffle: not the queue's order, and another seed gives another shuffle. */
  int in_order = 1, same = 1;
  for (uint32_t i = 0; i < 52; i++) {
    in_order &= mc_miller_shuffle(i, 7, 52) == i;
    same &= mc_miller_shuffle(i, 7, 52) == mc_miller_shuffle(i, 8, 52);
  }
  CHECK(!in_order);
  CHECK(!same);

  /* No items, or before the start: none. One item: always it. */
  mc_shuffle sh;
  mc_shuffle_init(&sh, 0, 1, -1);
  CHECK(mc_shuffle_at(&sh, 0) == -1);
  mc_shuffle_init(&sh, 1, 1, -1);
  CHECK(mc_shuffle_at(&sh, -1) == -1);
  CHECK(mc_shuffle_at(&sh, 0) == 0 && mc_shuffle_at(&sh, 1) == 0 && mc_shuffle_at(&sh, 5) == 0);

  /* Rounds: each plays every item once, and none starts with the item before it. With a
   * first item, the first round starts with it. */
  int rounds_ok = 1, no_repeat = 1, first_ok = 1;
  for (int n = 2; n <= 40; n++) {
    for (int s = 0; s < nseeds; s++) {
      int first = s % 2 ? (int)(seeds[s] % (uint32_t)n) : -1;
      mc_shuffle_init(&sh, n, seeds[s], first);
      if (first >= 0) first_ok &= mc_shuffle_at(&sh, 0) == first;
      int prev = -1;
      for (int r = 0; r < 6; r++) {
        unsigned char seen[40] = {0};
        for (int k = 0; k < n; k++) {
          int item = mc_shuffle_at(&sh, r * n + k);
          if (item < 0 || item >= n || seen[item]) rounds_ok = 0;
          else seen[item] = 1;
          if (item == prev) no_repeat = 0;
          prev = item;
        }
      }
    }
  }
  CHECK(rounds_ok);
  CHECK(no_repeat);
  CHECK(first_ok);

  /* The same place gives the same item whichever way it's reached: going back (previous)
   * after going on works the rounds out again. */
  enum { N = 7, PLACES = 10 * N };
  int forward[PLACES];
  mc_shuffle_init(&sh, N, 99, 3);
  for (int pos = 0; pos < PLACES; pos++) forward[pos] = mc_shuffle_at(&sh, pos);
  int stable = 1;
  for (int pos = PLACES - 1; pos >= 0; pos--) stable &= mc_shuffle_at(&sh, pos) == forward[pos];
  mc_shuffle_init(&sh, N, 99, 3);
  stable &= mc_shuffle_at(&sh, PLACES - 1) == forward[PLACES - 1];
  CHECK(stable);

  /* Later rounds are shuffled differently from the first. */
  mc_shuffle_init(&sh, 20, 5, -1);
  int differs = 0;
  for (int k = 0; k < 20; k++) differs |= mc_shuffle_at(&sh, k) != mc_shuffle_at(&sh, 20 + k);
  CHECK(differs);

  return TEST_RESULT();
}
