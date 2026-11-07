#include "kithipod/encoder.h"

#include <string.h>

#include <lame.h>

#include "alac_shim.h"
#include "id3.h"
#include "m4a.h"
#include "util.h"

static const char *const mp3_settings[] = {"v0", "v2", "320", "256", "192", "128"};
static const ipod_format_desc formats[] = {
    {"alac", "m4a", 1, NULL, 0, NULL},
    {"mp3", "mp3", 0, mp3_settings, sizeof mp3_settings / sizeof mp3_settings[0], "v0"},
};

const ipod_format_desc *ipod_encoder_formats(size_t *n) {
  *n = sizeof formats / sizeof formats[0];
  return formats;
}

int ipod_enc_settings_parse(const char *format, const char *setting, ipod_enc_settings *out) {
  memset(out, 0, sizeof *out);
  if (!format) return IPOD_ERR_ARG;
  if (strcmp(format, "alac") == 0) {
    out->codec = IPOD_CODEC_ALAC;
    return 0;
  }
  if (strcmp(format, "mp3") != 0) return IPOD_ERR_ARG;
  out->codec = IPOD_CODEC_MP3;
  if (!setting) setting = "v0";
  if ((setting[0] == 'v' || setting[0] == 'V') && setting[1] >= '0' && setting[1] <= '9' && !setting[2]) {
    out->mp3_vbr = 1;
    out->mp3_vbr_quality = setting[1] - '0';
    return 0;
  }
  static const int rates[] = {320, 256, 224, 192, 160, 128};
  int kbps = atoi(setting);
  for (size_t i = 0; i < sizeof rates / sizeof rates[0]; i++) {
    if (kbps == rates[i]) {
      out->mp3_bitrate = kbps;
      return 0;
    }
  }
  return IPOD_ERR_ARG;
}

/* --- Tags, copied --------------------------------------------------------------------- */

typedef struct tags_copy {
  ipod_tags t;
  char *s[13];
  uint8_t *cover;
} tags_copy;

static int tags_dup(tags_copy *c, const ipod_tags *src) {
  memset(c, 0, sizeof *c);
  if (!src) return 0;
  c->t = *src;
  const char **f[13] = {&c->t.title, &c->t.artist, &c->t.album, &c->t.album_artist, &c->t.genre,
                        &c->t.composer, &c->t.comment, &c->t.grouping, &c->t.sort_title, &c->t.sort_artist,
                        &c->t.sort_album, &c->t.sort_album_artist, &c->t.sort_composer};
  for (int i = 0; i < 13; i++) {
    if (!*f[i]) continue;
    if (!(c->s[i] = ipod_strdup(*f[i]))) return IPOD_ERR_NOMEM;
    *f[i] = c->s[i];
  }
  if (src->cover && src->cover_len) {
    if (!(c->cover = malloc(src->cover_len))) return IPOD_ERR_NOMEM;
    memcpy(c->cover, src->cover, src->cover_len);
    c->t.cover = c->cover;
  }
  return 0;
}

static void tags_release(tags_copy *c) {
  for (int i = 0; i < 13; i++) free(c->s[i]);
  free(c->cover);
  memset(c, 0, sizeof *c);
}

/* --- The encoder ---------------------------------------------------------------------------- */

struct ipod_encoder {
  ipod_codec codec;
  ipod_pcm_format fmt;
  char *path;
  uint64_t frames;
  size_t bytes_per_frame;

  /* ALAC */
  ipod_alac_enc *alac;
  ipod_m4a *m4a;
  uint8_t *pending; /* PCM waiting for a full packet */
  uint32_t n_pending;
  uint8_t *packet;
  tags_copy tags;

  /* MP3 */
  lame_global_flags *lame;
  FILE *out;
  long audio_start;
  uint64_t audio_bytes;
  uint8_t *mp3buf;
  size_t mp3buf_size;
  int32_t *wide; /* 24-bit samples scaled to 32 bits for LAME */
  size_t wide_cap;
};

static void encoder_free(ipod_encoder *e) {
  ipod_alac_enc_free(e->alac);
  free(e->pending);
  free(e->packet);
  tags_release(&e->tags);
  if (e->lame) lame_close(e->lame);
  if (e->out) fclose(e->out);
  free(e->mp3buf);
  free(e->wide);
  free(e->path);
  free(e);
}

void ipod_encoder_abort(ipod_encoder *e) {
  if (!e) return;
  if (e->m4a) ipod_m4a_abort(e->m4a);
  e->m4a = NULL;
  if (e->out) {
    fclose(e->out);
    e->out = NULL;
  }
  if (e->path && e->codec == IPOD_CODEC_MP3) ipod_remove(e->path);
  encoder_free(e);
}

