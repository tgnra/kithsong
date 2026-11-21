/* Editing an album's, artist's or track's tags by hand (kithcore/edit.h). The fields and what
 * they do are those of Windows' edit_dialog.c. Search MusicBrainz… opens another dialog; using
 * a release it finds fills in the form, and nothing changes until Save.
 *
 * Save calls only the _set() functions whose values changed, on a thread as they can rewrite
 * files and wait on MusicBrainz; the dialog stays open and says why if one fails. Cancel or
 * Escape stops it with nothing changed unless it has got past the copying
 * (mc_edit_set_watch()). The library reloads once the dialog has gone if anything was saved,
 * which can be so even after a failed, cancelled Save.
 *
 * Needs libadwaita 1.5 (AdwDialog).
 */
#ifndef KITHSONG_EDIT_DIALOG_H
#define KITHSONG_EDIT_DIALOG_H

#include "app.h"

typedef enum edit_kind {
  EDIT_ALBUM,
  EDIT_ARTIST,
  EDIT_TRACK,
} edit_kind;

/* Shows the dialog for the album, artist or track with uid (copied), over the main window. One
 * shows at a time. */
void edit_dialog_present(app *a, edit_kind kind, const char *uid);

/* Whether Save is running, which closing the main window waits for. */
gboolean edit_dialog_saving(void);

/* Closes the dialog, if it's open, and waits as edit_dialog_wait() does. Call before closing
 * the library while the window is up. */
void edit_dialog_close(void);

/* Closing the search dialog leaves its search running on its own (there's no stopping a
 * network call), so call this before closing the library: it waits for what's still using
 * it, which is seconds at the very worst (the core's network timeouts). */
void edit_dialog_wait(void);

#endif
