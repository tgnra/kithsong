/* Transcoding originals to Opus for devices that request lossy copies. Output is cached by
 * (file uid, bitrate) so repeated syncs don't re-encode:
 *
 *   <cache_dir>/<file_uid>/opus-<kbps>.opus
 *
 * Every function may be called from any thread. Calls for different files run
 * independently; two calls for the same file uid and bitrate must not overlap. */
#ifndef MUSICCORE_TRANSCODE_H
#define MUSICCORE_TRANSCODE_H

/* The format types come before musiccore.h, which includes sync.h, which uses them. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_format {
  MC_FORMAT_ORIGINAL,
  MC_FORMAT_OPUS,
  MC_FORMAT_AUTO /* sync profiles only: what the size budget picks (see mc_sync_profile);
                  * never a transcode's format */
} mc_format;

/* The Opus bitrates offered, highest first: the ladder a size budget walks down. */
#define MC_OPUS_BITRATE_COUNT 6
extern const int mc_opus_bitrates[MC_OPUS_BITRATE_COUNT]; /* 256, 192, 160, 128, 96, 64 */

typedef struct mc_transcode_opts {
  mc_format format;
  int bitrate_kbps; /* for MC_FORMAT_OPUS: one of mc_opus_bitrates; else 0 */
} mc_transcode_opts;

#ifdef __cplusplus
}
#endif

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Returns the path of a cached transcode of src_path (free with mc_free), creating it if
 * needed. A cache hit returns at once and marks the file as recently used. For
 * MC_FORMAT_ORIGINAL nothing is cached and the result is src_path itself.
 *
 * Encoding: the source is decoded (FLAC, MP3, WAV, AIFF or Opus), downmixed to stereo if it
 * has more than two channels, resampled to 48 kHz and encoded as Opus VBR at the bitrate.
 * Tags are copied, and the album's cover (the image file beside the source) is embedded. The
 * file appears atomically; on failure nothing is left behind. */
mc_status mc_transcode_cached(const char *cache_dir, const char *file_uid, const char *src_path,
                              const mc_transcode_opts *opts, char **out_path);

/* Estimated size of a transcode of a track: duration_ms × kbps / 8 + 4 KiB for the Ogg
 * framing and tags. */
mc_status mc_transcode_estimate_bytes(int duration_ms, int kbps, int64_t *out);

/* Deletes the least recently used files in the cache until it holds at most max_bytes.
 * Partial files of encodes in progress are left alone; ones abandoned over an hour ago are
 * deleted. A missing cache_dir is empty. */
mc_status mc_transcode_cache_trim(const char *cache_dir, int64_t max_bytes);

/* Measures how fast this device encodes: 200 ms of synthetic stereo audio is encoded at
 * 128 kbps over and over for about half a second. *score is the speed as a multiple of real
 * time, times the number of cores. Takes the calling thread for that long. */
mc_status mc_transcode_benchmark(int *score);

#ifdef __cplusplus
}
#endif

#endif
