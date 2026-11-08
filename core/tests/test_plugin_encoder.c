/* A plugin's encoder through the transcode cache (kithcore/transcode.h with
 * MC_FORMAT_ENCODER), with the test plugin's WAV encoder (plugin_folder.c):
 *   - a 24-bit 96 kHz source for a device capped at 16 bits and 48 kHz comes out at those,
 *     the length kept and a tone's level too; it's named for its limits, with a sidecar
 *     whose hash is the file's, and a second ask is a cache hit (the source can be gone);
 *   - a 16-bit source within the limits comes out sample for sample, undithered;
 *   - six channels are downmixed to the two the device takes;
 *   - settings and unknown encoders are checked;
 *   - the encode pool encodes it too, and the trim takes file and sidecar together. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/sha256.h>

#include "kithcore/kithcore.h"
#include "sync/encode.h"
#include "test.h"
#include "transcode/transcode_internal.h"
#include "util/platform.h"

#define UID "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6e"
#define UID2 "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d6f"
#define UID3 "0192f1e4-7c3a-7b21-9d4e-1f2a3b4c5d70"

static void put16(FILE *f, unsigned v) {
  fputc((int)(v & 0xff), f);
  fputc((int)(v >> 8 & 0xff), f);
}

static void put32(FILE *f, uint32_t v) {
  put16(f, v & 0xffff);
  put16(f, v >> 16);
}

/* A PCM WAV of a 1 kHz tone at half scale: bits 16 or 24. */
static int write_wav(const char *path, unsigned rate, unsigned channels, unsigned bits, unsigned ms) {
  FILE *f = mc_fopen(path, "wb");
  if (!f) return 0;
  uint32_t frames = rate * ms / 1000, bytes = frames * channels * (bits / 8);
  fwrite("RIFF", 1, 4, f);
  put32(f, 36 + bytes);
  fwrite("WAVEfmt ", 1, 8, f);
  put32(f, 16);
  put16(f, 1);
  put16(f, channels);
  put32(f, rate);
  put32(f, rate * channels * (bits / 8));
  put16(f, channels * (bits / 8));
  put16(f, bits);
  fwrite("data", 1, 4, f);
  put32(f, bytes);
  for (uint32_t i = 0; i < frames; i++)
    for (unsigned c = 0; c < channels; c++) {
      double v = 0.5 * sin(2 * 3.14159265358979 * 1000.0 * i / rate);
      if (bits == 16) {
        put16(f, (unsigned)(uint16_t)(int16_t)lrint(v * 32767));
      } else {
        uint32_t s = (uint32_t)(int32_t)lrint(v * 8388607);
        fputc((int)(s & 0xff), f);
        fputc((int)(s >> 8 & 0xff), f);
        fputc((int)(s >> 16 & 0xff), f);
      }
    }
  return fclose(f) == 0;
}

typedef struct wav {
  unsigned channels, rate, bits;
  uint32_t data_bytes;
  unsigned char *data;
} wav;

static unsigned get16(const unsigned char *p) {
  return p[0] | (unsigned)p[1] << 8;
}

static uint32_t get32(const unsigned char *p) {
  return get16(p) | (uint32_t)get16(p + 2) << 16;
}

/* Reads a canonical 44-byte-header WAV (as both writers here make). */
static int read_wav(const char *path, wav *w) {
  memset(w, 0, sizeof *w);
  FILE *f = mc_fopen(path, "rb");
  if (!f) return 0;
  unsigned char h[44];
  int ok = fread(h, 1, 44, f) == 44 && !memcmp(h, "RIFF", 4) && !memcmp(h + 36, "data", 4);
  if (ok) {
    w->channels = get16(h + 22);
    w->rate = get32(h + 24);
    w->bits = get16(h + 34);
    w->data_bytes = get32(h + 40);
    w->data = malloc(w->data_bytes ? w->data_bytes : 1);
    ok = w->data && fread(w->data, 1, w->data_bytes, f) == w->data_bytes;
  }
  fclose(f);
  return ok;
}

