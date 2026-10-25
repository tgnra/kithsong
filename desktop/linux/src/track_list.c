#include "track_list.h"

#include <string.h>

#include "cover.h"

enum { HEADER_COVER = 96 };

/* --- TrackItem: one row --- */

#define TRACK_TYPE_ITEM (track_item_get_type())
G_DECLARE_FINAL_TYPE(TrackItem, track_item, TRACK, ITEM, GObject)

struct _TrackItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *title;
  char num[16];      /* "3", "2-5" with several discs, or "" */
  char duration[16]; /* "4:44", or "" if unknown */
  int duration_ms;
  char *path;
};

G_DEFINE_FINAL_TYPE(TrackItem, track_item, G_TYPE_OBJECT)

static void track_item_finalize(GObject *obj) {
  TrackItem *self = TRACK_ITEM(obj);
  g_free(self->title);
  g_free(self->path);
  G_OBJECT_CLASS(track_item_parent_class)->finalize(obj);
}

static void track_item_class_init(TrackItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = track_item_finalize;
}

static void track_item_init(TrackItem *self) {
  (void)self;
}

static void format_duration(int ms, char *out, size_t n) {
  int sec = (ms + 500) / 1000;
  if (ms <= 0) out[0] = '\0';
  else if (sec >= 3600) g_snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else g_snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

/* --- List --- */

typedef struct row {
  GtkWidget *num, *speaker, *title, *duration;
  TrackItem *track; /* bound to, or NULL */
} row;

static struct {
  void (*play)(void *user, int index);
  void (*close)(void *user);
  void *user;

  GListStore *store;
  GtkSingleSelection *selection;
  GtkWidget *view, *scroller;
  GtkWidget *cover, *title, *artist, *summary;
  GCancellable *cover_loading;
  AlbumItem *album;
  GPtrArray *rows; /* bound rows, to mark the playing one */
  char playing_uid[MC_UID_STRLEN];
} s;

static void row_update(row *r) {
  gboolean playing = r->track && s.playing_uid[0] && !strcmp(r->track->uid, s.playing_uid);
  gtk_widget_set_visible(r->num, !playing);
  gtk_widget_set_visible(r->speaker, playing);
  GtkWidget *accented[] = {r->title, r->duration};
  for (size_t i = 0; i < G_N_ELEMENTS(accented); i++) {
    if (playing) gtk_widget_add_css_class(accented[i], "accent");
    else gtk_widget_remove_css_class(accented[i], "accent");
  }
  /* Dimmed unless playing: the accent colour shouldn't be faded. */
  if (playing) gtk_widget_remove_css_class(r->duration, "dim-label");
  else gtk_widget_add_css_class(r->duration, "dim-label");
}

static GtkWidget *row_label(float xalign) {
  GtkWidget *l = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(l), xalign);
  return l;
}

static void row_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  row *r = g_new0(row, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);

  /* The number, or a speaker on the playing track, in a column of fixed width. */
  GtkWidget *lead = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_size_request(lead, 28, -1);
  r->num = row_label(1);
  gtk_widget_set_hexpand(r->num, TRUE);
  gtk_widget_add_css_class(r->num, "dim-label");
  gtk_widget_add_css_class(r->num, "numeric");
  gtk_box_append(GTK_BOX(lead), r->num);
  r->speaker = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
  gtk_widget_set_hexpand(r->speaker, TRUE);
  gtk_widget_set_halign(r->speaker, GTK_ALIGN_END);
  gtk_widget_add_css_class(r->speaker, "accent");
  gtk_box_append(GTK_BOX(lead), r->speaker);
  gtk_box_append(GTK_BOX(box), lead);

  r->title = row_label(0);
  gtk_label_set_ellipsize(GTK_LABEL(r->title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(r->title, TRUE);
  gtk_box_append(GTK_BOX(box), r->title);

  r->duration = row_label(1);
  gtk_widget_add_css_class(r->duration, "numeric");
  gtk_box_append(GTK_BOX(box), r->duration);

  g_object_set_data_full(G_OBJECT(box), "row", r, g_free);
  gtk_list_item_set_child(item, box);
}

static void row_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  TrackItem *t = gtk_list_item_get_item(item);
  row *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "row");
  r->track = t;
  gtk_label_set_text(GTK_LABEL(r->num), t->num);
  gtk_label_set_text(GTK_LABEL(r->title), t->title);
  gtk_label_set_text(GTK_LABEL(r->duration), t->duration);
  gtk_widget_set_tooltip_text(r->title, t->path ? NULL : "Not on this device");
  gtk_widget_set_sensitive(r->title, t->path != NULL);
  gtk_list_item_set_accessible_label(item, t->title);
  row_update(r);
  g_ptr_array_add(s.rows, r);
}

