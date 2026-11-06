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
 * is the time listened that decides when a play counts, and what goes in the play log.
 *
 * A play log row opens when an item is started (or, gapless, first heard) and closes when
 * that play ends, however it ends. Segments carry what the row needs rather than leaving it to
 * the index, as what's still being heard may be the old queue's.
 *
 * With shuffle on, the queue plays in an order of places (shuffle.c) rather than item by
 * item; segments note both. In order, an item's place is its index. Turning shuffle on or
 * off renumbers the places from the audible item, so the new order carries on after it: what
 * was buffered of the items after it goes, as the old order's next track shouldn't still play.
 *
 * A radio play is a queue that a third thread, the feed, adds to from a station. Loading the
 * station and picking take a while on a big library, so neither can be on this thread, whose
 * buffer would run dry, nor in a call from the UI. The feed holds the station and only takes the
 * lock to look and to add. It reports through this thread, so callbacks still come from one
 * place. radio_gen tells it its work went stale, as gen does here.
 *
 * Up next (the queue people add to) is a list of entries, each a block of tracks, played before
 * the next place in the play order. It adds to the items, but not to the places: list says which
 * items have a place, so an item's place isn't always its index, and a station's items go on
 * being added around them. Playing from up next doesn't move the place, so the list carries on
 * after it where it left off. A track leaves up next once it's heard, not when it's buffered:
 * segments note those on their way (waiting), and they're always its first tracks, as changing
 * up next cuts them from the buffer first (up_settle()).
 *
 * The callback copies out of the ring buffer, applies the volume and counts. A flush (seek,
 * skip, stop) resets the buffer from the player thread, so the two share a spinlock that the
 * callback holds just for the copy (and the volume). Everything else is under the mutex; the thread drops it while
 * opening, decoding and touching the device, so calls from the UI never wait on the disk.
 */
#include "musiccore/playback.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "library/library_internal.h"
#include "miniaudio.h"
#include "musiccore/radio.h"
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
  MAX_ENDED = 64,    /* ended plays waiting for mc_player_take_ended() */
  MAX_SKIPS = 8,     /* skipped plays waiting for the feed, which takes them straight away */
  MAX_PLAY_MS = 4 * 60 * 1000,
  FADE_MS = 10, /* a volume change goes all the way from silence to full in this long */
  RADIO_AHEAD = 5, /* tracks a station keeps queued after the current one: skips use them up */
  RADIO_MAX_FAILS = 8, /* items in a row that won't open before a station gives up */
};

#define NO_CUT UINT64_MAX

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
  int index;           /* queue item; -1 once the queue it was from has been replaced */
  int pos;             /* its place in the play order */
  uint64_t start;      /* its first frame in the stream */
  uint64_t offset;     /* output frames into the item at start: after a seek, not 0 */
  int64_t duration_ms; /* 0 if unknown */
  int new_play;        /* starts a play once heard; not after a seek, which carries one on */
  int logged;          /* the play log has a row for it already */
  mc_play_event event; /* that row as it starts: which track, and from what */
  uint32_t entry;      /* the up next entry it's from, or 0 */
  int waiting;         /* ...and it's still there, till it's heard */
} segment;

typedef struct counted_play {
  int index;
  int64_t started_ms;
} counted_play;

/* What a play log row says of the items of an up next entry. */
typedef struct item_origin {
  mc_play_context context;
  char context_uid[MC_UID_STRLEN];
} item_origin;

typedef struct up_track {
  int item;
  int duration_ms;
} up_track;

/* An entry of up next. tracks[first] on are what's left: the ones before it have played, and
 * stay for previous() to go back to. */
typedef struct up_entry {
  uint32_t id;
  char *title, *artist;
  int block;
  int origin; /* into origins, from 1 */
  up_track *tracks;
  int count, first;
} up_entry;

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
  char (*track_uids)[MC_UID_STRLEN]; /* per item, or NULL if the queue isn't logged */
  int *item_origins; /* per item: 0 for the list's, else which of origins it's from, from 1 */
  mc_play_context context;
  char context_uid[MC_UID_STRLEN];
  int count;
  int *list; /* the item at each place, in order: what the play order goes through */
  int list_count;
  /* Up next. Origins are only added to: a played item can go on needing its own. */
  up_entry **up;
  int nup;
  up_entry *spent;       /* the entry whose last track started, for previous() to go back into */
  uint32_t last_id;
  unsigned up_changes;
  item_origin *origins;
  int norigins;
  int played_out;        /* played off the end, or Next did: up next goes first on resume */
  mc_player_state state;
  int shuffle;
  mc_shuffle order; /* while shuffle is on */
  command cmd; /* waiting for the thread; CMD_START plays cmd_index (at place cmd_pos) from cmd_ms, then cmd_state */
  int cmd_index, cmd_pos;
  uint32_t cmd_entry; /* the up next entry cmd_index is from, or 0 */
  int64_t cmd_ms;
  mc_player_state cmd_state;
  int cmd_seek;  /* the CMD_START is a seek */
  mc_play_end cmd_end; /* what the command makes of the play it ends */
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
  /* The play log's open row, while log_open, and its time heard so far. */
  int log_open;
  mc_play_event log;
  uint64_t log_frames;
  mc_play_event ended[MAX_ENDED]; /* closed rows, oldest first */
  int nended;
  unsigned pending_events; /* from the feed, for the thread to report */
  /* The station. Its seed is context_uid. */
  int radio;          /* the queue is a station's: in order whatever shuffle says, as it grows */
  int feeding;        /* and it's still being added to */
  unsigned radio_gen; /* bumped when the station changes or ends, so the feed drops its work */
  char *radio_dir;    /* the library it's from */
  char radio_start[MC_UID_STRLEN]; /* the track it started from, whatever becomes of the queue */
  int radio_skip;     /* next() ran out of items: play the first one added */
  int radio_failures; /* items in a row that wouldn't open */
  mc_play_event feed_skips[MAX_SKIPS]; /* those the station hasn't been told of, oldest first */
  int nfeed_skips;
  int feed_started;
  mc_thread feed;
  mc_cond feed_wake;

  /* Player thread only. */
  source *src;
  ma_device device;
  ma_pcm_rb rb;
  float *cut_frames; /* a buffer's worth, for cut_buffer() */

  int volume, muted; /* under lock, as set */

  /* Shared with the callback, under spin. */
  ma_spinlock spin;
  uint64_t played; /* stream frames handed to the device */
  float gain_target; /* from volume and muted */
  float gain;        /* fading towards gain_target */
  /* The stream frame the buffer is to end at, or NO_CUT. Written under lock as well, so
   * the thread needn't take spin to look. The callback stops there till the thread has cut. */
  uint64_t cut;
};

/* Where in list place pos in the play order is, or -1 if there's none. Under lock. */
static int list_at(mc_player *p, int pos) {
  if (p->shuffle && !p->radio) return mc_shuffle_at(&p->order, pos);
  return pos >= 0 && pos < p->list_count ? pos : -1;
}

/* The queue item at place pos in the play order, or -1 if there's none. Under lock. */
static int item_at(mc_player *p, int pos) {
  int at = list_at(p, pos);
  return at < 0 ? -1 : p->list[at];
}

/* Where item index is in list, which is in order, or -1 if it's up next's. Under lock. */
static int list_of(const mc_player *p, int index) {
  int lo = 0, hi = p->list_count - 1;
  while (lo <= hi) {
    int mid = lo + (hi - lo) / 2;
    if (p->list[mid] == index) return mid;
    if (p->list[mid] < index) lo = mid + 1;
    else hi = mid - 1;
  }
  return -1;
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
  if (p->cut - p->played < frames) frames = (ma_uint32)(p->cut - p->played);
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
  p->cut = NO_CUT;
  ma_spinlock_unlock(&p->spin);
  p->nsegs = 0;
}

