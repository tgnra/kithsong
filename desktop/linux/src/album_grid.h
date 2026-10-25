/* The album grid: covers with the album title and artist beneath, in as many columns as
 * fit. A GtkGridView over a GListStore of AlbumItems, so only the tiles in view exist;
 * covers are decoded off the UI thread as tiles come into view (cover.c).
 *
 * Clicking an album (or moving to it with the keyboard) selects it, and the selection
 * picks what the track list shows. Only one grid exists at a time. */
#ifndef MUSICAPP_ALBUM_GRID_H
#define MUSICAPP_ALBUM_GRID_H

#include <gtk/gtk.h>

#include "musiccore/musiccore.h"

#define ALBUM_TYPE_ITEM (album_item_get_type())
G_DECLARE_FINAL_TYPE(AlbumItem, album_item, ALBUM, ITEM, GObject)

/* One album, as mc_library_list_albums() gives it. Read-only. */
struct _AlbumItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *title;     /* "Untitled" if it has none */
  char *artist;    /* NULL if unknown */
  char *cover_dir; /* the album folder if it has a cover, else NULL */
  int year;        /* 0 if unknown */
};

/* changed is called when the user changes the selection, not by album_grid_reload() or
 * album_grid_unselect(). */
GtkWidget *album_grid_new(void (*changed)(void *user), void *user);

/* Reloads the albums of lib (NULL for none), in mc_library_list_albums() order. Only what
 * changed is replaced, so the grid keeps its scroll position and selection. Returns the
 * number of albums. */
int album_grid_reload(mc_library *lib);

/* The selected album (borrowed), or NULL. */
AlbumItem *album_grid_selection(void);
void album_grid_unselect(void);

#endif
