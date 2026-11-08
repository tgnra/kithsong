/* The transcode cache's internals that sync's encode pool and tethered sync share. Internal
 * to core; the rest is kithcore/transcode.h. */
#ifndef MC_TRANSCODE_INTERNAL_H
#define MC_TRANSCODE_INTERNAL_H

#include "kithcore/plugin.h"
#include "kithcore/transcode.h"

/* The cached file's name for opts, "opus-128.opus" or "mp3-v0.mp3", into out (cap bytes).
 * MC_ERR_NOT_FOUND for an encoder no loaded plugin has; MC_ERR_INVALID_ARG for anything
 * else that isn't cached. */
mc_status mc_transcode_cache_name(const mc_transcode_opts *opts, char *out, size_t cap);
/* <cache_dir>/<file_uid>/<name>, malloc'd; NULL as for mc_transcode_cache_name(). */
char *mc_transcode_cache_path(const char *cache_dir, const char *file_uid,
                              const mc_transcode_opts *opts);
/* Whether a and b name the same cached file. */
int mc_transcode_same_opts(const mc_transcode_opts *a, const mc_transcode_opts *b);

/* mc_transcode_cached(), with job for a plugin's encoder to check (NULL: never stopped).
 * MC_ERR_CANCELLED if it said stop. */
mc_status mc_transcode_cached_job(const char *cache_dir, const char *file_uid,
                                  const char *src_path, const mc_transcode_opts *opts,
                                  const kith_job *job, char **out_path);

#endif
