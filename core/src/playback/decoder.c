#include "playback/decoder.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "opusfile.h"
#include "util/platform.h"

typedef enum format { FMT_NONE, FMT_FLAC, FMT_MP3, FMT_WAV, FMT_OPUS } format;

struct mc_decoder {
  format fmt;
  drflac *flac;
  drmp3 mp3;
  drwav wav;
  OggOpusFile *opus;
  unsigned channels, rate;
  uint64_t length;
};

static format format_of(const char *path) {
  static const struct {
    const char *ext;
    format fmt;
  } known[] = {{"flac", FMT_FLAC}, {"mp3", FMT_MP3},   {"wav", FMT_WAV},
               {"aif", FMT_WAV},   {"aiff", FMT_WAV}, {"opus", FMT_OPUS}};
  const char *dot = strrchr(path, '.');
  if (!dot || strchr(dot, '/') || strchr(dot, '\\')) return FMT_NONE;
  for (size_t i = 0; i < sizeof known / sizeof known[0]; i++) {
    const char *a = dot + 1, *b = known[i].ext;
    while (*a && (*a | 0x20) == *b) a++, b++;
    if (!*a && !*b) return known[i].fmt;
  }
  return FMT_NONE;
}

int mc_decoder_supports(const char *path) {
  return path && format_of(path) != FMT_NONE;
}

mc_status mc_decoder_open(const char *path, mc_decoder **out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  format fmt = format_of(path);
  if (fmt == FMT_NONE) return MC_ERR_FORMAT;
  mc_decoder *d = calloc(1, sizeof *d);
  if (!d) return MC_ERR_NOMEM;
  int ok = 0;
#ifdef _WIN32
  wchar_t *wpath = mc_win_path(path);
#define OPEN_PATH wpath
#define OPEN(fn) fn##_w
#else
#define OPEN_PATH path
#define OPEN(fn) fn
#endif
  if (OPEN_PATH) {
    switch (fmt) {
      case FMT_FLAC:
        if ((d->flac = OPEN(drflac_open_file)(OPEN_PATH, NULL))) {
          ok = 1;
          d->channels = d->flac->channels;
          d->rate = d->flac->sampleRate;
          d->length = d->flac->totalPCMFrameCount;
        }
        break;
      case FMT_MP3:
        if ((ok = OPEN(drmp3_init_file)(&d->mp3, OPEN_PATH, NULL))) {
          d->channels = d->mp3.channels;
          d->rate = d->mp3.sampleRate;
          /* Only from the Xing/Info header: without one, counting means decoding the file. */
          d->length = d->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX ? drmp3_get_pcm_frame_count(&d->mp3) : 0;
        }
        break;
      case FMT_WAV:
        if ((ok = OPEN(drwav_init_file)(&d->wav, OPEN_PATH, NULL))) {
          d->channels = d->wav.channels;
          d->rate = d->wav.sampleRate;
          d->length = d->wav.totalPCMFrameCount;
        }
        break;
      case FMT_OPUS: {
        /* opusfile takes UTF-8 paths on Windows too. It trims the pre-skip and end padding,
         * and always decodes at 48 kHz; more than two channels are downmixed for us. */
        char *opus_path = mc_open_path(path);
        d->opus = opus_path ? op_open_file(opus_path, NULL) : NULL;
        free(opus_path);
        if (d->opus) {
          ok = 1;
          d->channels = 2;
          d->rate = 48000;
          ogg_int64_t total = op_pcm_total(d->opus, -1);
          d->length = total > 0 ? (uint64_t)total : 0;
        }
        break;
      }
      case FMT_NONE:
        break;
    }
  }
#undef OPEN_PATH
#undef OPEN
#ifdef _WIN32
  free(wpath);
#endif
  d->fmt = fmt;
  if (!ok || !d->channels || !d->rate) {
    if (ok) mc_decoder_close(d);
    else free(d);
    return MC_ERR_IO;
  }
  *out = d;
  return MC_OK;
}

void mc_decoder_close(mc_decoder *d) {
  if (!d) return;
  if (d->fmt == FMT_FLAC) drflac_close(d->flac);
  if (d->fmt == FMT_MP3) drmp3_uninit(&d->mp3);
  if (d->fmt == FMT_WAV) drwav_uninit(&d->wav);
  if (d->fmt == FMT_OPUS) op_free(d->opus);
  free(d);
}

unsigned mc_decoder_channels(const mc_decoder *d) {
  return d->channels;
}

unsigned mc_decoder_rate(const mc_decoder *d) {
  return d->rate;
}

uint64_t mc_decoder_length(const mc_decoder *d) {
  return d->length;
}

/* opusfile hands back at most a packet (up to 120 ms) per call, so this loops. */
static uint64_t read_opus(mc_decoder *d, float *out, uint64_t frames) {
  uint64_t done = 0;
  while (done < frames) {
    uint64_t want = frames - done;
    if (want > INT_MAX / 2) want = INT_MAX / 2;
    int got = op_read_float_stereo(d->opus, out + done * 2, (int)want * 2);
    if (got == OP_HOLE) continue; /* a gap in the data; decoding goes on after it */
    if (got <= 0) break;
    done += (uint64_t)got;
  }
  return done;
}

uint64_t mc_decoder_read(mc_decoder *d, float *out, uint64_t frames) {
  switch (d->fmt) {
    case FMT_FLAC: return drflac_read_pcm_frames_f32(d->flac, frames, out);
    case FMT_MP3: return drmp3_read_pcm_frames_f32(&d->mp3, frames, out);
    case FMT_WAV: return drwav_read_pcm_frames_f32(&d->wav, frames, out);
    case FMT_OPUS: return read_opus(d, out, frames);
    case FMT_NONE: break;
  }
  return 0;
}

void mc_decoder_seek(mc_decoder *d, uint64_t frame) {
  if (d->length && frame > d->length) frame = d->length;
  if (d->fmt == FMT_FLAC) drflac_seek_to_pcm_frame(d->flac, frame);
  if (d->fmt == FMT_MP3) drmp3_seek_to_pcm_frame(&d->mp3, frame);
  if (d->fmt == FMT_WAV) drwav_seek_to_pcm_frame(&d->wav, frame);
  if (d->fmt == FMT_OPUS) op_pcm_seek(d->opus, (ogg_int64_t)frame);
}