/* Under lock: everything buffered after the audible item is to go, and its segments go now.
 * Only the player thread can touch the ring buffer and the source, so fill() does the rest
 * (cut_buffer()). Until then the callback stops at the cut, so none of it is heard, not even
 * if the audible item runs out first. */
static void cut_after_audible(mc_player *p) {
  ma_spinlock_lock(&p->spin);
  int keep = 1;
  while (keep < p->nsegs && p->played >= p->segs[keep].start) keep++;
  if (keep < p->nsegs) {
    p->cut = p->segs[keep].start;
    p->nsegs = keep;
  }
  ma_spinlock_unlock(&p->spin);
}

/* Under lock, from the player thread: ends the buffer at p->cut. A ring buffer can't take
 * back what was written, so what stays is copied out and written again. That's under a
 * second of audio, which the callback waits for by well under its own period. */
static void cut_buffer(mc_player *p) {
  ma_spinlock_lock(&p->spin);
  ma_uint32 want = (ma_uint32)(p->cut - p->played), kept = 0, put = 0;
  while (kept < want) {
    ma_uint32 n = want - kept;
    void *buf;
    if (ma_pcm_rb_acquire_read(&p->rb, &n, &buf) != MA_SUCCESS || !n) break;
    memcpy(p->cut_frames + (size_t)kept * CHANNELS, buf, (size_t)n * CHANNELS * sizeof *p->cut_frames);
    ma_pcm_rb_commit_read(&p->rb, n);
    kept += n;
  }
  ma_pcm_rb_reset(&p->rb);
  while (put < kept) {
    ma_uint32 n = kept - put;
    void *buf;
    if (ma_pcm_rb_acquire_write(&p->rb, &n, &buf) != MA_SUCCESS || !n) break;
    memcpy(buf, p->cut_frames + (size_t)put * CHANNELS, (size_t)n * CHANNELS * sizeof *p->cut_frames);
    ma_pcm_rb_commit_write(&p->rb, n);
    put += n;
  }
  p->written = p->played + put;
  p->cut = NO_CUT;
  ma_spinlock_unlock(&p->spin);
}

/* The audible segment: the last one that has started. */
static const segment *audible(mc_player *p) {
  if (!p->nsegs) return NULL;
  uint64_t played = played_frames(p);
  int i = 0;
  while (i + 1 < p->nsegs && played >= p->segs[i + 1].start) i++;
  return &p->segs[i];
}

/* --- Up next --- */

static void up_entry_free(up_entry *e) {
  if (!e) return;
  free(e->title);
  free(e->artist);
  free(e->tracks);
  free(e);
}

/* What the list shows changed. Under lock. */
static void up_changed(mc_player *p) {
  p->up_changes++;
  p->pending_events |= EV(MC_PLAYER_EVENT_UP_NEXT);
  mc_cond_broadcast(&p->wake);
}

/* Takes entries[i] out of the list. Under lock. */
static up_entry *up_take(mc_player *p, int i) {
  up_entry *e = p->up[i];
  p->nup--;
  memmove(p->up + i, p->up + i + 1, (size_t)(p->nup - i) * sizeof *p->up);
  return e;
}

/* The first track of entry id has started: it's not up next any more. Under lock. */
static void up_pop(mc_player *p, uint32_t id) {
  if (!p->nup || p->up[0]->id != id) return;
  up_entry *e = p->up[0];
  if (++e->first == e->count) {
    up_entry_free(p->spent);
    p->spent = up_take(p, 0);
  }
  up_changed(p);
}

/* Tracks that have been heard since the last look leave up next. Under lock. */
static void up_heard(mc_player *p) {
  uint64_t played = played_frames(p);
  for (int i = 0; i < p->nsegs; i++) {
    segment *seg = &p->segs[i];
    if (!seg->waiting || played < seg->start) continue;
    seg->waiting = 0;
    up_pop(p, seg->entry);
  }
}

/* Before changing up next: whatever's buffered after the audible item goes, as it may not be
 * next any more, so its tracks are back in their places. What's audible plays on. Under lock. */
static void up_settle(mc_player *p) {
  if (p->cmd == CMD_NONE && p->nsegs) {
    int was = p->nsegs;
    cut_after_audible(p);
    if (p->nsegs != was) p->decoding_pos = p->segs[p->nsegs - 1].pos;
    p->gen++; /* an item being opened for the old order is dropped */
  }
  up_heard(p);
  p->drained = 0; /* the last item may have run out already: carry on after it */
}

/* The track n tracks into up next, or NULL. *entry gets its entry. Under lock. */
static const up_track *up_nth(const mc_player *p, int n, const up_entry **entry) {
  for (int i = 0; i < p->nup; i++) {
    const up_entry *e = p->up[i];
    int left = e->count - e->first;
    if (n < left) {
      *entry = e;
      return &e->tracks[e->first + n];
    }
    n -= left;
  }
  return NULL;
}

/* What comes after what's been decoded, which is at place *pos: up next's first track not on
 * its way yet, setting *entry, or the item at the next place, moving *pos and clearing it. -1
 * if there's nothing. Under lock. */
static int next_item(mc_player *p, int *pos, uint32_t *entry) {
  int waiting = 0;
  for (int i = 0; i < p->nsegs; i++) waiting += p->segs[i].waiting != 0;
  const up_entry *e;
  const up_track *t = up_nth(p, waiting, &e);
  if (t) {
    *entry = e->id;
    return t->item;
  }
  *entry = 0;
  return item_at(p, ++*pos);
}

/* Item index, of entry id, can't be played: it leaves up next. It's a no-op for one that has left
 * already. Under lock. */
static void up_drop(mc_player *p, uint32_t id, int index) {
  for (int i = 0; i < p->nup; i++) {
    up_entry *e = p->up[i];
    if (e->id != id) continue;
    for (int t = e->first; t < e->count; t++) {
      if (e->tracks[t].item != index) continue;
      memmove(e->tracks + t, e->tracks + t + 1, (size_t)(e->count - t - 1) * sizeof *e->tracks);
      if (--e->count == e->first) up_entry_free(up_take(p, i));
      up_changed(p);
      return;
    }
  }
}

/* Entry id, where it is in up next or just spent, or NULL. Under lock. */
static up_entry *up_find(const mc_player *p, uint32_t id) {
  if (!id) return NULL;
  for (int i = 0; i < p->nup; i++)
    if (p->up[i]->id == id) return p->up[i];
  return p->spent && p->spent->id == id ? p->spent : NULL;
}

/* Makes room for extra more items. 0 if out of memory. Under lock. */
static int reserve_items(mc_player *p, int extra) {
  size_t n = (size_t)p->count + (size_t)extra;
  char **queue = realloc(p->queue, n * sizeof *queue);
  if (!queue) return 0;
  p->queue = queue;
  /* A queue with no uids gets them now, those of its own items empty. */
  char (*uids)[MC_UID_STRLEN] = p->track_uids ? realloc(p->track_uids, n * sizeof *uids) : calloc(n, sizeof *uids);
  if (!uids) return 0;
  p->track_uids = uids;
  int *origins = realloc(p->item_origins, n * sizeof *origins);
  if (!origins) return 0;
  p->item_origins = origins;
  return 1;
}

