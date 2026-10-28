#include "album_grid.h"

#include <string.h>

#include "cover.h"

enum { COVER_SIZE = 160 }; /* tile covers, in logical pixels */

/* --- AlbumItem --- */

G_DEFINE_FINAL_TYPE(AlbumItem, album_item, G_TYPE_OBJECT)

static void album_item_finalize(GObject *obj) {
  AlbumItem *self = ALBUM_ITEM(obj);
  g_free(self->title);
  g_free(self->artist);
  g_free(self->cover_dir);
  G_OBJECT_CLASS(album_item_parent_class)->finalize(obj);
}

static void album_item_class_init(AlbumItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = album_item_finalize;
}

static void album_item_init(AlbumItem *self) {
  (void)self;
}

static AlbumItem *album_item_new(const mc_album_info *info) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->title = g_strdup(info->title && *info->title ? info->title : mc_str(MC_STR_UNTITLED));
  self->artist = info->artist && *info->artist ? g_strdup(info->artist) : NULL;
  self->cover_dir = info->has_art && info->folder ? g_strdup(info->folder) : NULL;
  self->year = info->year;
  return self;
}

static AlbumItem *album_item_new_playlist(const mc_playlist_info *info) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->title = g_strdup(info->name && *info->name ? info->name : mc_str(MC_STR_UNTITLED));
  self->is_playlist = TRUE;
  self->track_count = info->track_count;
  return self;
}

static gboolean album_item_equal(const AlbumItem *a, const AlbumItem *b) {
  return !strcmp(a->uid, b->uid) && !g_strcmp0(a->title, b->title) && !g_strcmp0(a->artist, b->artist) &&
         !g_strcmp0(a->cover_dir, b->cover_dir) && a->year == b->year && a->is_playlist == b->is_playlist &&
         a->track_count == b->track_count;
}

/* The line beneath the title: the artist, or a playlist's track count. g_free it. */
static char *album_item_subtitle(const AlbumItem *item) {
  if (item->is_playlist) {
    char count[64];
    mc_format(count, sizeof count, mc_str_plural(MC_STR_TRACKS, item->track_count), MC_D(item->track_count));
    return g_strdup(count);
  }
  return g_strdup(item->artist ? item->artist : mc_str(MC_STR_UNKNOWN_ARTIST));
}

/* --- Grid --- */

/* One grid's state, owned by its scrolled window. */
typedef struct grid {
  GListStore *store;
  GtkSingleSelection *selection;
  GtkWidget *view;
  void (*changed)(void *user);
  void *user;
  gboolean quiet; /* selection changes aren't the user's */
} grid;

static grid *grid_of(GtkWidget *widget) {
  return g_object_get_data(G_OBJECT(widget), "album-grid");
}

static void grid_free(gpointer data) {
  grid *g = data;
  g_object_unref(g->selection);
  g_free(g);
}

/* A tile's widgets, and the cover it's waiting for. */
typedef struct cell {
  GtkWidget *picture, *title, *artist;
  GCancellable *loading;
} cell;

static void cell_cancel(cell *c) {
  if (!c->loading) return;
  g_cancellable_cancel(c->loading);
  g_clear_object(&c->loading);
}

static void cell_free(gpointer data) {
  cell *c = data;
  cell_cancel(c);
  g_free(c);
}

static void tile_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  cell *c = g_new0(cell, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_size_request(box, COVER_SIZE, -1);
  gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(box), cover_frame_new(COVER_SIZE, COVER_SIZE / 3, &c->picture));

  c->title = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(c->title), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(c->title), 1); /* as wide as the cover, no wider */
  gtk_widget_set_margin_top(c->title, 8);
  gtk_box_append(GTK_BOX(box), c->title);

  c->artist = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(c->artist), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(c->artist), 1);
  gtk_widget_add_css_class(c->artist, "dim-label");
  gtk_widget_add_css_class(c->artist, "caption");
  gtk_box_append(GTK_BOX(box), c->artist);

  g_object_set_data_full(G_OBJECT(box), "cell", c, cell_free);
  gtk_list_item_set_child(item, box);
}

