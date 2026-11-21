/* The transcode cache's internals that sync's encode pool shares. Internal to core; the rest
 * is kithcore/transcode.h. */
#ifndef MC_TRANSCODE_INTERNAL_H
#define MC_TRANSCODE_INTERNAL_H

#include "kithcore/transcode.h"

/* The cached file's name for opts, "opus-128.opus", into out (cap bytes).
 * MC_ERR_INVALID_ARG for anything that isn't cached. */
mc_status mc_transcode_cache_name(const mc_transcode_opts *opts, char *out, size_t cap);
/* <cache_dir>/<file_uid>/<name>, malloc'd; NULL as for mc_transcode_cache_name(). */
char *mc_transcode_cache_path(const char *cache_dir, const char *file_uid,
                              const mc_transcode_opts *opts);
/* Whether a and b name the same cached file. */
int mc_transcode_same_opts(const mc_transcode_opts *a, const mc_transcode_opts *b);

#endif
