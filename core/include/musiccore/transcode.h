/* Transcoding originals to Opus for devices that request lossy copies. Output is cached by
 * (file uid, bitrate) so repeated syncs don't re-encode:
 *
 *   <cache_dir>/<file_uid>/opus-<kbps>.opus
 *   <cache_dir>/<file_uid>/opus-<kbps>.sha256   its sidecar: "<sha256> <size> <mtime_ms>"
 *
 * The sidecar holds the file's SHA-256 (hashed from the bytes as they were written) and the
 * size and mtime the file had then, so a sender needn't read the file twice. One whose size
 * or mtime differ from the file's is stale.
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
 * needed, with its sidecar. A cache hit returns at once and marks the file as recently used
 * (moving a valid sidecar's mtime along). For
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

/* The sidecar of the cached transcode at path: its hex SHA-256 (64 digits and a NUL) and
 * the size and mtime the file had when it was taken, for the caller to compare with the
 * file's own. MC_ERR_NOT_FOUND if there is none or it doesn't parse. */
mc_status mc_transcode_sha_read(const char *path, char sha[65], int64_t *size,
                                int64_t *mtime_ms);

/* Writes the sidecar of the cached transcode at path, atomically: sha is its hash when it
 * had size and mtime_ms. */
mc_status mc_transcode_sha_write(const char *path, const char *sha, int64_t size,
                                 int64_t mtime_ms);

/* Deletes the least recently used files in the cache until it holds at most max_bytes.
 * Partial files of encodes in progress are left alone; ones abandoned over an hour ago are
 * deleted. A file and its sidecar are one entry, deleted together; a sidecar without its
 * file goes. A missing cache_dir is empty. */
mc_status mc_transcode_cache_trim(const char *cache_dir, int64_t max_bytes);

/* Measures how fast this device encodes: 200 ms of synthetic stereo audio is encoded at
 * 128 kbps over and over for about half a second. *score is the speed as a multiple of real
 * time, times the number of cores. Takes the calling thread for that long. */
mc_status mc_transcode_benchmark(int *score);

#ifdef __cplusplus
}
#endif

#endif