static void snapshot(mc_player *p, mc_player_status *st) {
  memset(st, 0, sizeof *st);
  st->count = p->count;
  st->up_next = p->nup;
  st->up_next_changes = p->up_changes;
  st->shuffle = p->shuffle;
  st->radio = p->feeding;
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

/* --- Play log --- */

/* The start of a row for queue item index, with no times yet. Under lock. */
static mc_play_event log_item(const mc_player *p, int index) {
  mc_play_event e;
  memset(&e, 0, sizeof e);
  int ours = index >= 0 && index < p->count;
  if (p->track_uids && ours) memcpy(e.track_uid, p->track_uids[index], MC_UID_STRLEN);
  int origin = ours ? p->item_origins[index] : 0;
  e.context = origin ? p->origins[origin - 1].context : p->context;
  memcpy(e.context_uid, origin ? p->origins[origin - 1].context_uid : p->context_uid, MC_UID_STRLEN);
  return e;
}

/* Queues a closed row for mc_player_take_ended(). An item with no uid has no row. Under lock. */
static unsigned log_push(mc_player *p, const mc_play_event *e) {
  if (!e->track_uid[0]) return 0;
  if (p->nended == MAX_ENDED) /* nobody's taking them: keep the newest */
    memmove(p->ended, p->ended + 1, (size_t)--p->nended * sizeof *p->ended);
  p->ended[p->nended++] = *e;
  return EV(MC_PLAYER_EVENT_ENDED);
}

/* Opens a row for seg; any row open is closed first. Under lock. */
static void log_begin(mc_player *p, segment *seg) {
  p->log = seg->event;
  p->log.started_ms = (int64_t)mc_wall_ms();
  p->log_frames = 0;
  p->log_open = 1;
  seg->logged = 1;
}

/* Whether the command being carried out starts the open row's track again, as Previous does
 * past RESTART_MS and a seek to the start. It logs as a skip, but tells a station nothing. Under
 * lock. */
static int restarting(const mc_player *p) {
  return p->cmd == CMD_START && p->track_uids && p->cmd_index >= 0 && p->cmd_index < p->count &&
         !strcmp(p->track_uids[p->cmd_index], p->log.track_uid);
}

/* Closes the open row, if any. Under lock. */
static unsigned log_end(mc_player *p, mc_play_end end) {
  if (!p->log_open) return 0;
  p->log_open = 0;
  p->log.end = end;
  p->log.listened_ms = p->rate ? (int64_t)(p->log_frames * 1000 / p->rate) : 0;
  /* A station only hears of its own: a track from up next says nothing about it. */
  if (p->feeding && end == MC_PLAY_END_SKIPPED && p->log.context == MC_PLAY_CONTEXT_RADIO && !restarting(p)) {
    if (p->nfeed_skips == MAX_SKIPS) /* the feed is busy opening: keep the newest */
      memmove(p->feed_skips, p->feed_skips + 1, (size_t)--p->nfeed_skips * sizeof *p->feed_skips);
    p->feed_skips[p->nfeed_skips++] = p->log;
    mc_cond_broadcast(&p->feed_wake);
  }
  return log_push(p, &p->log);
}

/* Queue item index couldn't be opened, which is a row of its own. Under lock. */
static unsigned log_failed(mc_player *p, int index) {
  mc_play_event e = log_item(p, index);
  e.started_ms = (int64_t)mc_wall_ms();
  e.end = MC_PLAY_END_FAILED;
  return log_push(p, &e);
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
  ma_uint32 frames = ok ? p->device.sampleRate * BUFFER_MS / 1000 : 0;
  float *cut_frames = ok ? malloc((size_t)frames * CHANNELS * sizeof *cut_frames) : NULL;
  if (ok && (!cut_frames || ma_pcm_rb_init(ma_format_f32, CHANNELS, frames, NULL, NULL, &p->rb) != MA_SUCCESS)) {
    free(cut_frames);
    ma_device_uninit(&p->device);
    ok = 0;
  }
  mc_mutex_lock(&p->lock);
  if (ok) {
    p->device_open = 1;
    p->rate = p->device.sampleRate;
    p->cut_frames = cut_frames;
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
    return log_end(p, MC_PLAY_END_STOPPED) | EV(MC_PLAYER_EVENT_ERROR) | EV(MC_PLAYER_EVENT_STATE);
  }
  return 0;
}

/* The station adds nothing more; what it added plays out. Under lock. */
static void end_radio(mc_player *p) {
  if (!p->feeding) return;
  p->feeding = 0;
  p->radio_skip = 0;
  p->radio_gen++;
  p->pending_events |= EV(MC_PLAYER_EVENT_QUEUE);
  mc_cond_broadcast(&p->feed_wake);
  mc_cond_broadcast(&p->wake);
}

/* Opens the first playable item in the play order from *index (at place *pos, or of up next's
 * *entry) on, seeking the first one to ms. Drops the lock meanwhile. Returns NULL if none could
 * be opened (with *index -1; shuffled, after a try of as many items as the queue has), or if gen
 * moved on (another command came in), in which case the caller gives up. */
static source *open_from(mc_player *p, int *pos, int *index, uint32_t *entry, int64_t ms, unsigned gen,
                         unsigned *events) {
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
    if (src) {
      p->radio_failures = 0;
      return src;
    }
    *events |= EV(MC_PLAYER_EVENT_ERROR) | log_failed(p, *index);
    /* With the files gone, a station would otherwise fail through them for ever, as fast as
     * it could pick, and fill the play log. */
    if (++p->radio_failures >= RADIO_MAX_FAILS) end_radio(p);
    if (*entry) up_drop(p, *entry, *index);
    *index = next_item(p, pos, entry);
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
  p->played_out = 1;
}

/* Counts the frames heard since the last call towards the plays they belong to, and queues
 * a play for mc_player_take_played() once it counts. A segment heard for the first time ends
 * the row before it, which has played out. Under lock. */
static unsigned count_heard(mc_player *p, uint64_t played) {
  unsigned events = 0;
  for (int i = 0; i < p->nsegs && p->rate; i++) {
    segment *seg = &p->segs[i];
    uint64_t end = i + 1 < p->nsegs ? p->segs[i + 1].start : UINT64_MAX;
    uint64_t from = p->heard > seg->start ? p->heard : seg->start;
    uint64_t to = played < end ? played : end;
    if (to <= from) continue;
    int64_t began = (int64_t)mc_wall_ms() - (int64_t)((played - from) * 1000 / p->rate);
    if (!seg->logged) {
      events |= log_end(p, MC_PLAY_END_FINISHED);
      log_begin(p, seg);
    }
    if (!p->log_frames) p->log.started_ms = began;
    p->log_frames += to - from;
    if (seg->index < 0) continue; /* the old queue's last moments: only the log's business */
    if (seg->new_play || seg->index != p->play_index) {
      seg->new_play = 0;
      p->play_index = seg->index;
      p->play_frames = 0;
      p->play_counted = 0;
      p->play_started_ms = began;
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

/* Carries out p->cmd. The command stays posted (and is what mc_player_status_get reports)
 * until it's done, as opening the device and the file drop the lock. If another command
 * comes in meanwhile, this one is abandoned for it. */
static unsigned do_command(mc_player *p) {
  int pos = p->cmd_pos, asked = p->cmd_index, index = asked;
  uint32_t entry = p->cmd_entry;
  int64_t ms = p->cmd_ms;
  int seek = p->cmd_seek;
  unsigned gen = p->gen, events = EV(MC_PLAYER_EVENT_STATE);
  /* What's left to count of the old buffer, which goes now. Whether this command gets to
   * finish or not, the play it ends has ended; only a seek carries it on. */
  unsigned kept = count_heard(p, played_frames(p));
  if (!seek) kept |= log_end(p, p->cmd_end);
  events |= kept;
  close_source(p);

  if (p->cmd == CMD_STOP) {
    p->cmd = CMD_NONE;
    flush(p);
    p->state = MC_PLAYER_STOPPED;
    return events;
  }
  int ok = ensure_device(p);
  if (gen != p->gen) return kept;
  if (!ok) {
    events |= log_end(p, p->cmd_end);
    p->cmd = CMD_NONE;
    flush(p);
    p->state = MC_PLAYER_STOPPED;
    p->current = index;
    p->current_pos = pos;
    return events | EV(MC_PLAYER_EVENT_ERROR) | EV(MC_PLAYER_EVENT_TRACK);
  }
  source *src = open_from(p, &pos, &index, &entry, ms, gen, &events);
  if (gen != p->gen) return kept | (events & (EV(MC_PLAYER_EVENT_ERROR) | EV(MC_PLAYER_EVENT_ENDED)));
  int moved = index != asked; /* skipped past the one asked for */
  p->cmd = CMD_NONE;
  events |= EV(MC_PLAYER_EVENT_TRACK);
  if (!src) { /* nothing playable from there on */
    events |= log_end(p, p->cmd_end);
    stop_at_start(p);
    return events;
  }
  if (moved) ms = 0;
  flush(p);
  p->src = src;
  p->decoding = p->current = index;
  p->decoding_pos = p->current_pos = pos;
  p->segs[0] = (segment){index, pos, p->written, (uint64_t)ms * p->rate / 1000, source_duration_ms(src),
                         !seek || moved, 1, log_item(p, index), entry, moved && entry};
  p->nsegs = 1;
  /* Logged now, so an item skipped or paused before a frame of it is heard still has its row.
   * A seek carries on the row that's open; after a stop there's none, and it starts one. */
  if (!seek || moved || !p->log_open) {
    events |= log_end(p, p->cmd_end);
    log_begin(p, &p->segs[0]);
  }
  p->state = p->cmd_state; /* read now: pause() and resume() change it without a new command */
  return events;
}

/* Fills the ring buffer, moving on to the next item when the source runs out. Under lock. */
static unsigned fill(mc_player *p) {
  unsigned events = 0;
  while (p->cmd == CMD_NONE && !p->quit) {
    if (p->cut != NO_CUT) { /* what comes after it is the old order's, the source included */
      cut_buffer(p);
      close_source(p);
    }
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
      if (got == n || p->cmd != CMD_NONE || p->cut != NO_CUT) continue;
    } else if (p->drained || !p->nsegs) {
      break;
    }

    /* The source ran out (or there was none, as the last item had run out before shuffle
     * came on): carry on with the next item, straight after it in the buffer. */
    if (p->nsegs == MAX_SEGMENTS) break; /* lots of very short tracks; wait for some to play */
    int pos = p->decoding_pos;
    uint32_t entry;
    int index = next_item(p, &pos, &entry);
    unsigned gen = p->gen;
    source *done = p->src;
    p->src = NULL;
    source *next = open_from(p, &pos, &index, &entry, 0, gen, &events);
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
    if (gen != p->gen) { /* a command, which ends this, or the order changed */
      source_close(next);
      continue;
    }
    if (!next) {
      if (p->radio) { /* what the feed adds carries on past the failures */
        p->decoding_pos = pos - 1;
        mc_cond_broadcast(&p->feed_wake);
      }
      p->drained = 1;
      break;
    }
    p->src = next;
    p->decoding = index;
    p->decoding_pos = pos;
    p->segs[p->nsegs++] =
      (segment){index, pos, p->written, 0, source_duration_ms(next), 1, 0, log_item(p, index), entry, entry != 0};
  }
  return events;
}

/* Notices when the next item becomes audible, and when the last one has finished. */
static unsigned advance(mc_player *p) {
  uint64_t played = played_frames(p);
  unsigned events = count_heard(p, played);
  up_heard(p);
  int gone = 0;
  while (gone + 1 < p->nsegs && played >= p->segs[gone + 1].start) gone++;
  if (gone) {
    p->nsegs -= gone;
    memmove(p->segs, p->segs + gone, (size_t)p->nsegs * sizeof *p->segs);
    p->current = p->segs[0].index;
    p->current_pos = p->segs[0].pos;
    events |= EV(MC_PLAYER_EVENT_TRACK);
    mc_cond_broadcast(&p->feed_wake);
  }
  /* A station that hasn't caught up leaves a silence, which beats going back to its first track. */
  if (p->drained && !p->src && played >= p->written && p->state != MC_PLAYER_STOPPED && !p->feeding) {
    events |= log_end(p, MC_PLAY_END_FINISHED);
    stop_at_start(p);
    events |= EV(MC_PLAYER_EVENT_STATE) | EV(MC_PLAYER_EVENT_TRACK);
  }
  return events;
}

static void run(void *arg) {
  mc_player *p = arg;
  mc_mutex_lock(&p->lock);
  while (!p->quit) {
    unsigned events = p->pending_events;
    p->pending_events = 0;
    if (p->cmd != CMD_NONE) events |= do_command(p);
    if (p->cmd == CMD_NONE) {
      if (p->state != MC_PLAYER_STOPPED) events |= fill(p); /* paused too, so resuming is instant */
      events |= advance(p);
      events |= sync_device(p);
    }
    if (events && p->cb) {
      mc_mutex_unlock(&p->lock);
      for (int e = MC_PLAYER_EVENT_STATE; e <= MC_PLAYER_EVENT_UP_NEXT; e++)
        if (events & EV(e)) p->cb(p->user, (mc_player_event)e);
      mc_mutex_lock(&p->lock);
    }
    if (p->quit || p->cmd != CMD_NONE) continue;
    /* While playing, wake to top up the buffer and to see the next track start. Otherwise
     * there's nothing to do until a call. */
    if (p->state == MC_PLAYER_PLAYING) mc_cond_wait_ms(&p->wake, &p->lock, WAKE_MS);
    else if (!events && !p->pending_events) mc_cond_wait(&p->wake, &p->lock);
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
  p->cut = NO_CUT;
  mc_mutex_init(&p->lock);
  mc_cond_init(&p->wake);
  mc_cond_init(&p->feed_wake);
  mc_status st = mc_thread_start(&p->thread, run, p);
  if (st != MC_OK) {
    mc_cond_destroy(&p->feed_wake);
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
  free(p->track_uids);
  free(p->item_origins);
  free(p->list);
  p->queue = NULL;
  p->track_uids = NULL;
  p->item_origins = NULL;
  p->list = NULL;
  p->count = p->list_count = 0;
}

/* Empties up next. Under lock. */
static void up_clear(mc_player *p) {
  while (p->nup) up_entry_free(p->up[--p->nup]);
  up_entry_free(p->spent);
  p->spent = NULL;
}

void mc_player_free(mc_player *p) {
  if (!p) return;
  mc_mutex_lock(&p->lock);
  p->quit = 1;
  mc_cond_broadcast(&p->wake);
  mc_cond_broadcast(&p->feed_wake);
  mc_mutex_unlock(&p->lock);
  mc_thread_join(&p->thread);
  if (p->feed_started) mc_thread_join(&p->feed);
  if (p->device_open) {
    ma_device_uninit(&p->device);
    ma_pcm_rb_uninit(&p->rb);
    free(p->cut_frames);
  }
  source_close(p->src);
  free_queue(p);
  up_clear(p);
  free(p->up);
  free(p->origins);
  free(p->radio_dir);
  mc_cond_destroy(&p->feed_wake);
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
  p->cmd_entry = 0;
  p->cmd_seek = 0;
  p->cmd_end = MC_PLAY_END_SKIPPED; /* most commands; the others say so after */
  p->gen++;
  p->radio_skip = 0;
  p->played_out = 0;
  /* Every command drops what's buffered, so what was on its way stays up next. */
  for (int i = 0; i < p->nsegs; i++) p->segs[i].waiting = 0;
  mc_cond_broadcast(&p->wake);
  mc_cond_broadcast(&p->feed_wake); /* the place in the queue may have moved */
}

/* The place in the play order of the item the UI is showing as current (see snapshot()).
 * Under lock. */
static int current_pos(mc_player *p) {
  if (p->cmd == CMD_START) return p->cmd_pos;
  if (p->cmd == CMD_NONE && p->nsegs) return audible(p)->pos;
  return p->current_pos;
}

/* The up next entry of the item the UI is showing as current, or 0. Under lock. */
static uint32_t current_entry(mc_player *p) {
  if (p->cmd == CMD_START) return p->cmd_entry;
  return p->cmd == CMD_NONE && p->nsegs ? audible(p)->entry : 0;
}

/* Plays up next's first track; the list goes on from place pos afterwards. Under lock, after
 * up_heard(). */
static void play_up_next(mc_player *p, int pos) {
  up_entry *e = p->up[0];
  uint32_t id = e->id;
  post(p, CMD_START, pos, e->tracks[e->first].item, 0, MC_PLAYER_PLAYING);
  p->cmd_entry = id;
  up_pop(p, id);
}

/* The place the list goes on from: past its end if it has played out, so up next doesn't start
 * it again. Under lock. */
static int resume_pos(mc_player *p) {
  return p->played_out ? p->list_count - 1 : current_pos(p);
}

/* The origin's track uids as a queue keeps them: *out is NULL if it has none. 0 if out of
 * memory. */
static int copy_track_uids(const mc_play_origin *origin, int count, char (**out)[MC_UID_STRLEN]) {
  *out = NULL;
  if (!origin || !origin->track_uids || !count) return 1;
  char (*uids)[MC_UID_STRLEN] = calloc((size_t)count, sizeof *uids);
  if (!uids) return 0;
  for (int i = 0; i < count; i++) {
    const char *uid = origin->track_uids[i];
    if (uid && strlen(uid) < MC_UID_STRLEN) strcpy(uids[i], uid);
  }
  *out = uids;
  return 1;
}

/* --- The feed --- */

typedef struct feed_job {
  mc_player *p;
  unsigned gen;
} feed_job;

static int feed_cancelled(void *user) {
  feed_job *job = user;
  mc_mutex_lock(&job->p->lock);
  int gone = job->p->quit || job->gen != job->p->radio_gen;
  mc_mutex_unlock(&job->p->lock);
  return gone;
}

/* With a connection of its own, as a library handle stays on its thread. It's let go at once:
 * a station doesn't need it, and an open database would be in the way of moving the library. */
static mc_status feed_open(feed_job *job, const char *dir, const char *seed, const char *start,
                           mc_radio_station **out) {
  mc_library *lib = NULL;
  mc_status st = mc_library_open_internal(dir, &lib);
  if (st != MC_OK) return st;
  mc_radio_options opts;
  memset(&opts, 0, sizeof opts);
  opts.seed_track = seed;
  opts.start_track = start;
  opts.random = new_seed();
  st = mc_radio_station_open(lib, &opts, feed_cancelled, job, out);
  mc_library_close(lib);
  if (st == MC_OK) {
    mc_radio_pick first; /* the start, which is queued already */
    mc_radio_station_next(*out, &first, 1);
  }
  return st;
}

/* Adds picks to the end of the queue. Under lock. */
static mc_status feed_append(mc_player *p, const mc_radio_pick *picks, int n) {
  int first = p->count, place = p->list_count;
  int *list = realloc(p->list, (size_t)(place + n) * sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  p->list = list;
  if (!reserve_items(p, n)) return MC_ERR_NOMEM;
  for (int i = 0; i < n; i++) {
    if (!(p->queue[p->count] = mc_strdup(picks[i].path))) return MC_ERR_NOMEM;
    memcpy(p->track_uids[p->count], picks[i].track_uid, MC_UID_STRLEN);
    p->item_origins[p->count] = 0;
    p->list[p->list_count++] = p->count++;
  }
  p->drained = 0; /* the last item may have run out already: carry on after it */
  p->pending_events |= EV(MC_PLAYER_EVENT_QUEUE);
  if (p->radio_skip) {
    up_heard(p);
    if (p->nup) play_up_next(p, place - 1); /* added while it waited */
    else post(p, CMD_START, place, first, 0, MC_PLAYER_PLAYING);
  }
  mc_cond_broadcast(&p->wake);
  return MC_OK;
}

/* Keeps RADIO_AHEAD items queued after the current one for as long as a station plays. */
static void feed_run(void *arg) {
  mc_player *p = arg;
  mc_radio_station *station = NULL;
  unsigned station_gen = 0;
  mc_radio_pick picks[RADIO_AHEAD];
  mc_play_event heard[MAX_SKIPS];
  mc_mutex_lock(&p->lock);
  while (!p->quit) {
    if (station && (!p->feeding || station_gen != p->radio_gen)) {
      mc_radio_station *old = station;
      station = NULL;
      mc_mutex_unlock(&p->lock);
      mc_radio_station_close(old);
      mc_mutex_lock(&p->lock);
      continue;
    }
    if (!p->feeding) {
      mc_cond_wait(&p->feed_wake, &p->lock);
      continue;
    }
    unsigned gen = p->radio_gen;
    if (!station) {
      char *dir = mc_strdup(p->radio_dir);
      char seed[MC_UID_STRLEN], start[MC_UID_STRLEN];
      memcpy(seed, p->context_uid, sizeof seed);
      memcpy(start, p->radio_start, sizeof start);
      feed_job job = {p, gen};
      mc_mutex_unlock(&p->lock);
      mc_status st = dir ? feed_open(&job, dir, seed, start, &station) : MC_ERR_NOMEM;
      free(dir);
      mc_mutex_lock(&p->lock);
      station_gen = gen;
      if (st != MC_OK && gen == p->radio_gen) end_radio(p); /* the seed plays by itself */
      continue;
    }
    if (p->nfeed_skips) { /* before any pick. What's queued already stays. */
      int n = p->nfeed_skips;
      memcpy(heard, p->feed_skips, (size_t)n * sizeof *heard);
      p->nfeed_skips = 0;
      mc_mutex_unlock(&p->lock);
      for (int i = 0; i < n; i++) mc_radio_station_heard(station, &heard[i]);
      mc_mutex_lock(&p->lock);
      continue;
    }
    /* From the furthest the thread has got, not what's audible: items it has failed to open
     * are behind it, though the last track to play is still sounding or has just ended. */
    int place = current_pos(p);
    if (place < p->decoding_pos) place = p->decoding_pos;
    int want = RADIO_AHEAD - (p->list_count - 1 - place);
    if (want <= 0) {
      mc_cond_wait(&p->feed_wake, &p->lock);
      continue;
    }
    if (want > RADIO_AHEAD) want = RADIO_AHEAD;
    mc_mutex_unlock(&p->lock);
    mc_status st = mc_radio_station_next(station, picks, want);
    mc_mutex_lock(&p->lock);
    if (gen != p->radio_gen) continue;
    if (st == MC_OK) st = feed_append(p, picks, want);
    if (st != MC_OK) end_radio(p);
  }
  mc_mutex_unlock(&p->lock);
  mc_radio_station_close(station);
}

/* After the items were replaced: what's up next comes along from old, the old items, as new items
 * after the list. What has played of each entry is forgotten. If there's no memory for that, up
 * next goes. Under lock. */
static void up_rehome(mc_player *p, char **old, char (*old_uids)[MC_UID_STRLEN]) {
  up_entry_free(p->spent);
  p->spent = NULL;
  int tracks = 0;
  for (int i = 0; i < p->nup; i++) tracks += p->up[i]->count - p->up[i]->first;
  if (tracks && !reserve_items(p, tracks)) {
    up_clear(p);
    up_changed(p);
    return;
  }
  for (int i = 0; i < p->nup; i++) {
    up_entry *e = p->up[i];
    e->count -= e->first;
    memmove(e->tracks, e->tracks + e->first, (size_t)e->count * sizeof *e->tracks);
    e->first = 0;
    for (int t = 0; t < e->count; t++) {
      int was = e->tracks[t].item, now = p->count++;
      p->queue[now] = old[was];
      old[was] = NULL;
      memcpy(p->track_uids[now], old_uids[was], MC_UID_STRLEN);
      p->item_origins[now] = e->origin;
      e->tracks[t].item = now;
    }
  }
}

/* mc_player_set_queue(), with play 0 mc_player_load_queue(), and with radio_dir (the library of
 * the station that's to feed it) mc_player_play_radio(). */
static mc_status set_queue(mc_player *p, const char *const *paths, int count, int start, int play,
                           const mc_play_origin *origin, const char *radio_dir) {
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
  char (*track_uids)[MC_UID_STRLEN];
  char *dir = radio_dir ? mc_strdup(radio_dir) : NULL;
  int *list = count ? malloc((size_t)count * sizeof *list) : NULL;
  int *origins = count ? calloc((size_t)count, sizeof *origins) : NULL;
  if (!copy_track_uids(origin, count, &track_uids) || (radio_dir && !dir) || (count && (!list || !origins))) {
    free(dir);
    free(list);
    free(origins);
    free(track_uids);
    for (int i = 0; i < count; i++) free(queue[i]);
    free(queue);
    return MC_ERR_NOMEM;
  }
  for (int i = 0; i < count; i++) list[i] = i;
  mc_mutex_lock(&p->lock);
  up_heard(p);
  char **old = p->queue;
  char (*old_uids)[MC_UID_STRLEN] = p->track_uids;
  int *old_origins = p->item_origins, *old_list = p->list, old_count = p->count;
  p->queue = queue;
  p->track_uids = track_uids;
  p->item_origins = origins;
  p->list = list;
  p->count = p->list_count = count;
  up_rehome(p, old, old_uids);
  for (int i = 0; i < old_count; i++) free(old[i]);
  free(old);
  free(old_uids);
  free(old_origins);
  free(old_list);
  p->context = origin ? origin->context : MC_PLAY_CONTEXT_UNKNOWN;
  if (start < 0 && p->shuffle && p->context != MC_PLAY_CONTEXT_RADIO) p->context = MC_PLAY_CONTEXT_SHUFFLE;
  p->context_uid[0] = '\0';
  if (origin && origin->context_uid && strlen(origin->context_uid) < MC_UID_STRLEN)
    strcpy(p->context_uid, origin->context_uid);
  p->radio = p->feeding = radio_dir != NULL;
  p->radio_gen++; /* whatever was on is over, as is anything else that replaces the queue */
  free(p->radio_dir);
  p->radio_dir = dir;
  if (radio_dir) memcpy(p->radio_start, track_uids[0], sizeof p->radio_start);
  p->radio_failures = 0;
  p->nfeed_skips = 0;
  p->nplays = 0;
  p->play_index = -1;
  for (int i = 0; i < p->nsegs; i++) p->segs[i].index = -1; /* what's still sounding isn't this queue's */
  mc_shuffle_init(&p->order, count, new_seed(), start);
  int pos = start >= 0 && !p->shuffle ? start : 0;
  if (count && play) {
    post(p, CMD_START, pos, item_at(p, pos), 0, MC_PLAYER_PLAYING);
  } else if (count) { /* stopped there, to play on resume */
    p->current_pos = pos;
    p->current = item_at(p, pos);
    post(p, CMD_STOP, pos, p->current, 0, MC_PLAYER_STOPPED);
  } else {
    p->current = p->current_pos = -1;
    post(p, CMD_STOP, -1, -1, 0, MC_PLAYER_STOPPED);
  }
  p->cmd_end = MC_PLAY_END_REPLACED;
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_set_queue(mc_player *p, const char *const *paths, int count, int start,
                              const mc_play_origin *origin) {
  return set_queue(p, paths, count, start, 1, origin, NULL);
}

mc_status mc_player_load_queue(mc_player *p, const char *const *paths, int count, int start,
                               const mc_play_origin *origin) {
  return set_queue(p, paths, count, start, 0, origin, NULL);
}

mc_status mc_player_play_radio(mc_player *p, const char *library_dir, const char *seed_path,
                               const char *seed_uid) {
  return mc_player_play_station(p, library_dir, seed_path, seed_uid, seed_uid);
}

mc_status mc_player_play_station(mc_player *p, const char *library_dir, const char *start_path,
                                 const char *start_uid, const char *seed_uid) {
  if (!p || !library_dir || !start_path || !start_uid || !*start_uid || strlen(start_uid) >= MC_UID_STRLEN ||
      !seed_uid || !*seed_uid || strlen(seed_uid) >= MC_UID_STRLEN)
    return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_status st = p->feed_started ? MC_OK : mc_thread_start(&p->feed, feed_run, p);
  if (st == MC_OK) p->feed_started = 1;
  mc_mutex_unlock(&p->lock);
  if (st != MC_OK) return st;
  const char *const paths[] = {start_path}, *const uids[] = {start_uid};
  mc_play_origin origin = {uids, MC_PLAY_CONTEXT_RADIO, seed_uid};
  return set_queue(p, paths, 1, 0, 1, &origin, library_dir);
}

mc_status mc_player_stop_radio(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  end_radio(p);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_queue_uid(mc_player *p, int index, char *out) {
  if (!p || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_status st = index >= 0 && index < p->count ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK && p->track_uids) memcpy(out, p->track_uids[index], MC_UID_STRLEN);
  else if (st == MC_OK) out[0] = '\0';
  mc_mutex_unlock(&p->lock);
  return st;
}

mc_status mc_player_queue_set_path(mc_player *p, int index, const char *path) {
  if (!p || !path) return MC_ERR_INVALID_ARG;
  char *copy = mc_strdup(path);
  if (!copy) return MC_ERR_NOMEM;
  mc_mutex_lock(&p->lock);
  mc_status st = index >= 0 && index < p->count ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK) {
    free(p->queue[index]);
    p->queue[index] = copy;
    copy = NULL;
  }
  mc_mutex_unlock(&p->lock);
  free(copy);
  return st;
}

mc_status mc_player_skip_to(mc_player *p, int index) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  int at = index >= 0 && index < p->count ? list_of(p, index) : -1;
  mc_status st = at >= 0 ? MC_OK : MC_ERR_INVALID_ARG;
  up_heard(p);
  if (st == MC_OK && p->shuffle && !p->radio) { /* that one first, then the rest shuffled afresh */
    mc_shuffle_init(&p->order, p->list_count, new_seed(), at);
    post(p, CMD_START, 0, index, 0, MC_PLAYER_PLAYING);
  } else if (st == MC_OK) {
    post(p, CMD_START, at, index, 0, MC_PLAYER_PLAYING);
  }
  mc_mutex_unlock(&p->lock);
  return st;
}

mc_status mc_player_next(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_heard(p);
  int pos = current_pos(p) + 1, index = item_at(p, pos);
  if (p->nup) {
    play_up_next(p, resume_pos(p));
  } else if (index >= 0) {
    post(p, CMD_START, pos, index, 0, MC_PLAYER_PLAYING);
  } else if (p->feeding) { /* more is on its way, and at the start of a station it's all there is */
    p->radio_skip = 1;
    mc_cond_broadcast(&p->feed_wake);
  } else if (p->list_count) { /* past the end: stopped at the start, as when the queue plays out */
    p->current_pos = 0;
    p->current = item_at(p, 0);
    post(p, CMD_STOP, 0, p->current, 0, MC_PLAYER_STOPPED);
    p->played_out = 1;
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

/* Previous from a track of up next (st says it's current, at the very start of it): back a track
 * in its entry if it has one before it, else to the item the list had got to, with the track
 * going back up next at the front. With neither, back to its start. Under lock. */
static void previous_from_up_next(mc_player *p, const mc_player_status *st) {
  int pos = current_pos(p), before = item_at(p, pos);
  uint32_t id = current_entry(p);
  up_entry *e = up_find(p, id);
  int within = e && e->first >= 2;
  if (!within && before < 0) {
    post(p, CMD_START, pos, st->index, 0, MC_PLAYER_PLAYING);
    p->cmd_entry = id;
    return;
  }
  if (e) { /* to the front of up next: there's always room for spent (up_add()) */
    int at = 0;
    while (at < p->nup && p->up[at] != e) at++;
    if (at == p->nup) p->nup++;
    memmove(p->up + 1, p->up, (size_t)at * sizeof *p->up);
    p->up[0] = e;
    if (e == p->spent) p->spent = NULL;
  }
  if (within) {
    e->first -= 2;
    play_up_next(p, pos);
    return;
  }
  if (e) {
    e->first--;
    up_changed(p);
  }
  post(p, CMD_START, pos, before, 0, MC_PLAYER_PLAYING);
}

mc_status mc_player_previous(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_heard(p);
  mc_player_status st;
  snapshot(p, &st);
  if (st.index >= 0 && p->item_origins[st.index] && st.position_ms < RESTART_MS) {
    previous_from_up_next(p, &st);
  } else if (st.index >= 0) {
    int pos = current_pos(p), index = st.index, before = item_at(p, pos - 1);
    uint32_t entry = current_entry(p);
    if (before >= 0 && st.position_ms < RESTART_MS) {
      pos--;
      index = before;
      entry = 0;
    }
    post(p, CMD_START, pos, index, 0, MC_PLAYER_PLAYING);
    p->cmd_entry = entry;
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
  up_heard(p);
  mc_player_status st;
  snapshot(p, &st);
  if (p->cmd == CMD_START) p->cmd_state = MC_PLAYER_PLAYING;
  else if (st.state == MC_PLAYER_PAUSED) p->state = MC_PLAYER_PLAYING;
  else if (st.state == MC_PLAYER_STOPPED && p->nup && (p->played_out || st.index < 0))
    play_up_next(p, resume_pos(p));
  else if (st.state == MC_PLAYER_STOPPED && p->count && st.index >= 0 && st.index < p->count)
    post(p, CMD_START, current_pos(p), st.index, 0, MC_PLAYER_PLAYING);
  else if (st.state == MC_PLAYER_STOPPED && p->list_count)
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
  up_heard(p);
  mc_player_status st;
  snapshot(p, &st);
  int pos = current_pos(p);
  p->current = st.index;
  p->current_pos = pos;
  post(p, CMD_STOP, pos, st.index, 0, MC_PLAYER_STOPPED);
  p->cmd_end = MC_PLAY_END_STOPPED;
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_seek_ms(mc_player *p, int64_t position_ms) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_heard(p);
  mc_player_status st;
  snapshot(p, &st);
  if (st.index >= 0 && st.index < p->count) {
    uint32_t entry = current_entry(p);
    post(p, CMD_START, current_pos(p), st.index, position_ms,
         st.state == MC_PLAYER_PAUSED ? MC_PLAYER_PAUSED : MC_PLAYER_PLAYING);
    p->cmd_entry = entry;
    p->cmd_seek = position_ms > 0; /* back to the start is starting it again */
    if (p->cmd_seek) p->cmd_end = MC_PLAY_END_STOPPED; /* only if the file has gone and it can't go on */
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
  if (p->radio) { /* a station has its own order, so this waits for the next queue */
    p->shuffle = on;
  } else if (on != p->shuffle) {
    /* The new order carries on from the one playing, or the one about to start, or the
     * current one. An item being opened for the old order is dropped by the change of gen. */
    if (p->cmd == CMD_NONE && p->nsegs) cut_after_audible(p);
    up_heard(p); /* what was on its way stays up next, which doesn't shuffle */
    p->gen++;
    int last = p->cmd == CMD_START               ? p->cmd_pos
               : p->cmd == CMD_NONE && p->nsegs ? p->segs[p->nsegs - 1].pos
                                                 : p->current_pos;
    last = list_at(p, last); /* where the list has got to, up next or not */
    for (int i = 0; !on && i < p->nsegs; i++) p->segs[i].pos = list_at(p, p->segs[i].pos);
    p->shuffle = on;
    if (on) mc_shuffle_init(&p->order, p->list_count, new_seed(), last);
    int pos = on ? 0 : last; /* in order, a place is where in the list it is */
    p->cmd_pos = p->current_pos = p->decoding_pos = pos;
    for (int i = 0; on && i < p->nsegs; i++) p->segs[i].pos = pos - (p->nsegs - 1 - i);
    if (p->cmd == CMD_NONE && p->nsegs) p->current_pos = p->segs[0].pos;
    if (on) p->drained = 0; /* the last item may have run out already: carry on after it */
    mc_cond_broadcast(&p->wake);
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_order(mc_player *p, int from, int count, int *out) {
  if (!p || count < 0 || (count && !out)) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_heard(p);
  mc_player_status st;
  snapshot(p, &st);
  int pos = p->list_count ? current_pos(p) : -1;
  /* Playing from up next, the place is still the item played before it. */
  int detour = st.index >= 0 && p->item_origins[st.index];
  long long left = 0;
  for (int i = 0; i < p->nup; i++) left += p->up[i]->count - p->up[i]->first;
  for (int i = 0; i < count; i++) {
    long long at = (long long)from + i;
    const up_entry *e;
    if (!at) {
      out[i] = st.index;
    } else if (at > 0 && at <= left) {
      out[i] = up_nth(p, (int)at - 1, &e)->item;
    } else {
      /* Places far enough from the current one to overflow have no item. */
      long long place = pos + (at > 0 ? at - left : at + detour);
      out[i] = (pos >= 0 || at > 0) && place >= 0 && place <= INT_MAX ? item_at(p, (int)place) : -1;
    }
  }
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

/* --- Up next: the API --- */

/* mc_player_play_next() and mc_player_play_last(). The entries are made first, so the one thing
 * that can go wrong under the lock is running out of memory for the items. */
static mc_status up_add(mc_player *p, const mc_up_next_add *add, int front) {
  if (!p || !add || !add->tracks || add->count <= 0) return MC_ERR_INVALID_ARG;
  for (int i = 0; i < add->count; i++)
    if (!add->tracks[i].path) return MC_ERR_INVALID_ARG;
  int n = add->title ? 1 : add->count, count = add->count;
  up_entry **entries = calloc((size_t)n, sizeof *entries);
  char **paths = calloc((size_t)count, sizeof *paths);
  mc_status st = entries && paths ? MC_OK : MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < count; i++)
    if (!(paths[i] = mc_strdup(add->tracks[i].path))) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < n; i++) {
    int tracks = add->title ? count : 1;
    const mc_up_next_track *t = &add->tracks[i];
    const char *title = add->title ? add->title : t->title, *artist = add->title ? add->artist : t->artist;
    up_entry *e = entries[i] = calloc(1, sizeof *e);
    if (e) e->tracks = calloc((size_t)tracks, sizeof *e->tracks);
    if (e) e->title = mc_strdup(title ? title : "");
    if (e && artist) e->artist = mc_strdup(artist);
    if (!e || !e->tracks || !e->title || (artist && !e->artist)) st = MC_ERR_NOMEM;
    else e->count = tracks;
    if (e) e->block = add->title != NULL;
  }
  if (st == MC_OK) {
    mc_mutex_lock(&p->lock);
    up_entry **up = realloc(p->up, (size_t)(p->nup + n + 1) * sizeof *up); /* with room for spent to go back */
    if (up) p->up = up;
    item_origin *origins = realloc(p->origins, (size_t)(p->norigins + 1) * sizeof *origins);
    if (origins) p->origins = origins;
    if (!up || !origins || !reserve_items(p, count)) {
      st = MC_ERR_NOMEM;
    } else {
      up_settle(p);
      item_origin *o = &p->origins[p->norigins++];
      memset(o, 0, sizeof *o);
      o->context = add->context;
      if (add->context_uid && strlen(add->context_uid) < MC_UID_STRLEN) strcpy(o->context_uid, add->context_uid);
      for (int i = 0, e = 0; i < count; i++) {
        const char *uid = add->tracks[i].uid;
        int item = p->count++;
        p->queue[item] = paths[i];
        paths[i] = NULL;
        memset(p->track_uids[item], 0, MC_UID_STRLEN);
        if (uid && strlen(uid) < MC_UID_STRLEN) strcpy(p->track_uids[item], uid);
        p->item_origins[item] = p->norigins;
        up_track *t = &entries[e]->tracks[add->title ? i : 0];
        t->item = item;
        t->duration_ms = add->tracks[i].duration_ms;
        if (!add->title) e++;
      }
      int at = front ? 0 : p->nup;
      memmove(p->up + at + n, p->up + at, (size_t)(p->nup - at) * sizeof *p->up);
      for (int i = 0; i < n; i++) {
        entries[i]->id = ++p->last_id;
        entries[i]->origin = p->norigins;
        p->up[at + i] = entries[i];
        entries[i] = NULL;
      }
      p->nup += n;
      p->pending_events |= EV(MC_PLAYER_EVENT_QUEUE); /* there are new items */
      up_changed(p);
    }
    mc_mutex_unlock(&p->lock);
  }
  for (int i = 0; entries && i < n; i++) up_entry_free(entries[i]);
  for (int i = 0; paths && i < count; i++) free(paths[i]);
  free(entries);
  free(paths);
  return st;
}

mc_status mc_player_play_next(mc_player *p, const mc_up_next_add *add) {
  return up_add(p, add, 1);
}

mc_status mc_player_play_last(mc_player *p, const mc_up_next_add *add) {
  return up_add(p, add, 0);
}

mc_status mc_player_up_next(mc_player *p, mc_up_next_list **out) {
  if (!p || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  mc_up_next_list *list = calloc(1, sizeof *list);
  if (!list) return MC_ERR_NOMEM;
  mc_status st = MC_OK;
  mc_mutex_lock(&p->lock);
  up_heard(p);
  if (p->nup && !(list->items = calloc((size_t)p->nup, sizeof *list->items))) st = MC_ERR_NOMEM;
  for (int i = 0; st == MC_OK && i < p->nup; i++) {
    const up_entry *e = p->up[i];
    mc_up_next_entry *out_e = &list->items[list->count++];
    const item_origin *o = &p->origins[e->origin - 1];
    out_e->id = e->id;
    out_e->block = e->block;
    out_e->tracks = e->count - e->first;
    for (int t = e->first; t < e->count; t++) out_e->duration_ms += e->tracks[t].duration_ms > 0 ? e->tracks[t].duration_ms : 0;
    out_e->first_item = e->tracks[e->first].item;
    out_e->context = o->context;
    memcpy(out_e->context_uid, o->context_uid, MC_UID_STRLEN);
    out_e->title = mc_strdup(e->title);
    out_e->artist = e->artist ? mc_strdup(e->artist) : NULL;
    if (!out_e->title || (e->artist && !out_e->artist)) st = MC_ERR_NOMEM;
  }
  mc_mutex_unlock(&p->lock);
  if (st != MC_OK) {
    mc_up_next_list_free(list);
    return st;
  }
  *out = list;
  return MC_OK;
}

void mc_up_next_list_free(mc_up_next_list *list) {
  if (!list) return;
  for (int i = 0; i < list->count; i++) {
    free(list->items[i].title);
    free(list->items[i].artist);
  }
  free(list->items);
  free(list);
}

/* Entry id's place in up next, or -1. Under lock. */
static int up_place(const mc_player *p, uint32_t id) {
  for (int i = 0; i < p->nup; i++)
    if (p->up[i]->id == id) return i;
  return -1;
}

mc_status mc_player_up_next_move(mc_player *p, uint32_t id, int to) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_settle(p);
  int from = up_place(p, id);
  if (from >= 0) {
    to = to < 0 ? 0 : to >= p->nup ? p->nup - 1 : to;
    if (to != from) {
      up_entry *e = up_take(p, from);
      p->nup++;
      memmove(p->up + to + 1, p->up + to, (size_t)(p->nup - 1 - to) * sizeof *p->up);
      p->up[to] = e;
      up_changed(p);
    }
  }
  mc_mutex_unlock(&p->lock);
  return from >= 0 ? MC_OK : MC_ERR_NOT_FOUND;
}

mc_status mc_player_up_next_remove(mc_player *p, uint32_t id) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_settle(p);
  int at = up_place(p, id);
  if (at >= 0) {
    up_entry_free(up_take(p, at));
    up_changed(p);
  }
  mc_mutex_unlock(&p->lock);
  return at >= 0 ? MC_OK : MC_ERR_NOT_FOUND;
}

mc_status mc_player_up_next_clear(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  up_settle(p);
  up_clear(p);
  up_changed(p);
  mc_mutex_unlock(&p->lock);
  return MC_OK;
}

mc_status mc_player_up_next_rename(mc_player *p, uint32_t id, const char *title, const char *artist) {
  if (!p || !title) return MC_ERR_INVALID_ARG;
  char *t = mc_strdup(title), *a = artist ? mc_strdup(artist) : NULL;
  if (!t || (artist && !a)) {
    free(t);
    free(a);
    return MC_ERR_NOMEM;
  }
  mc_mutex_lock(&p->lock);
  int at = up_place(p, id);
  if (at >= 0) {
    up_entry *e = p->up[at];
    char *was_title = e->title, *was_artist = e->artist;
    e->title = t;
    e->artist = a;
    t = was_title;
    a = was_artist;
    up_changed(p);
  }
  mc_mutex_unlock(&p->lock);
  free(t);
  free(a);
  return at >= 0 ? MC_OK : MC_ERR_NOT_FOUND;
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

mc_status mc_player_take_ended(mc_player *p, mc_play_event *out) {
  if (!p || !out) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  mc_status st = p->nended ? MC_OK : MC_ERR_NOT_FOUND;
  if (st == MC_OK) {
    *out = p->ended[0];
    memmove(p->ended, p->ended + 1, (size_t)--p->nended * sizeof *p->ended);
  }
  mc_mutex_unlock(&p->lock);
  return st;
}

mc_status mc_player_end_play(mc_player *p) {
  if (!p) return MC_ERR_INVALID_ARG;
  mc_mutex_lock(&p->lock);
  count_heard(p, played_frames(p));
  log_end(p, MC_PLAY_END_STOPPED);
  for (int i = 0; i < p->nsegs; i++) p->segs[i].logged = 0; /* if it plays on: a new row, once heard */
  mc_mutex_unlock(&p->lock);
  return MC_OK;
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
