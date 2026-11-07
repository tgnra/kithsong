/* Sources: a decoded file as plugins see it, in the PCM format they ask for.
 *
 *   decoder (playback/decoder.c, f32 at the file's rate and channels) -> channels (miniaudio's
 *   converter down, copies up from mono) -> rate (libopusenc's Speex resampler) -> samples
 *   (f32, or integers: dithered with TPDF unless the samples were exact integers of no more
 *   bits all along)
 *
 * The format is fixed at the first read. Before then, decode_set_format() can change it. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"
#include "playback/decoder.h"
#include "plugin/plugins_internal.h"
#include "tagger/coverart.h"
#include "util/platform.h"

/* libopusenc's Speex resampler, built in with its symbols under the prefix "libopusenc"
 * (third_party/CMakeLists.txt). Its header isn't installed, so the few calls used here are
 * declared as it declares them. */
typedef struct SpeexResamplerState_ SpeexResamplerState;
SpeexResamplerState *libopusenc_resampler_init(unsigned nb_channels, unsigned in_rate,
                                               unsigned out_rate, int quality, int *err);
void libopusenc_resampler_destroy(SpeexResamplerState *st);
int libopusenc_resampler_process_interleaved_float(SpeexResamplerState *st, const float *in,
                                                   unsigned *in_len, float *out,
                                                   unsigned *out_len);
int libopusenc_resampler_get_input_latency(SpeexResamplerState *st);
int libopusenc_resampler_skip_zeros(SpeexResamplerState *st);

enum {
  CHUNK = 4096,     /* frames decoded at a time */
  RS_QUALITY = 10,  /* the best: these are kept, and encodes are slower anyway */
};

struct kith_source {
  mc_decoder *dec;
  unsigned src_rate, src_channels;
  int src_bits, lossless;
  uint64_t src_frames;
  kith_pcm_format fmt; /* what reads deliver; limits folded in */
  int started;

  /* The pipeline, set up by the first read. */
  float *in;           /* CHUNK × src_channels */
  float *mixed;        /* CHUNK × fmt.channels */
  float *pending;      /* converted frames not read yet */
  size_t pending_cap, pending_len, pending_pos; /* in frames */
  ma_channel_converter mixer;
  int have_mixer;
  SpeexResamplerState *rs;
  int eof, flushed;
  uint64_t in_done;      /* frames read from the decoder */
  uint64_t out_expected; /* once eof: what the resampled stream should come to */
  uint64_t out_done;     /* frames handed on from the resampler */
  int exact;             /* integer samples come out without dither */
  uint32_t rng;

  /* Tags and cover, read on first ask. */
  int tags_read;
  mc_tags tags;
  int cover_read;
  unsigned char *cover;
  size_t cover_size;
  const char *cover_mime;
  char *path;
};

/* --- Format --- */

static int lossless_path(const char *path) {
  static const char *const exts[] = {"flac", "wav", "aif", "aiff", NULL};
  const char *dot = strrchr(path, '.');
  if (!dot) return 0;
  for (const char *const *e = exts; *e; e++) {
    const char *a = dot + 1, *b = *e;
    while (*a && (*a | 0x20) == *b) a++, b++;
    if (!*a && !*b) return 1;
  }
  return 0;
}

