#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "util/platform.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#endif

#ifdef _WIN32

uint64_t mc_wall_ms(void) {
  FILETIME ft;
  GetSystemTimePreciseAsFileTime(&ft);
  uint64_t t100ns = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  return t100ns / 10000u - UINT64_C(11644473600000); /* 1601 -> 1970 */
}

uint64_t mc_mono_ms(void) {
  return GetTickCount64();
}

void mc_sleep_ms(uint32_t ms) {
  Sleep(ms);
}

mc_status mc_random_bytes(void *buf, size_t len) {
  if (len > ULONG_MAX) return MC_ERR_INVALID_ARG;
  NTSTATUS st = BCryptGenRandom(NULL, (PUCHAR)buf, (ULONG)len, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return st == 0 ? MC_OK : MC_ERR_IO;
}

void mc_mutex_init(mc_mutex *mutex) {
  InitializeSRWLock((PSRWLOCK)&mutex->srw);
}

void mc_mutex_destroy(mc_mutex *mutex) {
  (void)mutex; /* SRW locks need no cleanup */
}

void mc_mutex_lock(mc_mutex *mutex) {
  AcquireSRWLockExclusive((PSRWLOCK)&mutex->srw);
}

void mc_mutex_unlock(mc_mutex *mutex) {
  ReleaseSRWLockExclusive((PSRWLOCK)&mutex->srw);
}

#else

static uint64_t timespec_ms(clockid_t clock) {
  struct timespec ts;
  clock_gettime(clock, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

uint64_t mc_wall_ms(void) {
  return timespec_ms(CLOCK_REALTIME);
}

uint64_t mc_mono_ms(void) {
  return timespec_ms(CLOCK_MONOTONIC);
}

void mc_sleep_ms(uint32_t ms) {
  struct timespec ts = {(time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L};
  while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
  }
}

mc_status mc_random_bytes(void *buf, size_t len) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return MC_ERR_IO;
  unsigned char *p = buf;
  while (len > 0) {
    ssize_t n = read(fd, p, len);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) {
      close(fd);
      return MC_ERR_IO;
    }
    p += n;
    len -= (size_t)n;
  }
  close(fd);
  return MC_OK;
}

void mc_mutex_init(mc_mutex *mutex) {
  pthread_mutex_init(&mutex->m, NULL);
}

void mc_mutex_destroy(mc_mutex *mutex) {
  pthread_mutex_destroy(&mutex->m);
}

void mc_mutex_lock(mc_mutex *mutex) {
  pthread_mutex_lock(&mutex->m);
}

void mc_mutex_unlock(mc_mutex *mutex) {
  pthread_mutex_unlock(&mutex->m);
}

#endif

char *mc_strdup(const char *s) {
  if (!s) return NULL;
  size_t n = strlen(s) + 1;
  char *copy = malloc(n);
  if (copy) memcpy(copy, s, n);
  return copy;
}
