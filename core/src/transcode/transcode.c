/* Opus transcodes for sync.
 *
 *   decoder (playback/decoder.c) -> downmix to stereo -> libopusenc (Speex resampler to
 *   48 kHz, then libopus) -> <path>.part, hashed as it's written -> rename into place ->
 *   sidecar with the hash
 *
 * Tags are read from the source with TagLib (tag_io.cpp) and written as Opus comments while
 * encoding, so the file is complete the moment it's renamed into place. The cover is the
 * album's image beside the source (coverart.c finds it), never the source's embedded one. */
#include "musiccore/transcode.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

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

/* Spare bits at high bitrates make what the extra analysis finds inaudible, so it's skipped
 * there to encode faster. */
static int complexity(int kbps) {
  return kbps >= 192 ? 7 : kbps >= 128 ? 8 : 10;
}

static void hex(const unsigned char digest[32], char out[65]) {
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    out[2 * i] = digits[digest[i] >> 4];
    out[2 * i + 1] = digits[digest[i] & 15];
  }
  out[64] = '\0';
}

/* Where the encoder's pages go: the part file, hashed on the way. */
typedef struct sink {
  FILE *f;
  mbedtls_sha256_context sha;
  int failed;
} sink;

static int sink_write(void *user, const unsigned char *ptr, opus_int32 len) {
  sink *k = user;
  if (fwrite(ptr, 1, (size_t)len, k->f) != (size_t)len ||
      mbedtls_sha256_update(&k->sha, ptr, (size_t)len) != 0)
    k->failed = 1;
  return k->failed;
}

static int sink_close(void *user) {
  sink *k = user;
  if (k->f && fclose(k->f) != 0) k->failed = 1;
  k->f = NULL;
  return k->failed;
}

/* Encodes src into part_path and hashes what it wrote into sha. part_path may be left
 * behind on failure. */
static mc_status encode(const char *src_path, const char *part_path, int kbps, char sha[65]) {
  mc_decoder *dec;
  mc_status st = mc_decoder_open(src_path, &dec);
  if (st != MC_OK) return st;
  unsigned channels = mc_decoder_channels(dec), out_channels = channels > 2 ? 2 : channels;

  float *in = malloc((size_t)CHUNK * channels * sizeof *in);
  float *mixed = channels > 2 ? malloc((size_t)CHUNK * 2 * sizeof *mixed) : NULL;
  OggOpusComments *comments = ope_comments_create();
  OggOpusEnc *enc = NULL;
  sink out = {0};
  mbedtls_sha256_init(&out.sha);
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
  static const OpusEncCallbacks callbacks = {sink_write, sink_close};
  int err;
  out.f = mc_fopen(part_path, "wb");
  st = MC_ERR_IO;
  if (!out.f || mbedtls_sha256_starts(&out.sha, 0) != 0) goto done;
  enc = ope_encoder_create_callbacks(&callbacks, &out, comments, (opus_int32)mc_decoder_rate(dec),
                                     (int)out_channels, 0, &err);
  if (!enc) goto done;
  if (ope_encoder_ctl(enc, OPUS_SET_BITRATE(kbps * 1000)) != OPE_OK ||
      ope_encoder_ctl(enc, OPUS_SET_VBR(1)) != OPE_OK ||
      ope_encoder_ctl(enc, OPUS_SET_COMPLEXITY(complexity(kbps))) != OPE_OK ||
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
  ope_encoder_destroy(enc); /* closes the file, if drain didn't */
  enc = NULL;
  unsigned char digest[32];
  if (out.f || out.failed || mbedtls_sha256_finish(&out.sha, digest) != 0) goto done;
  hex(digest, sha);
  st = MC_OK;

done:
  if (enc) ope_encoder_destroy(enc);
  sink_close(&out);
  mbedtls_sha256_free(&out.sha);
  if (have_mixer) ma_channel_converter_uninit(&mixer, NULL);
  if (comments) ope_comments_destroy(comments);
  free(mixed);
  free(in);
  mc_decoder_close(dec);
  return st;
}

/* --- Sidecars --- */

/* <dir>/opus-<kbps>.sha256 for <dir>/opus-<kbps>.opus; malloc'd. */
static char *sidecar_path(const char *path) {
  const char *dot = strrchr(path, '.');
  const char *sep = strrchr(path, '/'), *native = strrchr(path, MC_PATH_SEP);
  if (native > sep) sep = native;
  size_t stem = dot && (!sep || dot > sep) ? (size_t)(dot - path) : strlen(path);
  char *s = malloc(stem + sizeof ".sha256");
  if (!s) return NULL;
  memcpy(s, path, stem);
  memcpy(s + stem, ".sha256", sizeof ".sha256");
  return s;
}

static int ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && strcmp(s + ls - lx, suffix) == 0;
}

