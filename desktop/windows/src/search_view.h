/* The Search page, beside the sidebar, custom-drawn: a search box with results beneath, a
 * section per kind, ordered by each section's best hit. Results follow the text once typing
 * pauses for 100 ms (Enter searches at once), so the page doesn't jump about with each
 * keystroke. Playable playlists open on their page, smart album groups whether playable or not.
 *
 * Covers are decoded on the UI thread, one per message, so typing isn't held up by them.
 * Screen readers see a list for each section (uia.h). */
#ifndef KITHSONG_SEARCH_VIEW_H
#define KITHSONG_SEARCH_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* Sent to the parent as a WM_COMMAND notification code when a result is opened;
 * search_view_opened() says which. */
#define SVN_OPEN 1
/* Sent likewise when a result's menu or a drop changed the library: posted, for the parent to
 * refresh. */
#define SVN_LIBRARY_CHANGED 2

typedef struct search_view_hit {
  mc_search_kind kind; /* MC_SEARCH_ARTIST, _ALBUM, _TRACK or _PLAYLIST */
  char uid[MC_UID_STRLEN];
  char album_uid[MC_UID_STRLEN]; /* a track's album */
} search_view_hit;

/* Creates the page, hidden, as a child of parent. Only one exists at a time. */
HWND search_view_create(HWND parent, int id);

/* Rebuilds fonts and metrics from the main window's font. Call after creating it, and on DPI
 * or theme changes. */
void search_view_update(UINT dpi, HFONT font);

/* The library to search (NULL finds nothing). Call again whenever the library changes: the
 * shown results are searched for again, so results that went with it go and the covers are
 * up to date. That waits until the page is next painted, so it costs nothing while hidden. */
void search_view_set_library(mc_library *lib);

void search_view_focus(void);

/* Searches for text, as if it had been typed into the box (--page search:<text>). */
void search_view_set_text(const wchar_t *text);

/* The result of the last SVN_OPEN. */
const search_view_hit *search_view_opened(void);

#endif
