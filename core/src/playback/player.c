/* Playback.
 *
 *   player thread                                  audio thread (miniaudio)
 *   source: decoder -> converter --> ring buffer --> device callback
 *
 * The player thread owns the source (the open queue item). It keeps the ring buffer about
 * full, and when a source runs out it opens the next queue item and goes on writing: that's
 * what makes playback gapless. Every source is converted to one output format (f32 stereo
 * at the device's rate), so the stream never changes format between tracks.
 *
 * Each item's first frame in the stream is noted as a segment. The frames the callback has
 * consumed ("played") then say which item is audible and how far into it, which is how the
 * thread knows when to report the next track.
 *
 * The callback only copies out of the ring buffer and counts. A flush (seek, skip, stop)
 * resets the buffer from the player thread, so the two share a spinlock that the callback
 * holds just for the copy. Everything else is under the mutex; the thread drops it while
 * opening, decoding and touching the device, so calls from the UI never wait on the disk.
 */
#include "musiccore/playback.h"

#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"
#include "playback/decoder.h"
#include "util/platform.h"

enum {
  CHANNELS = 2,
  BUFFER_MS = 1000, /* decoded ahead */
  WAKE_MS = 40,     /* how often the thread tops up the buffer and checks for a new track */
  CHUNK = 4096,     /* frames decoded at a time */
  MAX_SEGMENTS = 16,
  RESTART_MS = 3000, /* previous() restarts the item after this far in */
};

/* --- Sources --- */

typedef struct source {
  mc_decoder *dec;
  ma_uint32 channels, rate;
  ma_uint64 length; /* frames at rate, 0 if unknown */
  ma_data_converter *conv; /* passed on to the next item when its format is the same */
  float *in; /* decoded but not yet converted: up to CHUNK frames */
  ma_uint64 in_count, in_pos;
} source;

static void source_close(source *s) {
  if (!s) return;
  mc_decoder_close(s->dec);
  if (s->conv) ma_data_converter_uninit(s->conv, NULL);
  free(s->conv);
  free(s->in);
  free(s);
}

/* Opens path for output at rate, or returns NULL (with *err set) if it can't be played. */
static source *source_open(const char *path, ma_uint32 rate, mc_status *err) {
  source *s = calloc(1, sizeof *s);
  if (!s) {
    *err = MC_ERR_NOMEM;
    return NULL;
  }
  if ((*err = mc_decoder_open(path, &s->dec)) != MC_OK) {
    free(s);
    return NULL;
  }
  s->channels = mc_decoder_channels(s->dec);
  s->rate = mc_decoder_rate(s->dec);
  s->length = mc_decoder_length(s->dec);

  ma_data_converter_config config =
    ma_data_converter_config_init(ma_format_f32, ma_format_f32, s->channels, CHANNELS, s->rate, rate);
  config.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
  s->in = malloc((size_t)CHUNK * s->channels * sizeof *s->in);
  ma_data_converter *conv = s->in ? malloc(sizeof *conv) : NULL;
  if (!conv || ma_data_converter_init(&config, NULL, conv) != MA_SUCCESS) {
    free(conv);
    source_close(s);
    *err = MC_ERR_NOMEM;
    return NULL;
  }
  s->conv = conv;
  *err = MC_OK;
  return s;
}

static int64_t source_duration_ms(const source *s) {
  return (int64_t)(s->length * 1000 / s->rate);
}

/* Up to frames output frames; fewer only at the end. */
static ma_uint64 source_read(source *s, float *out, ma_uint64 frames) {
  ma_uint64 done = 0;
  while (done < frames) {
    if (s->in_pos == s->in_count) {
      s->in_count = mc_decoder_read(s->dec, s->in, CHUNK);
      s->in_pos = 0;
      if (!s->in_count) break;
    }
    ma_uint64 in = s->in_count - s->in_pos, got = frames - done;
    ma_data_converter_process_pcm_frames(s->conv, s->in + s->in_pos * s->channels, &in,
                                         out + done * CHANNELS, &got);
    s->in_pos += in;
    done += got;
    if (!in && !got) break;
  }
  return done;
}

