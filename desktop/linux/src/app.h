/* State shared by the GTK frontend's modules, and what they call back into main.c for.
 * Everything here runs on the UI thread. */
#ifndef MUSICAPP_APP_H
#define MUSICAPP_APP_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

typedef struct app {
  mc_library *lib;
  GtkApplication *gapp;
  GtkWindow *win;
  char *settings_dir;   /* ~/.config/musicapp */
  GtkStack *pages;      /* "library": the album grid and track list | "import" | "device" */
  GtkWidget *import;    /* the import page, import_view.c */
  GtkWidget *sidebar;
  GtkListBox *nav;      /* the sidebar's Albums, Artists and Playlists */
  GtkWidget *settings;  /* the header bar's Settings button */
  GtkStack *albums;     /* "empty" status page | "grid" | "no-playlists" status page | "playlists" */
  AdwStatusPage *empty; /* "No Albums" */
  GtkWidget *album_grid, *playlist_grid;
  gboolean show_playlists; /* the sidebar's Playlists is picked, rather than Albums or Artists */
  AdwOverlaySplitView *album_split; /* the track list is its sidebar, at the end */
} app;

/* Reloads the album and playlist grids and the shown tracks from the library, keeping the
 * selection and scroll position. Call whenever the library changes, e.g. after an import. */
void app_reload_library(app *a);

/* Plays the album or playlist shown in the track list, from track start on. */
void app_play_album(app *a, int start);

#endif
