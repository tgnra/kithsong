/* Editing an album's, artist's or track's tags by hand (musiccore/edit.h), from their
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
 * and genre. Enter saves, Escape cancels, Tab and Shift+Tab go round, Space presses what has
 * the focus.
 *
 * Search MusicBrainz… opens another such window over this one: artist and title boxes, filled
 * in from the form, and the releases found. Using one fills in the form, MusicBrainz release
 * included; nothing changes until Save.
 *
 * Change… (or a click on the cover) picks a new cover, and a file dropped anywhere on the window
 * is one too. While a file is over the window a ring runs round it and the cover says what a drop
 * does: that takes IDropTarget, as WM_DROPFILES says nothing until the drop. The image is read
 * and checked as it comes, off the UI thread, and shown at once; what's held is its bytes, as a
 * dropped file can be a temporary one. Save makes it the cover, last, as tags can move the
 * folder; Cancel forgets it. A file that won't do says so where Save's failures do. An album with
 * no file here has nowhere to put one, so Change… is greyed out.
 *
 * Save calls only the _set() functions whose values changed, on a thread of its own as they
 * can rewrite files and wait on MusicBrainz, and the dialog stays open and says why if one
 * fails. It can't be cancelled once it's started, so Cancel is greyed out meanwhile. A file in
 * use changes nothing and is most likely the track playing, so while something plays a Stop
 * Playing and Retry button joins Save, which empties the queue and saves again.
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

#include "musiccore/musiccore.h"

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