static void row_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  row *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "row");
  r->track = NULL;
  g_ptr_array_remove_fast(s.rows, r);
}

static void on_activate(GtkListView *view, guint position, gpointer user) {
  (void)view;
  (void)user;
  if (s.play) s.play(s.user, (int)position);
}

static void on_close(GtkWidget *widget, gpointer user) {
  (void)widget;
  (void)user;
  if (s.close) s.close(s.user);
}

static gboolean on_escape(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)args;
  on_close(widget, user);
  return TRUE;
}

static void cover_loaded(GdkTexture *texture, gpointer user) {
  (void)user;
  g_clear_object(&s.cover_loading);
  gtk_picture_set_paintable(GTK_PICTURE(s.cover), GDK_PAINTABLE(texture));
}

static GtkWidget *build_header(void) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 18);
  gtk_widget_set_margin_bottom(header, 18);
  gtk_box_append(GTK_BOX(header), cover_frame_new(HEADER_COVER, HEADER_COVER / 3, &s.cover));

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  s.title = gtk_label_new(NULL);
  gtk_widget_add_css_class(s.title, "title-3");
  gtk_label_set_xalign(GTK_LABEL(s.title), 0);
  gtk_label_set_wrap(GTK_LABEL(s.title), TRUE);
  gtk_label_set_lines(GTK_LABEL(s.title), 2);
  gtk_label_set_ellipsize(GTK_LABEL(s.title), PANGO_ELLIPSIZE_END);
  gtk_label_set_selectable(GTK_LABEL(s.title), FALSE);
  gtk_box_append(GTK_BOX(text), s.title);
  s.artist = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(s.artist), 0);
  gtk_label_set_ellipsize(GTK_LABEL(s.artist), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(text), s.artist);
  s.summary = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(s.summary), 0);
  gtk_label_set_ellipsize(GTK_LABEL(s.summary), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(s.summary, "dim-label");
  gtk_widget_add_css_class(s.summary, "caption");
  gtk_box_append(GTK_BOX(text), s.summary);
  gtk_box_append(GTK_BOX(header), text);

  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_set_tooltip_text(close, "Close");
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_add_css_class(close, "circular");
  gtk_widget_set_valign(close, GTK_ALIGN_START);
  g_signal_connect(close, "clicked", G_CALLBACK(on_close), NULL);
  gtk_box_append(GTK_BOX(header), close);
  return header;
}

GtkWidget *track_list_new(void (*play)(void *user, int index), void (*close)(void *user), void *user) {
  s.play = play;
  s.close = close;
  s.user = user;
  s.rows = g_ptr_array_new();
  s.store = g_list_store_new(TRACK_TYPE_ITEM);
  s.selection = gtk_single_selection_new(G_LIST_MODEL(s.store));
  gtk_single_selection_set_autoselect(s.selection, FALSE);
  gtk_single_selection_set_can_unselect(s.selection, TRUE);

  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(row_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(row_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(row_unbind), NULL);
  s.view = gtk_list_view_new(GTK_SELECTION_MODEL(s.selection), factory);
  gtk_widget_add_css_class(s.view, "navigation-sidebar");
  gtk_widget_add_css_class(s.view, "track-list");
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.view), GTK_ACCESSIBLE_PROPERTY_LABEL, "Tracks", -1);
  g_signal_connect(s.view, "activate", G_CALLBACK(on_activate), NULL);
  s.scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s.scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s.scroller), s.view);
  gtk_widget_set_vexpand(s.scroller, TRUE);

  GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(pane), build_header());
  gtk_box_append(GTK_BOX(pane), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(pane), s.scroller);

  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_callback_action_new(on_escape, NULL, NULL)));
  gtk_widget_add_controller(pane, keys);
  return pane;
}

