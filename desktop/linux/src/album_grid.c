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
  g_free(self->cover_file);
  g_free(self->subtitle);
  G_OBJECT_CLASS(album_item_parent_class)->finalize(obj);
}

static void album_item_class_init(AlbumItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = album_item_finalize;
}

static void album_item_init(AlbumItem *self) {
  (void)self;
}

AlbumItem *album_item_new(const mc_album_info *info) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->title = g_strdup(info->title && *info->title ? info->title : mc_str(MC_STR_UNTITLED));
  self->artist = info->artist && *info->artist ? g_strdup(info->artist) : NULL;
  /* Looked for now, so a reload after a cover arrives (sync fetches them as albums come in)
   * makes a different item, and its tile is bound again. */
  self->cover_file = info->has_art ? cover_path(info->folder) : NULL;
  self->year = info->year;
  self->here = info->folder != NULL;
  return self;
}

static AlbumItem *album_item_new_playlist(const mc_playlist_info *info, const char *cover_file) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->title = g_strdup(info->name && *info->name ? info->name : mc_str(MC_STR_UNTITLED));
  self->cover_file = g_strdup(cover_file);
  self->is_playlist = TRUE;
  self->track_count = info->track_count;
  return self;
}

static AlbumItem *album_item_new_group(const mc_album_group_info *info, const char *cover_file) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->title = g_strdup(info->name && *info->name ? info->name : mc_str(MC_STR_UNTITLED));
  self->cover_file = g_strdup(cover_file);
  self->is_group = TRUE;
  self->track_count = info->album_count;
  return self;
}

/* A smart playlist's or group's item. Its picture is mc_smart_art()'s, and it says what it is
 * beneath the name: counting what it holds would evaluate its rule again. */
static AlbumItem *album_item_new_smart(const mc_smart_info *info, const char *cover_file) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->title = g_strdup(info->name && *info->name ? info->name : mc_str(MC_STR_UNTITLED));
  self->cover_file = g_strdup(cover_file);
  self->is_playlist = info->subject == MC_SMART_TRACKS;
  self->is_group = info->subject == MC_SMART_ALBUMS;
  self->subtitle = g_strdup(mc_str(self->is_group ? MC_STR_SMART_ALBUM_GROUP : MC_STR_SMART_PLAYLIST));
  self->is_smart = TRUE;
  self->random = info->random;
  return self;
}

/* Appends the smart lists of subject with something to play here to items (n of them, with
 * room for every smart list after them), with their pictures as last looked up in art. */
static void add_smart(mc_smart_subject subject, AlbumItem **items, guint *n, mc_smart_list *smart, GHashTable *art) {
  for (int i = 0; smart && i < smart->count; i++)
    if (smart->items[i].subject == subject)
      items[(*n)++] =
        album_item_new_smart(&smart->items[i], art ? g_hash_table_lookup(art, smart->items[i].uid) : NULL);
}

/* A playlist's or group's item with another picture. */
static AlbumItem *album_item_with_cover(const AlbumItem *item, const char *cover_file) {
  AlbumItem *self = g_object_new(ALBUM_TYPE_ITEM, NULL);
  memcpy(self->uid, item->uid, sizeof self->uid);
  self->title = g_strdup(item->title);
  self->cover_file = g_strdup(cover_file);
  self->is_playlist = item->is_playlist;
  self->is_group = item->is_group;
  self->track_count = item->track_count;
  self->subtitle = g_strdup(item->subtitle);
  self->is_smart = item->is_smart;
  self->random = item->random;
  return self;
}

static gboolean album_item_equal(const AlbumItem *a, const AlbumItem *b) {
  return !strcmp(a->uid, b->uid) && !g_strcmp0(a->title, b->title) && !g_strcmp0(a->artist, b->artist) &&
         !g_strcmp0(a->cover_file, b->cover_file) &&
         a->year == b->year && a->is_playlist == b->is_playlist && a->is_group == b->is_group &&
         a->track_count == b->track_count && !g_strcmp0(a->subtitle, b->subtitle) && a->is_smart == b->is_smart &&
         a->random == b->random && a->here == b->here;
}

/* The line beneath the title: the artist, a playlist's track count or a group's album
 * count, unless the item says otherwise. g_free it. */