/* Folds want into src->fmt: exact fields replace, limits cap. */
static int apply(kith_source *src, const kith_pcm_format *want) {
  if (!want) return KITH_OK;
  if (want->sample_rate < 0 || want->channels < 0 || want->bits < 0 || want->max_sample_rate < 0 ||
      want->max_channels < 0 || want->max_bits < 0 || want->sample < KITH_SAMPLE_DEFAULT ||
      want->sample > KITH_SAMPLE_F32 || (want->sample_rate && want->sample_rate < 1000) ||
      want->channels > 8 || (want->bits && (want->bits < 8 || want->bits > 32)))
    return KITH_ERR_INVALID_ARG;
  kith_pcm_format *f = &src->fmt;
  if (want->sample_rate) f->sample_rate = want->sample_rate;
  if (want->channels) f->channels = want->channels;
  if (want->sample) f->sample = want->sample;
  if (want->bits) f->bits = want->bits;
  if (want->max_sample_rate && f->sample_rate > want->max_sample_rate) {
    /* 88.2 kHz goes to 44.1 rather than 48: whole multiples resample more cleanly. */
    int r = want->max_sample_rate;
    if (f->sample_rate % 44100 == 0 && r >= 44100) r = 44100;
    else if (f->sample_rate % 48000 == 0 && r >= 48000) r = 48000;
    f->sample_rate = r;
  }
  if (want->max_channels && f->channels > want->max_channels) f->channels = want->max_channels;
  if (want->max_bits && f->bits > want->max_bits) f->bits = want->max_bits;
  if (f->sample == KITH_SAMPLE_S16 && f->bits > 16) f->bits = 16;
  if (f->sample == KITH_SAMPLE_F32) f->bits = 32;
  if (f->sample != KITH_SAMPLE_F32 && f->bits > 24 && f->sample != KITH_SAMPLE_S32) f->bits = 24;
  return KITH_OK;
}

int mc_source_open(const char *path, const kith_pcm_format *want, kith_source **out) {
  if (!path || !out) return KITH_ERR_INVALID_ARG;
  *out = NULL;
  kith_source *src = calloc(1, sizeof *src);
  if (!src || !(src->path = mc_strdup(path))) {
    free(src);
    return KITH_ERR_NOMEM;
  }
  mc_status st = mc_decoder_open(path, &src->dec);
  if (st != MC_OK) {
    free(src->path);
    free(src);
    return st == MC_ERR_FORMAT ? KITH_ERR_FORMAT : KITH_ERR_IO;
  }
  src->src_rate = mc_decoder_rate(src->dec);
  src->src_channels = mc_decoder_channels(src->dec);
  src->src_frames = mc_decoder_length(src->dec);
  src->lossless = lossless_path(path);
  if (src->lossless) {
    mc_tags t;
    if (mc_tags_read(path, &t) == MC_OK) {
      src->src_bits = t.bits_per_sample;
      mc_tags_clear(&t);
    }
    if (src->src_bits <= 0) src->src_bits = 16;
  }
  src->fmt.sample_rate = (int)src->src_rate;
  src->fmt.channels = (int)src->src_channels;
  src->fmt.sample = KITH_SAMPLE_F32;
  src->fmt.bits = 32;
  if (mc_random_bytes(&src->rng, sizeof src->rng) != MC_OK || !src->rng) src->rng = 0x9e3779b9u;
  int rc = apply(src, want);
  if (rc != KITH_OK) {
    mc_source_close(src);
    return rc;
  }
  *out = src;
  return KITH_OK;
}

int mc_source_info(kith_source *src, kith_source_info *out) {
  if (!src || !out) return KITH_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  out->format = src->fmt;
  out->format.max_sample_rate = out->format.max_channels = out->format.max_bits = 0;
  out->source_rate = (int)src->src_rate;
  out->source_channels = (int)src->src_channels;
  out->source_bits = src->src_bits;
  out->lossless = src->lossless;
  if (src->src_frames && src->src_rate) {
    out->frames = (int64_t)(src->src_frames * (uint64_t)src->fmt.sample_rate / src->src_rate);
    out->duration_ms = (int)(src->src_frames * 1000 / src->src_rate);
  }
  return KITH_OK;
}

int mc_source_set_format(kith_source *src, const kith_pcm_format *want) {
  if (!src) return KITH_ERR_INVALID_ARG;
  if (src->started) return KITH_ERR_INVALID_ARG;
  return apply(src, want);
}

/* --- Reading --- */

