/* The Search page, beside the sidebar: a large search box, centred, with what it finds
 * beneath, custom-drawn.
 *
 *              [ (o) Search albums, artists, tracks and playlists ]
 *
 *   Albums
 *   +------+ +------+ +------+
 *   | art  | | art  | | art  |      <- tiles like the album grid's
 *   +------+ +------+ +------+
 *   Tracks
 *   | #  Come Together                         > |
 *   |    The Beatles – Abbey Road                |  <- a card of rows for every other kind
 *
 * Results follow the text once typing pauses for 100 ms (Enter searches at once), so the
 * page doesn't jump about with each keystroke; Escape clears the box. Each kind gets a
 * section, and the sections are in the order of their best hit, so the top result's kind
 * comes first. The results scroll beneath the box, with the album grid's overlay scrollbar.
 *
 * Clicking a result, or Enter on it, opens it (SVN_OPEN): artists, albums, playable
 * playlists and album groups on their pages, tracks on their album with the track
 * selected. Smart playlists have no page here yet, so their rows do nothing. Down in the
 * box moves the focus to the first result; the arrow keys, Home and End move it through the results,
 * and Up from the first goes back to the box.
 *
 * Covers are decoded on the UI thread, one per message, so typing isn't held up by them. */
#ifndef KITHSONG_SEARCH_VIEW_H
#define KITHSONG_SEARCH_VIEW_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Sent to the parent as a WM_COMMAND notification code when a result is opened;
 * search_view_opened() says which. */
#define SVN_OPEN 1

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

/* Puts the focus in the search box. */
void search_view_focus(void);

/* The result of the last SVN_OPEN. */
const search_view_hit *search_view_opened(void);

#endif
