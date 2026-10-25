/* Hybrid logical clock (Kulkarni et al.). A timestamp packs wall-clock milliseconds in the
 * high 48 bits and a logical counter in the low 16, so it is roughly real time but still
 * strictly increasing per device and causally ordered across devices. Fits in an SQLite
 * INTEGER. Not thread-safe; each mc_library owns one. */
#ifndef MC_UTIL_HLC_H
#define MC_UTIL_HLC_H

#include <stdint.h>

#define MC_HLC_COUNTER_BITS 16

typedef uint64_t (*mc_clock_fn)(void);

typedef struct mc_hlc {
  uint64_t last;
  mc_clock_fn wall_ms;
} mc_hlc;

/* last_seen is the highest timestamp already persisted (0 for a fresh library). */
void mc_hlc_init(mc_hlc *hlc, mc_clock_fn wall_ms, uint64_t last_seen);

/* Timestamp for a local event. */
uint64_t mc_hlc_now(mc_hlc *hlc);

/* Merges a timestamp received from a peer; returns the timestamp for the receive event. */
uint64_t mc_hlc_observe(mc_hlc *hlc, uint64_t remote);

static inline uint64_t mc_hlc_physical_ms(uint64_t t) {
  return t >> MC_HLC_COUNTER_BITS;
}

#endif
