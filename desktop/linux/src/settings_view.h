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
 *   |  | Move Library...  (not available yet)       | |
 *   +--------------------------------------------------+
 *
 * The library folder is read from settings.json in settings_dir. Open Another Library picks a
 * folder and hands it to the caller's open_library; if that succeeds the choice is saved to
 * settings.json (unknown keys are kept). Moving the library isn't implemented yet, so its row
 * is disabled, as on Windows.
 *
 * Wiring it into main.c (settings_dir is ~/.config/musicapp, as main() already builds):
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
 *   settings_view_present(win, a->settings_dir, open_library, a);
 *
 * Windows locks the sidebar (so Settings can't be reached) while an import session is open;
 * do the same here by making the Settings button insensitive from the import view's
 * session_changed callback, so the library never changes under an import.
 *
 * Needs libadwaita 1.5 (AdwPreferencesDialog).
 */
#ifndef MUSICAPP_SETTINGS_VIEW_H
#define MUSICAPP_SETTINGS_VIEW_H

#include <adwaita.h>

/* Switches to the library in dir (UTF-8). Returns TRUE if it's now open; FALSE if it couldn't
 * be, after telling the user why. Runs on the main thread, while the dialog is open. */
typedef gboolean (*settings_view_open_library_fn)(void *user, const char *dir);

/* Shows the Settings dialog over parent (any widget in the main window). */
void settings_view_present(GtkWidget *parent, const char *settings_dir,
                           settings_view_open_library_fn open_library, void *user);

#endif