/* 64 lowercase hex digits, then the end or a space. */
static int hex64(const char *s) {
  for (int i = 0; i < 64; i++)
    if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
  return s[64] == '\0' || s[64] == ' ';
}

mc_status mc_transcode_sha_read(const char *path, char sha[65], int64_t *size,
                                int64_t *mtime_ms) {
  if (!path || !sha || !size || !mtime_ms) return MC_ERR_INVALID_ARG;
  sha[0] = '\0';
  char *side = sidecar_path(path);
  if (!side) return MC_ERR_NOMEM;
  FILE *f = mc_fopen(side, "rb");
  free(side);
  if (!f) return MC_ERR_NOT_FOUND;
  char line[128];
  long long sz = -1, mt = 0;
  int ok = fgets(line, sizeof line, f) && hex64(line) &&
           sscanf(line + 64, " %lld %lld", &sz, &mt) == 2 && sz >= 0;
  fclose(f);
  if (!ok) return MC_ERR_NOT_FOUND;
  memcpy(sha, line, 64);
  sha[64] = '\0';
  *size = sz;
  *mtime_ms = mt;
  return MC_OK;
}

mc_status mc_transcode_sha_write(const char *path, const char *sha, int64_t size,
                                 int64_t mtime_ms) {
  if (!path || !sha || strlen(sha) != 64 || !hex64(sha) || size < 0) return MC_ERR_INVALID_ARG;
  char *side = sidecar_path(path);
  char *part = side ? concat(side, 0, ".part") : NULL;
  mc_status st = MC_ERR_NOMEM;
  FILE *f = part ? mc_fopen(part, "wb") : NULL;
  if (part && !f) st = MC_ERR_IO;
  if (f) {
    int ok = fprintf(f, "%s %lld %lld\n", sha, (long long)size, (long long)mtime_ms) > 0;
    if (fclose(f) != 0) ok = 0;
    st = ok ? mc_rename(part, side, 1) : MC_ERR_IO;
    if (st != MC_OK) mc_remove(part);
  }
  free(part);
  free(side);
  return st;
}

/* Records path's sidecar from the hash of its bytes, at the size and mtime it has now. A
 * failure only costs a sender a second read. */
static void write_sidecar(const char *path, const char *sha) {
  int64_t size, mtime_ms;
  if (mc_file_info(path, &size, &mtime_ms) == MC_OK)
    mc_transcode_sha_write(path, sha, size, mtime_ms);
}

/* Marks a cache hit as recently used, for the LRU trim, and moves a valid sidecar's mtime
 * along with the file's. */
static void mark_used(const char *path) {
  char sha[65];
  int64_t size, mtime_ms, side_size, side_mtime;
  int valid = mc_transcode_sha_read(path, sha, &side_size, &side_mtime) == MC_OK &&
              mc_file_info(path, &size, &mtime_ms) == MC_OK && size == side_size &&
              mtime_ms == side_mtime;
  mc_touch(path);
  if (valid) write_sidecar(path, sha);
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
  char *part = path ? concat(path, 0, ".part") : NULL;
  mc_status st = MC_ERR_NOMEM;
  if (part && mc_file_exists(path)) {
    mark_used(path);
    st = MC_OK;
  } else if (part && (st = mc_mkdirs(uid_dir)) == MC_OK) {
    char sha[65];
    st = encode(src_path, part, opts->bitrate_kbps, sha);
    if (st == MC_OK) st = mc_rename(part, path, 1);
    if (st == MC_OK) write_sidecar(path, sha);
    if (st != MC_OK) {
      if (mc_file_exists(part)) mc_remove(part);
      mc_rmdir(uid_dir); /* only if it's empty */
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

/* A sidecar goes with its file; one whose file is gone goes now. */
static void check_sidecar(const char *path) {
  size_t stem = strlen(path) - strlen(".sha256");
  char *file = malloc(stem + sizeof ".opus");
  if (!file) return;
  memcpy(file, path, stem);
  memcpy(file + stem, ".opus", sizeof ".opus");
  if (!mc_file_exists(file)) mc_remove(path);
  free(file);
}

static int on_cached_file(void *user, const char *path) {
  cache_scan *s = user;
  int64_t size, mtime_ms;
  if (mc_file_info(path, &size, &mtime_ms) != MC_OK) return 0;
  if (ends_with(path, ".part")) {
    if ((int64_t)s->now_ms - mtime_ms > STALE_PART_MS) mc_remove(path);
    return 0;
  }
  if (ends_with(path, ".sha256")) {
    check_sidecar(path);
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
      char *side = sidecar_path(s.files[i].path);
      if (side && mc_file_exists(side)) mc_remove(side);
      free(side);
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
