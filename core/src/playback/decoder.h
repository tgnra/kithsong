/* Audio file decoding to interleaved f32 at the file's own rate and channel count: dr_libs
 * for FLAC, MP3, WAV and AIFF, opusfile for Opus. Shared by the player and the transcoder.
 * A decoder is used by one thread at a time; separate decoders are independent. */
#ifndef MC_PLAYBACK_DECODER_H
#define MC_PLAYBACK_DECODER_H

#include <stdint.h>

#include "musiccore/musiccore.h"

typedef struct mc_decoder mc_decoder;

/* Whether the file's extension is one we can decode. */
int mc_decoder_supports(const char *path);

/* MC_ERR_FORMAT for an unknown extension, MC_ERR_IO if the file can't be opened or read. */
mc_status mc_decoder_open(const char *path, mc_decoder **out);
void mc_decoder_close(mc_decoder *d);

unsigned mc_decoder_channels(const mc_decoder *d);
unsigned mc_decoder_rate(const mc_decoder *d);
/* Frames at the decoder's rate, or 0 if unknown. */
uint64_t mc_decoder_length(const mc_decoder *d);

/* Reads up to frames frames into out (frames × channels floats); fewer only at the end. */
uint64_t mc_decoder_read(mc_decoder *d, float *out, uint64_t frames);
void mc_decoder_seek(mc_decoder *d, uint64_t frame);

#endif
