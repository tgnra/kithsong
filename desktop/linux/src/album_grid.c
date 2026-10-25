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
  self->title = g_strdup(info->title && *info->title ? info->title : "Untitled");
  self->artist = info->artist && *info->artist ? g_strdup(info->artist) : NULL;
  self->cover_dir = info->has_art && info->folder ? g_strdup(info->folder) : NULL;
  self->year = info->year;
  return self;
}

static gboolean album_item_equal(const AlbumItem *a, const AlbumItem *b) {
  return !strcmp(a->uid, b->uid) && !g_strcmp0(a->title, b->title) && !g_strcmp0(a->artist, b->artist) &&
         !g_strcmp0(a->cover_dir, b->cover_dir) && a->year == b->year;
}

/* --- Grid --- */

static struct {
  GListStore *store;
  GtkSingleSelection *selection;
  GtkWidget *view;
  void (*changed)(void *user);
  void *user;
  gboolean quiet; /* selection changes aren't the user's */
} g;

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
  gtk_label_set_text(GTK_LABEL(c->title), album->title);
  gtk_label_set_text(GTK_LABEL(c->artist), album->artist ? album->artist : "Unknown Artist");
  char *name = g_strdup_printf("%s, %s", album->title, album->artist ? album->artist : "Unknown Artist");
  gtk_list_item_set_accessible_label(item, name);
  g_free(name);

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
  (void)user;
  if (!g.quiet && g.changed) g.changed(g.user);
}

GtkWidget *album_grid_new(void (*changed)(void *user), void *user) {
  g.changed = changed;
  g.user = user;
  g.store = g_list_store_new(ALBUM_TYPE_ITEM);
  g.selection = gtk_single_selection_new(G_LIST_MODEL(g.store)); /* takes the store */
  gtk_single_selection_set_autoselect(g.selection, FALSE);
  gtk_single_selection_set_can_unselect(g.selection, TRUE);
  g_signal_connect(g.selection, "notify::selected", G_CALLBACK(on_selected), NULL);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(tile_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(tile_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(tile_unbind), NULL);
  /* The view takes both; the selection stays ours too. */
  GtkWidget *grid = gtk_grid_view_new(GTK_SELECTION_MODEL(g_object_ref(g.selection)), factory);
  g.view = grid;
  gtk_grid_view_set_min_columns(GTK_GRID_VIEW(grid), 1);
  gtk_grid_view_set_max_columns(GTK_GRID_VIEW(grid), 24);
  gtk_widget_add_css_class(grid, "album-grid");
  gtk_accessible_update_property(GTK_ACCESSIBLE(grid), GTK_ACCESSIBLE_PROPERTY_LABEL, "Albums", -1);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), grid);
  g_signal_connect_swapped(scroller, "destroy", G_CALLBACK(g_object_unref), g.selection);
  return scroller;
}

int album_grid_reload(mc_library *lib) {
  char keep[MC_UID_STRLEN] = "";
  AlbumItem *sel = album_grid_selection();
  if (sel) memcpy(keep, sel->uid, sizeof keep);

  mc_album_list *list = NULL;
  if (lib && mc_library_list_albums(lib, &list) != MC_OK) list = NULL;
  guint n = list ? (guint)list->count : 0;
  AlbumItem **items = g_new0(AlbumItem *, n + 1);
  guint selected = GTK_INVALID_LIST_POSITION;
  for (guint i = 0; i < n; i++) {
    items[i] = album_item_new(&list->items[i]);
    if (keep[0] && !strcmp(items[i]->uid, keep)) selected = i;
  }
  mc_album_list_free(list);

  /* Replace only the run that changed (an import usually adds an album or two), so the
   * tiles either side keep their covers and the view doesn't jump. */
  guint old = g_list_model_get_n_items(G_LIST_MODEL(g.store)), head = 0, tail = 0;
  while (head < old && head < n) {
    AlbumItem *a = g_list_model_get_item(G_LIST_MODEL(g.store), head);
    gboolean same = album_item_equal(a, items[head]);
    g_object_unref(a);
    if (!same) break;
    head++;
  }
  while (tail < old - head && tail < n - head) {
    AlbumItem *a = g_list_model_get_item(G_LIST_MODEL(g.store), old - 1 - tail);
    gboolean same = album_item_equal(a, items[n - 1 - tail]);
    g_object_unref(a);
    if (!same) break;
    tail++;
  }
  g.quiet = TRUE;
  if (head + tail < old || head + tail < n)
    g_list_store_splice(g.store, head, old - head - tail, (gpointer *)(items + head), n - head - tail);
  gtk_single_selection_set_selected(g.selection, selected);
  g.quiet = FALSE;
  if (!old && n) gtk_grid_view_scroll_to(GTK_GRID_VIEW(g.view), 0, GTK_LIST_SCROLL_NONE, NULL); /* start at the top */

  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  return (int)n;
}

AlbumItem *album_grid_selection(void) {
  return g.selection ? gtk_single_selection_get_selected_item(g.selection) : NULL;
}

void album_grid_unselect(void) {
  g.quiet = TRUE;
  gtk_single_selection_set_selected(g.selection, GTK_INVALID_LIST_POSITION);
  g.quiet = FALSE;
}
