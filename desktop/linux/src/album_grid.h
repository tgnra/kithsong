/* The album grid: covers with the album title and artist beneath, in as many columns as
 * fit. A GtkGridView over a GListStore of AlbumItems, so only the tiles in view exist;
 * covers are decoded off the UI thread as tiles come into view (cover.c).
 *
 * The same grid shows playlists, with their pictures (mc_playlist_art(), made off the UI
 * thread after each reload) and the track count beneath the name. Clicking a tile (or
 * moving to it with the keyboard) selects it, and the selection picks what the track list
 * shows. Each grid has a selection of its own.
 *
 * Album groups show the same way, with their pictures (mc_album_group_art()) and the album
 * count beneath the name; a group's own page shows its albums in a grid of their own.
 *
 * An album's tile can be dragged (drag.h), and what a tile does with what's dropped on it
 * is up to the grid's owner (album_grid_set_drop()). */
#ifndef KITHSONG_ALBUM_GRID_H
#define KITHSONG_ALBUM_GRID_H

#include <gtk/gtk.h>

#include "drag.h"
#include "musiccore/musiccore.h"

#define ALBUM_TYPE_ITEM (album_item_get_type())
G_DECLARE_FINAL_TYPE(AlbumItem, album_item, ALBUM, ITEM, GObject)

/* One album, as mc_library_list_albums() gives it, or one playlist or album group.
 * Read-only. */
struct _AlbumItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *title;      /* "Untitled" if it has none; a playlist's name */
  char *artist;     /* NULL if unknown, and for playlists and groups */
  char *cover_file; /* the album's cover as found when the item was made (a playlist's picture,
                     * once looked for); NULL if it has none */
  int year;         /* 0 if unknown */
  gboolean is_playlist;
  gboolean is_group; /* an album group */
  int track_count;  /* playlists only; for a group, its albums; not for smart ones */
  char *subtitle;   /* shown beneath the title in place of the artist, or NULL */
  gboolean is_smart; /* a smart playlist or group (smartlist.h), whose rule picks what it holds */
  gboolean random;   /* a smart one in random order, which can be reshuffled */
  gboolean here;     /* an album with a file on this device */
};

/* An album's item, as the album grid makes them. */
AlbumItem *album_item_new(const mc_album_info *info);

/* label names the grid for screen readers. changed is called when the user changes the
 * selection, not by album_grid_reload() or album_grid_unselect(), and when the selected
 * playlist's picture changes (its item is replaced). */
GtkWidget *album_grid_new(const char *label, void (*changed)(void *user), void *user);

/* Reloads the albums of lib (NULL for none), in mc_library_list_albums() order, or for
 * album_grid_reload_playlists() its playlists that have a track to play here
 * (as MC_LIST_PLAYABLE lists them) and keep's, if it isn't NULL (a playlist just made, still
 * empty), with the number left out in *hidden. Only what changed is replaced, so
 * the grid keeps its scroll position and selection. Returns the number of tiles. Playlists
 * keep the pictures they had while theirs are looked up again, in art_dir (see
 * mc_playlist_art()). The smart playlists with something to play here follow the others
 * (not counted in *hidden), with no picture and "Smart playlist" beneath the name. */
int album_grid_reload(GtkWidget *grid, mc_library *lib);
int album_grid_reload_playlists(GtkWidget *grid, mc_library *lib, const char *art_dir, const char *keep,
                                int *hidden);
/* The same for every manual album group (mc_library_list_album_groups()), with their
 * pictures from mc_album_group_art(), then the smart groups as the smart playlists; and for a
 * group's own albums, in its order (or a smart group's, as its rule picks them). A grid
 * should show only one of albums, playlists or groups. *found is FALSE if the group has
 * gone (it then shows nothing). */
int album_grid_reload_groups(GtkWidget *grid, mc_library *lib, const char *art_dir);
int album_grid_reload_group_albums(GtkWidget *grid, mc_library *lib, const char *group_uid, gboolean *found);

/* Right-clicking a tile calls menu with its item (borrowed) and where the click was, in
 * the grid's coordinates (the widget album_grid_new() returned), e.g. to pop up a menu
 * there. */
void album_grid_set_menu(GtkWidget *grid,
                         void (*menu)(void *user, GtkWidget *grid, AlbumItem *item, double x, double y),
                         void *user);
/* Pops up menu in the grid at x, y (as menu above gives them). Its actions resolve from the
 * grid's ancestors, e.g. the window. */
void album_grid_popup(GtkWidget *grid, GMenuModel *menu, double x, double y);
/* Drops on the tiles (drag.h): over says what dropping item on target would do, as the
 * pointer moves over it: GDK_ACTION_COPY to add it to target (a ring marks the tile),
 * GDK_ACTION_MOVE to move it before or after target (a line marks which), or 0 to refuse it.
 * dropped does it; after is whether it landed on target's right half. A grid with none set
 * refuses everything. */
void album_grid_set_drop(GtkWidget *grid, GdkDragAction (*over)(void *user, AlbumItem *target, const drag_item *item),
                         gboolean (*dropped)(void *user, AlbumItem *target, const drag_item *item, gboolean after),
                         void *user);
/* A tile was activated: clicked, if single_click, else double-clicked; or Enter on it. For
 * grids whose tiles open somewhere else rather than being selected (album groups). */
void album_grid_set_activate(GtkWidget *grid, gboolean single_click, void (*activate)(void *user, AlbumItem *item),
                             void *user);

/* Puts the keyboard focus in the grid. */
void album_grid_focus(GtkWidget *grid);

/* The selected tile's item (borrowed), or NULL. */
AlbumItem *album_grid_selection(GtkWidget *grid);
void album_grid_unselect(GtkWidget *grid);
/* Selects the tile with uid and scrolls it into view, without calling changed. FALSE if the
 * grid hasn't got it. */
gboolean album_grid_select(GtkWidget *grid, const char *uid);
/* The grid's item with uid (a new reference), or NULL if it hasn't got it. */
AlbumItem *album_grid_find(GtkWidget *grid, const char *uid);
/* Where the item with uid is in the grid, or -1; and the uid of the item at i, or NULL if i
 * is out of range (borrowed, until the grid is next reloaded). */
int album_grid_index(GtkWidget *grid, const char *uid);
const char *album_grid_uid_at(GtkWidget *grid, int i);

/* One tile on its own, as the grid shows album (cover, title, artist or track count), for
 * places that aren't a grid view, e.g. search results. Its cover loads as the grid's do. */
GtkWidget *album_tile_new(AlbumItem *album);

#endif
