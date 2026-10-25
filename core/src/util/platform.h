/* Thin OS layer: clocks, sleeping, randomness, threads, files. Everything else in core is
 * platform-independent. */
#ifndef MC_UTIL_PLATFORM_H
#define MC_UTIL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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

typedef struct mc_cond {
#ifdef _WIN32
  void *cv; /* CONDITION_VARIABLE, also pointer-sized */
#else
  pthread_cond_t c;
#endif
} mc_cond;

void mc_cond_init(mc_cond *cond);
void mc_cond_destroy(mc_cond *cond);
/* Atomically releases mutex and waits; reacquires it before returning. */
void mc_cond_wait(mc_cond *cond, mc_mutex *mutex);
/* Like mc_cond_wait, but gives up after about ms milliseconds. */
void mc_cond_wait_ms(mc_cond *cond, mc_mutex *mutex, uint32_t ms);
void mc_cond_broadcast(mc_cond *cond);

typedef struct mc_thread {
#ifdef _WIN32
  void *handle;
#else
  pthread_t t;
#endif
} mc_thread;

mc_status mc_thread_start(mc_thread *thread, void (*fn)(void *), void *arg);
void mc_thread_join(mc_thread *thread);

/* Native path separator, for building paths to show the user. */
#ifdef _WIN32
#define MC_PATH_SEP '\\'
#else
#define MC_PATH_SEP '/'
#endif

/* Calls fn for every regular file under path, recursing into subdirectories, or once for
 * path itself if it is a file. Paths are UTF-8. Stops early if fn returns non-zero. */
typedef int (*mc_walk_fn)(void *user, const char *path);
mc_status mc_walk(const char *path, mc_walk_fn fn, void *user);

/* Calls fn for each entry directly inside dir (no recursion, no "." or ".."). */
typedef void (*mc_list_fn)(void *user, const char *name, int is_dir);
mc_status mc_list_dir(const char *dir, mc_list_fn fn, void *user);

/* fopen with a UTF-8 path on every platform. */
FILE *mc_fopen(const char *path, const char *mode);
/* Creates a directory and any missing parents. Succeeds if it already exists. */
mc_status mc_mkdirs(const char *path);
mc_status mc_file_info(const char *path, int64_t *size, int64_t *mtime_ms);
int mc_file_exists(const char *path);
/* Copies a file. Fails if dst exists, unless overwrite is set. */
mc_status mc_copy_file(const char *src, const char *dst, int overwrite);
/* Renames or moves a file, across volumes if needed. Fails if dst exists, unless replace
 * is set. */
mc_status mc_rename(const char *src, const char *dst, int replace);
mc_status mc_remove(const char *path);
/* Removes a directory; fails if it isn't empty. */
mc_status mc_rmdir(const char *path);
/* Whether two paths name the same file as far as the file system is concerned: compared
 * case-insensitively on Windows. Doesn't resolve links. */
int mc_same_path(const char *a, const char *b);

#endif
