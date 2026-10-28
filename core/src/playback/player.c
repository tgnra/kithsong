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
 * thread knows when to report the next track. The same count, taken a segment at a time,
 * is the time listened that decides when a play counts.
 *
 * With shuffle on, the queue plays in an order of places (shuffle.c) rather than item by
 * item; segments note both. In order, an item's place is its index. Turning shuffle on or
 * off renumbers the places from the last item in the buffer, so what's buffered still plays
 * and the new order carries on after it.
 *
 * The callback copies out of the ring buffer, applies the volume and counts. A flush (seek,
 * skip, stop) resets the buffer from the player thread, so the two share a spinlock that the
 * callback holds just for the copy (and the volume). Everything else is under the mutex; the thread drops it while
 * opening, decoding and touching the device, so calls from the UI never wait on the disk.
 */
#include "musiccore/playback.h"

#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"
#include "playback/decoder.h"
#include "playback/shuffle.h"
#include "util/platform.h"

enum {
  CHANNELS = 2,
  BUFFER_MS = 1000, /* decoded ahead */
  WAKE_MS = 40,     /* how often the thread tops up the buffer and checks for a new track */
  CHUNK = 4096,     /* frames decoded at a time */
  MAX_SEGMENTS = 16,
  RESTART_MS = 3000, /* previous() restarts the item after this far in */
  MAX_PLAYED = 16,   /* counted plays waiting for mc_player_take_played() */
  MAX_PLAY_MS = 4 * 60 * 1000,
  FADE_MS = 10, /* a volume change goes all the way from silence to full in this long */
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
  int pos;             /* its place in the play order */
  uint64_t start;      /* its first frame in the stream */
  uint64_t offset;     /* output frames into the item at start: after a seek, not 0 */
  int64_t duration_ms; /* 0 if unknown */
  int new_play;        /* starts a play once heard; not after a seek, which carries one on */
} segment;

typedef struct counted_play {
  int index;
  int64_t started_ms;
} counted_play;

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
  int shuffle;
  mc_shuffle order; /* while shuffle is on */
  command cmd; /* waiting for the thread; CMD_START plays cmd_index (at place cmd_pos) from cmd_ms, then cmd_state */
  int cmd_index, cmd_pos;
  int64_t cmd_ms;
  mc_player_state cmd_state;
  int cmd_seek;  /* the CMD_START is a seek */
  unsigned gen;  /* bumped by every command, so the thread can tell its work went stale */
  int current;   /* the current item while there are no segments; -1 with an empty queue */
  int current_pos;
  segment segs[MAX_SEGMENTS]; /* segs[0] is audible, the rest are buffered after it */
  int nsegs;
  int decoding;      /* the source's item */
  int decoding_pos;
  int drained;       /* the source ran out after the last playable item */
  uint64_t written;  /* stream frames put into the ring buffer */
  ma_uint32 rate;    /* output rate, once the device is open */
  int device_open, device_running;
  /* The play being heard. heard is the stream frames already counted towards plays. */
  uint64_t heard;
  int play_index;
  uint64_t play_frames;
  int64_t play_started_ms;
  int play_counted;
  counted_play plays[MAX_PLAYED]; /* counted, oldest first */
  int nplays;

  /* Player thread only. */
  source *src;
  ma_device device;
  ma_pcm_rb rb;

  int volume, muted; /* under lock, as set */

  /* Shared with the callback, under spin. */
  ma_spinlock spin;
  uint64_t played; /* stream frames handed to the device */
  float gain_target; /* from volume and muted */
  float gain;        /* fading towards gain_target */
};

/* The queue item at place pos in the play order, or -1 if there's none. Under lock. */
static int item_at(mc_player *p, int pos) {
  if (p->shuffle) return mc_shuffle_at(&p->order, pos);
  return pos >= 0 && pos < p->count ? pos : -1;
}

static uint32_t new_seed(void) {
  uint32_t seed;
  if (mc_random_bytes(&seed, sizeof seed) != MC_OK) seed = (uint32_t)mc_mono_ms() * 2654435761u;
  return seed;
}

/* The volume's gain: the cube of the fraction, roughly how loud it sounds. */
static float volume_gain(int volume, int muted) {
  float v = muted ? 0.0f : (float)volume / 100.0f;
  return v * v * v;
}

