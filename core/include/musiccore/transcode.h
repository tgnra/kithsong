/* Transcoding originals to lossy formats for devices that request them. Output is cached
 * by (file uid, format, quality) so repeated syncs don't re-encode. */
#ifndef MUSICCORE_TRANSCODE_H
#define MUSICCORE_TRANSCODE_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum mc_format {
  MC_FORMAT_ORIGINAL,
  MC_FORMAT_OPUS,
  MC_FORMAT_MP3
} mc_format;

typedef struct mc_transcode_opts {
  mc_format format;
  int bitrate_kbps;
} mc_transcode_opts;

/* Returns the path of a cached transcode of src, creating it if needed. */
mc_status mc_transcode_cached(const char *cache_dir, const char *file_uid, const char *src_path,
                              const mc_transcode_opts *opts, char **out_path);

#ifdef __cplusplus
}
#endif

#endif