static int setup(kith_source *src) {
  src->started = 1;
  unsigned in_ch = src->src_channels, out_ch = (unsigned)src->fmt.channels;
  src->in = malloc((size_t)CHUNK * in_ch * sizeof *src->in);
  src->mixed = malloc((size_t)CHUNK * out_ch * sizeof *src->mixed);
  if (!src->in || !src->mixed) return KITH_ERR_NOMEM;
  if (out_ch < in_ch && in_ch > 1) {
    if (out_ch == 1) {
      /* Averaged by hand below: miniaudio's mono mix of stereo is the same, but this keeps
       * any channel count simple. */
    } else {
      ma_channel_converter_config config = ma_channel_converter_config_init(
        ma_format_f32, in_ch, NULL, out_ch, NULL, ma_channel_mix_mode_default);
      if (ma_channel_converter_init(&config, NULL, &src->mixer) != MA_SUCCESS) return KITH_ERR_NOMEM;
      src->have_mixer = 1;
    }
  }
  if ((unsigned)src->fmt.sample_rate != src->src_rate) {
    int err = 0;
    src->rs = libopusenc_resampler_init(out_ch, src->src_rate, (unsigned)src->fmt.sample_rate,
                                        RS_QUALITY, &err);
    if (!src->rs) return KITH_ERR_NOMEM;
    libopusenc_resampler_skip_zeros(src->rs);
  }
  /* Integers straight from a lossless source of no more bits, neither mixed nor resampled,
   * are the source's own samples: dither would only add noise. */
  src->exact = src->lossless && src->src_bits <= src->fmt.bits && !src->rs && out_ch == in_ch;
  return KITH_OK;
}

/* Room in pending for n more frames. */
static int reserve(kith_source *src, size_t n) {
  size_t ch = (size_t)src->fmt.channels;
  if (src->pending_pos && src->pending_pos == src->pending_len) src->pending_pos = src->pending_len = 0;
  if (src->pending_pos) { /* move what's left to the front */
    memmove(src->pending, src->pending + src->pending_pos * ch,
            (src->pending_len - src->pending_pos) * ch * sizeof *src->pending);
    src->pending_len -= src->pending_pos;
    src->pending_pos = 0;
  }
  if (src->pending_len + n <= src->pending_cap) return 1;
  size_t cap = src->pending_cap ? src->pending_cap : CHUNK;
  while (cap < src->pending_len + n) cap *= 2;
  float *more = realloc(src->pending, cap * ch * sizeof *more);
  if (!more) return 0;
  src->pending = more;
  src->pending_cap = cap;
  return 1;
}

/* Channels: frames of in (src_channels) into mixed (fmt.channels). */
static void mix(kith_source *src, uint64_t frames) {
  unsigned in_ch = src->src_channels, out_ch = (unsigned)src->fmt.channels;
  if (src->have_mixer) {
    ma_channel_converter_process_pcm_frames(&src->mixer, src->mixed, src->in, frames);
  } else if (in_ch == out_ch) {
    memcpy(src->mixed, src->in, (size_t)frames * in_ch * sizeof *src->in);
  } else if (out_ch == 1) {
    for (uint64_t i = 0; i < frames; i++) {
      float sum = 0;
      for (unsigned c = 0; c < in_ch; c++) sum += src->in[i * in_ch + c];
      src->mixed[i] = sum / (float)in_ch;
    }
  } else { /* up from mono (or fewer): the last channel copied into the rest */
    for (uint64_t i = 0; i < frames; i++)
      for (unsigned c = 0; c < out_ch; c++)
        src->mixed[i * out_ch + c] = src->in[i * in_ch + (c < in_ch ? c : in_ch - 1)];
  }
}

