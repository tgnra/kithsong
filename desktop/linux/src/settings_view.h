/* The Settings dialog, an AdwPreferencesDialog (the Linux counterpart of
 * desktop/windows/src/settings_view.c).
 *
 * The library folder is read from settings.json in settings_dir; Open Another Library hands the
 * picked folder to the caller's open_library and, if that succeeds, saves it (unknown keys in
 * settings.json are kept). Move Library and Reset close the dialog before calling back. The
 * Sync group renames this computer (through sync when it runs, and settings.json either way)
 * and leaves the group. The Scrobbling switch is this computer's own (settings.json); the
 * Last.fm account is the sync group's, signed in through playback_scrobbler() in the browser,
 * and a sign-in carries on if the dialog closes.
 *
 * The caller should make the Settings button insensitive while an import session is open, so
 * the library never changes under an import.
 */
#ifndef KITHSONG_SETTINGS_VIEW_H
#define KITHSONG_SETTINGS_VIEW_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* Switches to the library in dir (UTF-8). Returns TRUE if it's now open; FALSE if it couldn't
 * be, after telling the user why. Runs on the main thread, while the dialog is open. */
typedef gboolean (*settings_view_open_library_fn)(void *user, const char *dir);
/* Moves the library somewhere the user picks. Called once the dialog has closed. */
typedef void (*settings_view_move_library_fn)(void *user);
/* Resets the app on this device, once the user has confirmed: marks the settings folder
 * (mc_settings_request_reset()) and quits. Called once the dialog has closed. */
typedef void (*settings_view_reset_fn)(void *user);
/* How settings_view_choose_library() ended: dir is the library open now, or NULL if it
 * didn't change; saved, whether settings.json names it. */
typedef void (*settings_view_chosen_fn)(gpointer data, const char *dir, gboolean saved);

/* What the Sync group needs of the app. Every member runs on the main thread. */
typedef struct settings_view_sync {
  mc_sync *(*get)(void);   /* the running sync, or NULL (it changes with the library) */
  char *(*off_text)(void); /* why sync is off, "Sync is off (reason)."; g_free it */
  void (*renamed)(void *user, const char *name); /* save the name, refresh the devices */
  void (*group_changed)(void *user);             /* left the group: refresh the devices */
} settings_view_sync;

/* Shows the Settings dialog over parent (any widget in the main window). sync (copied) may be
 * NULL, which leaves the Sync group out. */
void settings_view_present(GtkWidget *parent, const char *settings_dir,
                           settings_view_open_library_fn open_library,
                           settings_view_move_library_fn move_library, settings_view_reset_fn reset,
                           const settings_view_sync *sync, void *user);

/* Open Another Library's steps, for Settings and for the pairing dialog's Create a New
 * Library (empty_only): a folder picker over parent; a folder that fails library_folder_check()
 * (with empty_only, one with anything in it) is refused with library_folder_explain(), which
 * may pick again or take Kithsong's own storage; then open_library(user, dir), and the folder
 * saved to settings.json. done(data, ...) says how it ended, then free_data(data) (either may
 * be NULL). */
void settings_view_choose_library(GtkWidget *parent, const char *settings_dir,
                                  settings_view_open_library_fn open_library, void *user, gboolean empty_only,
                                  settings_view_chosen_fn done, gpointer data, GDestroyNotify free_data);

#endif
