/* The Artists page, beside the sidebar, custom-drawn. It lists every artist, album artists
 * and those only credited on tracks alike, by sort name:
 *
 *   (o)  Low
 *        12 albums
 *   ------------------------------------------------
 *   (o)  Mimi Parker
 *
 * Clicking one (or Enter on it) opens its page in place of the list. The page's header is
 * this view; the main window puts the track list (track_list_show_artist) beneath it:
 *
 *   (<-) Low
 *        12 albums · 140 tracks
 *   Albums                                    (<) (>)
 *   +------+ +------+ +------+ +------+ +---
 *   | art  | | art  | | art  | | art  | |       <- scrolls sideways
 *   +------+ +------+ +------+ +------+ +---
 *   Tracks
 *   [the track list]
 *
 * The albums are every one the artist is on, those most theirs first (see
 * mc_library_list_artist_albums()); beneath each is its year, or its artist when that's
 * someone else. The wheel, the arrows in the heading, or Left and Right scroll them; clicking
 * one (or Enter) opens it (ARN_OPEN_ALBUM). Back is the back button, Escape, Backspace,
 * Alt+Left or the mouse's back button (ARN_BACK); the list keeps its place meanwhile.
 *
 * In the list the arrow keys, Page Up, Page Down, Home and End move the focus, and typing a
 * letter goes to the next artist starting with it. Covers are decoded on the UI thread, one
 * per message, as the search page does. */
#ifndef KITHSONG_ARTIST_VIEW_H
#define KITHSONG_ARTIST_VIEW_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Sent to the parent as WM_COMMAND notification codes. ARN_OPEN: an artist's page opened
 * from the list (artist_view_artist() says which). ARN_BACK: it went back to the list, by the
 * user or because the artist left the library (posted then). ARN_OPEN_ALBUM: an album was
 * clicked (artist_view_opened_album() says which). */
#define ARN_OPEN 1
#define ARN_BACK 2
#define ARN_OPEN_ALBUM 3
/* Posted after the right-click menu's Remove from This PC... or Sync to This PC (on a row, or
 * anywhere on an artist's page) changed what's here (remove_here.h): reload everything. */
#define ARN_LIBRARY_CHANGED 4

/* Creates the page, hidden, as a child of parent. Only one exists at a time. */
HWND artist_view_create(HWND parent, int id);

/* Rebuilds fonts and metrics from the main window's font. Call after creating it, and on DPI
 * or theme changes. */
void artist_view_update(UINT dpi, HFONT font);

/* The library to list (NULL shows "Could not open the library"). Call again whenever the
 * library changes; the list and the open artist's albums reload when next painted. */
void artist_view_set_library(mc_library *lib);

/* Opens the page of the artist with uid, without notifying the parent. FALSE if the library
 * hasn't got it. */
BOOL artist_view_open(const char *uid);
/* Back to the list, without notifying the parent. */
void artist_view_back(void);
/* The artist whose page is open, or NULL while the list shows. */
const char *artist_view_artist(void);
/* How tall an artist's page's header is (it depends only on the DPI and fonts): where the
 * track list goes. */
int artist_view_header_height(void);
/* The album of the last ARN_OPEN_ALBUM. */
const char *artist_view_opened_album(void);

#endif