static void file_sha(const char *path, char out[65]) {
  out[0] = '\0';
  FILE *f = mc_fopen(path, "rb");
  if (!f) return;
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  unsigned char buf[8192], digest[32];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) mbedtls_sha256_update(&ctx, buf, n);
  fclose(f);
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
}

static int ends_with(const char *s, const char *suffix) {
  size_t ls = strlen(s), lx = strlen(suffix);
  return ls >= lx && !strcmp(s + ls - lx, suffix);
}

static mc_transcode_opts encoder(const char *id, const char *setting, int rate, int bits, int ch) {
  mc_transcode_opts o;
  memset(&o, 0, sizeof o);
  o.format = MC_FORMAT_ENCODER;
  snprintf(o.encoder, sizeof o.encoder, "%s", id);
  snprintf(o.setting, sizeof o.setting, "%s", setting);
  o.max_sample_rate = rate;
  o.max_bits = bits;
  o.max_channels = ch;
  return o;
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  char base[1024], dir[1024], cfg[1024], cache[1024], src[1024], src16[1024], src6[1024];
  snprintf(base, sizeof base, "%s/plugin_encoder", argv[1]);
  snprintf(dir, sizeof dir, "%s/plugins", argv[1]);
  snprintf(cfg, sizeof cfg, "%s/cfg", base);
  snprintf(cache, sizeof cache, "%s/cache", base);
  snprintf(src, sizeof src, "%s/hires.wav", base);
  snprintf(src16, sizeof src16, "%s/cd.wav", base);
  snprintf(src6, sizeof src6, "%s/surround.wav", base);
  mc_remove_tree(base);
  CHECK(mc_mkdirs(base) == MC_OK);
  CHECK(write_wav(src, 96000, 2, 24, 1000));
  CHECK(write_wav(src16, 44100, 2, 16, 500));
  CHECK(write_wav(src6, 48000, 6, 16, 500));

  mc_settings *s = NULL;
  CHECK(mc_settings_load(cfg, &s) == MC_OK);
  const char *dirs[] = {dir};
  CHECK(mc_plugins_load(s, dirs, 1) == MC_OK);

  /* Off: no such encoder. */
  mc_transcode_opts capped = encoder("wav", "", 48000, 16, 2);
  char *out = NULL;
  CHECK(mc_transcode_cached(cache, UID, src, &capped, &out) == MC_ERR_NOT_FOUND && !out);
  CHECK(mc_plugins_set_enabled("kith_test_folder", 1) == MC_OK);

  /* Capped to 16 bits and 48 kHz, named for it. */
  CHECK(mc_transcode_cached(cache, UID, src, &capped, &out) == MC_OK && out);
  CHECK(out && ends_with(out, "wav-max16b48000r2c.wav"));
  wav w;
  CHECK(out && read_wav(out, &w));
  CHECK(w.channels == 2 && w.rate == 48000 && w.bits == 16);
  CHECK(w.data_bytes == 48000 * 2 * 2); /* the second kept, frame for frame */
  if (w.data && w.data_bytes == 48000 * 4) {
    /* The tone's level survives: RMS of 0.5 × sine is 0.354 of full scale. */
    double sum = 0;
    for (uint32_t i = 4800; i < 48000 - 4800; i++) {
      double x = (int16_t)get16(w.data + i * 4) / 32768.0;
      sum += x * x;
    }
    double rms = sqrt(sum / (48000 - 9600));
    CHECK(rms > 0.34 && rms < 0.37);
  }
  free(w.data);
  char sha[65], side[65];
  int64_t size = 0, mtime = 0, fsize = 0, fmtime = 0;
  file_sha(out, sha);
  CHECK(mc_transcode_sha_read(out, side, &size, &mtime) == MC_OK && !strcmp(sha, side));
  CHECK(mc_file_info(out, &fsize, &fmtime) == MC_OK && fsize == size && fmtime == mtime);

  /* A hit doesn't encode: the source can be gone. */
  char moved[1024];
  snprintf(moved, sizeof moved, "%s.away", src);
  CHECK(mc_rename(src, moved, 0) == MC_OK);
  char *again = NULL;
  CHECK(mc_transcode_cached(cache, UID, src, &capped, &again) == MC_OK && again &&
        !strcmp(again, out));
  mc_free(again);
  CHECK(mc_rename(moved, src, 0) == MC_OK);
  char *path = mc_transcode_cache_path(cache, UID, &capped);
  CHECK(path && !strcmp(path, out));
  mc_free(path);
  mc_free(out);

  /* Within the limits: the same samples, undithered. */
  mc_transcode_opts plain = encoder("wav", "", 0, 0, 0);
  CHECK(mc_transcode_cached(cache, UID2, src16, &plain, &out) == MC_OK && out);
  CHECK(out && ends_with(out, "wav.wav") && strstr(out, UID2));
  wav a, b;
  CHECK(out && read_wav(out, &a) && read_wav(src16, &b));
  CHECK(a.channels == 2 && a.rate == 44100 && a.bits == 16 && a.data_bytes == b.data_bytes &&
        a.data && b.data && !memcmp(a.data, b.data, a.data_bytes));
  free(a.data);
  free(b.data);
  mc_free(out);

  /* Six channels down to two, through the pool. */
  mc_encode_pool *pool = NULL;
  CHECK(mc_encode_pool_new(cache, (int64_t)1 << 30, &pool) == MC_OK);
  mc_transcode_opts stereo = encoder("lofi", "q2", 0, 0, 2);
  CHECK(mc_encode_pool_queue_as(pool, UID3, src6, &stereo, MC_ENCODE_AHEAD, pool) == MC_OK);
  CHECK(mc_encode_pool_encode_as(pool, UID3, src6, &stereo, NULL, NULL, &out) == MC_OK && out);
  CHECK(out && ends_with(out, "lofi-q2-max0b0r2c.lofi"));
  CHECK(out && read_wav(out, &w) && w.channels == 2 && w.rate == 48000 &&
        w.data_bytes == 24000 * 2 * 2);
  free(w.data);
  mc_free(out);
  mc_encode_stats stats;
  mc_encode_pool_stats(pool, &stats);
  CHECK(stats.encoded == 1);
  mc_encode_pool_free(pool);

  /* What's checked. */
  mc_transcode_opts odd = encoder("lofi", "zz", 0, 0, 0);
  CHECK(mc_transcode_cached(cache, UID, src, &odd, &out) == MC_ERR_INVALID_ARG && !out);
  odd = encoder("wav", "q1", 0, 0, 0);
  CHECK(mc_transcode_cached(cache, UID, src, &odd, &out) == MC_ERR_INVALID_ARG);
  odd = encoder("lofi", "", 0, 0, 0);
  CHECK(mc_transcode_cached(cache, UID, src, &odd, &out) == MC_ERR_INVALID_ARG);
  odd = encoder("nope", "", 0, 0, 0);
  CHECK(mc_transcode_cached(cache, UID, src, &odd, &out) == MC_ERR_NOT_FOUND);
  odd = encoder("../x", "", 0, 0, 0);
  CHECK(mc_transcode_cached(cache, UID, src, &odd, &out) == MC_ERR_INVALID_ARG);

  /* The trim takes them all, sidecars with them. */
  CHECK(mc_transcode_cache_trim(cache, 0) == MC_OK);
  path = mc_transcode_cache_path(cache, UID, &capped);
  char side_path[1100];
  snprintf(side_path, sizeof side_path, "%s", path ? path : "");
  char *dot = strrchr(side_path, '.');
  if (dot) snprintf(dot, sizeof side_path - (size_t)(dot - side_path), ".sha256");
  CHECK(path && !mc_file_exists(path) && !mc_file_exists(side_path));
  mc_free(path);

  mc_plugins_unload();
  mc_settings_free(s);
  mc_remove_tree(base);
  return TEST_RESULT();
}
