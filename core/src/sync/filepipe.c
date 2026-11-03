#include "sync/filepipe.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/platform_util.h>
#include <mbedtls/sha256.h>

#include "sync/channel.h"
#include "util/platform.h"

/* Buffers in a ring, of a chunk each: enough that a hiccup on one side (a slow write, a
 * frame waiting for the socket) doesn't stall the other at once. */
#define SLOTS 8
#define SLOT_LEN MC_CHANNEL_MAX_PAYLOAD

/* The ring between a producer and a consumer thread, which take turns at each slot: the
 * producer fills the one after the full ones, the consumer empties the oldest. */
typedef struct ring {
  mc_mutex lock;
  mc_cond cond; /* a slot was filled or emptied, or done or stop was set */
  unsigned char *buf; /* SLOTS * SLOT_LEN */
  size_t len[SLOTS];
  int head, count; /* the oldest full slot, and how many are full; under lock */
  int done;        /* the producer has put its last; under lock */
  int stop;        /* the consumer wants no more; under lock */
  mc_thread thread;
} ring;

static int ring_init(ring *r) {
  r->buf = malloc((size_t)SLOTS * SLOT_LEN);
  if (!r->buf) return 0;
  r->head = r->count = r->done = r->stop = 0;
  mc_mutex_init(&r->lock);
  mc_cond_init(&r->cond);
  return 1;
}

static void ring_destroy(ring *r) {
  mc_cond_destroy(&r->cond);
  mc_mutex_destroy(&r->lock);
  mbedtls_platform_zeroize(r->buf, (size_t)SLOTS * SLOT_LEN); /* as the channel does */
  free(r->buf);
}

/* Producer: the slot to fill next, once there is room; NULL if the consumer stopped. */
static unsigned char *ring_claim(ring *r) {
  mc_mutex_lock(&r->lock);
  while (r->count == SLOTS && !r->stop) mc_cond_wait(&r->cond, &r->lock);
  unsigned char *slot = NULL;
  if (!r->stop) slot = r->buf + (size_t)((r->head + r->count) % SLOTS) * SLOT_LEN;
  mc_mutex_unlock(&r->lock);
  return slot;
}

/* Producer: the claimed slot now holds len bytes. */
static void ring_push(ring *r, size_t len) {
  mc_mutex_lock(&r->lock);
  r->len[(r->head + r->count) % SLOTS] = len;
  r->count++;
  mc_cond_broadcast(&r->cond);
  mc_mutex_unlock(&r->lock);
}

/* Producer: nothing more follows. */
static void ring_end(ring *r) {
  mc_mutex_lock(&r->lock);
  r->done = 1;
  mc_cond_broadcast(&r->cond);
  mc_mutex_unlock(&r->lock);
}

/* Consumer: the oldest full slot, once there is one; NULL when the producer is done and
 * everything has been taken. */
static const unsigned char *ring_peek(ring *r, size_t *len) {
  mc_mutex_lock(&r->lock);
  while (r->count == 0 && !r->done) mc_cond_wait(&r->cond, &r->lock);
  const unsigned char *slot = NULL;
  if (r->count) {
    slot = r->buf + (size_t)r->head * SLOT_LEN;
    *len = r->len[r->head];
  }
  mc_mutex_unlock(&r->lock);
  return slot;
}

/* Consumer: done with the slot ring_peek() gave. */
static void ring_pop(ring *r) {
  mc_mutex_lock(&r->lock);
  r->head = (r->head + 1) % SLOTS;
  r->count--;
  mc_cond_broadcast(&r->cond);
  mc_mutex_unlock(&r->lock);
}

/* Consumer: wants no more, so the producer stops at its next claim. */
static void ring_stop(ring *r) {
  mc_mutex_lock(&r->lock);
  r->stop = 1;
  mc_cond_broadcast(&r->cond);
  mc_mutex_unlock(&r->lock);
}