static char *album_item_subtitle(const AlbumItem *item) {
  if (item->subtitle) return g_strdup(item->subtitle);
  if (item->is_group) {
    char count[64];
    mc_format(count, sizeof count, mc_str_plural(MC_STR_COUNT_ALBUMS, item->track_count), MC_D(item->track_count));
    return g_strdup(count);
  }
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
  GtkWidget *widget, *view; /* widget: the box around the scrolled window; it owns this */
  GtkWidget *popover;       /* album_grid_popup()'s, once there's been one */
  void (*changed)(void *user);
  void *user;
  void (*menu)(void *user, GtkWidget *grid, AlbumItem *item, double x, double y);
  void *menu_user;
  void (*activate)(void *user, AlbumItem *item);
  void *activate_user;
  GdkDragAction (*drop_over)(void *user, AlbumItem *target, const drag_item *item);
  gboolean (*dropped)(void *user, AlbumItem *target, const drag_item *item, gboolean after);
  void *drop_user;
  gboolean groups; /* it shows album groups: their pictures are mc_album_group_art()'s */
  gboolean quiet; /* selection changes aren't the user's */
  GHashTable *art; /* playlist uid -> its picture, or NULL for none, as last looked up */
  guint art_gen;   /* bumped by each lookup, so only the latest one's results are used */
} grid;

static grid *grid_of(GtkWidget *widget) {
  return g_object_get_data(G_OBJECT(widget), "album-grid");
}

static void grid_free(gpointer data) {
  grid *g = data;
  g_object_unref(g->selection);
  if (g->art) g_hash_table_destroy(g->art);
  g_free(g);
}

/* A tile's widgets, and the cover it's waiting for. */
typedef struct cell {
  GtkWidget *picture, *title, *artist;
  GCancellable *loading;
  struct grid *g;    /* the grid it's in, or NULL for a tile on its own */
  GtkListItem *item; /* its place in the grid, while bound */
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

/* A tile's widgets, empty; its cell is the box's "cell". */
static GtkWidget *tile_build(void) {
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
  return box;
}

static void tile_cover_loaded(GdkTexture *texture, gpointer user) {
  cell *c = user;
  g_clear_object(&c->loading);
  gtk_picture_set_paintable(GTK_PICTURE(c->picture), GDK_PAINTABLE(texture));
}

/* Shows album in the tile, loading its cover if it isn't cached. Returns the tile's name for
 * screen readers; g_free it. */
static char *tile_show(GtkWidget *box, AlbumItem *album) {
  cell *c = g_object_get_data(G_OBJECT(box), "cell");
  char *subtitle = album_item_subtitle(album);
  gtk_label_set_text(GTK_LABEL(c->title), album->title);
  gtk_label_set_text(GTK_LABEL(c->artist), subtitle);
  char *formatted = mc_format_alloc(mc_str(MC_STR_LINUX_ALBUM_TILE_NAME), MC_S(album->title), MC_S(subtitle));
  char *name = g_strdup(formatted ? formatted : album->title);
  mc_free(formatted);
  g_free(subtitle);

  cell_cancel(c);
  gtk_picture_set_paintable(GTK_PICTURE(c->picture), NULL);
  if (!album->cover_file) return name;
  int size = COVER_SIZE * gtk_widget_get_scale_factor(box);
  GdkTexture *texture = NULL;
  if (cover_lookup_file(album->cover_file, size, &texture)) {
    gtk_picture_set_paintable(GTK_PICTURE(c->picture), GDK_PAINTABLE(texture));
    g_clear_object(&texture);
  } else {
    c->loading = g_cancellable_new();
    cover_load_file(album->cover_file, size, c->loading, tile_cover_loaded, c);
  }
  return name;
}

/* Right-click: the grid's menu for the tile's item, where the pointer is. */
static void on_tile_menu(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  cell *c = user;
  AlbumItem *album = c->item ? gtk_list_item_get_item(c->item) : NULL;
  if (!c->g || !c->g->menu || !album) return;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  GtkWidget *tile = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
  graphene_point_t at;
  if (!gtk_widget_compute_point(tile, c->g->widget, &GRAPHENE_POINT_INIT((float)x, (float)y), &at)) return;
  c->g->menu(c->g->menu_user, c->g->widget, album, at.x, at.y);
}

/* --- Dragging albums, and drops on tiles (drag.h) --- */

/* An album's tile drags its album; playlists and groups don't drag. */
static gboolean on_tile_drag(gpointer user, drag_item *item) {
  cell *c = user;
  AlbumItem *album = c->item ? gtk_list_item_get_item(c->item) : NULL;
  if (!album || album->is_playlist || album->is_group) return FALSE;
  item->kind = DRAG_ALBUM;
  g_strlcpy(item->uid, album->uid, sizeof item->uid);
  return TRUE;
}

/* Marks the tile for what a drop would do: a ring for adding to it, a line before or after
 * it for moving there; 0 unmarks. */
static void mark_tile(GtkWidget *tile, GdkDragAction action, gboolean after) {
  const char *classes[] = {"drop-into", "drop-before", "drop-after"};
  int on = action == GDK_ACTION_COPY ? 0 : action == GDK_ACTION_MOVE ? (after ? 2 : 1) : -1;
  for (int i = 0; i < 3; i++) {
    if (i == on) gtk_widget_add_css_class(tile, classes[i]);
    else gtk_widget_remove_css_class(tile, classes[i]);
  }
}

static gboolean drop_after(GtkWidget *tile, double x) {
  return x > gtk_widget_get_width(tile) / 2.0;
}

static GdkDragAction on_tile_drop_motion(GtkDropTarget *target, double x, double y, gpointer user) {
  (void)y;
  cell *c = user;
  GtkWidget *tile = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  AlbumItem *album = c->item ? gtk_list_item_get_item(c->item) : NULL;
  GdkDragAction action = album && c->g && c->g->drop_over && drag_from_us(target)
                           ? c->g->drop_over(c->g->drop_user, album, drag_current())
                           : 0;
  mark_tile(tile, action, drop_after(tile, x));
  return action;
}

static void on_tile_drop_leave(GtkDropTarget *target, gpointer user) {
  (void)user;
  mark_tile(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)), 0, FALSE);
}

