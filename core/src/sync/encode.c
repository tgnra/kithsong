/* The encode pool; see encode.h. Everything in mc_encode_pool but cache_dir and cache_max
 * is under lock. Workers start as jobs need them, up to size, and live until stop. */
#include "sync/encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/transcode.h"
#include "util/platform.h"

enum { QUEUED, RUNNING, DONE };

typedef struct job {
  char file_uid[MC_UID_STRLEN];
  int kbps;
  char *src;
  mc_encode_prio prio;
  int background;    /* also asked for as MC_ENCODE_BACKGROUND */
  const void *owner; /* who asked for it ahead, or NULL */
  int state;
  mc_status st; /* once DONE */
  int waiters;  /* a DONE job is freed by its last waiter */
  struct job *next;
} job;

struct mc_encode_pool {
  char *cache_dir;
  int64_t cache_max;
  mc_mutex lock;
  mc_cond cond;
  int size, stopping;
  job *queue;   /* by prio, then age */
  job *running; /* unordered */
  int nqueued, nrunning;
  mc_thread *threads;
  int nthreads, threads_cap;
  int holds;
  int dirty; /* encoded something since the last trim */
  int trimming;
  mc_encode_stats stats;
};

/* <cache>/<file_uid>/opus-<kbps>.opus, as mc_transcode_cached() makes it. */
static char *out_path(const mc_encode_pool *p, const char *file_uid, int kbps) {
  size_t n = strlen(p->cache_dir) + MC_UID_STRLEN + 32;
  char *path = malloc(n);
  if (path) snprintf(path, n, "%s%c%s%copus-%d.opus", p->cache_dir, MC_PATH_SEP, file_uid,
                     MC_PATH_SEP, kbps);
  return path;
}

static void job_free(job *j) {
  free(j->src);
  free(j);
}

static job *find(job *list, const char *file_uid, int kbps) {
  for (; list; list = list->next)
    if (list->kbps == kbps && !strcmp(list->file_uid, file_uid)) return list;
  return NULL;
}

static void unlink_job(job **list, job *j) {
  for (job **pp = list; *pp; pp = &(*pp)->next)
    if (*pp == j) {
      *pp = j->next;
      return;
    }
}

/* Into the queue after every job of its prio or higher. */
static void enqueue(mc_encode_pool *p, job *j) {
  job **pp = &p->queue;
  while (*pp && (*pp)->prio <= j->prio) pp = &(*pp)->next;
  j->next = *pp;
  *pp = j;
}

/* Moves a queued job up to prio, if that is higher. */
static void raise_prio(mc_encode_pool *p, job *j, mc_encode_prio prio) {
  if (j->state != QUEUED || prio >= j->prio) return;
  unlink_job(&p->queue, j);
  j->prio = prio;
  enqueue(p, j);
}

/* Trims the cache if it's due and nothing uses it. Under lock, which it drops meanwhile. */
static void maybe_trim(mc_encode_pool *p) {
  if (!p->dirty || p->trimming || p->holds || p->nqueued || p->nrunning) return;
  p->dirty = 0;
  p->trimming = 1;
  mc_mutex_unlock(&p->lock);
  mc_transcode_cache_trim(p->cache_dir, p->cache_max);
  mc_mutex_lock(&p->lock);
  p->trimming = 0;
  p->stats.trims++;
  mc_cond_broadcast(&p->cond);
}

