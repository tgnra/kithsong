#include "playback/shuffle.h"

/* MillerShuffle_Large() from https://github.com/RondeSC/Miller_Shuffle_Algo (MillerShuffle.c,
 * January 2026), Copyright 2022 Ronald R. Miller, under the Apache License 2.0
 * (http://www.apache.org/licenses/LICENSE-2.0; see LICENSES.md). Changed from the original:
 * the randomizing values it keeps in statics between calls, for speed, are worked out on
 * every call instead, so it's safe from any thread, and n = 0 (for 2^32 items) isn't taken. */
uint32_t mc_miller_shuffle(uint32_t inx, uint32_t shuffle_id, uint32_t n) {
  if (n <= 1) return 0;
  uint64_t nlim = n;
  uint64_t p1 = 98323, p2 = 93251, p3 = 66107; /* primes; 64-bit so every product is */
  uint32_t mix = shuffle_id ^ (uint32_t)(19 * (inx / nlim)); /* places past n reach later shuffles */
  uint32_t si = (uint32_t)(((uint64_t)inx + mix % nlim) % nlim); /* cut the deck */

  /* The shuffle's randomizing values. */
  if (nlim % p1 == 0) p1 = p3; /* side-step n being a multiple of one of the primes */
  if (nlim % p2 == 0) p2 = p3;
  uint32_t r1 = (uint32_t)(mix % p2), r2 = (uint32_t)(mix % p1), r3 = mix % 6983, r4 = mix % 4793;
  uint32_t rx = (uint32_t)(mix / nlim % nlim + 1), rx2 = (uint32_t)(mix / 43 % nlim + 1);
  /* No key value may be a modulus that's a multiple of the prime it's multiplied by. */
  if ((nlim + 1) / 2 % p2 == 0) p2 = p3;
  if ((nlim + 2) / 3 % p1 == 0) p1 = p3;
  if (rx % p2 == 0) rx--;
  if ((nlim - rx) % p1 == 0) rx--;

  /* Spin multiples of 3, then of 2, mix each side of rx its own way, flip some bits, and
   * churn once more for large piles. */
  if (si % 3 == 0) si = (uint32_t)(((uint64_t)(si / 3) * p1 + r1) % ((nlim + 2) / 3) * 3);
  if (si % 2 == 0) si = (uint32_t)(((uint64_t)(si / 2) * p2 + r2) % ((nlim + 1) / 2) * 2);
  if (si < rx) si = (uint32_t)(((uint64_t)(rx - si - 1) * p2 + r3 + r1) % rx);
  else si = (uint32_t)(((uint64_t)(si - rx) * p1 + r3) % (nlim - rx) + rx);
  if ((si ^ rx2) < nlim) si ^= rx2;
  si = (uint32_t)(((nlim - si) * p1 + r4) % nlim);
  return si;
}

void mc_shuffle_init(mc_shuffle *sh, int count, uint32_t seed, int first) {
  sh->count = count > 0 ? count : 0;
  sh->seed = seed;
  sh->first = first >= 0 && first < count ? first : -1;
  sh->cached_round = 0;
  sh->cached_turn = 0;
}

/* Each round's seed, mixed from the first's (splitmix32's finaliser). */
static uint32_t round_seed(const mc_shuffle *sh, int round) {
  if (!round) return sh->seed;
  uint32_t x = sh->seed + (uint32_t)round * 0x9E3779B9u;
  x = (x ^ (x >> 16)) * 0x85EBCA6Bu;
  x = (x ^ (x >> 13)) * 0xC2B2AE35u;
  return x ^ (x >> 16);
}

/* Place k of a round's shuffle, before it's turned. The first round starts with first, if
 * it has one, and shuffles the rest. */
static int shuffled(const mc_shuffle *sh, int round, int k) {
  if (round || sh->first < 0) return (int)mc_miller_shuffle((uint32_t)k, round_seed(sh, round), (uint32_t)sh->count);
  if (!k) return sh->first;
  int i = (int)mc_miller_shuffle((uint32_t)(k - 1), sh->seed, (uint32_t)(sh->count - 1));
  return i >= sh->first ? i + 1 : i;
}

/* How far round's shuffle is turned: by one when it would start with the item the round
 * before ended with. That depends on how far the round before was turned, so it's worked
 * out round by round, from the last one worked out when that's not past round. */
static int turn(mc_shuffle *sh, int round) {
  if (sh->count < 2) return 0;
  int r = 0, t = 0;
  if (sh->cached_round <= round) {
    r = sh->cached_round;
    t = sh->cached_turn;
  }
  for (; r < round; r++) {
    int last = shuffled(sh, r, (sh->count - 1 + t) % sh->count);
    t = shuffled(sh, r + 1, 0) == last;
  }
  sh->cached_round = round;
  sh->cached_turn = t;
  return t;
}

int mc_shuffle_at(mc_shuffle *sh, int pos) {
  if (pos < 0 || sh->count <= 0) return -1;
  int round = pos / sh->count, k = pos % sh->count;
  return shuffled(sh, round, (k + turn(sh, round)) % sh->count);
}