static void source_seek(source *s, int64_t ms) {
  mc_decoder_seek(s->dec, (ma_uint64)ms * s->rate / 1000);
  ma_data_converter_reset(s->conv);
  s->in_pos = s->in_count = 0;
}

/* --- Player --- */

typedef struct segment {
  int index;           /* queue item */
  uint64_t start;      /* its first frame in the stream */
  uint64_t offset;     /* output frames into the item at start: after a seek, not 0 */
  int64_t duration_ms; /* 0 if unknown */
} segment;

typedef enum command { CMD_NONE, CMD_START, CMD_STOP } command;

/* Events to report, as bits. */
#define EV(e) (1u << (e))

struct mc_player {
  mc_player_cb cb;
  void *user;
  mc_thread thread;
  mc_mutex lock;
  mc_cond wake;

  /* Under lock. */
  int quit;
  char **queue;
  int count;
  mc_player_state state;
  command cmd; /* waiting for the thread; CMD_START plays cmd_index from cmd_ms, then cmd_state */
  int cmd_index;
  int64_t cmd_ms;
  mc_player_state cmd_state;
  unsigned gen;  /* bumped by every command, so the thread can tell its work went stale */
  int current;   /* the current item while there are no segments; -1 with an empty queue */
  segment segs[MAX_SEGMENTS]; /* segs[0] is audible, the rest are buffered after it */
  int nsegs;
  int decoding;      /* the source's item */
  int drained;       /* the source ran out after the last playable item */
  uint64_t written;  /* stream frames put into the ring buffer */
  ma_uint32 rate;    /* output rate, once the device is open */
  int device_open, device_running;

  /* Player thread only. */
  source *src;
  ma_device device;
  ma_pcm_rb rb;

  /* Shared with the callback, under spin. */
  ma_spinlock spin;
  uint64_t played; /* stream frames handed to the device */
};

static void on_audio(ma_device *device, void *output, const void *input, ma_uint32 frames) {
  (void)input;
  mc_player *p = device->pUserData;
  float *out = output; /* miniaudio has already filled it with silence */
  ma_uint32 done = 0;
  ma_spinlock_lock(&p->spin);
  while (done < frames) {
    ma_uint32 n = frames - done;
    void *buf;
    if (ma_pcm_rb_acquire_read(&p->rb, &n, &buf) != MA_SUCCESS || !n) break;
    memcpy(out + (size_t)done * CHANNELS, buf, (size_t)n * CHANNELS * sizeof *out);
    ma_pcm_rb_commit_read(&p->rb, n);
    done += n;
  }
  p->played += done;
  ma_spinlock_unlock(&p->spin);
}

static uint64_t played_frames(mc_player *p) {
  ma_spinlock_lock(&p->spin);
  uint64_t played = p->played;
  ma_spinlock_unlock(&p->spin);
  return played;
}

/* Under lock, from the player thread: drops everything buffered. */
static void flush(mc_player *p) {
  ma_spinlock_lock(&p->spin);
  if (p->device_open) ma_pcm_rb_reset(&p->rb);
  p->written = p->played;
  ma_spinlock_unlock(&p->spin);
  p->nsegs = 0;
}

/* The audible segment: the last one that has started. */
static const segment *audible(mc_player *p) {
  if (!p->nsegs) return NULL;
  uint64_t played = played_frames(p);
  int i = 0;
  while (i + 1 < p->nsegs && played >= p->segs[i + 1].start) i++;
  return &p->segs[i];
}

