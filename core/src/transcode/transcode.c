/* Opus transcodes for sync.
 *
 *   decoder (playback/decoder.c) -> downmix to stereo -> libopusenc (Speex resampler to
 *   48 kHz, then libopus) -> <path>.<random>.part -> rename into place
 *
 * Each encode writes a part file of its own, so two of the same file can run at once: the
 * last rename wins, and both files are whole.
 *
 * Tags are read from the source with TagLib (tag_io.cpp) and written as Opus comments while
 * encoding, so the file is complete the moment it's renamed into place. The cover is the
 * album's image beside the source (coverart.c finds it), never the source's embedded one. */
#include "musiccore/transcode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "miniaudio.h"
#include "opusenc.h"
#include "playback/decoder.h"
#include "tagger/coverart.h"
#include "tagger/tag_io.h"
#include "util/platform.h"

enum {
  CHUNK = 4096,          /* frames decoded at a time */
  OVERHEAD_BYTES = 4096, /* Ogg framing, headers and tags, in size estimates */
  BENCH_FRAMES = 9600,   /* 200 ms at 48 kHz */
  BENCH_MS = 500,
};

/* Partial files older than this belong to an encode that died. */
#define STALE_PART_MS (60 * 60 * 1000)

const int mc_opus_bitrates[MC_OPUS_BITRATE_COUNT] = {256, 192, 160, 128, 96, 64};

static int valid_bitrate(int kbps) {
  for (int i = 0; i < MC_OPUS_BITRATE_COUNT; i++)
    if (mc_opus_bitrates[i] == kbps) return 1;
  return 0;
}

/* A file uid names one directory in the cache, so it mustn't climb out of it. */
static int valid_uid(const char *uid) {
  if (!uid || !*uid || strcmp(uid, ".") == 0 || strcmp(uid, "..") == 0) return 0;
  return !strchr(uid, '/') && !strchr(uid, '\\');
}

/* a, then sep if it isn't 0, then b; malloc'd. */
static char *concat(const char *a, char sep, const char *b) {
  size_t la = strlen(a), lb = strlen(b), n = sep ? 1 : 0;
  char *s = malloc(la + n + lb + 1);
  if (!s) return NULL;
  memcpy(s, a, la);
  if (sep) s[la] = sep;
  memcpy(s + la + n, b, lb + 1);
  return s;
}

/* The directory part of path, malloc'd; NULL if it has none. */
static char *parent_dir(const char *path) {
  const char *sep = strrchr(path, '/');
  const char *native = strrchr(path, MC_PATH_SEP);
  if (native > sep) sep = native;
  if (!sep) return NULL;
  size_t len = sep > path ? (size_t)(sep - path) : 1; /* "/x" -> "/" */
  char *dir = malloc(len + 1);
  if (!dir) return NULL;
  memcpy(dir, path, len);
  dir[len] = '\0';
  return dir;
}

/* --- Tags --- */

static int add_str(OggOpusComments *c, const char *key, const char *value) {
  return !value || !*value || ope_comments_add(c, key, value) == OPE_OK;
}

static int add_int(OggOpusComments *c, const char *key, int value) {
  char buf[16];
  if (value <= 0) return 1;
  snprintf(buf, sizeof buf, "%d", value);
  return ope_comments_add(c, key, buf) == OPE_OK;
}

/* The Vorbis comment names TagLib reads back as the same properties (see tag_io.cpp). A
 * source without tags, or one TagLib can't read, gives an untagged copy. Returns 0 only
 * when out of memory. */