static gboolean on_tile_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user) {
  (void)value;
  (void)y;
  cell *c = user;
  GtkWidget *tile = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  mark_tile(tile, 0, FALSE);
  AlbumItem *album = c->item ? gtk_list_item_get_item(c->item) : NULL;
  if (!album || !c->g || !c->g->dropped || !drag_from_us(target)) return FALSE;
  drag_item item = *drag_current(); /* copies: dropping reloads the grid, and this tile with it */
  g_object_ref(album);
  gboolean done = c->g->dropped(c->g->drop_user, album, &item, drop_after(tile, x));
  g_object_unref(album);
  return done;
}

static void tile_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  GtkWidget *box = tile_build();
  cell *c = g_object_get_data(G_OBJECT(box), "cell");
  c->g = user;
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_tile_menu), c);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  drag_source_new(box, on_tile_drag, c);
  GtkDropTarget *drop = drag_target_new(GDK_ACTION_COPY | GDK_ACTION_MOVE);
  g_signal_connect(drop, "motion", G_CALLBACK(on_tile_drop_motion), c);
  g_signal_connect(drop, "leave", G_CALLBACK(on_tile_drop_leave), c);
  g_signal_connect(drop, "drop", G_CALLBACK(on_tile_drop), c);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));
  gtk_list_item_set_child(item, box);
}

static void tile_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "cell");
  c->item = item;
  char *name = tile_show(gtk_list_item_get_child(item), gtk_list_item_get_item(item));
  gtk_list_item_set_accessible_label(item, name);
  g_free(name);
}

GtkWidget *album_tile_new(AlbumItem *album) {
  GtkWidget *box = tile_build();
  char *name = tile_show(box, album);
  gtk_accessible_update_property(GTK_ACCESSIBLE(box), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_free(name);
  return box;
}

static void tile_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  cell *c = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "cell");
  c->item = NULL;
  cell_cancel(c);
  gtk_picture_set_paintable(GTK_PICTURE(c->picture), NULL);
}

static void on_selected(GObject *obj, GParamSpec *pspec, gpointer user) {
  (void)obj;
  (void)pspec;
  grid *g = user;
  if (!g->quiet && g->changed) g->changed(g->user);
}

