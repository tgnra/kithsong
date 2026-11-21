#include "util/hlc.h"

void mc_hlc_init(mc_hlc *hlc, mc_clock_fn wall_ms, uint64_t last_seen) {
  hlc->wall_ms = wall_ms;
  hlc->last = last_seen;
}

/* If the counter overflows it carries into the physical part, i.e. the clock briefly runs
 * ahead of wall time. That keeps it monotonic, which is the property sync relies on. */
uint64_t mc_hlc_now(mc_hlc *hlc) {
  uint64_t pt = hlc->wall_ms() << MC_HLC_COUNTER_BITS;
  hlc->last = pt > hlc->last ? pt : hlc->last + 1;
  return hlc->last;
}

uint64_t mc_hlc_observe(mc_hlc *hlc, uint64_t remote) {
  uint64_t pt = hlc->wall_ms() << MC_HLC_COUNTER_BITS;
  uint64_t seen = remote > hlc->last ? remote : hlc->last;
  hlc->last = pt > seen ? pt : seen + 1;
  return hlc->last;
}
