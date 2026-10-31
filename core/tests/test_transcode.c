/* Opus transcoding: a generated, tagged WAV with a cover.jpg beside it is transcoded and
 * read back with opusfile, with the player's decoder, and with TagLib. Also the cache hit,
 * the size estimate, the LRU trim, failures leaving nothing behind, encodes at once (two
 * files, and three of one file), and the benchmark. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/musiccore.h"
#include "opusfile.h"
#include "playback/decoder.h"
#include "tagger/tag_io.h"
#include "test.h"
#include "util/platform.h"

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A 16-bit PCM WAV of a 440 Hz tone, a little different on each channel. */
static int write_wav(const char *path, unsigned rate, unsigned channels, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t frames = rate * ms / 1000, bytes = frames * channels * 2;
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, channels);
  put32(f, rate);
  put32(f, rate * channels * 2);
  put16(f, channels * 2);
  put16(f, 16);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++) {
    for (unsigned c = 0; c < channels; c++) {
      double v = 0.4 * sin(2 * 3.14159265358979 * 440 * (1 + 0.1 * c) * i / rate);
      put16(f, (unsigned)(uint16_t)(int16_t)(v * 32767));
    }
  }
  return fclose(f) == 0;
}

static int write_bytes(const char *path, const void *data, size_t len) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  int ok = fwrite(data, 1, len, f) == len;
  return fclose(f) == 0 && ok;
}

static char *join(const char *dir, const char *name) {
  size_t n = strlen(dir) + strlen(name) + 2;
  char *s = malloc(n);
  if (s) snprintf(s, n, "%s/%s", dir, name);
  return s;
}

static int64_t file_size(const char *path) {
  int64_t size = -1, mtime;
  if (mc_file_info(path, &size, &mtime) != MC_OK) return -1;
  return size;
}

/* A 1×1 baseline JPEG header: enough for libopusenc to recognise and measure it. */
static const unsigned char k_jpeg[] = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x01, 0x00,
                                       0x01, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9};

typedef struct job {
  const char *cache, *uid, *src;
  mc_status st;
  char *out;
} job;

static void count_entry(void *user, const char *name, int is_dir) {
  (void)name;
  (void)is_dir;
  (*(int *)user)++;
}

/* Entries directly in dir. */
static int dir_entries(const char *dir) {
  int n = 0;
  return mc_list_dir(dir, count_entry, &n) == MC_OK ? n : -1;
}

static void run_job(void *arg) {
  job *j = arg;
  mc_transcode_opts opts = {MC_FORMAT_OPUS, 96};
  j->st = mc_transcode_cached(j->cache, j->uid, j->src, &opts, &j->out);
}

