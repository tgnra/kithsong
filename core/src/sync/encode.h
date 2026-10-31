/* The encode pool: Opus encodes into the transcode cache on a few worker threads, shared by
 * every session (FILE_REQ, encode-ahead and PREPARE_REQ). See "Lossy copies" in
 * docs/sync-protocol.md. Internal to core.
 *
 * A job is one (file uid, kbps). Asking for one already queued or running joins it, so a
 * file is never encoded twice at once. Jobs run by priority, then in the order asked for.
 * A job holds its own copies of what it needs: workers never touch a session's state, and
 * an owner is only ever compared, so a job may outlive the session that queued it.
 *
 * The cache is trimmed (MC_TRANSCODE_CACHE_MAX) once the pool has encoded something and
 * nothing is queued, running or held, and nothing starts or is held while it trims. */
#ifndef MC_SYNC_ENCODE_H
#define MC_SYNC_ENCODE_H

#include <stdint.h>

#include "musiccore/musiccore.h"

typedef enum mc_encode_prio {
  MC_ENCODE_NOW,       /* a FILE_REQ waits for it */
  MC_ENCODE_AHEAD,     /* the session's receiver is about to ask for it */
  MC_ENCODE_BACKGROUND /* PREPARE_REQ for a member that is offline */
} mc_encode_prio;

typedef struct mc_encode_pool mc_encode_pool;

/* For tests: counts since the pool was made. */
typedef struct mc_encode_stats {
  int encoded; /* jobs that made a file (it wasn't in the cache yet) */
  int unasked; /* mc_encode_pool_encode() calls that found no job for their file */
  int peak;    /* the most jobs that ran at once */
  int trims;
} mc_encode_stats;

/* A pool for cache_dir that trims it to cache_max bytes. One worker until set_size. */
mc_status mc_encode_pool_new(const char *cache_dir, int64_t cache_max, mc_encode_pool **out);
/* Stops it (as mc_encode_pool_stop()) and frees it. */
void mc_encode_pool_free(mc_encode_pool *p);
/* Up to n encodes at once (at least 1), for jobs started from now on. */
void mc_encode_pool_set_size(mc_encode_pool *p, int n);
/* Queues an encode of src, the file file_uid, at kbps, unless one is queued or running
 * already; that one moves up to prio if it was lower. owner tags MC_ENCODE_AHEAD jobs for
 * mc_encode_pool_drop() (NULL otherwise). */
mc_status mc_encode_pool_queue(mc_encode_pool *p, const char *file_uid, const char *src,
                               int kbps, mc_encode_prio prio, const void *owner);
/* Drops owner's queued jobs that nobody waits for; one also queued as MC_ENCODE_BACKGROUND
 * goes back to that. Running jobs finish. */
void mc_encode_pool_drop(mc_encode_pool *p, const void *owner);
/* Encodes file_uid at kbps now and waits for it: joins its job if it has one (moving it to
 * the front of the queue), else queues one at the front. On MC_OK, *out is the cached file
 * (malloc'd). Hold the pool around it and the sending, so the trim leaves the file alone. */
mc_status mc_encode_pool_encode(mc_encode_pool *p, const char *file_uid, const char *src,
                                int kbps, char **out);
/* Keeps the trim from running until the matching release (waiting for one that runs). */
void mc_encode_pool_hold(mc_encode_pool *p);
void mc_encode_pool_release(mc_encode_pool *p);
/* Cancels queued jobs (their waiters get an error), waits for running ones and joins the
 * workers. Until mc_encode_pool_resume(), jobs are refused. */
void mc_encode_pool_stop(mc_encode_pool *p);
void mc_encode_pool_resume(mc_encode_pool *p);
void mc_encode_pool_stats(mc_encode_pool *p, mc_encode_stats *out);

#endif