static int add_tags(OggOpusComments *c, const char *src_path) {
  mc_tags t;
  if (mc_tags_read(src_path, &t) != MC_OK) return 1;
  int ok = add_str(c, "TITLE", t.title) && add_str(c, "ARTIST", t.artist) &&
           add_str(c, "ALBUM", t.album) && add_str(c, "ALBUMARTIST", t.album_artist) &&
           add_str(c, "GENRE", t.genre) && add_int(c, "DATE", t.year) &&
           add_int(c, "ORIGINALDATE", t.original_year) && add_int(c, "TRACKNUMBER", t.track) &&
           add_int(c, "TRACKTOTAL", t.track_total) && add_int(c, "DISCNUMBER", t.disc) &&
           add_int(c, "DISCTOTAL", t.disc_total) &&
           add_str(c, "MUSICBRAINZ_TRACKID", t.mb_trackid) &&
           add_str(c, "MUSICBRAINZ_RELEASETRACKID", t.mb_releasetrackid) &&
           add_str(c, "MUSICBRAINZ_ALBUMID", t.mb_albumid) &&
           add_str(c, "MUSICBRAINZ_ARTISTID", t.mb_artistid) &&
           add_str(c, "MUSICBRAINZ_ALBUMARTISTID", t.mb_albumartistid) &&
           add_str(c, "MUSICBRAINZ_RELEASEGROUPID", t.mb_releasegroupid);
  mc_tags_clear(&t);
  return ok;
}

/* Embeds the album's cover as a front-cover picture. An image libopusenc can't parse (it
 * takes JPEG, PNG and GIF) is left out rather than failing the transcode. */
static void add_cover(OggOpusComments *c, const char *src_path) {
  char *dir = parent_dir(src_path);
  char *cover = dir ? mc_coverart_find(dir) : NULL;
  char *cover_path = cover ? mc_open_path(cover) : NULL;
  if (cover_path) ope_comments_add_picture(c, cover_path, -1, NULL);
  free(cover_path);
  free(cover);
  free(dir);
}

/* --- Encoding --- */

/* Encodes src into part_path. part_path may be left behind on failure. */
static mc_status encode(const char *src_path, const char *part_path, int kbps) {
  mc_decoder *dec;
  mc_status st = mc_decoder_open(src_path, &dec);
  if (st != MC_OK) return st;
  unsigned channels = mc_decoder_channels(dec), out_channels = channels > 2 ? 2 : channels;

  float *in = malloc((size_t)CHUNK * channels * sizeof *in);
  float *mixed = channels > 2 ? malloc((size_t)CHUNK * 2 * sizeof *mixed) : NULL;
  OggOpusComments *comments = ope_comments_create();
  OggOpusEnc *enc = NULL;
  ma_channel_converter mixer;
  int have_mixer = 0;
  st = MC_ERR_NOMEM;
  if (!in || (channels > 2 && !mixed) || !comments || !add_tags(comments, src_path)) goto done;
  add_cover(comments, src_path);
  if (channels > 2) {
    ma_channel_converter_config config = ma_channel_converter_config_init(
      ma_format_f32, channels, NULL, 2, NULL, ma_channel_mix_mode_default);
    if (ma_channel_converter_init(&config, NULL, &mixer) != MA_SUCCESS) goto done;
    have_mixer = 1;
  }

  /* libopusenc resamples anything that isn't 48 kHz itself, with its Speex resampler. */
  int err;
  char *out_path = mc_open_path(part_path);
  enc = out_path ? ope_encoder_create_file(out_path, comments, (opus_int32)mc_decoder_rate(dec), (int)out_channels,
                                           0, &err)
                 : NULL;
  free(out_path);
  st = MC_ERR_IO;
  if (!enc) goto done;
  if (ope_encoder_ctl(enc, OPUS_SET_BITRATE(kbps * 1000)) != OPE_OK ||
      ope_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPE_OK ||
      ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPE_OK ||
      ope_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC)) != OPE_OK)
    goto done;

  uint64_t got;
  while ((got = mc_decoder_read(dec, in, CHUNK)) > 0) {
    const float *pcm = in;
    if (have_mixer) {
      if (ma_channel_converter_process_pcm_frames(&mixer, mixed, in, got) != MA_SUCCESS) goto done;
      pcm = mixed;
    }
    if (ope_encoder_write_float(enc, pcm, (int)got) != OPE_OK) goto done;
  }
  if (ope_encoder_drain(enc) != OPE_OK) goto done;
  st = MC_OK;

