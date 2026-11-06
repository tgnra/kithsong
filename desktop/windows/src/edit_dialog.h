/* Editing an album's, artist's or track's tags by hand (kithcore/edit.h), from their
 * right-click menus. A Mica window over the main window, modal, as first_run.c's is:
 *
 *   Edit album                                 [x]
 *   +---------+  Title
 *   |         |  [ OK Computer                  ]
 *   |  cover  |  Album artist
 *   |         |  [ Radiohead                    ]
 *   |         |  Year           Original year
 *   +---------+  [ 1997      ]  [               ]
 *   [Change…  ]
 *   MusicBrainz release
 *   [ 3f0a...            ] [ Search MusicBrainz… ]
 *   +- [ ] Don't scrobble -----------------------+
 *   |      Its tracks are never scrobbled...     |
 *   |  [ ] Continuous mix                        |
 *   |      Its tracks stay out of search...      |
 *   +--------------------------------------------+
 *                             [ Save ] [ Cancel ]
 *
 * An artist has a name and sort name; a track a title, artist, track number, disc number, year
 * and genre, and if it's a single, Continuous mix too (mc_track_mix_set()). Enter saves, Escape
 * cancels, Tab and Shift+Tab go round, Space presses what has the focus, and Alt with a label's
 * letter goes to that field or presses that button; the letters are underlined from when Alt is
 * first pressed, or always if Windows is set to.
 *
 * Search MusicBrainz… opens another such window over this one: artist and title boxes, filled
 * in from the form, and the releases found, with the focus going to those if it's been left
 * alone. Using one fills in the form, MusicBrainz release included; nothing changes until Save.
 * Unlink empties the release, and Save takes away the link (mc_album_tags_set() with "").
 *
 * Change… (or a click on the cover) picks a new cover, and a file dropped anywhere on the window
 * is one too. While a file is over the window a ring runs round it and the cover says what a drop
 * does: that takes IDropTarget, as WM_DROPFILES says nothing until the drop. The image is read
 * and checked as it comes, off the UI thread, and shown at once; what's held is its bytes, as a
 * dropped file can be a temporary one. Save makes it the cover, last, as tags can move the
 * folder; Cancel forgets it. A file that won't do says so where Save's failures do. An album with
 * no file here has nowhere to put one, so Change… is greyed out. A cover the user chose has
 * Automatic cover under Change…, for Save to go back to what an import would give it
 * (mc_album_art_reset()).
 *
 * Saving an artist with a name another artist has asks whether to merge the two; Keep Both saves
 * the name, and they're two artists of that name.
 *
 * Save calls only the _set() functions whose values changed, on a thread of its own as they
 * can rewrite files and wait on MusicBrainz, and the dialog stays open and says why if one
 * fails. It says how many files it has copied as it goes (mc_edit_set_watch()), and Cancel stops
 * it with the file being copied, the dialog staying open; once they're copied an edit can only
 * finish. A file in use changes nothing, and it's another program's or sync's: the player
 * doesn't hold its files, so there's nothing here to let go of it.
 *
 * Text boxes are EDIT controls, which draw with GDI and leave every pixel's alpha at 0: over
 * Mica their background would show the backdrop and their text would vanish. So each is a
 * layered child, a surface of its own that the compositor shows opaque (it needs nothing
 * but the manifest's Windows 8 or later, which ours has), on an opaque field drawn in the
 * same colour, which WM_CTLCOLOREDIT gives it. Everything else is drawn with canvas.c.
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
