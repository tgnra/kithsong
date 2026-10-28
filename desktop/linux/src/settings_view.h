/* The Settings dialog (the Linux counterpart of desktop/windows/src/settings_view.c), an
 * AdwPreferencesDialog:
 *
 *   +--------------------------------------------------+
 *   |                     Settings                     |
 *   |  Library Location                                |
 *   |  Opening another library switches to ...         |
 *   |  | Library Folder                             | |
 *   |  | /home/me/Music                             | |
 *   |  | Open Another Library...                  > | |
 *   |  | Move Library...                          > | |
 *   |  Sync                                            |
 *   |  Other devices show this computer by its name... |
 *   |  | This Computer's Name         [Study PC   v] | |
 *   |  | This Computer's Address                    | |
 *   |  | 192.168.1.20:47823                         | |
 *   |  |              Leave Group...                | |
 *   |  Scrobbling                                      |
 *   |  Signing in opens Last.fm in your browser...     |
 *   |  | Last.fm                                    | |
 *   |  | Signed in as tania                         | |
 *   |  |                 Sign Out                   | |
 *   |  | Scrobble what I play on this device   [==] | |
 *   +--------------------------------------------------+
 *
 * The library folder is read from settings.json in settings_dir. Open Another Library picks a
 * folder and hands it to the caller's open_library; if that succeeds the choice is saved to
 * settings.json (unknown keys are kept). Move Library closes the dialog and calls the caller's
 * move_library, which moves it (move_library.c). The Sync group renames this computer (through
 * sync when it runs, and settings.json either way), shows where other devices reach it, and
 * leaves the group after asking, as Windows' Settings does. The Scrobbling group signs in
 * with playback_scrobbler(), in the browser; a sign-in carries on if the dialog closes. The
 * switch is this computer's own (settings.json); the account is the sync group's.
 *
 * Wiring it into main.c (settings_dir is ~/.config/kithsong, as main() already builds):
 *
 *   static gboolean open_library(void *user, const char *dir) {
 *     app *a = user;
 *     mc_library *lib = NULL;
 *     mc_status st = mc_library_open(dir, &lib);
 *     if (st != MC_OK) { ...tell the user, e.g. with an AdwAlertDialog...; return FALSE; }
 *     import_view_set_library(a->import, lib); // before the old library is closed
 *     mc_library_close(a->lib);
 *     a->lib = lib;
 *     ...reload the album grid, clear the track list...
 *     return TRUE;
 *   }
 *
 *   // the header bar's Settings button:
 *   settings_view_present(win, a->settings_dir, open_library, move_library, a);
 *
 * Windows locks the sidebar (so Settings can't be reached) while an import session is open;
 * do the same here by making the Settings button insensitive from the import view's
 * session_changed callback, so the library never changes under an import.
 *
 * Needs libadwaita 1.5 (AdwPreferencesDialog).
 */
#ifndef KITHSONG_SETTINGS_VIEW_H
#define KITHSONG_SETTINGS_VIEW_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* Switches to the library in dir (UTF-8). Returns TRUE if it's now open; FALSE if it couldn't
 * be, after telling the user why. Runs on the main thread, while the dialog is open. */
typedef gboolean (*settings_view_open_library_fn)(void *user, const char *dir);
/* Moves the library somewhere the user picks. Called once the dialog has closed. */
typedef void (*settings_view_move_library_fn)(void *user);
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
                           settings_view_move_library_fn move_library, const settings_view_sync *sync,
                           void *user);

/* Open Another Library's steps, for Settings and for the pairing dialog's Create a New
 * Library (empty_only): a folder picker over parent; with empty_only, a folder with anything
 * in it is refused and the picker opens again; then open_library(user, dir), and the folder
 * saved to settings.json. done(data, ...) says how it ended, then free_data(data) (either may
 * be NULL). */
void settings_view_choose_library(GtkWidget *parent, const char *settings_dir,
                                  settings_view_open_library_fn open_library, void *user, gboolean empty_only,
                                  settings_view_chosen_fn done, gpointer data, GDestroyNotify free_data);

#endif
