#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L /* fseeko */
#endif

#include "playback/decoder.h"

#include <limits.h>
#include <stdio.h>
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
  FILE *file;
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

/* The decoders read through these rather than open the file themselves, so that it's opened
 * with mc_fopen_shared(): a track can be tagged or moved while it plays, on Windows too. */
static int file_seek(FILE *f, int64_t offset, int whence) {
#ifdef _WIN32
  return _fseeki64(f, offset, whence);
#else
  return fseeko(f, (off_t)offset, whence);
#endif
}

static int64_t file_tell(FILE *f) {
#ifdef _WIN32
  return _ftelli64(f);
#else
  return (int64_t)ftello(f);
#endif
}

static size_t file_read(void *user, void *out, size_t bytes) {
  return fread(out, 1, bytes, user);
}

static drflac_bool32 flac_seek(void *user, int offset, drflac_seek_origin origin) {
  int whence = origin == DRFLAC_SEEK_SET ? SEEK_SET : origin == DRFLAC_SEEK_CUR ? SEEK_CUR : SEEK_END;
  return file_seek(user, offset, whence) == 0;
}

static drflac_bool32 flac_tell(void *user, drflac_int64 *cursor) {
  *cursor = file_tell(user);
  return *cursor >= 0;
}

static drmp3_bool32 mp3_seek(void *user, int offset, drmp3_seek_origin origin) {
  int whence = origin == DRMP3_SEEK_SET ? SEEK_SET : origin == DRMP3_SEEK_CUR ? SEEK_CUR : SEEK_END;
  return file_seek(user, offset, whence) == 0;
}

static drmp3_bool32 mp3_tell(void *user, drmp3_int64 *cursor) {
  *cursor = file_tell(user);
  return *cursor >= 0;
}

static drwav_bool32 wav_seek(void *user, int offset, drwav_seek_origin origin) {
  int whence = origin == DRWAV_SEEK_SET ? SEEK_SET : origin == DRWAV_SEEK_CUR ? SEEK_CUR : SEEK_END;
  return file_seek(user, offset, whence) == 0;
}

static drwav_bool32 wav_tell(void *user, drwav_int64 *cursor) {
  *cursor = file_tell(user);
  return *cursor >= 0;
}

static int opus_read(void *user, unsigned char *out, int bytes) {
  size_t got = fread(out, 1, (size_t)bytes, user);
  return got == 0 && ferror((FILE *)user) ? -1 : (int)got;
}

static int opus_seek(void *user, opus_int64 offset, int whence) {
  return file_seek(user, offset, whence);
}

static opus_int64 opus_tell(void *user) {
  return file_tell(user);
}

mc_status mc_decoder_open(const char *path, mc_decoder **out) {
  if (!path || !out) return MC_ERR_INVALID_ARG;
  *out = NULL;
  format fmt = format_of(path);
  if (fmt == FMT_NONE) return MC_ERR_FORMAT;
  mc_decoder *d = calloc(1, sizeof *d);
  if (!d) return MC_ERR_NOMEM;
  int ok = 0;
  d->file = mc_fopen_shared(path);
  if (d->file) {
    switch (fmt) {
      case FMT_FLAC:
        if ((d->flac = drflac_open(file_read, flac_seek, flac_tell, d->file, NULL))) {
          ok = 1;
          d->channels = d->flac->channels;
          d->rate = d->flac->sampleRate;
          d->length = d->flac->totalPCMFrameCount;
        }
        break;
      case FMT_MP3:
        if ((ok = drmp3_init(&d->mp3, file_read, mp3_seek, mp3_tell, NULL, d->file, NULL))) {
          d->channels = d->mp3.channels;
          d->rate = d->mp3.sampleRate;
          /* Only from the Xing/Info header: without one, counting means decoding the file. */
          d->length = d->mp3.totalPCMFrameCount != DRMP3_UINT64_MAX ? drmp3_get_pcm_frame_count(&d->mp3) : 0;
        }
        break;
      case FMT_WAV:
        if ((ok = drwav_init(&d->wav, file_read, wav_seek, wav_tell, d->file, NULL))) {
          d->channels = d->wav.channels;
          d->rate = d->wav.sampleRate;
          d->length = d->wav.totalPCMFrameCount;
        }
        break;
      case FMT_OPUS: {
        /* opusfile trims the pre-skip and end padding, and always decodes at 48 kHz; more
         * than two channels are downmixed for us. No close: the file is ours to close. */
        static const OpusFileCallbacks callbacks = {opus_read, opus_seek, opus_tell, NULL};
        d->opus = op_open_callbacks(d->file, &callbacks, NULL, 0, NULL);
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
  d->fmt = fmt;
  if (!ok || !d->channels || !d->rate) {
    if (ok) {
      mc_decoder_close(d);
    } else {
      if (d->file) fclose(d->file);
      free(d);
    }
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
  fclose(d->file);
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
