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
 * in the order of their best hit, so the top result's kind comes first. Albums and playable
 * playlists open on their pages, tracks on their album with the track selected; artists,
 * album groups and smart playlists have no page here yet, so their rows do nothing. */
#ifndef MUSICAPP_SEARCH_VIEW_H
#define MUSICAPP_SEARCH_VIEW_H

#include "app.h"

GtkWidget *search_view_new(app *a);

/* Puts the focus in the search box, as when the page is opened. */
void search_view_focus(void);

/* Runs the last search again, so results that went with a library change go and the albums'
 * covers are up to date. Call whenever the library changes. */
void search_view_refresh(void);

#endif
