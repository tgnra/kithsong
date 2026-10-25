/* Thin OS layer: clocks, sleeping, randomness, mutexes. Everything else in core is
 * platform-independent. */
#ifndef MC_UTIL_PLATFORM_H
#define MC_UTIL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#include "musiccore/musiccore.h"

#ifndef _WIN32
#include <pthread.h>
#endif

/* Milliseconds since the Unix epoch. */
uint64_t mc_wall_ms(void);
/* Monotonic milliseconds, arbitrary origin. */
uint64_t mc_mono_ms(void);
void mc_sleep_ms(uint32_t ms);

/* Cryptographically secure random bytes from the OS. */
mc_status mc_random_bytes(void *buf, size_t len);

/* Duplicates a string with malloc; NULL in, NULL out. */
char *mc_strdup(const char *s);

typedef struct mc_mutex {
#ifdef _WIN32
  void *srw; /* SRWLOCK, which is pointer-sized; zero-initialised = unlocked */
#else
  pthread_mutex_t m;
#endif
} mc_mutex;

void mc_mutex_init(mc_mutex *mutex);
void mc_mutex_destroy(mc_mutex *mutex);
void mc_mutex_lock(mc_mutex *mutex);
void mc_mutex_unlock(mc_mutex *mutex);

#endif
