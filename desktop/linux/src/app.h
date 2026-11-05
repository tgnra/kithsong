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
  gboolean first_run;  /* no library yet, while the first run asks where it goes (first_run.h) */
  GtkApplication *gapp;
  GtkWindow *win;
  AdwToastOverlay *toasts; /* over the whole window, e.g. "Restored 3 tracks" */
  char *settings_dir;  /* ~/.config/kithsong */
  gboolean from_source; /* running from a checkout (musiccore/source_run.h) */
  char *reset_failed;  /* why Reset Kithsong… couldn't clear all of settings_dir as this
                        * start began, to say once the window is up; else NULL */
  char *art_dir;        /* settings_dir/playlist-art: playlists' and album groups' pictures, see
                         * mc_playlist_art() */
  GtkStack *pages;      /* "library": the album grids and track list | "home" | "artists"
                         * | "search" | "import" | "device" */
  GtkWidget *import;    /* the import page, import_view.c */
  GtkWidget *search;    /* the search page, search_view.c */
  GtkWidget *artists;   /* the Artists page, artist_view.c */
  GtkWidget *home;      /* the Home page, home_view.c */
  GtkWidget *sidebar;
  GtkListBox *top_nav;  /* the sidebar's Home and Search, above nav */
  GtkListBox *nav;      /* the sidebar's Albums, Artists, Playlists and Album groups */
  GtkWidget *settings;  /* the header bar's Settings button */
  GtkWidget *analysing; /* next to it, while audio analysis runs in the background (analysis_ui.h) */
  GtkStack *albums;     /* "empty" status page | "grid" | "playlists" (a header over playlists)
                         * | "groups" (album_groups.c) */
  AdwStatusPage *empty;        /* "No music yet", or "No albums" when the music is all singles */
  gboolean all_singles;        /* the Albums page has nothing, but the library has tracks */
  GtkWidget *empty_import;     /* its Import button */
  GtkWidget *empty_choose_sync; /* its Choose what to sync button, in place of Import in a group */
  AdwStatusPage *no_playlists; /* "No playlists yet", or none to play here */
  GtkWidget *choose_sync;      /* its Choose what to sync button */
  GtkStack *playlists;         /* "none" (no_playlists) | "grid", under the Playlists header */
  int hidden_playlists;        /* playlists left out for having nothing to play here */
  GtkWidget *album_grid, *playlist_grid;
  app_view view;        /* the sidebar's library page picked last */
  AdwOverlaySplitView *album_split; /* the track list is its sidebar, at the end */
  AdwBanner *waiting;   /* above the pages: tracks waiting for a device (app_update_waiting()) */
  AdwBanner *problems;  /* above that: what's wrong with the files here (app_update_waiting()) */
} app;

/* Reloads the album and playlist grids, the album groups, the artists and the shown tracks
 * from the library, keeping the selection and scroll position. Call whenever the library
 * changes, e.g. after an import. */
void app_reload_library(app *a);

/* Reads the tracks waiting for a device that can't be reached now (mc_sync_get_waiting())
 * into the banner above the pages: "12 tracks are waiting for Tania's desktop to come
 * online." It shows on the "library" page (Albums, Playlists and Album groups, as on
 * Windows) while there are any, and goes by itself once they've arrived. Call when sync says
 * they changed (SYNC_EV_WAITING) and when sync starts. */
void app_update_waiting(app *a);
/* Updates what the empty Albums and Playlists pages suggest from this computer's place in
 * sync (devices_sync_state()): Import while it isn't in a group, Choose what to sync once
 * it is. */
void app_update_empty_pages(app *a);

/* A header for a list page, as an album group's: its name (title-2) and a round + button
 * with tooltip (and its shortcut, accel, as app_set_tooltip() takes it, or NULL), which
 * calls clicked(button, user). For Playlists and Album groups. With a menu, the + has an
 * arrow beside it that drops it down (New smart playlist…), whose tooltip is menu_tooltip. */
GtkWidget *app_list_header(const char *title, const char *tooltip, const char *accel, GCallback clicked,
                           gpointer user, GMenuModel *menu, const char *menu_tooltip);

/* Plays the album or playlist shown in the track list, from track start on (or with start
 * -1 from the first track, which shuffled is any), and puts it first in the recents. An
 * album on an album group's page plays as part of the group, going on into the albums after
 * it, and puts the group first instead. */
void app_play_album(app *a, int start);

/* How the open album group plays (album_groups.h): its albums in its order, or in a random
 * order, each album's tracks in their own; or every track shuffled. */
typedef enum app_group_play {
  APP_PLAY_IN_ORDER,
  APP_PLAY_SHUFFLE_ALBUMS,
  APP_PLAY_SHUFFLE_TRACKS,
} app_group_play;
/* Plays the open album group's tracks that have a file here, as how says, and puts the group
 * first in the recents. */
void app_play_album_group(app *a, app_group_play how);

/* Shows the album (or the playlist, if playlist) with uid selected on the Albums (or
 * Playlists) page, as picking it in Recents does. */
void app_open(app *a, gboolean playlist, const char *uid);

/* Shows the album with uid. One that has come down to one track is a single now, which the
 * Albums page leaves out, so its artist's page (artist, by name, which may be NULL) opens. */
void app_open_album_or_single(app *a, const char *uid, const char *artist);

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

/* Stress test libraries (musiccore/stress.h). Opens the one in dir and saves that, remembering
 * the library to go back to; and goes back to it. With no way back saved, Settings opens. */
void app_open_stress(app *a, const char *dir);
void app_back_to_library(app *a);
/* Shows a library page, as its sidebar row does. */
void app_show_view(app *a, app_view view);
/* Opens Settings with the Testing group open. */
void app_show_testing(app *a);

/* Sets widget's tooltip to label with its keyboard shortcut, "Next track (Ctrl+→)": accel
 * as gtk_accelerator_parse() reads it, e.g. "<Control>Right", shown as GTK labels it. */
void app_set_tooltip(GtkWidget *widget, const char *label, const char *accel);

#endif
