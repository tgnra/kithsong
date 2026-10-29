/* The Artists page, a page of the window's stack ("artists"). It lists every artist, album
 * artists and those only credited on tracks alike, by sort name (mc_library_list_artists()):
 *
 *   (LO)  Low
 *         12 albums
 *   (MP)  Mimi Parker
 *
 * Clicking one (or Enter on it) opens its page in place of the list, which puts it first in
 * the recents:
 *
 *   (<-)  (LO)  Low
 *               12 albums · 140 tracks
 *               [ |> Play ] [ >< Shuffle ]
 *   Albums
 *   +------+ +------+ +------+ +---
 *   | art  | | art  | | art  | |       <- scrolls sideways
 *   +------+ +------+ +------+ +---
 *   Tracks
 *   Words                    I Could Live in Hope     3:22
 *   Lazy                     Long Division            4:51
 *
 * The albums are every one the artist is on, those most theirs first (see
 * mc_library_list_artist_albums()); beneath each is its year, or its artist when that's
 * someone else. Clicking one opens it on the Albums page. The tracks are every one credited
 * to the artist (mc_library_list_artist_tracks()), with their albums; double-click or Enter
 * plays the artist's tracks from there, and right-click (or the Menu key) gives Add to playlist
 * and Delete from library. Play and Shuffle play them all; the header's menu ("...", or the
 * Menu key elsewhere on the page) has Remove from this
 * computer... or Sync to this computer where they apply (remove_here.h), and Delete from
 * library..., which deletes their tracks and their own albums (delete_dialog.h); in the list,
 * right-clicking an artist (a long press on a touchscreen, the Menu key or Shift+F10) gives
 * the same menu. Back is the
 * back button, Escape, Alt+Left, Backspace or the mouse's
 * back button; the list keeps its place meanwhile.
 *
 * Only one exists at a time. As on Windows, artists have no pictures yet: their initials
 * stand in. */
#ifndef KITHSONG_ARTIST_VIEW_H
#define KITHSONG_ARTIST_VIEW_H

#include "app.h"

typedef struct artist_view_callbacks {
  void (*set_shuffle)(void *user, gboolean on); /* Play (off) and Shuffle (on), before playing */
  void (*add_to_playlist)(void *user, const char *track_uid);
  void (*open_album)(void *user, const char *album_uid);
  /* An artist's page opened or closed, or what it has to play changed: the playback
   * buttons may need updating. */
  void (*changed)(void *user);
  void *user;
} artist_view_callbacks;

GtkWidget *artist_view_new(app *a, const artist_view_callbacks *cb);

/* Lists the artists, and the open artist's albums and tracks, from a->lib again, keeping the
 * list's place; if the open artist has gone, back to the list. Call whenever the library
 * changes. */
void artist_view_reload(void);

/* Opens the page of the artist with uid and records it in the recents. FALSE if the library
 * hasn't got it. */
gboolean artist_view_open(const char *uid);
/* The artist whose page is open, or NULL while the list shows. */
const char *artist_view_artist(void);

/* Whether the open artist has a track with a file here. */
gboolean artist_view_playable(void);
/* Plays the open artist's tracks from track start on, or with start -1 from the first
 * (shuffled, any). Tracks with no file here are left out. */
void artist_view_play(int start);

/* Marks the playing track, by uid, or none for NULL, as track_list_set_playing() does. */
void artist_view_set_playing(const char *uid);

#endif