static void on_activate(GtkGridView *view, guint position, gpointer user) {
  (void)view;
  grid *g = user;
  AlbumItem *album = g_list_model_get_item(G_LIST_MODEL(g->store), position);
  if (album && g->activate) g->activate(g->activate_user, album);
  if (album) g_object_unref(album);
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
  g_signal_connect(factory, "setup", G_CALLBACK(tile_setup), g);
  g_signal_connect(factory, "bind", G_CALLBACK(tile_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(tile_unbind), NULL);
  /* The view takes both; the selection stays ours too. */
  GtkWidget *view = gtk_grid_view_new(GTK_SELECTION_MODEL(g_object_ref(g->selection)), factory);
  g->view = view;
  gtk_grid_view_set_min_columns(GTK_GRID_VIEW(view), 1);
  gtk_grid_view_set_max_columns(GTK_GRID_VIEW(view), 24);
  gtk_widget_add_css_class(view, "album-grid");
  gtk_accessible_update_property(GTK_ACCESSIBLE(view), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  g_signal_connect(view, "activate", G_CALLBACK(on_activate), g);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
  gtk_widget_set_hexpand(scroller, TRUE);
  gtk_widget_set_vexpand(scroller, TRUE);
  /* In a box, which places popovers (a scrolled window doesn't). */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), scroller);
  g->widget = box;
  g_object_set_data_full(G_OBJECT(box), "album-grid", g, grid_free);
  return box;
}

void album_grid_popup(GtkWidget *widget, GMenuModel *menu, double x, double y) {
  grid *g = grid_of(widget);
  if (!g->popover) {
    g->popover = gtk_popover_menu_new_from_model(NULL);
    gtk_popover_set_has_arrow(GTK_POPOVER(g->popover), FALSE);
    gtk_widget_set_halign(g->popover, GTK_ALIGN_START);
    gtk_widget_set_parent(g->popover, widget);
  }
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(g->popover), menu);
  gtk_popover_set_pointing_to(GTK_POPOVER(g->popover), &(GdkRectangle){(int)x, (int)y, 1, 1});
  gtk_popover_popup(GTK_POPOVER(g->popover));
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

/* --- Playlists' pictures --- */

/* A lookup of every playlist's picture, on a worker thread with a library connection of its
 * own: a picture that isn't cached yet takes decoding four covers. */
typedef struct art_job {
  GtkWidget *widget; /* the grid, kept alive until the job is done */
  guint gen;
  char *lib_dir, *art_dir;
  gboolean groups; /* album groups' pictures rather than playlists' */
  char **uids;  /* NULL-terminated */
  char **paths; /* results, one per uid, NULL for none */
  guint n;
} art_job;

static void art_job_free(gpointer data) {
  art_job *j = data;
  g_object_unref(j->widget);
  g_free(j->lib_dir);
  g_free(j->art_dir);
  g_strfreev(j->uids);
  for (guint i = 0; i < j->n; i++) g_free(j->paths[i]);
  g_free(j->paths);
  g_free(j);
}

static void art_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  (void)cancellable;
  art_job *j = data;
  mc_library *lib = NULL;
  if (mc_library_open(j->lib_dir, &lib) == MC_OK) {
    for (guint i = 0; i < j->n; i++) {
      char *path = NULL;
      mc_status st = j->groups ? mc_album_group_art(lib, j->uids[i], j->art_dir, &path)
                               : mc_playlist_art(lib, j->uids[i], j->art_dir, &path);
      if (st == MC_ERR_NOT_FOUND) st = mc_smart_art(lib, j->uids[i], j->art_dir, &path); /* uids never clash */
      if (st == MC_OK && path) j->paths[i] = g_strdup(path);
      mc_free(path);
    }
    mc_library_close(lib);
  }
  g_task_return_boolean(task, TRUE);
}

