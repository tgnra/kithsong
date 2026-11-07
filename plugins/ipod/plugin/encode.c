/* The plugin's encoder: ALAC (Apple's reference encoder, in an M4A) and MP3 (LAME), over
 * kithipod's encoders, fed from core's decoded source. Safe on several threads at once: all
 * state is per call. */
#include <stdlib.h>
#include <string.h>

#include "kithipod/encoder.h"
#include "plugin_internal.h"

#define CHUNK_FRAMES 4096

static kith_encoder_setting mp3_settings[6];
static kith_encoder_format formats[2];

static void to_ipod_tags(const kith_tags *k, ipod_tags *t) {
  memset(t, 0, sizeof *t);
  t->title = k->title;
  t->artist = k->artist;
  t->album = k->album;
  t->album_artist = k->album_artist;
  t->genre = k->genre;
  t->composer = k->composer;
  t->year = k->year;
  t->track_number = k->track_no;
  t->track_total = k->track_total;
  t->disc_number = k->disc_no;
  t->disc_total = k->disc_total;
  t->compilation = k->compilation ? 1 : 0;
}

static int encode(kith_source *src, const char *format, const char *setting, const char *out_path,
                  const kith_job *job) {
  const kith_host *h = ipod_plugin_host;
  ipod_enc_settings es;
  if (!src || !format || !out_path ||
      ipod_enc_settings_parse(format, setting && *setting ? setting : NULL, &es) != 0)
    return KITH_ERR_INVALID_ARG;

  /* The format: 16-bit, or 24 for ALAC when the source has more than 16 bits and nothing
   * caps it (the iPod's limits, applied by core, do); two channels at most; MP3 at 48 kHz
   * at most. */
  kith_source_info si;
  int rc = h->decode_info(src, &si);
  if (rc != KITH_OK) return rc;
  kith_pcm_format want;
  memset(&want, 0, sizeof want);
  int wide = es.codec == IPOD_CODEC_ALAC && si.format.bits > 16;
  want.sample = wide ? KITH_SAMPLE_S32 : KITH_SAMPLE_S16;
  want.bits = wide ? 24 : 16;
  want.max_channels = 2;
  if (es.codec == IPOD_CODEC_MP3) want.max_sample_rate = 48000;
  if ((rc = h->decode_set_format(src, &want)) != KITH_OK) return rc;
  if ((rc = h->decode_info(src, &si)) != KITH_OK) return rc;
  wide = si.format.sample == KITH_SAMPLE_S32;
  if (si.format.channels < 1 || si.format.channels > 2 || si.format.sample_rate <= 0) return KITH_ERR_FORMAT;
  ipod_pcm_format pf;
  pf.sample_rate = (uint32_t)si.format.sample_rate;
  pf.channels = (uint16_t)si.format.channels;
  pf.bits = (uint16_t)(wide ? 24 : 16);

  kith_tags kt;
  ipod_tags tags;
  memset(&kt, 0, sizeof kt);
  if (h->decode_tags(src, &kt) != KITH_OK) memset(&kt, 0, sizeof kt);
  to_ipod_tags(&kt, &tags);
  kith_cover cover;
  if (h->decode_cover(src, &cover) == KITH_OK && cover.data && cover.size) {
    tags.cover = cover.data;
    tags.cover_len = cover.size;
  }

  ipod_error err = {0};
  ipod_encoder *enc = ipod_encoder_open(out_path, &es, &pf, &tags, &err);
  if (!enc) {
    ipod_plugin_log(KITH_LOG_ERROR, "can't start encoding %s: %s", out_path, err.message);
    return err.code == IPOD_ERR_NOMEM ? KITH_ERR_NOMEM : KITH_ERR_IO;
  }
  size_t frame_bytes = (size_t)pf.channels * (wide ? 4 : 2);
  void *buf = malloc(CHUNK_FRAMES * frame_bytes);
  if (!buf) {
    ipod_encoder_abort(enc);
    return KITH_ERR_NOMEM;
  }
  rc = KITH_OK;
  for (;;) {
    if (h->cancelled(job)) {
      rc = KITH_ERR_CANCELLED;
      break;
    }
    int64_t n = h->decode_read(src, buf, CHUNK_FRAMES);
    if (n < 0) {
      rc = (int)n;
      break;
    }
    if (n == 0) break;
    if (wide) { /* core's S32 holds the sample in the top bits; the encoder takes it at the bottom */
      int32_t *s = buf;
      for (int64_t i = 0; i < n * pf.channels; i++) s[i] /= 256;
    }
    if (ipod_encoder_write(enc, buf, (size_t)n, &err) != 0) {
      ipod_plugin_log(KITH_LOG_ERROR, "encoding %s failed: %s", out_path, err.message);
      rc = KITH_ERR_IO;
      break;
    }
  }
  free(buf);
  if (rc != KITH_OK) {
    ipod_encoder_abort(enc);
    return rc;
  }
  if (ipod_encoder_finish(enc, NULL, &err) != 0) {
    ipod_plugin_log(KITH_LOG_ERROR, "finishing %s failed: %s", out_path, err.message);
    return err.code == IPOD_ERR_NOMEM ? KITH_ERR_NOMEM : KITH_ERR_IO;
  }
  return KITH_OK;
}

void ipod_plugin_encoder_init(const ipod_plugin_strings *s, kith_encoder *out) {
  static const char *const ids[6] = {"v0", "v2", "320", "256", "192", "128"};
  static const int kbps[6] = {245, 190, 320, 256, 192, 128};
  const char *labels[6] = {s->mp3_v0, s->mp3_v2, s->mp3_320, s->mp3_256, s->mp3_192, s->mp3_128};
  for (int i = 0; i < 6; i++) {
    mp3_settings[i].id = ids[i];
    mp3_settings[i].label = labels[i];
    mp3_settings[i].kbps = kbps[i];
  }
  memset(formats, 0, sizeof formats);
  formats[0].id = "alac";
  formats[0].name = "ALAC";
  formats[0].extension = "m4a";
  formats[0].lossless = 1;
  formats[0].input.max_channels = 2;
  formats[1].id = "mp3";
  formats[1].name = "MP3";
  formats[1].extension = "mp3";
  formats[1].lossless = 0;
  formats[1].settings = mp3_settings;
  formats[1].setting_count = 6;
  formats[1].default_setting = 0;
  formats[1].input.sample = KITH_SAMPLE_S16;
  formats[1].input.bits = 16;
  formats[1].input.max_channels = 2;
  formats[1].input.max_sample_rate = 48000;
  out->formats = formats;
  out->format_count = 2;
  out->encode = encode;
}
