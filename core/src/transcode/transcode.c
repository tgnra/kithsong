#include "musiccore/transcode.h"

mc_status mc_transcode_cached(const char *cache_dir, const char *file_uid, const char *src_path,
                              const mc_transcode_opts *opts, char **out_path) {
  (void)cache_dir;
  (void)file_uid;
  (void)src_path;
  (void)opts;
  if (out_path) *out_path = NULL;
  return MC_ERR_UNIMPLEMENTED;
}
