/* The Search page: a large search box, centred, with what it finds beneath.
 *
 *              [ Search albums, artists, tracks and playlists ]
 *
 *   Albums
 *   +------+ +------+ +------+
 *   | art  | | art  | | art  |      <- tiles like the album grid's
 *   +------+ +------+ +------+
 *   Tracks
 *   | >  Come Together                   |
 *   |    The Beatles – Abbey Road        |  <- a boxed list for every other kind
 *
 * Results follow the text once typing pauses for a while (Enter searches at once), so the
 * page doesn't jump about with each keystroke. Each kind gets a section, and the sections are
 * in the order of their best hit, so the top result's kind comes first. Albums, artists,
 * album groups and playable playlists (smart ones too) open on their pages (opening an artist
 * or group puts it in the recents), tracks on their album with the track selected. */
#ifndef KITHSONG_SEARCH_VIEW_H
#define KITHSONG_SEARCH_VIEW_H

#include "app.h"

GtkWidget *search_view_new(app *a);

/* Puts the focus in the search box, as when the page is opened. */
void search_view_focus(void);
/* Type to search: replaces what's in the search box with text, typed elsewhere, with the
 * focus and cursor after it. */
void search_view_start(const char *text);

/* Runs the last search again, so results that went with a library change go and the albums'
 * covers are up to date. Call whenever the library changes. */
void search_view_refresh(void);

#endif