/* "1997 · 12 tracks · 47 min" */
static void set_summary(int year, int count, int total_ms) {
  GString *str = g_string_new(NULL);
  if (year > 0) g_string_append_printf(str, "%d · ", year);
  int min = (total_ms + 30000) / 60000;
  if (!count) g_string_append(str, "No tracks");
  else g_string_append_printf(str, "%d %s", count, count == 1 ? "track" : "tracks");
  if (count && min >= 60) g_string_append_printf(str, " · %d hr %d min", min / 60, min % 60);
  else if (count) g_string_append_printf(str, " · %d min", min);
  gtk_label_set_text(GTK_LABEL(s.summary), str->str);
  g_string_free(str, TRUE);
}

void track_list_show(mc_library *lib, AlbumItem *album) {
  gboolean same = album && s.album && !strcmp(album->uid, s.album->uid);
  char keep[MC_UID_STRLEN] = "";
  TrackItem *sel = gtk_single_selection_get_selected_item(s.selection);
  if (same && sel) memcpy(keep, sel->uid, sizeof keep);

  if (album) g_object_ref(album);
  g_clear_object(&s.album);
  s.album = album;

  mc_track_list *list = NULL;
  if (lib && album && mc_library_list_album_tracks(lib, album->uid, &list) != MC_OK) list = NULL;
  int n = list ? list->count : 0, total_ms = 0;
  gboolean discs = FALSE;
  for (int i = 0; i < n; i++) discs |= list->items[i].disc_no > 1;
  TrackItem **items = g_new0(TrackItem *, (gsize)n + 1);
  guint selected = GTK_INVALID_LIST_POSITION;
  for (int i = 0; i < n; i++) {
    const mc_track_info *t = &list->items[i];
    TrackItem *it = items[i] = g_object_new(TRACK_TYPE_ITEM, NULL);
    memcpy(it->uid, t->uid, sizeof it->uid);
    it->title = g_strdup(t->title && *t->title ? t->title : "Untitled");
    if (t->track_no && discs) g_snprintf(it->num, sizeof it->num, "%d-%d", t->disc_no ? t->disc_no : 1, t->track_no);
    else if (t->track_no) g_snprintf(it->num, sizeof it->num, "%d", t->track_no);
    format_duration(t->duration_ms, it->duration, sizeof it->duration);
    it->duration_ms = t->duration_ms;
    it->path = g_strdup(t->path);
    total_ms += t->duration_ms > 0 ? t->duration_ms : 0;
    if (keep[0] && !strcmp(it->uid, keep)) selected = (guint)i;
  }
  mc_track_list_free(list);
  g_list_store_splice(s.store, 0, g_list_model_get_n_items(G_LIST_MODEL(s.store)), (gpointer *)items, (guint)n);
  gtk_single_selection_set_selected(s.selection, selected);
  for (int i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);

  gtk_label_set_text(GTK_LABEL(s.title), album ? album->title : "");
  gtk_label_set_text(GTK_LABEL(s.artist), album && album->artist ? album->artist : "Unknown Artist");
  set_summary(album ? album->year : 0, n, total_ms);
  if (!same) {
    gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s.scroller)), 0);
    if (s.cover_loading) g_cancellable_cancel(s.cover_loading);
    g_clear_object(&s.cover_loading);
    GdkTexture *texture = NULL;
    if (album && album->cover_dir) {
      int size = HEADER_COVER * gtk_widget_get_scale_factor(s.cover);
      s.cover_loading = g_cancellable_new();
      texture = cover_get(album->cover_dir, size, s.cover_loading, cover_loaded, NULL);
    }
    gtk_picture_set_paintable(GTK_PICTURE(s.cover), GDK_PAINTABLE(texture));
    g_clear_object(&texture);
  }
}

AlbumItem *track_list_album(void) {
  return s.album;
}

int track_list_count(void) {
  return (int)g_list_model_get_n_items(G_LIST_MODEL(s.store));
}

track_list_item track_list_item_at(int i) {
  track_list_item item = {0};
  if (i < 0 || i >= track_list_count()) return item;
  TrackItem *t = g_list_model_get_item(G_LIST_MODEL(s.store), (guint)i);
  item.uid = t->uid;
  item.title = t->title;
  item.duration_ms = t->duration_ms;
  item.path = t->path;
  g_object_unref(t); /* the store keeps it */
  return item;
}

void track_list_set_playing(const char *uid) {
  const char *now = uid ? uid : "";
  if (!strcmp(now, s.playing_uid) || strlen(now) >= MC_UID_STRLEN) return;
  strcpy(s.playing_uid, now);
  for (guint i = 0; s.rows && i < s.rows->len; i++) row_update(g_ptr_array_index(s.rows, i));
}