static void snapshot(mc_player *p, mc_player_status *st) {
  memset(st, 0, sizeof *st);
  st->count = p->count;
  st->state = p->state;
  st->index = p->current;
  if (p->cmd == CMD_START) { /* report it as done already, so the UI doesn't flicker back */
    st->state = p->cmd_state;
    st->index = p->cmd_index;
    st->position_ms = p->cmd_ms;
    const segment *seg = audible(p);
    if (seg && seg->index == p->cmd_index) st->duration_ms = seg->duration_ms;
  } else if (p->cmd == CMD_STOP) {
    st->state = MC_PLAYER_STOPPED;
  } else if (p->nsegs) {
    uint64_t played = played_frames(p);
    const segment *seg = audible(p);
    st->index = seg->index;
    st->duration_ms = seg->duration_ms;
    uint64_t frames = played - seg->start + seg->offset;
    st->position_ms = p->rate ? (int64_t)(frames * 1000 / p->rate) : 0;
    if (st->duration_ms && st->position_ms > st->duration_ms) st->position_ms = st->duration_ms;
  }
}

/* --- Player thread --- */

/* Opens the device if it isn't yet; it's started separately. Under lock. */
static int ensure_device(mc_player *p) {
  if (p->device_open) return 1;
  mc_mutex_unlock(&p->lock);
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = CHANNELS;
  config.sampleRate = 0; /* the device's own, so the OS doesn't resample again */
  config.dataCallback = on_audio;
  config.pUserData = p;
  int ok = ma_device_init(NULL, &config, &p->device) == MA_SUCCESS;
  if (ok && ma_pcm_rb_init(ma_format_f32, CHANNELS, p->device.sampleRate * BUFFER_MS / 1000, NULL, NULL, &p->rb) !=
              MA_SUCCESS) {
    ma_device_uninit(&p->device);
    ok = 0;
  }
  mc_mutex_lock(&p->lock);
  if (ok) {
    p->device_open = 1;
    p->rate = p->device.sampleRate;
  }
  return ok;
}

/* Starts or stops the device to match the state. Under lock. */
static unsigned sync_device(mc_player *p) {
  int want = p->state == MC_PLAYER_PLAYING;
  if (want == p->device_running || !p->device_open) return 0;
  mc_mutex_unlock(&p->lock);
  int ok = (want ? ma_device_start(&p->device) : ma_device_stop(&p->device)) == MA_SUCCESS;
  mc_mutex_lock(&p->lock);
  if (ok) {
    p->device_running = want;
    return 0;
  }
  if (want) { /* the device went away, say */
    p->state = MC_PLAYER_STOPPED;
    return EV(MC_PLAYER_EVENT_ERROR) | EV(MC_PLAYER_EVENT_STATE);
  }
  return 0;
}

/* Opens the first playable item from index on, seeking the first one to ms. Drops the lock
 * meanwhile. Returns NULL with *index past the end if none could be opened, or if gen moved
 * on (another command came in), in which case the caller gives up. */
static source *open_from(mc_player *p, int *index, int64_t ms, unsigned gen, unsigned *events) {
  while (*index < p->count) {
    char *path = mc_strdup(p->queue[*index]);
    ma_uint32 rate = p->rate;
    mc_mutex_unlock(&p->lock);
    mc_status err = MC_ERR_NOMEM;
    source *src = path ? source_open(path, rate, &err) : NULL;
    if (src && ms > 0) source_seek(src, ms);
    free(path);
    mc_mutex_lock(&p->lock);
    if (gen != p->gen) {
      source_close(src);
      return NULL;
    }
    if (src) return src;
    *events |= EV(MC_PLAYER_EVENT_ERROR);
    ++*index;
    ms = 0;
  }
  return NULL;
}

static void close_source(mc_player *p) {
  source_close(p->src);
  p->src = NULL;
  p->decoding = -1;
  p->drained = 0;
}

/* Stopped, at the start of the queue: where playing off the end leaves it. */
static void stop_at_start(mc_player *p) {
  close_source(p);
  flush(p);
  p->state = MC_PLAYER_STOPPED;
  p->current = p->count ? 0 : -1;
}

