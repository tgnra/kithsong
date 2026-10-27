/* The Import page, a widget for the main window's content stack (the Linux counterpart of
 * desktop/windows/src/import_view.c):
 *
 *   before an import              during / after an import
 *   +------------------------+    +--------------------------------------------+
 *   |  .- - - - - - - - - -.  |    | Importing 12 of 40                  [Done] |
 *   |  :   Import music    :  |    | ==========-------------------------------- |
 *   |  :  [Choose folders] :  |    |  Needs a decision                          |
 *   |  '- - - - - - - - - -'  |    |  | Things We Lost  Low - ... 70%       > | |
 *   +------------------------+    |  Imported                                  |
 *                                 |  | OK Computer  Radiohead - ...  100%   > | |
 *                                 +--------------------------------------------+
 *
 * Folders dropped on the page (or on any widget given to import_view_add_drop_target) or
 * picked with Choose Folders start an import. The core runs it on its own thread; its events
 * are marshalled to the main loop. Matches at or above the confidence threshold are imported
 * straight away; the rest wait under "Needs a decision". Activating an item opens the
 * review dialog, where the user picks a candidate, searches again, keeps the existing tags or
 * skips it. A session lasts from the start of an import until Done.
 * While MusicBrainz can't be reached, a banner says so and offers Import Without Tagging
 * (mc_import_skip_tagging); it goes away once MusicBrainz answers again or it's used.
 *
 * Wiring it into main.c:
 *
 *   static void on_library_changed(void *user) { ...reload the album grid... }
 *   static void on_session(void *user, gboolean active) {
 *     ...make the sidebar and Settings insensitive while active, as Windows does...
 *   }
 *   static void on_import_done(void *user) { ...show the albums page again... }
 *   static void on_present(void *user) { ...show the import page... }
 *
 *   import_view_callbacks cb = {on_library_changed, on_session, on_import_done, on_present, a};
 *   a->import = import_view_new(a->lib, &cb);
 *   gtk_stack_add_named(GTK_STACK(content), a->import, "import");
 *   import_view_add_drop_target(a->import, toolbar);  // folders dropped anywhere import
 *
 *   The header bar's Import button shows the "import" page; Choose Folders is on the page.
 *   import_view_choose_folders() can be bound to an action as well.
 *
 *   Closing the window while an import runs should ask first: connect the window's
 *   "close-request", return TRUE if import_view_is_running(), and call
 *   import_view_confirm_close(view, then, win) where then() calls gtk_window_destroy(win)
 *   if closed is TRUE.
 *
 *   Switching libraries: call import_view_set_library(view, new_lib) before closing the old
 *   library; the session (which uses the old one) is ended first. Likewise the view must be
 *   destroyed (or set to a NULL library) before the library it was given is closed.
 *
 * Needs libadwaita 1.5 (AdwDialog, AdwAlertDialog) and GTK 4.12.
 */
#ifndef MUSICAPP_IMPORT_VIEW_H
#define MUSICAPP_IMPORT_VIEW_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* All run on the main thread; any may be NULL. */
typedef struct import_view_callbacks {
  /* Items were imported, or a session ended: reload the album grid. */
  void (*library_changed)(void *user);
  /* A session started (TRUE) or ended (FALSE). */
  void (*session_changed)(void *user, gboolean active);
  /* Done was clicked and the session ended: go back to the albums. */
  void (*done)(void *user);
  /* Folders were dropped on a widget from import_view_add_drop_target: show the import page.
   * Called before the import starts. */
  void (*present)(void *user);
  void *user;
} import_view_callbacks;

/* lib may be NULL (the library couldn't be opened); importing then explains why it can't. */
GtkWidget *import_view_new(mc_library *lib, const import_view_callbacks *cb);

/* Switches to another library (or none); ends any session first, without asking. */
void import_view_set_library(GtkWidget *view, mc_library *lib);

/* Starts importing the given folders and/or files (UTF-8 paths). Returns FALSE, after telling
 * the user why, if an import is still running or it couldn't start. A finished session is
 * replaced. */
gboolean import_view_start(GtkWidget *view, const char *const *paths, int count);

/* Opens the folder chooser (several folders may be picked) and imports them. */
void import_view_choose_folders(GtkWidget *view);

/* Makes folders dropped on widget start an import too (calling the present callback first). */
void import_view_add_drop_target(GtkWidget *view, GtkWidget *widget);

/* TRUE while a session is open. */
gboolean import_view_has_session(GtkWidget *view);
/* TRUE while the import thread still has work: matching, or writing an accepted item. */
gboolean import_view_is_running(GtkWidget *view);

/* Ends the session, first asking whether to stop it if it's still running. then(user, closed)
 * is called once the user has decided; closed is FALSE if they want to keep it running. Items
 * already imported stay in the library. then may be NULL. */
void import_view_confirm_close(GtkWidget *view, void (*then)(void *user, gboolean closed),
                               void *user);

#endif