/* Puts frames of mixed (or zeros, for flushing) through the resampler into pending. */
static int resample(kith_source *src, const float *in, unsigned frames) {
  size_t ch = (size_t)src->fmt.channels;
  unsigned done = 0;
  while (done < frames || (!frames && !done)) {
    unsigned in_len = frames - done;
    unsigned out_len = (unsigned)((uint64_t)in_len * (unsigned)src->fmt.sample_rate / src->src_rate) + 64;
    if (!reserve(src, out_len)) return KITH_ERR_NOMEM;
    float *out = src->pending + src->pending_len * ch;
    libopusenc_resampler_process_interleaved_float(src->rs, in + (size_t)done * ch, &in_len, out,
                                                   &out_len);
    /* Past the end, only what the stream should come to. */
    if (src->eof && src->out_done + out_len > src->out_expected)
      out_len = src->out_expected > src->out_done ? (unsigned)(src->out_expected - src->out_done) : 0;
    src->pending_len += out_len;
    src->out_done += out_len;
    done += in_len;
    if (!frames) break;
    if (!in_len && !out_len) break;
  }
  return KITH_OK;
}

/* Decodes and converts at least one more chunk into pending, or marks the end. */
static int produce(kith_source *src) {
  size_t ch = (size_t)src->fmt.channels;
  uint64_t got = src->eof ? 0 : mc_decoder_read(src->dec, src->in, CHUNK);
  if (got) {
    src->in_done += got;
    mix(src, got);
    if (!src->rs) {
      if (!reserve(src, (size_t)got)) return KITH_ERR_NOMEM;
      memcpy(src->pending + src->pending_len * ch, src->mixed, (size_t)got * ch * sizeof *src->mixed);
      src->pending_len += (size_t)got;
      return KITH_OK;
    }
    return resample(src, src->mixed, (unsigned)got);
  }
  if (!src->eof) {
    src->eof = 1;
    src->out_expected = src->in_done * (uint64_t)src->fmt.sample_rate / src->src_rate;
  }
  if (!src->rs || src->flushed) {
    src->flushed = 1;
    return KITH_OK;
  }
  /* The resampler holds back its latency's worth: zeros push it out. */
  int latency = libopusenc_resampler_get_input_latency(src->rs);
  unsigned n = latency > 0 ? (unsigned)latency : 0;
  float *zeros = calloc((size_t)(n ? n : 1) * ch, sizeof *zeros);
  if (!zeros) return KITH_ERR_NOMEM;
  int rc = n ? resample(src, zeros, n) : KITH_OK;
  free(zeros);
  src->flushed = 1;
  return rc;
}

static uint32_t next_rand(kith_source *src) {
  uint32_t x = src->rng; /* xorshift32 */
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  src->rng = x;
  return x;
}

/* One sample in -1..1 to a signed integer of bits, dithered unless exact. */
static int32_t quantise(kith_source *src, float v, int bits) {
  double scale = (double)(1u << (bits - 1));
  double x = (double)v * scale;
  if (!src->exact) {
    /* TPDF: two uniform values in [0, 1) less one, ±1 LSB, triangular. */
    double r = (next_rand(src) >> 8) / 16777216.0 + (next_rand(src) >> 8) / 16777216.0 - 1.0;
    x += r;
  }
  double lo = -scale, hi = scale - 1;
  x = floor(x + 0.5);
  if (x < lo) x = lo;
  if (x > hi) x = hi;
  return (int32_t)x;
}

int64_t mc_source_read(kith_source *src, void *buf, int64_t frames) {
  if (!src || !buf || frames < 0) return KITH_ERR_INVALID_ARG;
  if (!src->started) {
    int rc = setup(src);
    if (rc != KITH_OK) return rc;
  }
  size_t ch = (size_t)src->fmt.channels;
  int64_t done = 0;
  while (done < frames) {
    if (src->pending_pos == src->pending_len) {
      if (src->flushed) break;
      int rc = produce(src);
      if (rc != KITH_OK) return rc;
      continue;
    }
    size_t avail = src->pending_len - src->pending_pos;
    size_t n = (size_t)(frames - done) < avail ? (size_t)(frames - done) : avail;
    const float *from = src->pending + src->pending_pos * ch;
    size_t samples = n * ch, base = (size_t)done * ch;
    switch (src->fmt.sample) {
      case KITH_SAMPLE_S16: {
        int16_t *o = (int16_t *)buf + base;
        for (size_t i = 0; i < samples; i++) o[i] = (int16_t)quantise(src, from[i], 16);
        break;
      }
      case KITH_SAMPLE_S32: {
        int32_t *o = (int32_t *)buf + base;
        int bits = src->fmt.bits;
        for (size_t i = 0; i < samples; i++)
          o[i] = bits >= 32 ? quantise(src, from[i], 31) * 2
                            : (int32_t)((uint32_t)quantise(src, from[i], bits) << (32 - bits));
        break;
      }
      default:
        memcpy((float *)buf + base, from, samples * sizeof *from);
        break;
    }
    src->pending_pos += n;
    done += (int64_t)n;
  }
  return done;
}

