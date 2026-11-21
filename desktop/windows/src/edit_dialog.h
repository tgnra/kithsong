/* Editing an album's, artist's or track's tags by hand (kithcore/edit.h): a modal Mica window
 * over the main window, as first_run.c's is. Search MusicBrainz… opens another such window;
 * using a release it finds fills in the form, and nothing changes until Save.
 *
 * A cover can be picked with Change… or dropped anywhere on the window. Drops go through
 * IDropTarget, not WM_DROPFILES, so the cover can say what a drop does while a file is over the
 * window. The image is read off the UI thread and its bytes are held, as a dropped file can be
 * temporary. Save writes it last, as tags can move the folder.
 *
 * Save calls only the _set() functions whose values changed, on a thread of its own as they can
 * rewrite files and wait on MusicBrainz; the dialog stays open and says why if one fails.
 * Cancel stops it during the copying (mc_edit_set_watch()); after that an edit can only finish.
 * A file in use is another program's or sync's: the player doesn't hold files open.
 *
 * Text boxes are EDIT controls, which draw with GDI and leave alpha at 0, so over Mica their
 * text would vanish. Each is therefore a layered child (needs the manifest's Windows 8 or
 * later), which the compositor shows opaque, on a field drawn in the colour WM_CTLCOLOREDIT
 * gives it. Everything else is drawn with canvas.c.
 */
#ifndef KITHSONG_EDIT_DIALOG_H
#define KITHSONG_EDIT_DIALOG_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* Each shows the dialog for uid (copied), and returns TRUE if the library changed, which it
 * can have even if Save failed partway and the user cancelled. owner is the main window. */
BOOL edit_dialog_album(HWND owner, mc_library *lib, const char *uid);
BOOL edit_dialog_artist(HWND owner, mc_library *lib, const char *uid);
BOOL edit_dialog_track(HWND owner, mc_library *lib, const char *uid);

/* Closing the search window leaves its search running on its own (there's no stopping a
 * network call), so call this before closing the library: it waits for what's still using
 * it, which is seconds at the very worst (the core's network timeouts). */
void edit_dialog_wait(void);

#endif