/* Carries out p->cmd. The command stays posted (and is what mc_player_status_get reports)
 * until it's done, as opening the device and the file drop the lock. If another command
 * comes in meanwhile, this one is abandoned for it. */
static unsigned do_command(mc_player *p) {
  int asked = p->cmd_index, index = asked;
  int64_t ms = p->cmd_ms;
  unsigned gen = p->gen, events = EV(MC_PLAYER_EVENT_STATE);
  close_source(p);

  if (p->cmd == CMD_STOP) {
    p->cmd = CMD_NONE;
    flush(p);
    p->state = MC_PLAYER_STOPPED;
    return events;
  }
  int ok = ensure_device(p);
  if (gen != p->gen) return 0;
  if (!ok) {
    p->cmd = CMD_NONE;
    flush(p);
    p->state = MC_PLAYER_STOPPED;
    p->current = index;
    return events | EV(MC_PLAYER_EVENT_ERROR) | EV(MC_PLAYER_EVENT_TRACK);
  }
  source *src = open_from(p, &index, ms, gen, &events);
  if (gen != p->gen) return events & EV(MC_PLAYER_EVENT_ERROR);
  p->cmd = CMD_NONE;
  events |= EV(MC_PLAYER_EVENT_TRACK);
  if (!src) { /* nothing playable from there on */
    stop_at_start(p);
    return events;
  }
  if (index != asked) ms = 0; /* skipped past the one asked for */
  flush(p);
  p->src = src;
  p->decoding = p->current = index;
  p->segs[0] = (segment){index, p->written, (uint64_t)ms * p->rate / 1000, source_duration_ms(src)};
  p->nsegs = 1;
  p->state = p->cmd_state; /* read now: pause() and resume() change it without a new command */
  return events;
}

/* Fills the ring buffer, moving on to the next item when the source runs out. Under lock. */
static unsigned fill(mc_player *p) {
  unsigned events = 0;
  while (p->src && p->cmd == CMD_NONE && !p->quit) {
    ma_uint32 n = CHUNK;
    void *buf;
    if (ma_pcm_rb_acquire_write(&p->rb, &n, &buf) != MA_SUCCESS || !n) break;
    source *src = p->src;
    mc_mutex_unlock(&p->lock);
    ma_uint64 got = source_read(src, buf, n);
    mc_mutex_lock(&p->lock);
    ma_pcm_rb_commit_write(&p->rb, (ma_uint32)got);
    p->written += got;
    if (got == n || p->cmd != CMD_NONE) continue;

    /* The source ran out: carry on with the next item, straight after it in the buffer. */
    if (p->nsegs == MAX_SEGMENTS) break; /* lots of very short tracks; wait for some to play */
    int index = p->decoding + 1;
    unsigned gen = p->gen;
    source *done = p->src;
    p->src = NULL;
    source *next = open_from(p, &index, 0, gen, &events);
    /* The resampler holds the last few frames back. Given the same converter, the next item
     * continues the stream exactly, as one file would: the usual case, as albums keep one
     * format. Across a change of rate those frames are lost, which is under a millisecond. */
    if (next && next->rate == done->rate && next->channels == done->channels) {
      ma_data_converter *conv = next->conv;
      next->conv = done->conv;
      done->conv = conv;
    }
    source_close(done);
    p->decoding = -1;
    p->drained = 0;
    if (gen != p->gen) {
      source_close(next);
      break;
    }
    if (!next) {
      p->drained = 1;
      break;
    }
    p->src = next;
    p->decoding = index;
    p->segs[p->nsegs++] = (segment){index, p->written, 0, source_duration_ms(next)};
  }
  return events;
}