done:
  if (enc) ope_encoder_destroy(enc); /* closes the file */
  if (have_mixer) ma_channel_converter_uninit(&mixer, NULL);
  if (comments) ope_comments_destroy(comments);
  free(mixed);
  free(in);
  mc_decoder_close(dec);
  return st;
}

mc_status mc_transcode_cached(const char *cache_dir, const char *file_uid, const char *src_path,
                              const mc_transcode_opts *opts, char **out_path) {
  if (out_path) *out_path = NULL;
  if (!src_path || !opts || !out_path) return MC_ERR_INVALID_ARG;
  if (opts->format == MC_FORMAT_ORIGINAL) {
    *out_path = mc_strdup(src_path);
    return *out_path ? MC_OK : MC_ERR_NOMEM;
  }
  if (opts->format != MC_FORMAT_OPUS || !valid_bitrate(opts->bitrate_kbps) || !cache_dir ||
      !valid_uid(file_uid))
    return MC_ERR_INVALID_ARG;

  char name[32];
  snprintf(name, sizeof name, "opus-%d.opus", opts->bitrate_kbps);
  char *uid_dir = concat(cache_dir, MC_PATH_SEP, file_uid);
  char *path = uid_dir ? concat(uid_dir, MC_PATH_SEP, name) : NULL;
  unsigned char nonce[6];
  char suffix[32];
  mc_status st = mc_random_bytes(nonce, sizeof nonce);
  snprintf(suffix, sizeof suffix, ".%02x%02x%02x%02x%02x%02x.part", nonce[0], nonce[1],
           nonce[2], nonce[3], nonce[4], nonce[5]);
  char *part = path && st == MC_OK ? concat(path, 0, suffix) : NULL;
  st = MC_ERR_NOMEM;
  if (part && mc_file_exists(path)) {
    mc_touch(path); /* for the LRU trim */
    st = MC_OK;
  } else if (part) {
    /* Twice if another encode's cleanup took the folder away from under this one. */
    for (int tries = 0; tries < 2; tries++) {
      if ((st = mc_mkdirs(uid_dir)) != MC_OK) break;
      st = encode(src_path, part, opts->bitrate_kbps);
      if (st == MC_OK) {
        st = mc_rename(part, path, 1);
        /* Another encode of it got there first (on Windows, it may be open to be sent). */
        if (st != MC_OK && mc_file_exists(path)) st = MC_OK;
      }
      if (st == MC_OK) break;
      int gone = !mc_file_exists(uid_dir);
      if (mc_file_exists(part)) mc_remove(part);
      mc_rmdir(uid_dir); /* only if it's empty */
      if (!gone) break;
    }
  }
  if (st == MC_OK) {
    *out_path = path;
    path = NULL;
  }
  free(part);
  free(path);
  free(uid_dir);
  return st;
}

mc_status mc_transcode_estimate_bytes(int duration_ms, int kbps, int64_t *out) {
  if (!out) return MC_ERR_INVALID_ARG;
  *out = 0;
  if (duration_ms < 0 || kbps <= 0) return MC_ERR_INVALID_ARG;
  *out = (int64_t)duration_ms * kbps / 8 + OVERHEAD_BYTES; /* kbps is bits per millisecond */
  return MC_OK;
}

/* --- Cache trim --- */

typedef struct cached_file {
  char *path;
  int64_t size, mtime_ms;
} cached_file;

typedef struct cache_scan {
  cached_file *files;
  int count, cap;
  int64_t total;
  uint64_t now_ms;
  int oom;
} cache_scan;

static int is_part(const char *path) {
  size_t len = strlen(path);
  return len >= 5 && strcmp(path + len - 5, ".part") == 0;
}