static void tile_cover_loaded(GdkTexture *texture, gpointer user) {
  cell *c = user;
  g_clear_object(&c->loading);
  gtk_picture_set_paintable(GTK_PICTURE(c->picture), GDK_PAINTABLE(texture));
}

static void tile_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  AlbumItem *album = gtk_list_item_get_item(item);
  GtkWidget *box = gtk_list_item_get_child(item);
  cell *c = g_object_get_data(G_OBJECT(box), "cell");
  char *subtitle = album_item_subtitle(album);
  gtk_label_set_text(GTK_LABEL(c->title), album->title);
  gtk_label_set_text(GTK_LABEL(c->artist), subtitle);
  char *name = mc_format_alloc(mc_str(MC_STR_LINUX_ALBUM_TILE_NAME), MC_S(album->title), MC_S(subtitle));
  gtk_list_item_set_accessible_label(item, name ? name : album->title);
  mc_free(name);
  g_free(subtitle);

  cell_cancel(c);
  gtk_picture_set_paintable(GTK_PICTURE(c->picture), NULL);
  if (!album->cover_dir) return;
  int size = COVER_SIZE * gtk_widget_get_scale_factor(box);
  GdkTexture *texture = NULL;
  if (cover_lookup(album->cover_dir, size, &texture)) {
    gtk_picture_set_paintable(GTK_PICTURE(c->picture), GDK_PAINTABLE(texture));
    g_clear_object(&texture);
  } else {
    c->loading = g_cancellable_new();
    cover_load(album->cover_dir, size, c->loading, tile_cover_loaded, c);
  }
}

static void tile_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "cell");
  cell_cancel(c);
  gtk_picture_set_paintable(GTK_PICTURE(c->picture), NULL);
}

static void on_selected(GObject *obj, GParamSpec *pspec, gpointer user) {
  (void)obj;
  (void)pspec;
  grid *g = user;
  if (!g->quiet && g->changed) g->changed(g->user);
}

