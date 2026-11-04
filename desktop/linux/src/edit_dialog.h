/* Editing an album's, artist's or track's tags by hand (musiccore/edit.h), from their menus:
 * the album tiles' (album_groups.c, and artist_view.c's on an artist's page), the tracks'
 * (track_list.c, artist_view.c), the artists' (artist_view.c) and the recents' (recents.c).
 * The fields and what they do are those of Windows' edit_dialog.c:
 *
 *   +-----------------------------------------------+
 *   | [Cancel]         Edit album            [Save] |
 *   | (banner: why Save failed [Stop playing...])   |
 *   |  +-----------------------------------------+  |
 *   |  | Title                                   |  |
 *   |  | Album artist                            |  |
 *   |  | Year                                    |  |
 *   |  | Original year                           |  |
 *   |  +-----------------------------------------+  |
 *   |  | MusicBrainz release  [Search MusicBrainz…] |
 *   |  +-----------------------------------------+  |
 *   |  | Don't scrobble                     (o ) |  |
 *   |  | Continuous mix                     ( o) |  |
 *   |  +-----------------------------------------+  |
 *   +-----------------------------------------------+
 *
 * An artist has a name and sort name; a track a title, artist, track number, disc number, year
 * and genre. Each is a list of its own in the dialog's column of groups, so something else for
 * an album, as its art will be, is one more group (build_album()). Enter in a field saves,
 * Escape cancels.
 *
 * Search MusicBrainz… opens another dialog over this one: artist and title, filled in from the
 * form, and the releases found. Using one fills in the form, MusicBrainz release included;
 * nothing changes until Save.
 *
 * Save calls only the _set() functions whose values changed, on a thread as they can rewrite
 * files and wait on MusicBrainz, and the dialog stays open and says why if one fails. It can't
 * be cancelled once it's started, so Cancel is greyed out, and the main window doesn't close,
 * meanwhile. A file in use changes nothing and is most likely the track playing, so while
 * something plays the banner offers Stop playing and retry, which empties the queue and saves
 * again.
 *
 * The library reloads once the dialog has gone, if anything was saved, which can be the case
 * even if Save failed partway and the user cancelled.
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