static int on_cached_file(void *user, const char *path) {
  cache_scan *s = user;
  int64_t size, mtime_ms;
  if (mc_file_info(path, &size, &mtime_ms) != MC_OK) return 0;
  if (is_part(path)) {
    if ((int64_t)s->now_ms - mtime_ms > STALE_PART_MS) mc_remove(path);
    return 0;
  }
  if (s->count == s->cap) {
    int cap = s->cap ? s->cap * 2 : 64;
    cached_file *grown = realloc(s->files, (size_t)cap * sizeof *grown);
    if (!grown) {
      s->oom = 1;
      return 1;
    }
    s->files = grown;
    s->cap = cap;
  }
  char *copy = mc_strdup(path);
  if (!copy) {
    s->oom = 1;
    return 1;
  }
  s->files[s->count++] = (cached_file){copy, size, mtime_ms};
  s->total += size;
  return 0;
}

static int by_mtime(const void *a, const void *b) {
  const cached_file *x = a, *y = b;
  return (x->mtime_ms > y->mtime_ms) - (x->mtime_ms < y->mtime_ms);
}

mc_status mc_transcode_cache_trim(const char *cache_dir, int64_t max_bytes) {
  if (!cache_dir || max_bytes < 0) return MC_ERR_INVALID_ARG;
  if (!mc_file_exists(cache_dir)) return MC_OK;
  cache_scan s = {0};
  s.now_ms = mc_wall_ms();
  mc_status st = mc_walk(cache_dir, on_cached_file, &s);
  if (s.oom) st = MC_ERR_NOMEM;
  if (st == MC_OK) {
    qsort(s.files, (size_t)s.count, sizeof *s.files, by_mtime);
    for (int i = 0; i < s.count && s.total > max_bytes; i++) {
      if (mc_remove(s.files[i].path) != MC_OK) continue;
      s.total -= s.files[i].size;
      char *dir = parent_dir(s.files[i].path);
      if (dir && !mc_same_path(dir, cache_dir)) mc_rmdir(dir); /* only if it's empty now */
      free(dir);
    }
  }
  for (int i = 0; i < s.count; i++) free(s.files[i].path);
  free(s.files);
  return st;
}

/* --- Benchmark --- */

mc_status mc_transcode_benchmark(int *score) {
  if (!score) return MC_ERR_INVALID_ARG;
  *score = 0;
  float *pcm = malloc((size_t)BENCH_FRAMES * 2 * sizeof *pcm);
  OggOpusComments *comments = ope_comments_create();
  int err;
  OggOpusEnc *enc = comments ? ope_encoder_create_pull(comments, 48000, 2, 0, &err) : NULL;
  mc_status st = MC_ERR_NOMEM;
  if (!pcm || !enc) goto done;
  st = MC_ERR_IO;
  if (ope_encoder_ctl(enc, OPUS_SET_BITRATE(128000)) != OPE_OK ||
      ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(10)) != OPE_OK)
    goto done;

  /* Something like music: two detuned tones and a little noise, so the encoder can't coast. */
  uint32_t noise = 0x12345678u;
  for (int i = 0; i < BENCH_FRAMES; i++) {
    double w = 2 * 3.14159265358979 * i / 48000.0;
    noise = noise * 1664525u + 1013904223u;
    float n = (float)(noise >> 8) / 16777216.0f - 0.5f;
    pcm[i * 2] = (float)(0.3 * sin(220 * w) + 0.2 * sin(1375 * w)) + 0.05f * n;
    pcm[i * 2 + 1] = (float)(0.3 * sin(277 * w) + 0.2 * sin(990 * w)) - 0.05f * n;
  }

  uint64_t start = mc_mono_ms(), elapsed = 0, frames = 0;
  while (elapsed < BENCH_MS) {
    if (ope_encoder_write_float(enc, pcm, BENCH_FRAMES) != OPE_OK) goto done;
    unsigned char *page;
    opus_int32 len;
    while (ope_encoder_get_page(enc, &page, &len, 0)) {
    }
    frames += BENCH_FRAMES;
    elapsed = mc_mono_ms() - start;
  }
  /* Seconds of audio per second, per core, times the cores. */
  double realtime = (double)frames / 48000.0 / ((double)elapsed / 1000.0);
  *score = (int)(realtime * mc_cpu_count() + 0.5);
  st = MC_OK;

done:
  if (enc) ope_encoder_destroy(enc);
  if (comments) ope_comments_destroy(comments);
  free(pcm);
  return st;
}
