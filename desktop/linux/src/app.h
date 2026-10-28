/* State shared by the GTK frontend's modules, and what they call back into main.c for.
 * Everything here runs on the UI thread. */
#ifndef KITHSONG_APP_H
#define KITHSONG_APP_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* The sidebar's library pages, in its order (nav's rows). */
typedef enum app_view {
  APP_VIEW_ALBUMS,
  APP_VIEW_ARTISTS,
  APP_VIEW_PLAYLISTS,
  APP_VIEW_ALBUM_GROUPS,
} app_view;

typedef struct app {
  mc_library *lib;
  GtkApplication *gapp;
  GtkWindow *win;
  char *settings_dir;   /* ~/.config/kithsong */
  char *art_dir;        /* settings_dir/playlist-art: playlists' and album groups' pictures, see
                         * mc_playlist_art() */
  GtkStack *pages;      /* "library": the album grids and track list | "artists" | "search"
                         * | "import" | "device" */
  GtkWidget *import;    /* the import page, import_view.c */
  GtkWidget *search;    /* the search page, search_view.c */
  GtkWidget *artists;   /* the Artists page, artist_view.c */
  GtkWidget *sidebar;
  GtkListBox *top_nav;  /* the sidebar's Home (not available yet) and Search, above nav */
  GtkListBox *nav;      /* the sidebar's Albums, Artists, Playlists and Album groups */
  GtkWidget *settings;  /* the header bar's Settings button */
  GtkStack *albums;     /* "empty" status page | "grid" | "no-playlists" status page | "playlists"
                         * | "groups" (album_groups.c) */
  AdwStatusPage *empty;        /* "No Albums" */
  AdwStatusPage *no_playlists; /* "No playlists yet", or none to play here */
  GtkWidget *album_grid, *playlist_grid;
  app_view view;        /* the sidebar's library page picked last */
  AdwOverlaySplitView *album_split; /* the track list is its sidebar, at the end */
} app;

/* Reloads the album and playlist grids, the album groups, the artists and the shown tracks
 * from the library, keeping the selection and scroll position. Call whenever the library
 * changes, e.g. after an import. */
void app_reload_library(app *a);

/* Plays the album or playlist shown in the track list, from track start on (or with start
 * -1 from the first track, which shuffled is any), and puts it first in the recents. */
void app_play_album(app *a, int start);

/* Shows the album (or the playlist, if playlist) with uid selected on the Albums (or
 * Playlists) page, as picking it in Recents does. */
void app_open(app *a, gboolean playlist, const char *uid);

/* Shows the album with album_uid, as app_open() does, with its track track_uid selected. */
void app_open_track(app *a, const char *album_uid, const char *track_uid);

/* Opens the page of the artist, or of the album group, with uid, and puts it first in the
 * recents. */
void app_open_artist(app *a, const char *uid);
void app_open_album_group(app *a, const char *uid);

/* Switches to the library in dir, as Settings' Open Another Library does (sync restarts with
 * it). TRUE if it's open; FALSE after telling the user why it couldn't be. Saves nothing:
 * settings_view_choose_library() does. */
gboolean app_open_library(app *a, const char *dir);

#endif
