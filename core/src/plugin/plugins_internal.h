/* What the rest of core uses of the loaded plugins (plugins.c) and of the sources it hands
 * them (source.c). Internal to core; the frontends' side is kithcore/plugins.h.
 *
 * Plugins are loaded once per process and unloaded only at shutdown, so the tables found
 * here stay valid until mc_plugins_unload(). Lookups skip plugins turned off since they were
 * loaded. */
#ifndef MC_PLUGIN_PLUGINS_INTERNAL_H
#define MC_PLUGIN_PLUGINS_INTERNAL_H

#include "kithcore/kithcore.h"
#include "kithcore/plugin.h"

/* A job a plugin can check with host->cancelled(). stop NULL is never cancelled. */
struct kith_job {
  int (*stop)(void *user);
  void *user;
};

/* An encoder format some loaded plugin provides. */
typedef struct mc_plugin_encoder {
  const kith_plugin *plugin;
  const kith_encoder *encoder;
  const kith_encoder_format *format;
} mc_plugin_encoder;

/* The encoder format with this id, from a plugin that is loaded and on. MC_ERR_NOT_FOUND if
 * there's none. */
mc_status mc_plugins_find_encoder(const char *format_id, mc_plugin_encoder *out);

/* A device target some loaded plugin provides. */
typedef struct mc_plugin_target {
  const kith_plugin *plugin;
  const kith_device_target *target;
} mc_plugin_target;

/* The device targets of plugins that are loaded and on, at most max of them; returns how
 * many there are (which may be more than max). */
int mc_plugins_targets(mc_plugin_target *out, int max);
/* The target targetid of plugin plugin_id. MC_ERR_NOT_FOUND unless it's loaded and on. */
mc_status mc_plugins_find_target(const char *plugin_id, const char *target_id, mc_plugin_target *out);

/* --- Sources (source.c) --- */

/* Opens path as a source delivering want (NULL: the file's own format as F32), with the
 * file's tags and the album's cover (the image beside it). What host->decode_open() is. */
int mc_source_open(const char *path, const kith_pcm_format *want, kith_source **out);
int mc_source_info(kith_source *src, kith_source_info *out);
int mc_source_set_format(kith_source *src, const kith_pcm_format *want);
int64_t mc_source_read(kith_source *src, void *buf, int64_t frames);
void mc_source_close(kith_source *src);
int mc_source_tags(kith_source *src, kith_tags *out);
int mc_source_cover(kith_source *src, kith_cover *out);

/* Whether job says stop: host->cancelled(). */
int mc_plugin_job_cancelled(const kith_job *job);

#endif
