/* How fast a session's files are moving and how long they have left, as numbers fit to show
 * as they are. See "Showing speed and time left" in docs/sync-protocol.md. Internal to core.
 *
 * Bytes don't arrive evenly: they come in bursts, stop while the sender encodes, and go
 * slowly through small files and fast through large ones. So nothing here looks at single
 * updates. Time is cut into one-second bins; speed is the mean of the last few, the time
 * left comes from a mean over a longer horizon, and each shown value moves only for a real
 * difference. The clock is the caller's, so tests can run a whole session in no time. */
#ifndef MC_SYNC_PACE_H
#define MC_SYNC_PACE_H

#include <stdint.h>

#define MC_SYNC_PACE_WINDOW 10 /* one-second bins behind the speed */

/* All zeros is a new one. */
typedef struct mc_sync_pace {
  /* What to show: 0 for nothing. speed is in bytes per second, a multiple of a tenth of an
   * MB (a whole MB from 100 up); eta is in seconds, on the grid of "about 12 min", and any
   * value under 60 means less than a minute. */
  int64_t speed;
  int eta;

  int started;
  uint64_t bin_start; /* when the bin being filled began */
  int64_t bin_bytes;  /* what it holds so far */
  int64_t moved;      /* the last update's */
  int64_t left;       /* likewise, 0 if it wasn't known */
  uint64_t bins;      /* closed so far */
  int64_t window[MC_SYNC_PACE_WINDOW];
  int64_t window_sum;
  uint64_t speed_bin; /* the bin speed last changed in */
  /* The longer mean behind the time left: bytes and bins, each bin counting a little less
   * than the one after it. */
  double slow_bytes, slow_bins;
  int shift_way, shifted; /* which way the speed stands from that mean, for how many bins */
  uint64_t eta_since; /* bins when the work last grew */
  int held;           /* the eta to show, kept while a stall hides it */
  uint64_t held_bin;  /* the bin it last changed in */
  int climbing;       /* bins in a row the estimate has stood well above it */
} mc_sync_pace;

/* At now_ms, moved bytes have gone in all (either way, for the whole session), and left are
 * still to go: 0 or less if that isn't known or there's nothing left. Call it now and then
 * also while nothing moves, or a stall isn't seen. More left than before means more work
 * was announced, and the time left starts over. */
void mc_sync_pace_update(mc_sync_pace *p, uint64_t now_ms, int64_t moved, int64_t left);

#endif