GtkWidget *album_grid_new(const char *label, void (*changed)(void *user), void *user) {
  grid *g = g_new0(grid, 1);
  g->changed = changed;
  g->user = user;
  g->store = g_list_store_new(ALBUM_TYPE_ITEM);
  g->selection = gtk_single_selection_new(G_LIST_MODEL(g->store)); /* takes the store */
  gtk_single_selection_set_autoselect(g->selection, FALSE);
  gtk_single_selection_set_can_unselect(g->selection, TRUE);
  g_signal_connect(g->selection, "notify::selected", G_CALLBACK(on_selected), g);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(tile_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(tile_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(tile_unbind), NULL);
  /* The view takes both; the selection stays ours too. */
  GtkWidget *view = gtk_grid_view_new(GTK_SELECTION_MODEL(g_object_ref(g->selection)), factory);
  g->view = view;
  gtk_grid_view_set_min_columns(GTK_GRID_VIEW(view), 1);
  gtk_grid_view_set_max_columns(GTK_GRID_VIEW(view), 24);
  gtk_widget_add_css_class(view, "album-grid");
  gtk_accessible_update_property(GTK_ACCESSIBLE(view), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
  g_object_set_data_full(G_OBJECT(scroller), "album-grid", g, grid_free);
  return scroller;
}

/* Shows items (n of them; the grid takes its own references), keeping the tile with uid
 * keep selected if it's still there. */
static void grid_set_items(grid *g, AlbumItem **items, guint n, const char *keep) {
  guint selected = GTK_INVALID_LIST_POSITION;
  for (guint i = 0; i < n && keep[0]; i++)
    if (!strcmp(items[i]->uid, keep)) selected = i;

  /* Replace only the run that changed (an import usually adds an album or two), so the
   * tiles either side keep their covers and the view doesn't jump. */
  guint old = g_list_model_get_n_items(G_LIST_MODEL(g->store)), head = 0, tail = 0;
  while (head < old && head < n) {
    AlbumItem *a = g_list_model_get_item(G_LIST_MODEL(g->store), head);
    gboolean same = album_item_equal(a, items[head]);
    g_object_unref(a);
    if (!same) break;
    head++;
  }
  while (tail < old - head && tail < n - head) {
    AlbumItem *a = g_list_model_get_item(G_LIST_MODEL(g->store), old - 1 - tail);
    gboolean same = album_item_equal(a, items[n - 1 - tail]);
    g_object_unref(a);
    if (!same) break;
    tail++;
  }
  g->quiet = TRUE;
  if (head + tail < old || head + tail < n)
    g_list_store_splice(g->store, head, old - head - tail, (gpointer *)(items + head), n - head - tail);
  gtk_single_selection_set_selected(g->selection, selected);
  g->quiet = FALSE;
  if (!old && n) gtk_grid_view_scroll_to(GTK_GRID_VIEW(g->view), 0, GTK_LIST_SCROLL_NONE, NULL); /* start at the top */
}

/* The selected tile's uid, or "", into keep. */
static void grid_keep(GtkWidget *widget, char keep[MC_UID_STRLEN]) {
  AlbumItem *sel = album_grid_selection(widget);
  keep[0] = '\0';
  if (sel) memcpy(keep, sel->uid, MC_UID_STRLEN);
}

int album_grid_reload(GtkWidget *widget, mc_library *lib) {
  char keep[MC_UID_STRLEN];
  grid_keep(widget, keep);
  mc_album_list *list = NULL;
  if (lib && mc_library_list_albums(lib, &list) != MC_OK) list = NULL;
  guint n = list ? (guint)list->count : 0;
  AlbumItem **items = g_new0(AlbumItem *, n + 1);
  for (guint i = 0; i < n; i++) items[i] = album_item_new(&list->items[i]);
  mc_album_list_free(list);

  grid_set_items(grid_of(widget), items, n, keep);
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  return (int)n;
}

int album_grid_reload_playlists(GtkWidget *widget, mc_library *lib) {
  char keep[MC_UID_STRLEN];
  grid_keep(widget, keep);
  mc_playlist_list *list = NULL;
  if (lib && mc_library_list_playlists(lib, &list) != MC_OK) list = NULL;
  guint n = list ? (guint)list->count : 0;
  AlbumItem **items = g_new0(AlbumItem *, n + 1);
  for (guint i = 0; i < n; i++) items[i] = album_item_new_playlist(&list->items[i]);
  mc_playlist_list_free(list);

  grid_set_items(grid_of(widget), items, n, keep);
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  return (int)n;
}

AlbumItem *album_grid_selection(GtkWidget *widget) {
  grid *g = grid_of(widget);
  return g ? gtk_single_selection_get_selected_item(g->selection) : NULL;
}

void album_grid_unselect(GtkWidget *widget) {
  grid *g = grid_of(widget);
  g->quiet = TRUE;
  gtk_single_selection_set_selected(g->selection, GTK_INVALID_LIST_POSITION);
  g->quiet = FALSE;
}

gboolean album_grid_select(GtkWidget *widget, const char *uid) {
  grid *g = grid_of(widget);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(g->store));
  for (guint i = 0; i < n; i++) {
    AlbumItem *item = g_list_model_get_item(G_LIST_MODEL(g->store), i);
    gboolean found = !strcmp(item->uid, uid);
    g_object_unref(item);
    if (!found) continue;
    g->quiet = TRUE;
    gtk_single_selection_set_selected(g->selection, i);
    g->quiet = FALSE;
    gtk_grid_view_scroll_to(GTK_GRID_VIEW(g->view), i, GTK_LIST_SCROLL_NONE, NULL);
    return TRUE;
  }
  return FALSE;
}
