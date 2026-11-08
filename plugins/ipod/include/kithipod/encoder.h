/* ALAC (Apple's reference encoder, in an M4A) and MP3 (LAME, with an ID3v2.3 tag).
 *
 * Push interleaved PCM in chunks of any size; the host decodes, resamples and dithers to a
 * format the device takes first (for the iPod: 16-bit, 44.1 or 48 kHz, mono or stereo;
 * the encoders take 24-bit too). Each ipod_encoder is independent: different threads may
 * encode different files at once. */
#ifndef KITHIPOD_ENCODER_H
#define KITHIPOD_ENCODER_H

#include "kithipod/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum ipod_codec { IPOD_CODEC_ALAC = 0, IPOD_CODEC_MP3 = 1 } ipod_codec;

typedef struct ipod_enc_settings {
  ipod_codec codec;
  int mp3_vbr;          /* 1: VBR at mp3_vbr_quality; 0: CBR at mp3_bitrate */
  int mp3_vbr_quality;  /* LAME -V: 0 (best) to 9 */
  int mp3_bitrate;      /* kbps, for CBR */
} ipod_enc_settings;

/* What the plugin's `formats` callback declares. */
typedef struct ipod_format_desc {
  const char *id;         /* "alac", "mp3" */
  const char *extension;  /* "m4a", "mp3" */
  int lossless;
  const char *const *settings; /* "v0", "v2", "320", "256", "192", "128"; none for ALAC */
  size_t n_settings;
  const char *default_setting;  /* NULL for ALAC */
} ipod_format_desc;

const ipod_format_desc *ipod_encoder_formats(size_t *n);
/* "alac" + NULL, "mp3" + "v0" / "v2" / "320" ... Returns IPOD_ERR_ARG for anything else. */
int ipod_enc_settings_parse(const char *format, const char *setting, ipod_enc_settings *out);

typedef struct ipod_encoder ipod_encoder;

/* Starts writing out_path (overwritten). tags (and its cover) are copied into the file;
 * they need only live through this call. */
ipod_encoder *ipod_encoder_open(const char *out_path, const ipod_enc_settings *settings,
                                const ipod_pcm_format *format, const ipod_tags *tags, ipod_error *err);
int ipod_encoder_write(ipod_encoder *enc, const void *frames, size_t n_frames, ipod_error *err);

typedef struct ipod_enc_result {
  uint64_t frames;
  uint32_t sample_rate;
  uint32_t length_ms;
  uint32_t bitrate_kbps; /* average, over the audio data */
  uint64_t file_size;
} ipod_enc_result;

/* Finishes the file and frees enc (also on failure, after deleting the output). */
int ipod_encoder_finish(ipod_encoder *enc, ipod_enc_result *result, ipod_error *err);
/* Stops, deletes the output and frees enc. */
void ipod_encoder_abort(ipod_encoder *enc);

#ifdef __cplusplus
}
#endif

#endif
