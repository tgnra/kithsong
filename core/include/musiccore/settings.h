/* Per-device preferences: where the library folder is, the device's name and a frontend's
 * own yes/no answers. Stored as settings.json in a folder the frontend picks
 * (%LOCALAPPDATA%\musicapp on Windows, ~/.config/musicapp on Linux). Never synced: each
 * device has its own library folder. */
#ifndef MUSICCORE_SETTINGS_H
#define MUSICCORE_SETTINGS_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_settings mc_settings;

/* Loads dir/settings.json. A missing or unreadable file gives empty settings; nothing is
 * written until mc_settings_save(). */
mc_status mc_settings_load(const char *dir, mc_settings **out);
void mc_settings_free(mc_settings *s);

/* Writes the settings back, creating dir if needed. Keys this version doesn't know about
 * are kept. */
mc_status mc_settings_save(const mc_settings *s);

/* The library folder, or NULL if none has been chosen yet (first run). */
const char *mc_settings_library_dir(const mc_settings *s);
mc_status mc_settings_set_library_dir(mc_settings *s, const char *dir);

/* The name this device shows to others (see mc_sync_new()), or NULL if none is saved (the
 * host name is used). */
const char *mc_settings_device_name(const mc_settings *s);
/* NULL or "" clears it. */
mc_status mc_settings_set_device_name(mc_settings *s, const char *name);

/* A frontend's own yes/no preference under key (e.g. Windows' "firewall_dont_ask"), or def
 * if none is saved or it isn't a boolean. */
int mc_settings_bool(const mc_settings *s, const char *key, int def);
mc_status mc_settings_set_bool(mc_settings *s, const char *key, int value);

#ifdef __cplusplus
}
#endif

#endif
