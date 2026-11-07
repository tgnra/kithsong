/* Plugins for the GTK app (kithcore/plugins.h; the counterpart of Windows' plugin loading in
 * main.c and desktop/windows/src/plugins_view.c): loading them at startup, saving their
 * settings, and Settings' Plugins group.
 *
 *   Plugins
 *   Plugins add what Kithsong doesn't come with ... only turn on plugins you trust.
 *   | iPod sync                                                  [==]  v |
 *   | Version 1.0 · ~/.local/share/kithsong/plugins/kithsong-ipod.so     |
 *   | On                                                                 |
 *   |   | Write artwork                                          [==] |  |  (a switch it declares)
 *   |   | Sound check                                         [Off v] |  |  (a choice)
 *   |   | Max tracks                                       [5000 -+]  |  |  (a number, from its range)
 *   | kithsong-other                                             [  ]    |
 *   | ~/.local/share/kithsong/plugins/kithsong-other.so · Off            |
 *
 * A plugin that's running and declares settings is an expander row, with its settings inside;
 * any other is a switch row. A plugin is native code with the app's full rights, so turning one
 * on asks first, saying so and naming its file. Turning it on loads it at once
 * (mc_plugins_set_enabled()); turning it off stops its devices and encoders being used at once,
 * and it's unloaded at the next start, which its state line says. With none found, the group
 * says where to put one.
 *
 * mc_plugins_load() keeps the settings it's given until mc_plugins_unload(), but the rest of
 * the app loads settings.json afresh for each change; so changes are saved by loading it
 * afresh, copying the plugins section in (mc_settings_copy_plugins()) and saving that, which
 * keeps whatever else was saved since. */
#ifndef KITHSONG_PLUGINS_UI_H
#define KITHSONG_PLUGINS_UI_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* Loads the plugins that are on, from the platform's folders, with settings.json in
 * settings_dir (which must outlive them). Once, at startup, before sync starts. */
void plugins_ui_load(const char *settings_dir);
/* Saves what the plugins stored themselves and unloads them. At exit, after sync and every
 * tethered device have stopped. */
void plugins_ui_unload(void);
/* What's called after a plugin is turned on or off (to look for its devices). */
void plugins_ui_init(void (*changed)(void *user), void *user);
/* Settings' Plugins group, for an AdwPreferencesDialog. Only one exists at a time. */
GtkWidget *plugins_ui_group(void);

#endif