void mc_source_close(kith_source *src) {
  if (!src) return;
  if (src->dec) mc_decoder_close(src->dec);
  if (src->have_mixer) ma_channel_converter_uninit(&src->mixer, NULL);
  if (src->rs) libopusenc_resampler_destroy(src->rs);
  if (src->tags_read == 1) mc_tags_clear(&src->tags);
  free(src->cover);
  free(src->in);
  free(src->mixed);
  free(src->pending);
  free(src->path);
  free(src);
}

/* --- Tags and cover --- */

int mc_source_tags(kith_source *src, kith_tags *out) {
  if (!src || !out) return KITH_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  if (!src->tags_read) src->tags_read = mc_tags_read(src->path, &src->tags) == MC_OK ? 1 : 2;
  if (src->tags_read != 1) return KITH_OK; /* untagged */
  const mc_tags *t = &src->tags;
  out->title = t->title;
  out->artist = t->artist;
  out->album = t->album;
  out->album_artist = t->album_artist;
  out->genre = t->genre;
  out->mb_trackid = t->mb_trackid;
  out->mb_albumid = t->mb_albumid;
  out->year = t->year;
  out->track_no = t->track;
  out->track_total = t->track_total;
  out->disc_no = t->disc;
  out->disc_total = t->disc_total;
  return KITH_OK;
}

/* The directory part of path, malloc'd; NULL if it has none. */
static char *parent_dir(const char *path) {
  const char *sep = strrchr(path, '/');
  const char *native = strrchr(path, MC_PATH_SEP);
  if (native > sep) sep = native;
  if (!sep) return NULL;
  size_t len = sep > path ? (size_t)(sep - path) : 1;
  char *dir = malloc(len + 1);
  if (!dir) return NULL;
  memcpy(dir, path, len);
  dir[len] = '\0';
  return dir;
}

/* The whole of a file, malloc'd, if it's under max bytes. */
static unsigned char *slurp(const char *path, size_t max, size_t *len) {
  FILE *f = mc_fopen(path, "rb");
  if (!f) return NULL;
  unsigned char *buf = malloc(max);
  size_t n = buf ? fread(buf, 1, max, f) : 0;
  int whole = buf && n < max && !ferror(f);
  fclose(f);
  if (!whole) {
    free(buf);
    return NULL;
  }
  *len = n;
  return buf;
}

int mc_source_cover(kith_source *src, kith_cover *out) {
  if (!src || !out) return KITH_ERR_INVALID_ARG;
  memset(out, 0, sizeof *out);
  if (!src->cover_read) {
    src->cover_read = 1;
    char *dir = parent_dir(src->path);
    char *cover = dir ? mc_coverart_find(dir) : NULL;
    if (cover) src->cover = slurp(cover, (size_t)32 << 20, &src->cover_size);
    if (src->cover && src->cover_size >= 4)
      src->cover_mime = src->cover[0] == 0x89 && src->cover[1] == 'P' ? "image/png" : "image/jpeg";
    free(cover);
    free(dir);
  }
  if (!src->cover) return KITH_ERR_NOT_FOUND;
  out->data = src->cover;
  out->size = src->cover_size;
  out->mime = src->cover_mime;
  return KITH_OK;
}