static void check_transcode(const char *dir) {
  char *src_dir = join(dir, "transcode_src");
  char *cache = join(dir, "transcode_cache");
  mc_mkdirs(src_dir);
  CHECK(mc_transcode_cache_trim(cache, 0) == MC_OK); /* left over from an earlier run */
  char *src = join(src_dir, "song.wav");
  char *cover = join(src_dir, "cover.jpg");
  CHECK(write_wav(src, 44100, 2, 1500));
  CHECK(write_bytes(cover, k_jpeg, sizeof k_jpeg));

  mc_tags in = {0};
  in.title = "Sunflower";
  in.artist = "Low";
  in.album = "Things We Lost in the Fire";
  in.album_artist = "Low";
  in.genre = "Slowcore";
  in.mb_trackid = "8f8e3b9c-2b4e-4f6a-9d2a-1c3b5e7f9a0b";
  in.mb_albumid = "0d9f3a21-6c7e-4b8a-a1f2-3e4d5c6b7a89";
  in.mb_releasegroupid = "1b2c3d4e-5f60-4718-8293-a4b5c6d7e8f9";
  in.year = 2001;
  in.track = 1;
  in.track_total = 13;
  in.disc = 1;
  in.disc_total = 2;
  CHECK(mc_tags_write(src, &in) == MC_OK);

  mc_transcode_opts opts = {MC_FORMAT_OPUS, 128};
  char *out = NULL;
  CHECK(mc_transcode_cached(cache, "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e", src, &opts, &out) == MC_OK);
  char expected_buf[1024];
  const char *expected = expected_buf;
  snprintf(expected_buf, sizeof expected_buf, "%s%c0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e%copus-128.opus",
           cache, MC_PATH_SEP, MC_PATH_SEP);
  CHECK_STR(out, expected);
  CHECK(out && mc_file_exists(out));
  char uid_dir[1100];
  snprintf(uid_dir, sizeof uid_dir, "%s%c0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e", cache, MC_PATH_SEP);
  CHECK(dir_entries(uid_dir) == 1); /* no part file left */

  /* opusfile reads it back at 48 kHz, stereo, 1.5 s long (±20 ms). */
  OggOpusFile *of = out ? op_open_file(out, NULL) : NULL;
  CHECK(of != NULL);
  if (of) {
    ogg_int64_t total = op_pcm_total(of, -1);
    CHECK(total >= 72000 - 960 && total <= 72000 + 960);
    CHECK(op_channel_count(of, -1) == 2);
    CHECK(op_head(of, -1)->input_sample_rate == 44100);
    op_free(of);
  }

  /* The player's decoder: always 48 kHz stereo, and it decodes to the end. */
  mc_decoder *dec = NULL;
  CHECK(out && mc_decoder_open(out, &dec) == MC_OK);
  if (dec) {
    CHECK(mc_decoder_rate(dec) == 48000 && mc_decoder_channels(dec) == 2);
    uint64_t length = mc_decoder_length(dec), frames = 0, got;
    float buf[4096 * 2];
    double energy = 0;
    while ((got = mc_decoder_read(dec, buf, 4096)) > 0) {
      for (uint64_t i = 0; i < got * 2; i++) energy += buf[i] * buf[i];
      frames += got;
    }
    CHECK(frames == length);
    CHECK(energy / (double)(frames * 2) > 0.05); /* the tone survived: 0.4² / 2 = 0.08 */
    mc_decoder_seek(dec, 48000);
    CHECK(mc_decoder_read(dec, buf, 4096) == 4096);
    mc_decoder_close(dec);
  }

  /* Tags and cover were copied. */
  mc_tags t;
  CHECK(out && mc_tags_read(out, &t) == MC_OK);
  if (out) {
    CHECK_STR(t.title, in.title);
    CHECK_STR(t.artist, in.artist);
    CHECK_STR(t.album, in.album);
    CHECK_STR(t.album_artist, in.album_artist);
    CHECK_STR(t.genre, in.genre);
    CHECK_STR(t.mb_trackid, in.mb_trackid);
    CHECK_STR(t.mb_albumid, in.mb_albumid);
    CHECK_STR(t.mb_releasegroupid, in.mb_releasegroupid);
    CHECK(t.year == 2001);
    CHECK(t.track == 1 && t.track_total == 13);
    CHECK(t.disc == 1 && t.disc_total == 2);
    CHECK(t.sample_rate == 48000 && t.channels == 2);
    mc_tags_clear(&t);
  }
  unsigned char *img = NULL;
  size_t img_len = 0;
  CHECK(out && mc_tags_read_picture(out, &img, &img_len) == MC_OK);
  CHECK(img && img_len == sizeof k_jpeg && memcmp(img, k_jpeg, sizeof k_jpeg) == 0);
  free(img);

  /* A second call is a cache hit: immediate, and the file is untouched apart from mtime. */
  int64_t size = out ? file_size(out) : -1;
  char *again = NULL;
  uint64_t start = mc_mono_ms();
  CHECK(mc_transcode_cached(cache, "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e", src, &opts, &again) == MC_OK);
  CHECK(mc_mono_ms() - start < 50);
  CHECK_STR(again, expected);
  CHECK(file_size(expected) == size);
  mc_free(again);

  /* Original needs no transcode. */
  mc_transcode_opts original = {MC_FORMAT_ORIGINAL, 0};
  CHECK(mc_transcode_cached(cache, "x", src, &original, &again) == MC_OK);
  CHECK_STR(again, src);
  mc_free(again);

  /* Bad arguments. */
  mc_transcode_opts odd = {MC_FORMAT_OPUS, 100};
  CHECK(mc_transcode_cached(cache, "x", src, &odd, &again) == MC_ERR_INVALID_ARG && !again);
  CHECK(mc_transcode_cached(cache, "..", src, &opts, &again) == MC_ERR_INVALID_ARG);
  CHECK(mc_transcode_cached(cache, "a/b", src, &opts, &again) == MC_ERR_INVALID_ARG);

  /* A bad source fails and leaves no part file, nor its directory. */
  char *bad = join(src_dir, "bad.flac");
  CHECK(write_bytes(bad, "not a flac file at all", 22));
  CHECK(mc_transcode_cached(cache, "bad-uid", bad, &opts, &again) != MC_OK && !again);
  char *bad_dir = join(cache, "bad-uid");
  char *bad_part = join(bad_dir, "opus-128.opus.part");
  CHECK(!mc_file_exists(bad_part));
  CHECK(!mc_file_exists(bad_dir));
  char *missing = join(src_dir, "missing.wav");
  CHECK(mc_transcode_cached(cache, "bad-uid", missing, &opts, &again) != MC_OK);
  CHECK(!mc_file_exists(bad_dir));

  /* More than two channels are downmixed; 48 kHz isn't resampled. Two encodes at once. */
  char *surround = join(src_dir, "surround.wav");
  char *mono = join(src_dir, "mono.wav");
  CHECK(write_wav(surround, 48000, 6, 500));
  CHECK(write_wav(mono, 22050, 1, 500));
  job a = {cache, "surround", surround, MC_ERR_IO, NULL}, b = {cache, "mono", mono, MC_ERR_IO, NULL};
  mc_thread ta, tb;
  CHECK(mc_thread_start(&ta, run_job, &a) == MC_OK);
  CHECK(mc_thread_start(&tb, run_job, &b) == MC_OK);
  mc_thread_join(&ta);
  mc_thread_join(&tb);
  CHECK(a.st == MC_OK && b.st == MC_OK);
  of = a.out ? op_open_file(a.out, NULL) : NULL;
  CHECK(of && op_channel_count(of, -1) == 2);
  if (of) {
    ogg_int64_t total = op_pcm_total(of, -1);
    CHECK(total >= 24000 - 960 && total <= 24000 + 960);
    op_free(of);
  }
  of = b.out ? op_open_file(b.out, NULL) : NULL;
  CHECK(of && op_channel_count(of, -1) == 1);
  if (of) op_free(of);
  mc_free(a.out);
  mc_free(b.out);

  /* Three encodes of the same file at once: each writes its own part file, so the one left
   * is whole, and nothing else is. */
  {
    job same[3];
    mc_thread ts[3];
    for (int i = 0; i < 3; i++) {
      same[i] = (job){cache, "same", src, MC_ERR_IO, NULL};
      CHECK(mc_thread_start(&ts[i], run_job, &same[i]) == MC_OK);
    }
    for (int i = 0; i < 3; i++) mc_thread_join(&ts[i]);
    for (int i = 0; i < 3; i++) {
      CHECK(same[i].st == MC_OK);
      CHECK(same[i].out && same[0].out && !strcmp(same[i].out, same[0].out));
    }
    of = same[0].out ? op_open_file(same[0].out, NULL) : NULL;
    CHECK(of != NULL);
    if (of) {
      ogg_int64_t total = op_pcm_total(of, -1);
      CHECK(total >= 72000 - 960 && total <= 72000 + 960);
      op_free(of);
    }
    char *same_dir = join(cache, "same");
    CHECK(dir_entries(same_dir) == 1);
    free(same_dir);
    for (int i = 0; i < 3; i++) mc_free(same[i].out);
  }

  /* Trim: everything fits, then only the most recently used, then nothing. */
  CHECK(mc_transcode_cache_trim(cache, INT64_MAX) == MC_OK);
  CHECK(mc_file_exists(expected));
  mc_sleep_ms(20);
  CHECK(mc_transcode_cached(cache, "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e", src, &opts, &again) == MC_OK);
  mc_free(again); /* touched: now the most recently used */
  CHECK(mc_transcode_cache_trim(cache, size) == MC_OK);
  CHECK(mc_file_exists(expected));
  char *surround_dir = join(cache, "surround");
  CHECK(!mc_file_exists(surround_dir)); /* emptied directories go too */
  CHECK(mc_transcode_cache_trim(cache, 0) == MC_OK);
  CHECK(!mc_file_exists(expected));
  char *missing_cache = join(dir, "no_such_cache");
  CHECK(mc_transcode_cache_trim(missing_cache, 0) == MC_OK);

  mc_free(out);
  free(missing_cache);
  free(surround_dir);
  free(surround);
  free(mono);
  free(missing);
  free(bad_part);
  free(bad_dir);
  free(bad);
  free(cover);
  free(src);
  free(cache);
  free(src_dir);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : ".";

  int64_t est = 0;
  CHECK(mc_transcode_estimate_bytes(180000, 128, &est) == MC_OK && est == 180000 * 128 / 8 + 4096);
  CHECK(mc_transcode_estimate_bytes(0, 64, &est) == MC_OK && est == 4096);
  CHECK(mc_transcode_estimate_bytes(-1, 64, &est) == MC_ERR_INVALID_ARG);
  CHECK(mc_transcode_estimate_bytes(1000, 0, &est) == MC_ERR_INVALID_ARG);
  CHECK(mc_opus_bitrates[0] == 256 && mc_opus_bitrates[MC_OPUS_BITRATE_COUNT - 1] == 64);

  check_transcode(dir);

  int score = 0;
  CHECK(mc_transcode_benchmark(&score) == MC_OK);
  CHECK(score > 0);
  printf("benchmark score %d\n", score);

  return TEST_RESULT();
}