static int open_alac(ipod_encoder *e, const ipod_tags *tags, ipod_error *err) {
  e->alac = ipod_alac_enc_new(e->fmt.sample_rate, e->fmt.channels, e->fmt.bits);
  if (!e->alac) return ipod_seterr(err, IPOD_ERR_ENCODER, "the ALAC encoder refused the format");
  uint8_t cookie[64];
  uint32_t clen = ipod_alac_enc_cookie(e->alac, cookie, sizeof cookie);
  if (!clen) return ipod_seterr(err, IPOD_ERR_ENCODER, "no ALAC magic cookie");
  e->pending = malloc(IPOD_ALAC_FRAMES_PER_PACKET * e->bytes_per_frame);
  e->packet = malloc(ipod_alac_enc_max_packet(e->alac));
  if (!e->pending || !e->packet || tags_dup(&e->tags, tags) != 0)
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  e->m4a = ipod_m4a_open(e->path, e->fmt.sample_rate, e->fmt.channels, e->fmt.bits, cookie, clen, err);
  return e->m4a ? 0 : (err ? err->code : IPOD_ERR_IO);
}

static int open_mp3(ipod_encoder *e, const ipod_enc_settings *s, const ipod_tags *tags, ipod_error *err) {
  int r = 0;
  /* lame_init and lame_init_params fill tables shared by every encoder. */
  ipod_global_lock();
  e->lame = lame_init();
  if (e->lame) {
    lame_set_in_samplerate(e->lame, (int)e->fmt.sample_rate);
    lame_set_out_samplerate(e->lame, (int)e->fmt.sample_rate);
    lame_set_num_channels(e->lame, e->fmt.channels);
    lame_set_mode(e->lame, e->fmt.channels == 1 ? MONO : JOINT_STEREO);
    lame_set_quality(e->lame, 2);
    lame_set_write_id3tag_automatic(e->lame, 0);
    lame_set_bWriteVbrTag(e->lame, 1);
    if (s->mp3_vbr) {
      lame_set_VBR(e->lame, vbr_default);
      lame_set_VBR_q(e->lame, s->mp3_vbr_quality < 0 ? 0 : s->mp3_vbr_quality > 9 ? 9 : s->mp3_vbr_quality);
    } else {
      lame_set_VBR(e->lame, vbr_off);
      lame_set_brate(e->lame, s->mp3_bitrate ? s->mp3_bitrate : 320);
    }
    r = lame_init_params(e->lame);
  }
  ipod_global_unlock();
  if (!e->lame || r < 0) return ipod_seterr(err, IPOD_ERR_ENCODER, "LAME refused the settings");

  e->out = ipod_fopen(e->path, "wb");
  if (!e->out) return ipod_seterr(err, IPOD_ERR_IO, "can't create %s", e->path);
  ipod_buf tag = {0};
  if (tags) ipod_id3_build(&tag, tags);
  if (tag.oom) {
    ipod_buf_free(&tag);
    return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  }
  int ok = tag.len == 0 || fwrite(tag.data, 1, tag.len, e->out) == tag.len;
  e->audio_start = (long)tag.len;
  ipod_buf_free(&tag);
  if (!ok) return ipod_seterr(err, IPOD_ERR_FULL, "can't write %s", e->path);
  e->mp3buf_size = 16384 + 7200;
  e->mp3buf = malloc(e->mp3buf_size);
  if (!e->mp3buf) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
  return 0;
}

ipod_encoder *ipod_encoder_open(const char *out_path, const ipod_enc_settings *settings,
                                const ipod_pcm_format *format, const ipod_tags *tags, ipod_error *err) {
  if (!settings || !format || (format->channels != 1 && format->channels != 2) ||
      (format->bits != 16 && format->bits != 24) || format->sample_rate < 8000 || format->sample_rate > 192000) {
    ipod_seterr(err, IPOD_ERR_ARG, "unsupported PCM format");
    return NULL;
  }
  if (settings->codec == IPOD_CODEC_MP3 && format->sample_rate > 48000) {
    ipod_seterr(err, IPOD_ERR_ARG, "MP3 needs 48 kHz or less");
    return NULL;
  }
  ipod_encoder *e = calloc(1, sizeof *e);
  if (!e || !(e->path = ipod_strdup(out_path))) {
    free(e);
    ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
    return NULL;
  }
  e->codec = settings->codec;
  e->fmt = *format;
  e->bytes_per_frame = (size_t)format->channels * (format->bits == 16 ? 2 : 4);
  int r = e->codec == IPOD_CODEC_ALAC ? open_alac(e, tags, err) : open_mp3(e, settings, tags, err);
  if (r != 0) {
    ipod_encoder_abort(e);
    return NULL;
  }
  return e;
}

static int alac_flush_packet(ipod_encoder *e, ipod_error *err) {
  uint32_t n = ipod_alac_enc_packet(e->alac, e->pending, e->n_pending, e->packet);
  if (!n) return ipod_seterr(err, IPOD_ERR_ENCODER, "ALAC encoding failed");
  e->audio_bytes += n;
  int r = ipod_m4a_packet(e->m4a, e->packet, n, e->n_pending, err);
  e->n_pending = 0;
  return r;
}