/* On the UI thread: the playlists whose pictures changed get new items. */
static void art_done(GObject *source, GAsyncResult *result, gpointer user) {
  (void)source;
  (void)user;
  art_job *j = g_task_get_task_data(G_TASK(result));
  grid *g = grid_of(j->widget);
  if (!g || j->gen != g->art_gen) return; /* a later lookup is on its way */

  GHashTable *old = g->art;
  g->art = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  for (guint i = 0; i < j->n; i++) g_hash_table_insert(g->art, g_strdup(j->uids[i]), g_strdup(j->paths[i]));
  if (old) g_hash_table_destroy(old);

  guint n = g_list_model_get_n_items(G_LIST_MODEL(g->store));
  AlbumItem **items = g_new0(AlbumItem *, n + 1);
  gboolean changed = FALSE, selected_changed = FALSE;
  AlbumItem *sel = album_grid_selection(j->widget);
  for (guint i = 0; i < n; i++) {
    AlbumItem *a = g_list_model_get_item(G_LIST_MODEL(g->store), i);
    const char *path = g_hash_table_lookup(g->art, a->uid);
    if (g_hash_table_contains(g->art, a->uid) && g_strcmp0(path, a->cover_file)) {
      items[i] = album_item_with_cover(a, path);
      changed = TRUE;
      selected_changed |= a == sel;
      g_object_unref(a);
    } else {
      items[i] = a;
    }
  }
  if (changed) {
    char keep[MC_UID_STRLEN];
    grid_keep(j->widget, keep);
    grid_set_items(g, items, n, keep);
  }
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  if (selected_changed && g->changed) g->changed(g->user); /* the track list shows it too */
}

/* Looks up the pictures of the grid's playlists again, as they may have changed. */
static void art_lookup(GtkWidget *widget, mc_library *lib, const char *art_dir) {
  grid *g = grid_of(widget);
  g->art_gen++;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(g->store));
  if (!lib || !art_dir || !n) return;
  art_job *j = g_new0(art_job, 1);
  j->widget = g_object_ref(widget);
  j->gen = g->art_gen;
  j->lib_dir = g_strdup(mc_library_dir(lib));
  j->art_dir = g_strdup(art_dir);
  j->groups = g->groups;
  j->uids = g_new0(char *, n + 1);
  j->paths = g_new0(char *, n);
  for (guint i = 0; i < n; i++) {
    AlbumItem *a = g_list_model_get_item(G_LIST_MODEL(g->store), i);
    j->uids[j->n++] = g_strdup(a->uid);
    g_object_unref(a);
  }
  GTask *task = g_task_new(NULL, NULL, art_done, NULL);
  g_task_set_task_data(task, j, art_job_free);
  g_task_run_in_thread(task, art_thread);
  g_object_unref(task);
}

int album_grid_reload_playlists(GtkWidget *widget, mc_library *lib, const char *art_dir, int *hidden) {
  char keep[MC_UID_STRLEN];
  grid_keep(widget, keep);
  grid *g = grid_of(widget);
  mc_playlist_list *list = NULL;
  if (lib && mc_library_list_playlists(lib, MC_LIST_PLAYABLE, &list) != MC_OK) list = NULL;
  mc_smart_list *smart = NULL;
  if (lib && mc_library_list_smart(lib, MC_LIST_PLAYABLE, &smart) != MC_OK) smart = NULL;
  guint count = list ? (guint)list->count : 0, n = 0;
  *hidden = list ? list->hidden : 0;
  AlbumItem **items = g_new0(AlbumItem *, count + (smart ? (guint)smart->count : 0) + 1);
  for (guint i = 0; i < count; i++) {
    const char *known = g->art ? g_hash_table_lookup(g->art, list->items[i].uid) : NULL;
    items[n++] = album_item_new_playlist(&list->items[i], known);
  }
  mc_playlist_list_free(list);
  add_smart(MC_SMART_TRACKS, items, &n, smart, g->art);
  mc_smart_list_free(smart);

  grid_set_items(g, items, n, keep);
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  art_lookup(widget, lib, art_dir);
  return (int)n;
}

