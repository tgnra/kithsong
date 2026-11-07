/* Plugins, as frontends see them: finding, listing, turning on and off, and their declared
 * settings. What a plugin is and may do is in kithcore/plugin.h and docs/plugins.md.
 *
 * Plugins are process-wide: mc_plugins_load() once at startup, after mc_init() and loading
 * the settings, and mc_plugins_unload() once at shutdown, after every tethered sync has
 * ended (mc_tethered_free()). A plugin is native code with the app's full rights, so every
 * one found stays off until its user turns it on, and one that is off is never loaded.
 *
 * Plugins are turned on by file id: the plugin's file name without its extension
 * ("kithsong-ipod" for kithsong-ipod.dll), since one that is off can't be asked its id. Once
 * it has been loaded, its id, name and version are remembered in settings.json and listed
 * while it's off.
 *
 * Not on Android: there mc_plugins_load() finds nothing. Unless noted, these are called
 * from the UI thread. */
#ifndef KITHCORE_PLUGINS_H
#define KITHCORE_PLUGINS_H

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_settings mc_settings;

typedef enum mc_plugin_state {
  MC_PLUGIN_OFF,     /* found, and off: not loaded */
  MC_PLUGIN_LOADED,  /* on and running */
  MC_PLUGIN_REFUSED, /* on, but it couldn't be loaded: reason says why */
  /* Turned off since it was loaded: it runs on, unused, until the app restarts. Say so
   * ("Restart Kithsong to finish turning it off"). */
  MC_PLUGIN_UNLOADING
} mc_plugin_state;

typedef struct mc_plugin_info {
  char *file_id;  /* what mc_plugins_set_enabled() takes */
  char *id;       /* "ng.kithso.ipod"; NULL until it has been loaded once */
  char *name;     /* the plugin's own, else file_id: always set */
  char *version;  /* NULL until it has been loaded once */
  char *path;     /* the file's full path, to show */
  mc_plugin_state state;
  int enabled;
  char *reason;   /* MC_PLUGIN_REFUSED: why, in the UI language, to go after "Couldn't load
                   * it: " ; else NULL */
  int device_targets; /* what it provides, while loaded */
  int encoders;
} mc_plugin_info;

typedef struct mc_plugin_list {
  mc_plugin_info *items; /* by name */
  int count;
} mc_plugin_list;

/* Finds the plugins in dirs (dir_count of them), or, for NULL, in the platform's:
 *  - Windows: <the exe's folder>\plugins, then %LOCALAPPDATA%\Kithsong\plugins;
 *  - Linux: <prefix>/lib/kithsong/plugins (the exe being in <prefix>/bin), then
 *    $XDG_DATA_HOME/kithsong/plugins (~/.local/share), and in the Flatpak /app/plugins;
 * and loads those settings has on. A file id found twice is listed and loaded from the
 * first folder only. settings must stay valid until mc_plugins_unload(): plugins' own
 * settings are read from it here, and turning one on writes to it (save it afterwards).
 * Loading again without unloading is MC_ERR_INVALID_ARG. */
mc_status mc_plugins_load(mc_settings *settings, const char *const *dirs, int dir_count);
/* Calls each loaded plugin's shutdown and unloads it. Every tethered sync must have ended. */
void mc_plugins_unload(void);

/* A snapshot. Free with mc_plugin_list_free(). */
mc_status mc_plugins_list(mc_plugin_list **out);
void mc_plugin_list_free(mc_plugin_list *list);

/* Turns a plugin on or off, in the settings mc_plugins_load() was given (save them after).
 * On loads it now, unless it's still loaded from before (MC_PLUGIN_UNLOADING): the result is
 * then MC_OK and its state MC_PLUGIN_LOADED again, or MC_PLUGIN_REFUSED, whose reason the
 * list says, though the plugin stays on. Off stops its device targets and encoders being
 * used at once; it's unloaded at the next start (MC_PLUGIN_UNLOADING). MC_ERR_NOT_FOUND for
 * a file id that wasn't found. */
mc_status mc_plugins_set_enabled(const char *file_id, int on);

/* --- A plugin's settings ---
 * Declared by the plugin (kith_option), drawn by the frontend under the plugin in Settings ›
 * Plugins. Only a loaded plugin's are known. */

typedef enum mc_plugin_option_kind {
  MC_PLUGIN_OPTION_SWITCH, /* value "1" or "0" */
  MC_PLUGIN_OPTION_CHOICE, /* value one of choice_ids */
  MC_PLUGIN_OPTION_NUMBER  /* value a whole number from min to max */
} mc_plugin_option_kind;

typedef struct mc_plugin_option {
  char *key;
  mc_plugin_option_kind kind;
  char *label;        /* in the UI language */
  char *description;  /* NULL if none */
  char *value;        /* the stored value, else the plugin's default */
  char **choice_ids;  /* MC_PLUGIN_OPTION_CHOICE */
  char **choice_labels;
  int choice_count;
  int64_t min, max;   /* MC_PLUGIN_OPTION_NUMBER */
} mc_plugin_option;

typedef struct mc_plugin_option_list {
  mc_plugin_option *items; /* in the plugin's order */
  int count;
} mc_plugin_option_list;

/* The options of the loaded plugin file_id. MC_ERR_NOT_FOUND unless it's loaded. Free with
 * mc_plugin_option_list_free(). */
mc_status mc_plugins_options(const char *file_id, mc_plugin_option_list **out);
void mc_plugin_option_list_free(mc_plugin_option_list *list);
/* Stores an option's value, for the plugin and in the settings (save them after).
 * MC_ERR_INVALID_ARG for a key it didn't declare or a value its kind doesn't take. */
mc_status mc_plugins_set_option(const char *file_id, const char *key, const char *value);
/* Copies values the plugins stored themselves (host->settings_set()) into the settings,
 * returning whether any changed, so the frontend saves them. Before saving the settings. */
int mc_plugins_flush_settings(void);

#ifdef __cplusplus
}
#endif

#endif