static int mp3_out(ipod_encoder *e, int n, ipod_error *err) {
  if (n < 0) return ipod_seterr(err, IPOD_ERR_ENCODER, "LAME failed (%d)", n);
  if (n > 0 && fwrite(e->mp3buf, 1, (size_t)n, e->out) != (size_t)n)
    return ipod_seterr(err, IPOD_ERR_FULL, "can't write %s", e->path);
  e->audio_bytes += (uint64_t)n;
  return 0;
}

int ipod_encoder_write(ipod_encoder *e, const void *frames, size_t n_frames, ipod_error *err) {
  const uint8_t *p = frames;
  e->frames += n_frames;
  if (e->codec == IPOD_CODEC_ALAC) {
    while (n_frames) {
      uint32_t take = IPOD_ALAC_FRAMES_PER_PACKET - e->n_pending;
      if (take > n_frames) take = (uint32_t)n_frames;
      memcpy(e->pending + e->n_pending * e->bytes_per_frame, p, take * e->bytes_per_frame);
      e->n_pending += take;
      p += take * e->bytes_per_frame;
      n_frames -= take;
      if (e->n_pending == IPOD_ALAC_FRAMES_PER_PACKET) {
        int r = alac_flush_packet(e, err);
        if (r != 0) return r;
      }
    }
    return 0;
  }
  /* LAME, in slices that fit mp3buf (1.25 * frames + 7200 bytes at most). */
  while (n_frames) {
    size_t take = n_frames > 8192 ? 8192 : n_frames;
    int n;
    if (e->fmt.bits == 16) {
      const short *s = (const short *)p;
      if (e->fmt.channels == 2)
        n = lame_encode_buffer_interleaved(e->lame, (short *)s, (int)take, e->mp3buf, (int)e->mp3buf_size);
      else
        n = lame_encode_buffer(e->lame, s, s, (int)take, e->mp3buf, (int)e->mp3buf_size);
    } else {
      size_t samples = take * e->fmt.channels;
      if (e->wide_cap < samples) {
        int32_t *w = realloc(e->wide, samples * sizeof *w);
        if (!w) return ipod_seterr(err, IPOD_ERR_NOMEM, "out of memory");
        e->wide = w;
        e->wide_cap = samples;
      }
      const int32_t *s = (const int32_t *)p;
      for (size_t i = 0; i < samples; i++) e->wide[i] = (int32_t)((uint32_t)s[i] << 8);
      if (e->fmt.channels == 2)
        n = lame_encode_buffer_interleaved_int(e->lame, e->wide, (int)take, e->mp3buf, (int)e->mp3buf_size);
      else
        n = lame_encode_buffer_int(e->lame, e->wide, e->wide, (int)take, e->mp3buf, (int)e->mp3buf_size);
    }
    int r = mp3_out(e, n, err);
    if (r != 0) return r;
    p += take * e->bytes_per_frame;
    n_frames -= take;
  }
  return 0;
}

int ipod_encoder_finish(ipod_encoder *e, ipod_enc_result *res, ipod_error *err) {
  ipod_enc_result out = {0};
  out.frames = e->frames;
  out.sample_rate = e->fmt.sample_rate;
  out.length_ms = (uint32_t)(e->frames * 1000 / e->fmt.sample_rate);
  uint64_t audio = 0;
  int r = 0;
  if (e->codec == IPOD_CODEC_ALAC) {
    if (e->n_pending && (r = alac_flush_packet(e, err)) != 0) {
      ipod_encoder_abort(e);
      return r;
    }
    ipod_m4a *m = e->m4a;
    e->m4a = NULL;
    r = ipod_m4a_finish(m, &e->tags.t, &out.file_size, err);
    if (r != 0) {
      encoder_free(e);
      return r;
    }
    audio = e->audio_bytes;
  } else {
    r = mp3_out(e, lame_encode_flush(e->lame, e->mp3buf, (int)e->mp3buf_size), err);
    if (r == 0) {
      /* The Xing/LAME frame goes over the placeholder LAME wrote first. */
      size_t n = lame_get_lametag_frame(e->lame, e->mp3buf, e->mp3buf_size);
      if (n > 0 && n <= e->mp3buf_size) {
        if (fseek(e->out, e->audio_start, SEEK_SET) != 0 || fwrite(e->mp3buf, 1, n, e->out) != n)
          r = ipod_seterr(err, IPOD_ERR_IO, "can't write the LAME tag");
      }
    }
    if (r == 0 && fclose(e->out) != 0) r = ipod_seterr(err, IPOD_ERR_FULL, "can't write %s", e->path);
    e->out = NULL;
    if (r != 0) {
      ipod_encoder_abort(e);
      return r;
    }
    audio = e->audio_bytes;
    out.file_size = (uint64_t)e->audio_start + e->audio_bytes;
  }
  if (out.length_ms) out.bitrate_kbps = (uint32_t)(audio * 8 / out.length_ms);
  if (res) *res = out;
  encoder_free(e);
  return 0;
}