/* Notices when the next item becomes audible, and when the last one has finished. */
static unsigned advance(mc_player *p) {
  unsigned events = 0;
  uint64_t played = played_frames(p);
  int gone = 0;
  while (gone + 1 < p->nsegs && played >= p->segs[gone + 1].start) gone++;
  if (gone) {
    p->nsegs -= gone;
    memmove(p->segs, p->segs + gone, (size_t)p->nsegs * sizeof *p->segs);
    p->current = p->segs[0].index;
    events |= EV(MC_PLAYER_EVENT_TRACK);
  }
  if (p->drained && !p->src && played >= p->written && p->state != MC_PLAYER_STOPPED) {
    stop_at_start(p);
    events |= EV(MC_PLAYER_EVENT_STATE) | EV(MC_PLAYER_EVENT_TRACK);
  }
  return events;
}

static void run(void *arg) {
  mc_player *p = arg;
  mc_mutex_lock(&p->lock);
  while (!p->quit) {
    unsigned events = 0;
    if (p->cmd != CMD_NONE) events |= do_command(p);
    if (p->cmd == CMD_NONE) {
      if (p->state != MC_PLAYER_STOPPED) events |= fill(p); /* paused too, so resuming is instant */
      events |= advance(p);
      events |= sync_device(p);
    }
    if (events && p->cb) {
      mc_mutex_unlock(&p->lock);
      for (int e = MC_PLAYER_EVENT_STATE; e <= MC_PLAYER_EVENT_ERROR; e++)
        if (events & EV(e)) p->cb(p->user, (mc_player_event)e);
      mc_mutex_lock(&p->lock);
    }
    if (p->quit || p->cmd != CMD_NONE) continue;
    /* While playing, wake to top up the buffer and to see the next track start. Otherwise
     * there's nothing to do until a call. */
    if (p->state == MC_PLAYER_PLAYING) mc_cond_wait_ms(&p->wake, &p->lock, WAKE_MS);
    else if (!events) mc_cond_wait(&p->wake, &p->lock);
  }
  mc_mutex_unlock(&p->lock);
}

/* --- API --- */

