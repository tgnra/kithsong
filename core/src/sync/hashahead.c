/* Hash-ahead; see hashahead.h. Jobs are an array in queue order; next is the first not
 * passed over (everything before it was taken or skipped). Everything is under lock. */
#include "sync/hashahead.h"

#include <stdlib.h>
#include <string.h>

#include "tagger/dupes.h"
#include "util/platform.h"

enum { QUEUED, RUNNING, DONE, FAILED, GONE }; /* GONE: taken, or passed over unhashed */

typedef struct job {
  char *key, *path;
  int state;
  mc_hash_ahead_result r; /* once DONE */
} job;

struct mc_hash_ahead {
  mc_mutex lock;
  mc_cond cond;
  mc_thread thread;
  int ahead, stopping;
  job *jobs;
  int n, cap, next;
  int hashed, taken;
};

/* The job the worker should hash next, or NULL: the first queued one within ahead of next. */
static job *due(mc_hash_ahead *ha) {
  int end = ha->next + ha->ahead < ha->n ? ha->next + ha->ahead : ha->n;
  for (int i = ha->next; i < end; i++)
    if (ha->jobs[i].state == QUEUED) return &ha->jobs[i];
  return NULL;
}

static void worker(void *arg) {
  mc_hash_ahead *ha = arg;
  mc_mutex_lock(&ha->lock);
  for (;;) {
    job *j;
    while (!ha->stopping && !(j = due(ha))) mc_cond_wait(&ha->cond, &ha->lock);
    if (ha->stopping) break;
    j->state = RUNNING;
    char *path = j->path; /* jobs may move as more are queued; the path doesn't */
    mc_mutex_unlock(&ha->lock);

    /* The file as it was before and after: a change meanwhile could have mixed versions. */
    mc_hash_ahead_result r;
    int64_t size, mtime;
    int ok = mc_file_info(path, &r.size, &r.mtime) == MC_OK && mc_sha256_file(path, r.sha) == MC_OK &&
             mc_file_info(path, &size, &mtime) == MC_OK && size == r.size && mtime == r.mtime;

    mc_mutex_lock(&ha->lock);
    for (int i = 0; i < ha->n; i++)
      if (ha->jobs[i].path == path) {
        ha->jobs[i].state = ok ? DONE : FAILED;
        if (ok) ha->jobs[i].r = r;
        break;
      }
    ha->hashed += ok;
    mc_cond_broadcast(&ha->cond);
  }
  mc_mutex_unlock(&ha->lock);
}

mc_status mc_hash_ahead_new(int ahead, mc_hash_ahead **out) {
  *out = NULL;
  mc_hash_ahead *ha = calloc(1, sizeof *ha);
  if (!ha) return MC_ERR_NOMEM;
  ha->ahead = ahead > 0 ? ahead : 1;
  mc_mutex_init(&ha->lock);
  mc_cond_init(&ha->cond);
  mc_status st = mc_thread_start(&ha->thread, worker, ha);
  if (st != MC_OK) {
    mc_cond_destroy(&ha->cond);
    mc_mutex_destroy(&ha->lock);
    free(ha);
    return st;
  }
  *out = ha;
  return MC_OK;
}

static void stop(mc_hash_ahead *ha) {
  mc_mutex_lock(&ha->lock);
  int joined = ha->stopping;
  ha->stopping = 1;
  mc_cond_broadcast(&ha->cond);
  mc_mutex_unlock(&ha->lock);
  if (!joined) mc_thread_join(&ha->thread);
}

void mc_hash_ahead_free(mc_hash_ahead *ha) {
  if (!ha) return;
  stop(ha);
  for (int i = 0; i < ha->n; i++) {
    free(ha->jobs[i].key);
    free(ha->jobs[i].path);
  }
  free(ha->jobs);
  mc_cond_destroy(&ha->cond);
  mc_mutex_destroy(&ha->lock);
  free(ha);
}

mc_status mc_hash_ahead_queue(mc_hash_ahead *ha, const char *key, const char *path) {
  char *k = mc_strdup(key), *p = mc_strdup(path);
  mc_status st = k && p ? MC_OK : MC_ERR_NOMEM;
  mc_mutex_lock(&ha->lock);
  if (st == MC_OK && ha->n == ha->cap) {
    int grown = ha->cap ? ha->cap * 2 : 64;
    job *more = realloc(ha->jobs, (size_t)grown * sizeof *more);
    if (more) {
      ha->jobs = more;
      ha->cap = grown;
    } else {
      st = MC_ERR_NOMEM;
    }
  }
  if (st == MC_OK) {
    ha->jobs[ha->n++] = (job){k, p, QUEUED, {"", 0, 0}};
    mc_cond_broadcast(&ha->cond);
  }
  mc_mutex_unlock(&ha->lock);
  if (st != MC_OK) {
    free(k);
    free(p);
  }
  return st;
}

int mc_hash_ahead_take(mc_hash_ahead *ha, const char *key, mc_hash_ahead_result *out) {
  mc_mutex_lock(&ha->lock);
  int at = -1;
  for (int i = ha->next; i < ha->n && at < 0; i++)
    if (ha->jobs[i].state != GONE && !strcmp(ha->jobs[i].key, key)) at = i;
  if (at < 0) {
    mc_mutex_unlock(&ha->lock);
    return 0;
  }
  /* Those before it won't be asked for now (a hashed one stays, for finish), and the worker
   * moves on past it. */
  for (int i = ha->next; i < at; i++)
    if (ha->jobs[i].state == QUEUED) ha->jobs[i].state = GONE;
  ha->next = at + 1;
  mc_cond_broadcast(&ha->cond);
  while (ha->jobs[at].state == RUNNING) mc_cond_wait(&ha->cond, &ha->lock);
  job *j = &ha->jobs[at];
  int ok = j->state == DONE;
  if (ok) {
    *out = j->r;
    ha->taken++;
  }
  j->state = GONE;
  mc_mutex_unlock(&ha->lock);
  return ok;
}

void mc_hash_ahead_finish(mc_hash_ahead *ha,
                          void (*fn)(void *user, const char *key, const char *path,
                                     const mc_hash_ahead_result *r),
                          void *user) {
  stop(ha);
  /* The worker is gone: nothing else touches the jobs. */
  for (int i = 0; i < ha->n; i++)
    if (ha->jobs[i].state == DONE) {
      fn(user, ha->jobs[i].key, ha->jobs[i].path, &ha->jobs[i].r);
      ha->jobs[i].state = GONE;
    }
  ha->next = ha->n;
}

void mc_hash_ahead_stats(mc_hash_ahead *ha, int *hashed, int *taken) {
  mc_mutex_lock(&ha->lock);
  *hashed = ha->hashed;
  *taken = ha->taken;
  mc_mutex_unlock(&ha->lock);
}
