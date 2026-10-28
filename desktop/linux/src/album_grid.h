/* The album grid: covers with the album title and artist beneath, in as many columns as
 * fit. A GtkGridView over a GListStore of AlbumItems, so only the tiles in view exist;
 * covers are decoded off the UI thread as tiles come into view (cover.c).
 *
 * The same grid shows playlists, with a placeholder for the cover and the track count
 * beneath the name. Clicking a tile (or moving to it with the keyboard) selects it, and the
 * selection picks what the track list shows. Each grid has a selection of its own. */
#ifndef MUSICAPP_ALBUM_GRID_H
#define MUSICAPP_ALBUM_GRID_H

#include <gtk/gtk.h>

#include "musiccore/musiccore.h"

#define ALBUM_TYPE_ITEM (album_item_get_type())
G_DECLARE_FINAL_TYPE(AlbumItem, album_item, ALBUM, ITEM, GObject)

/* One album, as mc_library_list_albums() gives it, or one playlist. Read-only. */
struct _AlbumItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *title;      /* "Untitled" if it has none; a playlist's name */
  char *artist;     /* NULL if unknown, and for playlists */
  char *cover_dir;  /* the album folder if it has a cover, else NULL */
  int year;         /* 0 if unknown */
  gboolean is_playlist;
  int track_count;  /* playlists only */
};

/* label names the grid for screen readers. changed is called when the user changes the
 * selection, not by album_grid_reload() or album_grid_unselect(). */
GtkWidget *album_grid_new(const char *label, void (*changed)(void *user), void *user);

/* Reloads the albums of lib (NULL for none), in mc_library_list_albums() order, or for
 * album_grid_reload_playlists() its playlists. Only what changed is replaced, so the grid
 * keeps its scroll position and selection. Returns the number of tiles. */
int album_grid_reload(GtkWidget *grid, mc_library *lib);
int album_grid_reload_playlists(GtkWidget *grid, mc_library *lib);

/* The selected tile's item (borrowed), or NULL. */
AlbumItem *album_grid_selection(GtkWidget *grid);
void album_grid_unselect(GtkWidget *grid);
/* Selects the tile with uid and scrolls it into view, without calling changed. FALSE if the
 * grid hasn't got it. */
gboolean album_grid_select(GtkWidget *grid, const char *uid);

#endif
