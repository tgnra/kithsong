/* settings.json's plugin entries, for the loader (plugin/plugins.c). Internal to core:
 * frontends go through kithcore/plugins.h.
 *
 *   "plugins": {"enabled": [file ids], "known": {file id: {"id", "name", "version"}},
 *               "settings": {plugin id: {key: text}}}
 *
 * A plugin is turned on by its file id (its file name without the extension), since one
 * that is off is never loaded and so can't say its own id. What it said the last time it
 * was loaded is kept under "known", so the list can show it while it's off. */
#ifndef MC_SETTINGS_INTERNAL_H
#define MC_SETTINGS_INTERNAL_H

#include "kithcore/settings.h"

int mc_settings_plugin_enabled(const mc_settings *s, const char *file_id);
mc_status mc_settings_set_plugin_enabled(mc_settings *s, const char *file_id, int on);
/* field is "id", "name" or "version"; NULL if unknown. Valid until the settings change. */
const char *mc_settings_plugin_known(const mc_settings *s, const char *file_id, const char *field);
mc_status mc_settings_set_plugin_known(mc_settings *s, const char *file_id, const char *id,
                                       const char *name, const char *version);
/* Calls each for every value stored for plugin_id. */
void mc_settings_plugin_values(const mc_settings *s, const char *plugin_id,
                               void (*each)(void *user, const char *key, const char *value),
                               void *user);
/* NULL value removes it. */
mc_status mc_settings_set_plugin_value(mc_settings *s, const char *plugin_id, const char *key,
                                       const char *value);

#endif
