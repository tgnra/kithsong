/* The Artists page ("artists" in the window's stack): every artist, album artists and those
 * only credited on tracks alike, by sort name (mc_library_list_artists()). Opening one shows
 * its page in place of the list (and puts it first in the recents): Albums (every album it is
 * on, its own first; mc_library_list_artist_albums()), Singles (tracks alone on their album or
 * on none, which album listings leave out; mc_library_list_artist_singles()) and Tracks
 * (mc_library_list_artist_tracks()). Empty sections are hidden. The list keeps its place while
 * an artist is open.
 *
 * Only one exists at a time, so the functions below take no widget. An artist's picture is
 * the cover of their first own album here that has one (mc_artist_info.cover_dir), loaded on
 * the cover thread (cover.h); initials stand in until then. */
#ifndef KITHSONG_ARTIST_VIEW_H
#define KITHSONG_ARTIST_VIEW_H

#include "app.h"

typedef struct artist_view_callbacks {
  void (*set_shuffle)(void *user, gboolean on); /* Play (off) and Shuffle (on), before playing */
  void (*add_to_playlist)(void *user, const char *track_uid, gboolean here); /* here: it has a file here */
  void (*open_album)(void *user, const char *album_uid);
  /* An artist's page opened or closed, or what it has to play changed: the playback
   * buttons may need updating. */
  void (*changed)(void *user);
  void *user;
} artist_view_callbacks;

GtkWidget *artist_view_new(app *a, const artist_view_callbacks *cb);

/* The menu the artist with uid has on this page, for showing elsewhere: the Home page's tiles.
 * g_object_unref() it. Its items are "artist." actions, artist_view_actions(), which whatever
 * shows it has to insert. What it's for is copied, so it can go while the menu is open. */
GMenuModel *artist_view_menu(const char *uid);
GActionGroup *artist_view_actions(void);

/* Lists the artists, and the open artist's albums, singles and tracks, from a->lib again,
 * keeping the list's place; if the open artist has gone, back to the list. Call whenever the
 * library changes. */
void artist_view_reload(void);

/* Opens the page of the artist with uid and records it in the recents. FALSE if the library
 * hasn't got it. */
gboolean artist_view_open(const char *uid);
/* The artist whose page is open, or NULL while the list shows. */
const char *artist_view_artist(void);
/* The uid of the one artist in the list named name (borrowed, until the list next changes),
 * or NULL if there's none or more than one. For singles found by search or in the recents,
 * which have no album to open on and name their artist but give no uid. */
const char *artist_view_find(const char *name);

/* Whether the open artist has a track with a file here. */
gboolean artist_view_playable(void);
/* Plays the open artist's tracks from track start on, or with start -1 from the first
 * (shuffled, any). Tracks with no file here are left out. */
void artist_view_play(int start);

/* Marks the playing track, by uid, or none for NULL, as track_list_set_playing() does. */
void artist_view_set_playing(const char *uid);

GtkWidget *artist_view_scrolled(void);

#endif
