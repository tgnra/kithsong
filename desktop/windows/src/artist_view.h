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
 *   Singles                                   (<) (>)
 *   +------+ +------+
 *   | art  | | art  |                            <- likewise
 *   +------+ +------+
 *   Tracks
 *   [the track list]
 *
 * The albums are every one the artist is on, those most theirs first (see
 * mc_library_list_artist_albums()); beneath each is its year, or its artist when that's
 * someone else. The singles are their tracks that are alone on their album here, or on none
 * (mc_library_list_artist_singles()), each with its album's cover and its year; one with no
 * file here is greyed out. A strip with nothing in it isn't shown, heading and all, so the
 * header's height depends on the artist (ARN_RESIZED). The wheel over a strip, the arrows in
 * its heading, or Left and Right scroll it, and Up and Down go between them; clicking an
 * album (or Enter) opens it (ARN_OPEN_ALBUM), and clicking a single plays the singles from it
 * (ARN_PLAY_SINGLE). A single has a track's right-click menu, as in the track list, and can
 * be dragged onto a playlist (drag.h). Back is the back button, Escape, Backspace, Alt+Left
 * or the mouse's back button (ARN_BACK); the list keeps its place meanwhile.
 *
 * In the list the arrow keys, Page Up, Page Down, Home and End move the focus, and typing a
 * letter goes to the next artist starting with it. Covers are decoded on the UI thread, one
 * per message, as the search page does. */
#ifndef KITHSONG_ARTIST_VIEW_H
#define KITHSONG_ARTIST_VIEW_H

#include <windows.h>

#include "musiccore/musiccore.h"
#include "track_list.h"

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
/* A single was clicked (or Enter on it): play the singles from it (artist_view_play_index()
 * says which, artist_view_single_at() has them). */
#define ARN_PLAY_SINGLE 5
/* A single's menu, or a drop of it, asks for it to be added to a playlist (track_uid, to the
 * end of playlist_uid, "" for a new one), or deleted from the library (track_uid). As the
 * track list's TLN_ADD_TO_PLAYLIST and TLN_DELETE_TRACK; artist_view_last_request() has the
 * details, and the parent does it. */
#define ARN_ADD_TO_PLAYLIST 6
#define ARN_DELETE_TRACK 7
/* Posted when the open artist's header changed height (artist_view_header_height()): a strip
 * came or went. Lay the track list out again. */
#define ARN_RESIZED 8
/* A single's menu's Go to Artist, offered when the single is credited to someone else (it's
 * on an album of the open artist's): open artist_uid's page, as the track list's
 * TLN_GO_TO_ARTIST does. artist_view_last_request() has it. */
#define ARN_GO_TO_ARTIST 9

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
/* How tall the open artist's page's header is: where the track list goes. It depends on the
 * DPI, the fonts and which strips show. */
int artist_view_header_height(void);
/* The album of the last ARN_OPEN_ALBUM. */
const char *artist_view_opened_album(void);

/* The open artist's singles, in order, for the play queue. Valid until the page next
 * changes. */
int artist_view_single_count(void);
track_list_item artist_view_single_at(int i);
/* The single the last ARN_PLAY_SINGLE was for. */
int artist_view_play_index(void);
/* The request the last ARN_ADD_TO_PLAYLIST, ARN_DELETE_TRACK or ARN_GO_TO_ARTIST was for. */
const track_list_request *artist_view_last_request(void);

#endif