/* --- Reading ahead: the reader's thread produces, the session consumes --- */

struct mc_file_reader {
  ring r;
  FILE *f;
  int64_t size;
  int held; /* the session holds the oldest slot, from the last mc_file_reader_next() */
};

static void reader_main(void *arg) {
  mc_file_reader *rd = arg;
  for (int64_t done = 0; done < rd->size;) {
    unsigned char *slot = ring_claim(&rd->r);
    if (!slot) break;
    size_t want = rd->size - done < SLOT_LEN ? (size_t)(rd->size - done) : SLOT_LEN;
    size_t n = fread(slot, 1, want, rd->f);
    if (n < want) memset(slot + n, 0, want - n);
    ring_push(&rd->r, want);
    done += (int64_t)want;
  }
  ring_end(&rd->r);
}

mc_status mc_file_reader_start(FILE *f, int64_t size, mc_file_reader **out) {
  *out = NULL;
  mc_file_reader *rd = calloc(1, sizeof *rd);
  if (!rd || !ring_init(&rd->r)) {
    free(rd);
    return MC_ERR_NOMEM;
  }
  rd->f = f;
  rd->size = size;
  if (mc_thread_start(&rd->r.thread, reader_main, rd) != MC_OK) {
    ring_destroy(&rd->r);
    free(rd);
    return MC_ERR_NOMEM;
  }
  *out = rd;
  return MC_OK;
}

const unsigned char *mc_file_reader_next(mc_file_reader *rd, size_t *len) {
  if (rd->held) ring_pop(&rd->r);
  const unsigned char *chunk = ring_peek(&rd->r, len);
  rd->held = chunk != NULL;
  return chunk;
}

void mc_file_reader_free(mc_file_reader *rd) {
  if (!rd) return;
  ring_stop(&rd->r);
  mc_thread_join(&rd->r.thread);
  ring_destroy(&rd->r);
  free(rd);
}

/* --- Writing behind: the session produces, the writer's thread consumes --- */

struct mc_file_writer {
  ring r;
  FILE *f;
  mbedtls_sha256_context sha;
  int ok; /* every write so far succeeded; once one fails, the rest is only drained */
};

static void writer_main(void *arg) {
  mc_file_writer *w = arg;
  const unsigned char *slot;
  size_t len;
  while ((slot = ring_peek(&w->r, &len)) != NULL) {
    if (w->ok) {
      mbedtls_sha256_update(&w->sha, slot, len);
      if (fwrite(slot, 1, len, w->f) != len) w->ok = 0;
    }
    ring_pop(&w->r);
  }
}

mc_status mc_file_writer_start(FILE *f, mc_file_writer **out) {
  *out = NULL;
  mc_file_writer *w = calloc(1, sizeof *w);
  if (!w || !ring_init(&w->r)) {
    free(w);
    return MC_ERR_NOMEM;
  }
  w->f = f;
  w->ok = 1;
  mbedtls_sha256_init(&w->sha);
  mbedtls_sha256_starts(&w->sha, 0);
  if (mc_thread_start(&w->r.thread, writer_main, w) != MC_OK) {
    mbedtls_sha256_free(&w->sha);
    ring_destroy(&w->r);
    free(w);
    return MC_ERR_NOMEM;
  }
  *out = w;
  return MC_OK;
}

void mc_file_writer_put(mc_file_writer *w, const void *data, size_t len) {
  unsigned char *slot = ring_claim(&w->r); /* never NULL: the writer doesn't stop */
  if (len) memcpy(slot, data, len);
  ring_push(&w->r, len);
}

int mc_file_writer_finish(mc_file_writer *w, unsigned char digest[32]) {
  ring_end(&w->r);
  mc_thread_join(&w->r.thread);
  int ok = w->ok;
  mbedtls_sha256_finish(&w->sha, digest);
  mbedtls_sha256_free(&w->sha);
  ring_destroy(&w->r);
  free(w);
  return ok;
}