mc_status mc_player_new(mc_player_cb cb, void *user, mc_player **out) {
  if (!out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_player *p = calloc(1, sizeof *p);
  if (!p) return MC_ERR_NOMEM;
  p->cb = cb;
  p->user = user;
  p->current = p->decoding = -1;
  mc_mutex_init(&p->lock);
  mc_cond_init(&p->wake);
  mc_status st = mc_thread_start(&p->thread, run, p);
  if (st != MC_OK) {
    mc_cond_destroy(&p->wake);
    mc_mutex_destroy(&p->lock);
    free(p);
    return st;
  }
  *out = p;
  return MC_OK;
}

static void free_queue(mc_player *p) {
  for (int i = 0; i < p->count; i++) free(p->queue[i]);
  free(p->queue);
  p->queue = NULL;
  p->count = 0;
}

void mc_player_free(mc_player *p) {
  if (!p) return;
  mc_mutex_lock(&p->lock);
  p->quit = 1;
  mc_cond_broadcast(&p->wake);
  mc_mutex_unlock(&p->lock);
  mc_thread_join(&p->thread);
  if (p->device_open) {
    ma_device_uninit(&p->device);
    ma_pcm_rb_uninit(&p->rb);
  }
  source_close(p->src);
  free_queue(p);
  mc_cond_destroy(&p->wake);
  mc_mutex_destroy(&p->lock);
  free(p);
}

/* Under lock. */
static void post(mc_player *p, command cmd, int index, int64_t ms, mc_player_state state) {
  p->cmd = cmd;
  p->cmd_index = index;
  p->cmd_ms = ms > 0 ? ms : 0;
  p->cmd_state = state;
  p->gen++;
  mc_cond_broadcast(&p->wake);
}

/* The item the UI is showing as current. Under lock. */
static int current_index(mc_player *p) {
  mc_player_status st;
  snapshot(p, &st);
  return st.index;
}

mc_status mc_player_set_queue(mc_player *p, const char *const *paths, int count, int start) {
  if (!p || count < 0 || (count && (!paths || start < 0 || start >= count))) return MC_ERR_INVALID_ARG;
  char **queue = count ? calloc((size_t)count, sizeof *queue) : NULL;
  if (count && !queue) return MC_ERR_NOMEM;
  for (int i = 0; i < count; i++) {
    if (!paths[i] || !(queue[i] = mc_strdup(paths[i]))) {
      while (i--) free(queue[i]);
      free(queue);
      return paths[i] ? MC_ERR_NOMEM : MC_ERR_INVALID_ARG;
    }
  }
  mc_mutex_lock(&p->lock);
  free_queue(p);
  p->queue = queue;
  p->count = count;
  if (count) {
    post(p, CMD_START, start, 0, MC_PLAYER_PLAYING);
  } else {
    p->current = -1;
    post(p, CMD_STOP, -1, 0, MC_PLAYER_STOPPED);
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_skip_to(mc_player *p, int index) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_status st = index >= 0 && index < p->count ? MC_OK : MC_ERR_INVALID_ARG;
  if (st == MC_OK) post(p, CMD_START, index, 0, MC_PLAYER_PLAYING);
  mc_mutex_unlock(&p->lock);
  return st;
}

mc_status mc_player_next(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  int next = current_index(p) + 1;
  if (next > 0 && next < p->count) {
    post(p, CMD_START, next, 0, MC_PLAYER_PLAYING);
  } else if (p->count) { /* past the end: stopped at the start, as when the queue plays out */
    p->current = 0;
    post(p, CMD_STOP, 0, 0, MC_PLAYER_STOPPED);
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_previous(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_player_status st;
  snapshot(p, &st);
  if (st.index >= 0) {
    int index = st.index > 0 && st.position_ms < RESTART_MS ? st.index - 1 : st.index;
    post(p, CMD_START, index, 0, MC_PLAYER_PLAYING);
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_pause(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  if (p->cmd == CMD_START && p->cmd_state == MC_PLAYER_PLAYING) p->cmd_state = MC_PLAYER_PAUSED;
  if (p->state == MC_PLAYER_PLAYING) p->state = MC_PLAYER_PAUSED;
  mc_cond_broadcast(&p->wake);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_resume(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_player_status st;
  snapshot(p, &st);
  if (p->cmd == CMD_START) p->cmd_state = MC_PLAYER_PLAYING;
  else if (st.state == MC_PLAYER_PAUSED) p->state = MC_PLAYER_PLAYING;
  else if (st.state == MC_PLAYER_STOPPED && p->count)
    post(p, CMD_START, st.index >= 0 && st.index < p->count ? st.index : 0, 0, MC_PLAYER_PLAYING);
  mc_cond_broadcast(&p->wake);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_toggle(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_player_status st;
  snapshot(p, &st);
  mc_mutex_unlock(&p->lock);
  return st.state == MC_PLAYER_PLAYING ? mc_player_pause(p) : mc_player_resume(p);
}

mc_status mc_player_stop(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  int index = current_index(p);
  p->current = index;
  post(p, CMD_STOP, index, 0, MC_PLAYER_STOPPED);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_seek_ms(mc_player *p, int64_t position_ms) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_player_status st;
  snapshot(p, &st);
  if (st.index >= 0 && st.index < p->count)
    post(p, CMD_START, st.index, position_ms,
         st.state == MC_PLAYER_PAUSED ? MC_PLAYER_PAUSED : MC_PLAYER_PLAYING);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

void mc_player_status_get(mc_player *p, mc_player_status *out) {
  if (!out) return;
  if (!p) {
    memset(out, 0, sizeof *out);
    out->index = -1;
    return;
  }
  mc_mutex_lock(&p->lock);
  snapshot(p, out);
  mc_mutex_unlock(&p->lock);
}
