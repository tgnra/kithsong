/* The encode pool: encodes into the transcode cache on a few worker threads, shared by
 * every session (FILE_REQ, encode-ahead and PREPARE_REQ). See "Lossy copies" in
 * docs/sync-protocol.md. Internal to core.
 *
 * A job is one (file uid, format), e.g. Opus at a kbps (mc_transcode_opts). Asking for one
 * already queued or running joins it, so a file is never encoded twice at once. Jobs run by
 * priority, then in the order asked for.
 * A job holds its own copies of what it needs: workers never touch a session's state, and
 * an owner is only ever compared, so a job may outlive the session that queued it.
 *
 * The cache is trimmed (MC_TRANSCODE_CACHE_MAX) once the pool has encoded something and
 * nothing is queued, running or held, and nothing starts or is held while it trims. */
#ifndef MC_SYNC_ENCODE_H
#define MC_SYNC_ENCODE_H

#include <stdint.h>

#include "kithcore/kithcore.h"
#include "kithcore/transcode.h"

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

/* Test hook: each job waits this long before it encodes (0: none). Set before the pool is used. */
extern uint32_t mc_encode_test_delay_ms;

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
/* The same for any format mc_transcode_cached() makes. */
mc_status mc_encode_pool_queue_as(mc_encode_pool *p, const char *file_uid, const char *src,
                                  const mc_transcode_opts *opts, mc_encode_prio prio,
                                  const void *owner);
/* The same, saying which member it's for (device, a uid; NULL if not said), for
 * mc_encode_pool_devices(). A job asked for again is for whoever wants it soonest. */
mc_status mc_encode_pool_queue_for(mc_encode_pool *p, const char *file_uid, const char *src,
                                   const mc_transcode_opts *opts, mc_encode_prio prio,
                                   const void *owner, const char *device);
/* Drops owner's queued jobs that nobody waits for; one also queued as MC_ENCODE_BACKGROUND
 * goes back to that. Running jobs finish. */
void mc_encode_pool_drop(mc_encode_pool *p, const void *owner);
/* Encodes file_uid at kbps now and waits for it: joins its job if it has one (moving it to
 * the front of the queue), else queues one at the front. On MC_OK, *out is the cached file
 * (malloc'd). Hold the pool around it and the sending, so the trim leaves the file alone.
 * idle, unless NULL, runs now and then during the wait. */
mc_status mc_encode_pool_encode(mc_encode_pool *p, const char *file_uid, const char *src,
                                int kbps, void (*idle)(void *user), void *user, char **out);
/* mc_encode_pool_encode() saying which member it's for, as mc_encode_pool_queue_for(). */
mc_status mc_encode_pool_encode_for(mc_encode_pool *p, const char *file_uid, const char *src,
                                    int kbps, void (*idle)(void *user), void *user,
                                    const char *device, char **out);
/* The same for any format, and idle may give up: once it returns non-zero, this returns
 * MC_ERR_CANCELLED without waiting for the job, which still runs if it has started (or
 * another wants it). */
mc_status mc_encode_pool_encode_as(mc_encode_pool *p, const char *file_uid, const char *src,
                                   const mc_transcode_opts *opts, int (*idle)(void *user),
                                   void *user, char **out);
/* The jobs queued or running for each member they're for. */
typedef struct mc_encode_device {
  char device[MC_UID_STRLEN]; /* "" for jobs that didn't say */
  int jobs;
} mc_encode_device;

/* Fills out with up to max members, those running first; returns how many. */
int mc_encode_pool_devices(mc_encode_pool *p, mc_encode_device *out, int max);
/* Keeps the trim from running until the matching release (waiting for one that runs). */
void mc_encode_pool_hold(mc_encode_pool *p);
void mc_encode_pool_release(mc_encode_pool *p);
/* Cancels queued jobs (their waiters get an error), waits for running ones and joins the
 * workers. Until mc_encode_pool_resume(), jobs are refused. */
void mc_encode_pool_stop(mc_encode_pool *p);
void mc_encode_pool_resume(mc_encode_pool *p);
/* closed = 1: transcoding is no longer allowed here. Queued jobs are cancelled, and callers of
 * mc_encode_pool_encode() still waiting, even for a running job, get MC_ERR_CANCELLED at once;
 * running jobs finish into the cache. Until closed = 0, new jobs are refused with
 * MC_ERR_CANCELLED. Independent of stop and resume. */
void mc_encode_pool_set_closed(mc_encode_pool *p, int closed);
/* How many times it has been closed, so a caller can tell its queued jobs were cancelled
 * meanwhile. */
unsigned mc_encode_pool_closings(mc_encode_pool *p);
/* Whether a job is queued or running. */
int mc_encode_pool_busy(mc_encode_pool *p);
/* fn(user) runs, on whichever thread made the change and never under the pool's lock, after
 * mc_encode_pool_busy() may have changed. Set before the pool is used. */
void mc_encode_pool_set_notify(mc_encode_pool *p, void (*fn)(void *user), void *user);
void mc_encode_pool_stats(mc_encode_pool *p, mc_encode_stats *out);

#endif
