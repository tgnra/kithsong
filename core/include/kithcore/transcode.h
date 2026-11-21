/* Transcoding originals to Opus for devices that want lossy copies, cached by file uid and
 * bitrate:
 *
 *   <cache_dir>/<file_uid>/opus-<kbps>.opus
 *   <cache_dir>/<file_uid>/opus-<kbps>.sha256   sidecar: "<sha256> <size> <mtime_ms>"
 *
 * The sidecar's hash is taken from the bytes as written, so a sender needn't read the file
 * twice; it is stale if the file's size or mtime differ.
 *
 * Any thread; calls may overlap, even for the same file and bitrate (each encode writes its
 * own part file and renames it into place). A trim may delete a finished file at once, so
 * callers keep it from running while they use one. */
#ifndef KITHCORE_TRANSCODE_H
#define KITHCORE_TRANSCODE_H

/* The format types come before kithcore.h, which includes sync.h, which uses them. */
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

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Path of a cached transcode of src_path (free with mc_free), creating it and its sidecar if
 * needed. A hit marks the file as recently used (moving a valid sidecar's mtime along). For
 * MC_FORMAT_ORIGINAL nothing is cached and the result is a copy of src_path.
 *
 * src_path must be the track's original (file_uid is its uid), never a transcode (see
 * "Lossy copies" in docs/sync-protocol.md).
 *
 * Decodes FLAC, MP3, WAV, AIFF or Opus; downmixes to stereo, resamples to 48 kHz and encodes
 * constrained-VBR Opus. Tags are copied and the cover beside the source is embedded. The file
 * appears atomically; on failure nothing is left behind. */
mc_status mc_transcode_cached(const char *cache_dir, const char *file_uid, const char *src_path,
                              const mc_transcode_opts *opts, char **out_path);

/* Estimated size of a transcode of a track: duration_ms × kbps / 8 + 4 KiB for the Ogg
 * framing and tags. */
mc_status mc_transcode_estimate_bytes(int duration_ms, int kbps, int64_t *out);

/* Reads path's sidecar; the caller compares size and mtime with the file's own.
 * MC_ERR_NOT_FOUND if there is none or it doesn't parse. */
mc_status mc_transcode_sha_read(const char *path, char sha[65], int64_t *size,
                                int64_t *mtime_ms);

/* Atomically. sha is the file's hash when it had size and mtime_ms. */
mc_status mc_transcode_sha_write(const char *path, const char *sha, int64_t size,
                                 int64_t mtime_ms);

/* Deletes least recently used files until the cache holds at most max_bytes. Part files of
 * running encodes are left alone; ones over an hour old are deleted. A file and its sidecar
 * go together; an orphan sidecar goes. A missing cache_dir counts as empty. */
mc_status mc_transcode_cache_trim(const char *cache_dir, int64_t max_bytes);

/* Encodes synthetic stereo at 128 kbps for about half a second of this thread's CPU time
 * (so a busy device doesn't score lower; at most 5 s wall time). *score is the speed as a
 * multiple of real time, times the number of cores. */
mc_status mc_transcode_benchmark(int *score);

#ifdef __cplusplus
}
#endif

#endif
