/* The shuffled play order, for the player's shuffle.
 *
 * Built on the Miller Shuffle (https://github.com/RondeSC/Miller_Shuffle_Algo), which maps an
 * index to its place in a shuffle of n items from a seed alone: no permutation is stored, so
 * a shuffle of the whole library costs nothing to make.
 *
 * The order goes on forever, in rounds: each round plays every item once, in a shuffle of
 * its own. A round never starts with the item the last one ended with (unless there's only
 * one item). The first round can be given the item to start with, for shuffle turned on
 * while something plays, or a track picked to play first.
 */
#ifndef MC_PLAYBACK_SHUFFLE_H
#define MC_PLAYBACK_SHUFFLE_H

#include <stdint.h>

/* Miller Shuffle "Large": the item at place inx in the shuffle shuffle_id of n items, for
 * inx in 0..n-1; each comes once. n is 1 to 2^32 - 1. */
uint32_t mc_miller_shuffle(uint32_t inx, uint32_t shuffle_id, uint32_t n);

typedef struct mc_shuffle {
  int count;
  uint32_t seed; /* the first round's; the others' are made from it */
  int first;     /* the item the first round starts with, or -1 */
  /* The last round worked out, and how far its shuffle is turned (0 or 1) so as not to
   * start with the item before it. */
  int cached_round, cached_turn;
} mc_shuffle;

/* An order of count items. first is the item to play first, or -1 for any. */
void mc_shuffle_init(mc_shuffle *sh, int count, uint32_t seed, int first);
/* The item at place pos (0 on) in the order, or -1 if pos < 0 or there are no items. */
int mc_shuffle_at(mc_shuffle *sh, int pos);

#endif