static void worker(void *arg) {
  mc_encode_pool *p = arg;
  mc_mutex_lock(&p->lock);
  for (;;) {
    while (!p->stopping && (p->trimming || !p->queue || p->nrunning >= p->size))
      mc_cond_wait(&p->cond, &p->lock);
    if (p->stopping) break;
    job *j = p->queue;
    p->queue = j->next;
    p->nqueued--;
    j->state = RUNNING;
    j->next = p->running;
    p->running = j;
    if (++p->nrunning > p->stats.peak) p->stats.peak = p->nrunning;
    mc_mutex_unlock(&p->lock);

    char *path = out_path(p, j->file_uid, j->kbps), *out = NULL;
    int had = path && mc_file_exists(path);
    mc_transcode_opts opts = {MC_FORMAT_OPUS, j->kbps};
    mc_status st = mc_transcode_cached(p->cache_dir, j->file_uid, j->src, &opts, &out);
    if (st != MC_OK)
      fprintf(stderr, "musiccore: sync: transcoding %s failed: %s\n", j->src, mc_status_str(st));
    free(out);
    free(path);

    mc_mutex_lock(&p->lock);
    if (st == MC_OK && !had) {
      p->dirty = 1;
      p->stats.encoded++;
    }
    unlink_job(&p->running, j);
    p->nrunning--;
    j->state = DONE;
    j->st = st;
    if (!j->waiters) job_free(j);
    mc_cond_broadcast(&p->cond);
    maybe_trim(p);
  }
  mc_mutex_unlock(&p->lock);
}

/* Starts workers while queued jobs outnumber the free ones, up to size. Under lock. */
static void spawn(mc_encode_pool *p) {
  while (p->nthreads < p->size && p->nthreads - p->nrunning < p->nqueued) {
    if (p->nthreads == p->threads_cap) {
      int grown = p->threads_cap ? p->threads_cap * 2 : 4;
      mc_thread *more = realloc(p->threads, (size_t)grown * sizeof *more);
      if (!more) break;
      p->threads = more;
      p->threads_cap = grown;
    }
    if (mc_thread_start(&p->threads[p->nthreads], worker, p) != MC_OK) break;
    p->nthreads++;
  }
  mc_cond_broadcast(&p->cond);
}

/* A new queued job. Under lock. */
static job *add(mc_encode_pool *p, const char *file_uid, const char *src, int kbps,
                mc_encode_prio prio) {
  job *j = calloc(1, sizeof *j);
  if (!j || !(j->src = mc_strdup(src))) {
    free(j);
    return NULL;
  }
  memcpy(j->file_uid, file_uid, MC_UID_STRLEN);
  j->kbps = kbps;
  j->prio = prio;
  j->background = prio == MC_ENCODE_BACKGROUND;
  j->state = QUEUED;
  enqueue(p, j);
  p->nqueued++;
  return j;
}

mc_status mc_encode_pool_new(const char *cache_dir, int64_t cache_max, mc_encode_pool **out) {
  *out = NULL;
  mc_encode_pool *p = calloc(1, sizeof *p);
  if (!p || !(p->cache_dir = mc_strdup(cache_dir))) {
    free(p);
    return MC_ERR_NOMEM;
  }
  p->cache_max = cache_max;
  p->size = 1;
  mc_mutex_init(&p->lock);
  mc_cond_init(&p->cond);
  *out = p;
  return MC_OK;
}

void mc_encode_pool_free(mc_encode_pool *p) {
  if (!p) return;
  mc_encode_pool_stop(p);
  free(p->threads);
  mc_cond_destroy(&p->cond);
  mc_mutex_destroy(&p->lock);
  free(p->cache_dir);
  free(p);
}

void mc_encode_pool_set_size(mc_encode_pool *p, int n) {
  mc_mutex_lock(&p->lock);
  p->size = n < 1 ? 1 : n;
  if (!p->stopping) spawn(p);
  mc_mutex_unlock(&p->lock);
}

mc_status mc_encode_pool_queue(mc_encode_pool *p, const char *file_uid, const char *src,
                               int kbps, mc_encode_prio prio, const void *owner) {
  mc_mutex_lock(&p->lock);
  mc_status st = MC_OK;
  job *j = find(p->queue, file_uid, kbps);
  if (!j) j = find(p->running, file_uid, kbps);
  if (p->stopping) {
    st = MC_ERR_INVALID_ARG;
  } else if (j) {
    if (prio == MC_ENCODE_BACKGROUND) j->background = 1;
    if (prio == MC_ENCODE_AHEAD && j->state == QUEUED && j->prio > prio) j->owner = owner;
    raise_prio(p, j, prio);
  } else if ((j = add(p, file_uid, src, kbps, prio)) != NULL) {
    if (prio == MC_ENCODE_AHEAD) j->owner = owner;
    spawn(p);
  } else {
    st = MC_ERR_NOMEM;
  }
  mc_mutex_unlock(&p->lock);
  return st;
}

