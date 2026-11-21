/* The Search page: a search box with results beneath, a section per kind, ordered by each
 * section's best hit. Results follow the text once typing pauses (Enter searches at once), so
 * the page doesn't jump about with each keystroke. Singles and tracks with no album, which the
 * Albums page leaves out, open on their artist's page. */
#ifndef KITHSONG_SEARCH_VIEW_H
#define KITHSONG_SEARCH_VIEW_H

#include "app.h"
#include "track_list.h"

/* cb is what the tracks' menu does, as the track list's (copied). */
GtkWidget *search_view_new(app *a, const track_list_callbacks *cb);

/* Puts the focus in the search box, as when the page is opened. */
void search_view_focus(void);
/* Type to search: replaces what's in the search box with text, typed elsewhere, with the
 * focus and cursor after it. */
void search_view_start(const char *text);

/* Runs the last search again, so results that went with a library change go and the albums'
 * covers are up to date. Call whenever the library changes. */
void search_view_refresh(void);

#endif
