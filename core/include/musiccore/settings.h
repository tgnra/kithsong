/* Per-device preferences: where the library folder is and the device's name. Stored as
 * settings.json in a folder the frontend picks (%LOCALAPPDATA%\musicapp on Windows,
 * ~/.config/musicapp on Linux). Never synced: each device has its own library folder. */
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

/* Whether playback shuffles (mc_player_set_shuffle()), as it was left. Off if never set. */
int mc_settings_shuffle(const mc_settings *s);
mc_status mc_settings_set_shuffle(mc_settings *s, int on);

#ifdef __cplusplus
}
#endif

#endif