int album_grid_reload_groups(GtkWidget *widget, mc_library *lib, const char *art_dir) {
  char keep[MC_UID_STRLEN];
  grid_keep(widget, keep);
  grid *g = grid_of(widget);
  g->groups = TRUE;
  mc_album_group_list *list = NULL;
  if (lib && mc_library_list_album_groups(lib, &list) != MC_OK) list = NULL;
  mc_smart_list *smart = NULL;
  if (lib && mc_library_list_smart(lib, MC_LIST_PLAYABLE, &smart) != MC_OK) smart = NULL;
  guint n = list ? (guint)list->count : 0;
  AlbumItem **items = g_new0(AlbumItem *, n + (smart ? (guint)smart->count : 0) + 1);
  for (guint i = 0; i < n; i++) {
    const char *known = g->art ? g_hash_table_lookup(g->art, list->items[i].uid) : NULL;
    items[i] = album_item_new_group(&list->items[i], known);
  }
  mc_album_group_list_free(list);
  add_smart(MC_SMART_ALBUMS, items, &n, smart, g->art);
  mc_smart_list_free(smart);

  grid_set_items(g, items, n, keep);
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  art_lookup(widget, lib, art_dir);
  return (int)n;
}

int album_grid_reload_group_albums(GtkWidget *widget, mc_library *lib, const char *group_uid, gboolean *found) {
  char keep[MC_UID_STRLEN];
  grid_keep(widget, keep);
  mc_album_list *list = NULL;
  mc_status st = lib && group_uid ? mc_library_list_album_group_albums(lib, group_uid, &list) : MC_ERR_NOT_FOUND;
  if (st == MC_ERR_NOT_FOUND && lib && group_uid) { /* not a manual group: a smart one? */
    mc_album_list_free(list);
    list = NULL;
    st = mc_library_list_smart_albums(lib, group_uid, &list);
    if (st == MC_ERR_INVALID_ARG) st = MC_ERR_NOT_FOUND; /* a smart playlist */
  }
  if (st != MC_OK) list = NULL;
  *found = st != MC_ERR_NOT_FOUND;
  guint n = list ? (guint)list->count : 0;
  AlbumItem **items = g_new0(AlbumItem *, n + 1);
  for (guint i = 0; i < n; i++) items[i] = album_item_new(&list->items[i]);
  mc_album_list_free(list);

  grid_set_items(grid_of(widget), items, n, keep);
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  return (int)n;
}

void album_grid_set_menu(GtkWidget *widget,
                         void (*menu)(void *user, GtkWidget *grid, AlbumItem *item, double x, double y),
                         void *user) {
  grid *g = grid_of(widget);
  g->menu = menu;
  g->menu_user = user;
}

void album_grid_set_drop(GtkWidget *widget, GdkDragAction (*over)(void *user, AlbumItem *target, const drag_item *item),
                         gboolean (*dropped)(void *user, AlbumItem *target, const drag_item *item, gboolean after),
                         void *user) {
  grid *g = grid_of(widget);
  g->drop_over = over;
  g->dropped = dropped;
  g->drop_user = user;
}

void album_grid_set_activate(GtkWidget *widget, gboolean single_click, void (*activate)(void *user, AlbumItem *item),
                             void *user) {
  grid *g = grid_of(widget);
  g->activate = activate;
  g->activate_user = user;
  gtk_grid_view_set_single_click_activate(GTK_GRID_VIEW(g->view), single_click);
}

void album_grid_focus(GtkWidget *widget) {
  gtk_widget_grab_focus(grid_of(widget)->view);
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

AlbumItem *album_grid_find(GtkWidget *widget, const char *uid) {
  grid *g = grid_of(widget);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(g->store));
  for (guint i = 0; i < n; i++) {
    AlbumItem *item = g_list_model_get_item(G_LIST_MODEL(g->store), i);
    if (!strcmp(item->uid, uid)) return item;
    g_object_unref(item);
  }
  return NULL;
}

int album_grid_index(GtkWidget *widget, const char *uid) {
  grid *g = grid_of(widget);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(g->store));
  for (guint i = 0; i < n; i++) {
    AlbumItem *item = g_list_model_get_item(G_LIST_MODEL(g->store), i);
    gboolean found = !strcmp(item->uid, uid);
    g_object_unref(item);
    if (found) return (int)i;
  }
  return -1;
}

const char *album_grid_uid_at(GtkWidget *widget, int i) {
  grid *g = grid_of(widget);
  if (i < 0 || (guint)i >= g_list_model_get_n_items(G_LIST_MODEL(g->store))) return NULL;
  AlbumItem *item = g_list_model_get_item(G_LIST_MODEL(g->store), (guint)i);
  const char *uid = item->uid;
  g_object_unref(item); /* the store still holds it */
  return uid;
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