/* Scales frames of out by the gain, fading it towards target. Under spin. */
static void apply_gain(mc_player *p, float *out, ma_uint32 frames, float target, ma_uint32 rate) {
  float g = p->gain;
  if (g == target && g == 1.0f) return;
  float step = 1.0f / (float)(rate ? rate * FADE_MS / 1000 : 480);
  for (ma_uint32 i = 0; i < frames; i++) {
    if (g < target) g = g + step < target ? g + step : target;
    else if (g > target) g = g - step > target ? g - step : target;
    for (int c = 0; c < CHANNELS; c++) out[(size_t)i * CHANNELS + c] *= g;
  }
  p->gain = g;
}

static void on_audio(ma_device *device, void *output, const void *input, ma_uint32 frames) {
  (void)input;
  mc_player *p = device->pUserData;
  float *out = output; /* miniaudio has already filled it with silence */
  ma_uint32 done = 0;
  ma_spinlock_lock(&p->spin);
  float target = p->gain_target;
  while (done < frames) {
    ma_uint32 n = frames - done;
    void *buf;
    if (ma_pcm_rb_acquire_read(&p->rb, &n, &buf) != MA_SUCCESS || !n) break;
    memcpy(out + (size_t)done * CHANNELS, buf, (size_t)n * CHANNELS * sizeof *out);
    ma_pcm_rb_commit_read(&p->rb, n);
    done += n;
  }
  p->played += done;
  apply_gain(p, out, done, target, device->sampleRate);
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
  st->shuffle = p->shuffle;
  st->volume = p->volume;
  st->muted = p->muted;
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

/* Opens the first playable item in the play order from *index (at place *pos) on, seeking
 * the first one to ms. Drops the lock meanwhile. Returns NULL if none could be opened (with
 * *index -1; shuffled, after a try of as many items as the queue has), or if gen moved on
 * (another command came in), in which case the caller gives up. */
static source *open_from(mc_player *p, int *pos, int *index, int64_t ms, unsigned gen, unsigned *events) {
  for (int tries = 0; *index >= 0 && tries < p->count; tries++) {
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
    *index = item_at(p, ++*pos);
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
  p->current_pos = 0;
  p->current = item_at(p, 0);
}

/* Carries out p->cmd. The command stays posted (and is what mc_player_status_get reports)
 * until it's done, as opening the device and the file drop the lock. If another command
 * comes in meanwhile, this one is abandoned for it. */
static unsigned do_command(mc_player *p) {
  int asked = p->cmd_pos, pos = asked, index = p->cmd_index;
  int64_t ms = p->cmd_ms;
  int seek = p->cmd_seek;
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
    p->current_pos = pos;
    return events | EV(MC_PLAYER_EVENT_ERROR) | EV(MC_PLAYER_EVENT_TRACK);
  }
  source *src = open_from(p, &pos, &index, ms, gen, &events);
  if (gen != p->gen) return events & EV(MC_PLAYER_EVENT_ERROR);
  p->cmd = CMD_NONE;
  events |= EV(MC_PLAYER_EVENT_TRACK);
  if (!src) { /* nothing playable from there on */
    stop_at_start(p);
    return events;
  }
  if (pos != asked) ms = 0; /* skipped past the one asked for */
  flush(p);
  p->src = src;
  p->decoding = p->current = index;
  p->decoding_pos = p->current_pos = pos;
  p->segs[0] = (segment){index, pos, p->written, (uint64_t)ms * p->rate / 1000, source_duration_ms(src),
                         !seek || pos != asked};
  p->nsegs = 1;
  p->state = p->cmd_state; /* read now: pause() and resume() change it without a new command */
  return events;
}

/* Fills the ring buffer, moving on to the next item when the source runs out. Under lock. */
static unsigned fill(mc_player *p) {
  unsigned events = 0;
  while (p->cmd == CMD_NONE && !p->quit) {
    if (p->src) {
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
    } else if (p->drained || !p->nsegs) {
      break;
    }

    /* The source ran out (or there was none, as the last item had run out before shuffle
     * came on): carry on with the next item, straight after it in the buffer. */
    if (p->nsegs == MAX_SEGMENTS) break; /* lots of very short tracks; wait for some to play */
    int pos = p->decoding_pos + 1, index = item_at(p, pos);
    unsigned gen = p->gen;
    source *done = p->src;
    p->src = NULL;
    source *next = open_from(p, &pos, &index, 0, gen, &events);
    /* The resampler holds the last few frames back. Given the same converter, the next item
     * continues the stream exactly, as one file would: the usual case, as albums keep one
     * format. Across a change of rate those frames are lost, which is under a millisecond. */
    if (next && done && next->rate == done->rate && next->channels == done->channels) {
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
    p->decoding_pos = pos;
    p->segs[p->nsegs++] = (segment){index, pos, p->written, 0, source_duration_ms(next), 1};
  }
  return events;
}

/* Counts the frames heard since the last call towards the plays they belong to, and queues
 * a play for mc_player_take_played() once it counts. Under lock. */
static unsigned count_heard(mc_player *p, uint64_t played) {
  unsigned events = 0;
  for (int i = 0; i < p->nsegs && p->rate; i++) {
    segment *seg = &p->segs[i];
    uint64_t end = i + 1 < p->nsegs ? p->segs[i + 1].start : UINT64_MAX;
    uint64_t from = p->heard > seg->start ? p->heard : seg->start;
    uint64_t to = played < end ? played : end;
    if (to <= from) continue;
    if (seg->new_play || seg->index != p->play_index) {
      seg->new_play = 0;
      p->play_index = seg->index;
      p->play_frames = 0;
      p->play_counted = 0;
      p->play_started_ms = (int64_t)mc_wall_ms() - (int64_t)((played - from) * 1000 / p->rate);
    }
    p->play_frames += to - from;
    if (!p->play_counted &&
        mc_play_counts(seg->duration_ms, (int64_t)(p->play_frames * 1000 / p->rate))) {
      p->play_counted = 1;
      if (p->nplays == MAX_PLAYED) /* nobody's taking them: keep the newest */
        memmove(p->plays, p->plays + 1, (size_t)--p->nplays * sizeof *p->plays);
      p->plays[p->nplays++] = (counted_play){seg->index, p->play_started_ms};
      events |= EV(MC_PLAYER_EVENT_PLAYED);
    }
  }
  p->heard = played;
  return events;
}

/* Notices when the next item becomes audible, and when the last one has finished. */
static unsigned advance(mc_player *p) {
  uint64_t played = played_frames(p);
  unsigned events = count_heard(p, played);
  int gone = 0;
  while (gone + 1 < p->nsegs && played >= p->segs[gone + 1].start) gone++;
  if (gone) {
    p->nsegs -= gone;
    memmove(p->segs, p->segs + gone, (size_t)p->nsegs * sizeof *p->segs);
    p->current = p->segs[0].index;
    p->current_pos = p->segs[0].pos;
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
      for (int e = MC_PLAYER_EVENT_STATE; e <= MC_PLAYER_EVENT_PLAYED; e++)
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
  p->current = p->decoding = p->play_index = -1;
  p->current_pos = p->decoding_pos = -1;
  p->volume = 100;
  p->gain = p->gain_target = 1.0f;
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

/* Under lock. index is the item at place pos. */
static void post(mc_player *p, command cmd, int pos, int index, int64_t ms, mc_player_state state) {
  p->cmd = cmd;
  p->cmd_pos = pos;
  p->cmd_index = index;
  p->cmd_ms = ms > 0 ? ms : 0;
  p->cmd_state = state;
  p->cmd_seek = 0;
  p->gen++;
  mc_cond_broadcast(&p->wake);
}

/* The place in the play order of the item the UI is showing as current (see snapshot()).
 * Under lock. */
static int current_pos(mc_player *p) {
  if (p->cmd == CMD_START) return p->cmd_pos;
  if (p->cmd == CMD_NONE && p->nsegs) return audible(p)->pos;
  return p->current_pos;
}

mc_status mc_player_set_queue(mc_player *p, const char *const *paths, int count, int start) {
  if (!p || count < 0 || (count && (!paths || start < -1 || start >= count))) return MC_ERR_INVALID_ARG;
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
  p->nplays = 0;
  p->play_index = -1;
  mc_shuffle_init(&p->order, count, new_seed(), start);
  int pos = start >= 0 && !p->shuffle ? start : 0;
  if (count) {
    post(p, CMD_START, pos, item_at(p, pos), 0, MC_PLAYER_PLAYING);
  } else {
    p->current = p->current_pos = -1;
    post(p, CMD_STOP, -1, -1, 0, MC_PLAYER_STOPPED);
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_skip_to(mc_player *p, int index) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_status st = index >= 0 && index < p->count ? MC_OK : MC_ERR_INVALID_ARG;
  if (st == MC_OK && p->shuffle) { /* that one first, then the rest shuffled afresh */
    mc_shuffle_init(&p->order, p->count, new_seed(), index);
    post(p, CMD_START, 0, index, 0, MC_PLAYER_PLAYING);
  } else if (st == MC_OK) {
    post(p, CMD_START, index, index, 0, MC_PLAYER_PLAYING);
  }
  mc_mutex_unlock(&p->lock);
  return st;
}

mc_status mc_player_next(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  int pos = current_pos(p) + 1, index = item_at(p, pos);
  if (index >= 0) {
    post(p, CMD_START, pos, index, 0, MC_PLAYER_PLAYING);
  } else if (p->count) { /* past the end: stopped at the start, as when the queue plays out */
    p->current_pos = 0;
    p->current = item_at(p, 0);
    post(p, CMD_STOP, 0, p->current, 0, MC_PLAYER_STOPPED);
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
    int pos = current_pos(p), index = st.index, before = item_at(p, pos - 1);
    if (before >= 0 && st.position_ms < RESTART_MS) {
      pos--;
      index = before;
    }
    post(p, CMD_START, pos, index, 0, MC_PLAYER_PLAYING);
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
  else if (st.state == MC_PLAYER_STOPPED && p->count && st.index >= 0 && st.index < p->count)
    post(p, CMD_START, current_pos(p), st.index, 0, MC_PLAYER_PLAYING);
  else if (st.state == MC_PLAYER_STOPPED && p->count)
    post(p, CMD_START, 0, item_at(p, 0), 0, MC_PLAYER_PLAYING);
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
  mc_player_status st;
  snapshot(p, &st);
  int pos = current_pos(p);
  p->current = st.index;
  p->current_pos = pos;
  post(p, CMD_STOP, pos, st.index, 0, MC_PLAYER_STOPPED);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_seek_ms(mc_player *p, int64_t position_ms) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_player_status st;
  snapshot(p, &st);
  if (st.index >= 0 && st.index < p->count) {
    post(p, CMD_START, current_pos(p), st.index, position_ms,
         st.state == MC_PLAYER_PAUSED ? MC_PLAYER_PAUSED : MC_PLAYER_PLAYING);
    p->cmd_seek = position_ms > 0; /* back to the start is starting it again */

  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

/* Under lock. With the device stopped there's nothing to fade, so the next sound starts at
 * the new volume. */
static void update_gain(mc_player *p) {
  float gain = volume_gain(p->volume, p->muted);
  ma_spinlock_lock(&p->spin);
  p->gain_target = gain;
  if (!p->device_running) p->gain = gain;
  ma_spinlock_unlock(&p->spin);
}

mc_status mc_player_set_volume(mc_player *p, int volume) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  p->volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
  update_gain(p);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_set_muted(mc_player *p, int muted) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  p->muted = muted != 0;
  update_gain(p);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_set_shuffle(mc_player *p, int on) {
  if (!p) return MC_ERR_INVALID_ARG;
  on = on != 0;
  mc_mutex_lock(&p->lock);
  if (on != p->shuffle) {
    /* The new order carries on from the last item in the buffer, as what's buffered plays
     * anyway: usually the one playing. Or from the one about to start, or the current one. */
    int last = p->cmd == CMD_START               ? p->cmd_index
               : p->cmd == CMD_NONE && p->nsegs ? p->segs[p->nsegs - 1].index
                                                 : p->current;
    p->shuffle = on;
    if (on) mc_shuffle_init(&p->order, p->count, new_seed(), last);
    int pos = on ? 0 : last; /* in order, an item's place is its index */
    p->cmd_pos = p->current_pos = p->decoding_pos = pos;
    for (int i = 0; i < p->nsegs; i++) p->segs[i].pos = on ? pos - (p->nsegs - 1 - i) : p->segs[i].index;
    if (p->cmd == CMD_NONE && p->nsegs) p->current_pos = p->segs[0].pos;
    if (on) p->drained = 0; /* the last item may have run out already: carry on after it */
    mc_cond_broadcast(&p->wake);
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_take_played(mc_player *p, int *index, int64_t *started_ms) {
  if (!p || !index || !started_ms) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_status st = p->nplays ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK) {
    *index = p->plays[0].index;
    *started_ms = p->plays[0].started_ms;
    memmove(p->plays, p->plays + 1, (size_t)--p->nplays * sizeof *p->plays);
  }
  mc_mutex_unlock(&p->lock);
  return st;
}

int mc_play_counts(int64_t duration_ms, int64_t listened_ms) {
  int64_t needed = duration_ms > 0 && duration_ms / 2 < MAX_PLAY_MS ? duration_ms / 2 : MAX_PLAY_MS;
  return listened_ms >= needed;
}

void mc_player_status_get(mc_player *p, mc_player_status *out) {
  if (!out) return;
  if (!p) {
    memset(out, 0, sizeof *out);
    out->index = -1;
    out->volume = 100;
    return;
  }
  mc_mutex_lock(&p->lock);
  snapshot(p, out);
  mc_mutex_unlock(&p->lock);
}
