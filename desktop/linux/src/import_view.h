/* The Import page (the Linux counterpart of desktop/windows/src/import_view.c).
 *
 * Folders and zips dropped on the page (or on a widget given to import_view_add_drop_target),
 * or picked with Choose Folders / Choose Zip Files, start an import. The core extracts zips
 * under ~/.cache/kithsong and runs the import on its own thread; its events are marshalled to
 * the main loop. Matches at or above the confidence threshold import straight away; the rest
 * wait under "Needs a decision" for the review dialog. A session lasts from the start of an
 * import until Done. Banners offer Import Without Tagging while MusicBrainz can't be reached
 * (mc_import_skip_tagging) and Retry MusicBrainz for failed lookups (mc_import_retry_unreachable).
 *
 * The view must be given a new library (import_view_set_library) or destroyed before the
 * library it holds is closed. Closing the window mid-import should go through
 * import_view_confirm_close().
 */
#ifndef KITHSONG_IMPORT_VIEW_H
#define KITHSONG_IMPORT_VIEW_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* All run on the main thread; any may be NULL. */
typedef struct import_view_callbacks {
  void (*library_changed)(void *user);
  void (*session_changed)(void *user, gboolean active);
  void (*done)(void *user);
  /* A drop on a widget from import_view_add_drop_target: show the import page. Called before
   * the import starts. */
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

/* For screenshots (--page import, import-review, with --demo-library): starts the demo import
 * (kithcore/demo.h) as import_view_start() starts a real one, if no session is open.
 * import_view_item_shown() says whether item idx is in the list yet, and
 * import_view_open_review() opens its review dialog. */
gboolean import_view_start_demo(GtkWidget *view);
gboolean import_view_item_shown(GtkWidget *view, int idx);
void import_view_open_review(GtkWidget *view, int idx);

void import_view_choose_folders(GtkWidget *view);

void import_view_choose_zips(GtkWidget *view);

/* Makes folders and files (zips, audio) dropped on widget start an import too (calling the
 * present callback first). Files that are neither are passed over by the core. */
void import_view_add_drop_target(GtkWidget *view, GtkWidget *widget);

gboolean import_view_has_session(GtkWidget *view);
/* TRUE while the import thread still has work: matching, or writing an accepted item. */
gboolean import_view_is_running(GtkWidget *view);

/* Ends the session, first asking whether to stop it if it's still running. then(user, closed)
 * is called once the user has decided; closed is FALSE if they want to keep it running. Items
 * already imported stay in the library. then may be NULL. */
void import_view_confirm_close(GtkWidget *view, void (*then)(void *user, gboolean closed),
                               void *user);

#endif