void mc_encode_pool_drop(mc_encode_pool *p, const void *owner) {
  mc_mutex_lock(&p->lock);
  job *keep = NULL, **tail = &keep;
  for (job **pp = &p->queue; *pp;) {
    job *j = *pp;
    if (j->owner != owner || j->waiters) {
      pp = &j->next;
      continue;
    }
    *pp = j->next;
    if (j->background) { /* back to the end of the background ones */
      j->owner = NULL;
      j->prio = MC_ENCODE_BACKGROUND;
      j->next = NULL;
      *tail = j;
      tail = &j->next;
    } else {
      p->nqueued--;
      job_free(j);
    }
  }
  while (keep) {
    job *j = keep;
    keep = j->next;
    enqueue(p, j);
  }
  for (job *j = p->running; j; j = j->next)
    if (j->owner == owner) j->owner = NULL;
  mc_cond_broadcast(&p->cond);
  maybe_trim(p);
  mc_mutex_unlock(&p->lock);
}

mc_status mc_encode_pool_encode(mc_encode_pool *p, const char *file_uid, const char *src,
                                int kbps, char **out) {
  *out = NULL;
  mc_mutex_lock(&p->lock);
  job *j = find(p->queue, file_uid, kbps);
  if (!j) j = find(p->running, file_uid, kbps);
  if (p->stopping) {
    mc_mutex_unlock(&p->lock);
    return MC_ERR_INVALID_ARG;
  }
  if (j) {
    raise_prio(p, j, MC_ENCODE_NOW);
  } else if ((j = add(p, file_uid, src, kbps, MC_ENCODE_NOW)) != NULL) {
    p->stats.unasked++;
  } else {
    mc_mutex_unlock(&p->lock);
    return MC_ERR_NOMEM;
  }
  j->waiters++;
  spawn(p);
  while (j->state != DONE) mc_cond_wait(&p->cond, &p->lock);
  mc_status st = j->st;
  if (!--j->waiters) job_free(j);
  mc_mutex_unlock(&p->lock);
  if (st == MC_OK && !(*out = out_path(p, file_uid, kbps))) st = MC_ERR_NOMEM;
  return st;
}

void mc_encode_pool_hold(mc_encode_pool *p) {
  mc_mutex_lock(&p->lock);
  while (p->trimming) mc_cond_wait(&p->cond, &p->lock);
  p->holds++;
  mc_mutex_unlock(&p->lock);
}

void mc_encode_pool_release(mc_encode_pool *p) {
  mc_mutex_lock(&p->lock);
  p->holds--;
  maybe_trim(p);
  mc_mutex_unlock(&p->lock);
}

void mc_encode_pool_stop(mc_encode_pool *p) {
  mc_mutex_lock(&p->lock);
  p->stopping = 1;
  while (p->queue) {
    job *j = p->queue;
    p->queue = j->next;
    p->nqueued--;
    j->state = DONE;
    j->st = MC_ERR_INVALID_ARG;
    if (!j->waiters) job_free(j);
  }
  mc_cond_broadcast(&p->cond);
  while (p->nrunning || p->trimming) mc_cond_wait(&p->cond, &p->lock);
  int n = p->nthreads;
  p->nthreads = 0;
  mc_mutex_unlock(&p->lock);
  for (int i = 0; i < n; i++) mc_thread_join(&p->threads[i]); /* only stop starts or joins */
  mc_mutex_lock(&p->lock);
  maybe_trim(p);
  mc_mutex_unlock(&p->lock);
}

void mc_encode_pool_resume(mc_encode_pool *p) {
  mc_mutex_lock(&p->lock);
  p->stopping = 0;
  if (p->queue) spawn(p);
  mc_mutex_unlock(&p->lock);
}

void mc_encode_pool_stats(mc_encode_pool *p, mc_encode_stats *out) {
  mc_mutex_lock(&p->lock);
  *out = p->stats;
  mc_mutex_unlock(&p->lock);
}
