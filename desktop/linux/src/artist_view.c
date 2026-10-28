#include "artist_view.h"

#include <string.h>

#include "album_grid.h"
#include "delete_dialog.h"
#include "playback.h"
#include "recents.h"

enum { LIST_AVATAR = 40, PAGE_AVATAR = 96 };

/* --- ArtistItem: one row of the list --- */

#define ARTIST_TYPE_ITEM (artist_item_get_type())
G_DECLARE_FINAL_TYPE(ArtistItem, artist_item, ARTIST, ITEM, GObject)

struct _ArtistItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *name; /* "Unknown artist" if it has none */
  char *sub;  /* "12 albums", or NULL if it's only credited on tracks */
};

G_DEFINE_FINAL_TYPE(ArtistItem, artist_item, G_TYPE_OBJECT)

static void artist_item_finalize(GObject *obj) {
  ArtistItem *self = ARTIST_ITEM(obj);
  g_free(self->name);
  g_free(self->sub);
  G_OBJECT_CLASS(artist_item_parent_class)->finalize(obj);
}

static void artist_item_class_init(ArtistItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = artist_item_finalize;
}

static void artist_item_init(ArtistItem *self) {
  (void)self;
}

static ArtistItem *artist_item_new(const mc_artist_info *info) {
  ArtistItem *self = g_object_new(ARTIST_TYPE_ITEM, NULL);
  memcpy(self->uid, info->uid, sizeof self->uid);
  self->name = g_strdup(info->name && *info->name ? info->name : mc_str(MC_STR_UNKNOWN_ARTIST));
  if (info->album_count > 0) {
    char sub[64];
    mc_format(sub, sizeof sub, mc_str_plural(MC_STR_COUNT_ALBUMS, info->album_count), MC_D(info->album_count));
    self->sub = g_strdup(sub);
  }
  return self;
}

static gboolean artist_item_equal(const ArtistItem *a, const ArtistItem *b) {
  return !strcmp(a->uid, b->uid) && !strcmp(a->name, b->name) && !g_strcmp0(a->sub, b->sub);
}

/* --- ArtistTrack: one row of an artist's tracks --- */

#define ARTIST_TYPE_TRACK (artist_track_get_type())
G_DECLARE_FINAL_TYPE(ArtistTrack, artist_track, ARTIST, TRACK, GObject)

struct _ArtistTrack {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *title, *artist, *album, *cover_dir, *path; /* as mc_track_info has them */
  int duration_ms;
  char duration[16]; /* "4:44", or "" if unknown */
};

G_DEFINE_FINAL_TYPE(ArtistTrack, artist_track, G_TYPE_OBJECT)

static void artist_track_finalize(GObject *obj) {
  ArtistTrack *self = ARTIST_TRACK(obj);
  g_free(self->title);
  g_free(self->artist);
  g_free(self->album);
  g_free(self->cover_dir);
  g_free(self->path);
  G_OBJECT_CLASS(artist_track_parent_class)->finalize(obj);
}

static void artist_track_class_init(ArtistTrackClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = artist_track_finalize;
}

static void artist_track_init(ArtistTrack *self) {
  (void)self;
}

static void format_duration(int ms, char *out, size_t n) {
  int sec = (ms + 500) / 1000;
  if (ms <= 0) out[0] = '\0';
  else if (sec >= 3600) g_snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else g_snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

static ArtistTrack *artist_track_new(const mc_track_info *t) {
  ArtistTrack *self = g_object_new(ARTIST_TYPE_TRACK, NULL);
  memcpy(self->uid, t->uid, sizeof self->uid);
  self->title = g_strdup(t->title && *t->title ? t->title : mc_str(MC_STR_UNTITLED));
  self->artist = g_strdup(t->artist);
  self->album = g_strdup(t->album);
  self->cover_dir = g_strdup(t->cover_dir);
  self->path = g_strdup(t->path);
  self->duration_ms = t->duration_ms;
  format_duration(t->duration_ms, self->duration, sizeof self->duration);
  return self;
}

/* --- State --- */

typedef struct track_row {
  GtkWidget *title, *album, *duration;
  ArtistTrack *track; /* bound to, or NULL */
  GtkListItem *item;
} track_row;

static struct {
  app *a;
  artist_view_callbacks cb;
  GtkWidget *stack; /* "none" status page | "list" | "artist" page */
  GtkWidget *none;
  GListStore *artists;
  GtkSingleSelection *artist_selection;
  GtkWidget *list;

  /* The open artist's page. */
  char open[MC_UID_STRLEN];
  GtkWidget *page, *avatar, *name, *summary, *actions;
  GtkWidget *albums_heading, *albums_scroller, *albums;
  GListStore *tracks;
  GtkSingleSelection *track_selection;
  GtkWidget *track_view, *track_scroller, *menu;
  char menu_uid[MC_UID_STRLEN]; /* the track the menu was opened on */
  GPtrArray *rows;              /* bound track rows, to mark the playing one */
  char playing_uid[MC_UID_STRLEN];
} v;

static ArtistItem *artist_at(guint i) {
  ArtistItem *item = g_list_model_get_item(G_LIST_MODEL(v.artists), i);
  if (item) g_object_unref(item); /* the store keeps it */
  return item;
}

static ArtistTrack *track_at(guint i) {
  ArtistTrack *t = g_list_model_get_item(G_LIST_MODEL(v.tracks), i);
  if (t) g_object_unref(t);
  return t;
}

static void changed(void) {
  if (v.cb.changed) v.cb.changed(v.cb.user);
}

/* --- The list --- */

static void artist_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_box_append(GTK_BOX(box), adw_avatar_new(LIST_AVATAR, NULL, TRUE));
  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(text, TRUE);
  GtkWidget *name = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(text), name);
  GtkWidget *sub = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(sub), 0);
  gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(sub, "dim-label");
  gtk_widget_add_css_class(sub, "caption");
  gtk_box_append(GTK_BOX(text), sub);
  gtk_box_append(GTK_BOX(box), text);
  gtk_list_item_set_child(item, box);
}

static void artist_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  ArtistItem *artist = gtk_list_item_get_item(item);
  GtkWidget *box = gtk_list_item_get_child(item);
  GtkWidget *avatar = gtk_widget_get_first_child(box);
  GtkWidget *text = gtk_widget_get_next_sibling(avatar);
  GtkWidget *name = gtk_widget_get_first_child(text), *sub = gtk_widget_get_next_sibling(name);
  adw_avatar_set_text(ADW_AVATAR(avatar), artist->name);
  gtk_label_set_text(GTK_LABEL(name), artist->name);
  gtk_label_set_text(GTK_LABEL(sub), artist->sub ? artist->sub : "");
  gtk_widget_set_visible(sub, artist->sub != NULL);
  gtk_list_item_set_accessible_label(item, artist->name);
}

static void on_artist_activated(GtkListView *view, guint position, gpointer user) {
  (void)view;
  (void)user;
  ArtistItem *artist = artist_at(position);
  if (!artist) return;
  char uid[MC_UID_STRLEN];
  memcpy(uid, artist->uid, sizeof uid);
  artist_view_open(uid);
}

/* Shows items (n of them; the store takes its own references), replacing only the run
 * that changed so the list keeps its place, as the album grid does. */
static void set_artists(ArtistItem **items, guint n) {
  guint old = g_list_model_get_n_items(G_LIST_MODEL(v.artists)), head = 0, tail = 0;
  while (head < old && head < n && artist_item_equal(artist_at(head), items[head])) head++;
  while (tail < old - head && tail < n - head && artist_item_equal(artist_at(old - 1 - tail), items[n - 1 - tail]))
    tail++;
  if (head + tail < old || head + tail < n)
    g_list_store_splice(v.artists, head, old - head - tail, (gpointer *)(items + head), n - head - tail);
}

static void reload_list(void) {
  mc_artist_list *list = NULL;
  mc_status st = v.a->lib ? mc_library_list_artists(v.a->lib, &list) : MC_OK;
  if (st != MC_OK) list = NULL;
  guint n = list ? (guint)list->count : 0;
  ArtistItem **items = g_new0(ArtistItem *, n + 1);
  for (guint i = 0; i < n; i++) items[i] = artist_item_new(&list->items[i]);
  mc_artist_list_free(list);
  set_artists(items, n);
  for (guint i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);

  if (!v.a->lib) {
    adw_status_page_set_title(ADW_STATUS_PAGE(v.none), mc_str(MC_STR_LIBRARY_OPEN_FAILED));
  } else if (st != MC_OK) {
    char *text = mc_format_alloc(mc_str(MC_STR_LIST_FAILED), MC_S(mc_status_str(st)));
    adw_status_page_set_title(ADW_STATUS_PAGE(v.none), text);
    mc_free(text);
  } else {
    adw_status_page_set_title(ADW_STATUS_PAGE(v.none), mc_str(MC_STR_NO_ARTISTS));
  }
}

/* --- The artist's page --- */

static void track_row_update(track_row *r) {
  gboolean playing = r->track && v.playing_uid[0] && !strcmp(r->track->uid, v.playing_uid);
  if (playing) {
    gtk_widget_add_css_class(r->title, "accent");
    gtk_widget_add_css_class(r->duration, "accent");
    gtk_widget_remove_css_class(r->duration, "dim-label");
  } else {
    gtk_widget_remove_css_class(r->title, "accent");
    gtk_widget_remove_css_class(r->duration, "accent");
    gtk_widget_add_css_class(r->duration, "dim-label");
  }
}

static GtkWidget *row_label(float xalign) {
  GtkWidget *l = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(l), xalign);
  gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
  return l;
}

/* Right-click: selects the row and opens its menu where the pointer is. */
static void on_track_menu(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  track_row *r = user;
  if (!r->track || !r->item) return;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  memcpy(v.menu_uid, r->track->uid, sizeof v.menu_uid);
  gtk_single_selection_set_selected(v.track_selection, gtk_list_item_get_position(r->item));
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
  graphene_point_t at;
  if (!gtk_widget_compute_point(widget, v.page, &GRAPHENE_POINT_INIT((float)x, (float)y), &at)) return;
  gtk_popover_set_pointing_to(GTK_POPOVER(v.menu), &(GdkRectangle){(int)at.x, (int)at.y, 1, 1});
  gtk_popover_popup(GTK_POPOVER(v.menu));
}

static void track_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  track_row *r = g_new0(track_row, 1);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  /* The title and the album in two columns of the same width. */
  GtkWidget *columns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_set_homogeneous(GTK_BOX(columns), TRUE);
  gtk_widget_set_hexpand(columns, TRUE);
  r->title = row_label(0);
  gtk_box_append(GTK_BOX(columns), r->title);
  r->album = row_label(0);
  gtk_widget_add_css_class(r->album, "dim-label");
  gtk_box_append(GTK_BOX(columns), r->album);
  gtk_box_append(GTK_BOX(box), columns);
  r->duration = row_label(1);
  gtk_widget_add_css_class(r->duration, "numeric");
  gtk_box_append(GTK_BOX(box), r->duration);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_track_menu), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));
  g_object_set_data_full(G_OBJECT(box), "row", r, g_free);
  gtk_list_item_set_child(item, box);
}

static void track_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  ArtistTrack *t = gtk_list_item_get_item(item);
  track_row *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "row");
  r->track = t;
  r->item = item;
  gtk_label_set_text(GTK_LABEL(r->title), t->title);
  gtk_label_set_text(GTK_LABEL(r->album), t->album ? t->album : "");
  gtk_label_set_text(GTK_LABEL(r->duration), t->duration);
  gtk_widget_set_tooltip_text(r->title, t->path ? NULL : mc_str(MC_STR_NOT_ON_DEVICE));
  gtk_widget_set_sensitive(r->title, t->path != NULL);
  gtk_list_item_set_accessible_label(item, t->title);
  track_row_update(r);
  if (v.rows) g_ptr_array_add(v.rows, r);
}

static void track_unbind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  track_row *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "row");
  r->track = NULL;
  r->item = NULL;
  if (v.rows) g_ptr_array_remove_fast(v.rows, r); /* not if the page has gone */
}

static void on_track_activated(GtkListView *view, guint position, gpointer user) {
  (void)view;
  (void)user;
  artist_view_play((int)position);
}

static void on_play_all(GtkButton *button, gpointer user) {
  (void)button;
  gboolean shuffled = GPOINTER_TO_INT(user);
  if (v.cb.set_shuffle) v.cb.set_shuffle(v.cb.user, shuffled);
  artist_view_play(shuffled ? -1 : 0);
}

static void on_album_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user) {
  (void)box;
  (void)user;
  char uid[MC_UID_STRLEN];
  g_strlcpy(uid, g_object_get_data(G_OBJECT(child), "uid"), sizeof uid); /* opening may reload the page */
  if (v.cb.open_album) v.cb.open_album(v.cb.user, uid);
}

static void on_add_to_playlist(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  char uid[MC_UID_STRLEN];
  memcpy(uid, v.menu_uid, sizeof uid);
  if (v.cb.add_to_playlist && uid[0]) v.cb.add_to_playlist(v.cb.user, uid);
}

/* The open artist's name as the list has it, or NULL. */
static const char *open_name(void) {
  for (guint i = 0, n = g_list_model_get_n_items(G_LIST_MODEL(v.artists)); v.open[0] && i < n; i++)
    if (!strcmp(artist_at(i)->uid, v.open)) return artist_at(i)->name;
  return NULL;
}

static void on_deleted(void *user) {
  app_reload_library(user);
}

/* The track menu's Delete from library... (delete_dialog.h). */
static void on_delete_track(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  for (guint i = 0, n = g_list_model_get_n_items(G_LIST_MODEL(v.tracks)); v.menu_uid[0] && i < n; i++) {
    ArtistTrack *t = track_at(i);
    if (strcmp(t->uid, v.menu_uid)) continue;
    delete_dialog_present(GTK_WIDGET(v.a->win), v.a->lib, v.a->settings_dir, MC_DELETE_TRACKS, t->uid, t->title,
                          on_deleted, v.a);
    return;
  }
}

/* The header menu's Delete artist...: the open artist's tracks and own albums. */
static void on_delete_artist(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  if (v.open[0])
    delete_dialog_present(GTK_WIDGET(v.a->win), v.a->lib, v.a->settings_dir, MC_DELETE_ARTISTS, v.open, open_name(),
                          on_deleted, v.a);
}

/* An album's tile, with its year beneath it, or its artist when that's someone else. */
static GtkWidget *album_child(const mc_album_info *info, const char *artist) {
  AlbumItem *album = album_item_new(info);
  if (!info->artist || g_strcmp0(info->artist, artist)) album->subtitle = g_strdup(album->artist ? album->artist : mc_str(MC_STR_UNKNOWN_ARTIST));
  else album->subtitle = info->year > 0 ? g_strdup_printf("%d", info->year) : g_strdup("");
  GtkWidget *child = gtk_flow_box_child_new();
  gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), album_tile_new(album));
  g_object_set_data_full(G_OBJECT(child), "uid", g_strdup(info->uid), g_free);
  g_object_unref(album);
  return child;
}

/* Fills the page from the library: the artist's name, albums and tracks. */
static void load_page(void) {
  const char *name = open_name();
  adw_avatar_set_text(ADW_AVATAR(v.avatar), name);
  gtk_label_set_text(GTK_LABEL(v.name), name ? name : "");

  gtk_flow_box_remove_all(GTK_FLOW_BOX(v.albums));
  mc_album_list *albums = NULL;
  if (!v.a->lib || mc_library_list_artist_albums(v.a->lib, v.open, &albums) != MC_OK) albums = NULL;
  int album_count = albums ? albums->count : 0;
  for (int i = 0; i < album_count; i++) gtk_flow_box_append(GTK_FLOW_BOX(v.albums), album_child(&albums->items[i], name));
  mc_album_list_free(albums);
  gtk_widget_set_visible(v.albums_heading, album_count > 0);
  gtk_widget_set_visible(v.albums_scroller, album_count > 0);

  /* The selected track stays selected, if it's still there. */
  char keep[MC_UID_STRLEN] = "";
  ArtistTrack *sel = gtk_single_selection_get_selected_item(v.track_selection);
  if (sel) memcpy(keep, sel->uid, sizeof keep);
  mc_track_list *tracks = NULL;
  if (!v.a->lib || mc_library_list_artist_tracks(v.a->lib, v.open, &tracks) != MC_OK) tracks = NULL;
  int n = tracks ? tracks->count : 0;
  gboolean playable = FALSE;
  guint selected = GTK_INVALID_LIST_POSITION;
  ArtistTrack **items = g_new0(ArtistTrack *, (gsize)n + 1);
  for (int i = 0; i < n; i++) {
    items[i] = artist_track_new(&tracks->items[i]);
    playable |= items[i]->path != NULL;
    if (keep[0] && selected == GTK_INVALID_LIST_POSITION && !strcmp(items[i]->uid, keep)) selected = (guint)i;
  }
  mc_track_list_free(tracks);
  g_list_store_splice(v.tracks, 0, g_list_model_get_n_items(G_LIST_MODEL(v.tracks)), (gpointer *)items, (guint)n);
  gtk_single_selection_set_selected(v.track_selection, selected);
  for (int i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);
  gtk_widget_set_sensitive(v.actions, playable);

  /* "12 albums · 140 tracks" */
  char a[64], t[64];
  mc_format(a, sizeof a, mc_str_plural(MC_STR_COUNT_ALBUMS, album_count), MC_D(album_count));
  mc_format(t, sizeof t, mc_str_plural(MC_STR_TRACKS, n), MC_D(n));
  char *summary = g_strdup_printf("%s%s%s", a, mc_str(MC_STR_SEPARATOR_DOT), t);
  gtk_label_set_text(GTK_LABEL(v.summary), summary);
  g_free(summary);
}

static void go_back(void) {
  if (!v.open[0]) return;
  v.open[0] = '\0';
  g_list_store_remove_all(v.tracks);
  gtk_flow_box_remove_all(GTK_FLOW_BOX(v.albums));
  gtk_stack_set_visible_child_name(GTK_STACK(v.stack),
                                   g_list_model_get_n_items(G_LIST_MODEL(v.artists)) ? "list" : "none");
  gtk_widget_grab_focus(v.list);
  changed();
}

static void on_back(GtkButton *button, gpointer user) {
  (void)button;
  (void)user;
  go_back();
}

static gboolean on_back_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  (void)user;
  if (!v.open[0]) return FALSE;
  go_back();
  return TRUE;
}

static void on_back_click(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  (void)x;
  (void)y;
  (void)user;
  if (!v.open[0]) return;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  go_back();
}

static GtkWidget *play_button(const char *icon, const char *label, gboolean shuffled) {
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), icon);
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), label);
  GtkWidget *b = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(b), content);
  g_signal_connect(b, "clicked", G_CALLBACK(on_play_all), GINT_TO_POINTER(shuffled));
  return b;
}

static GtkWidget *heading(const char *text) {
  GtkWidget *label = gtk_label_new(text);
  gtk_widget_add_css_class(label, "title-4");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_margin_start(label, 18);
  gtk_widget_set_margin_end(label, 18);
  gtk_widget_set_margin_top(label, 12);
  gtk_widget_set_margin_bottom(label, 6);
  return label;
}

static GtkWidget *build_header(void) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 18);
  gtk_widget_set_margin_top(header, 12);
  GtkWidget *back = gtk_button_new_from_icon_name("go-previous-symbolic");
  gtk_widget_set_tooltip_text(back, mc_str(MC_STR_BACK));
  gtk_widget_add_css_class(back, "flat");
  gtk_widget_add_css_class(back, "circular");
  gtk_widget_set_valign(back, GTK_ALIGN_START);
  g_signal_connect(back, "clicked", G_CALLBACK(on_back), NULL);
  gtk_box_append(GTK_BOX(header), back);
  v.avatar = adw_avatar_new(PAGE_AVATAR, NULL, TRUE);
  gtk_box_append(GTK_BOX(header), v.avatar);

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(text, TRUE);
  v.name = gtk_label_new(NULL);
  gtk_widget_add_css_class(v.name, "title-1");
  gtk_label_set_xalign(GTK_LABEL(v.name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(v.name), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(text), v.name);
  v.summary = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(v.summary), 0);
  gtk_label_set_ellipsize(GTK_LABEL(v.summary), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(v.summary, "dim-label");
  gtk_box_append(GTK_BOX(text), v.summary);

  v.actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_margin_top(v.actions, 8);
  GtkWidget *play = play_button("media-playback-start-symbolic", mc_str(MC_STR_PLAY), FALSE);
  gtk_widget_add_css_class(play, "suggested-action");
  gtk_box_append(GTK_BOX(v.actions), play);
  gtk_box_append(GTK_BOX(v.actions), play_button("media-playlist-shuffle-symbolic", mc_str(MC_STR_SHUFFLE), TRUE));
  gtk_box_append(GTK_BOX(text), v.actions);
  gtk_box_append(GTK_BOX(header), text);

  /* Delete artist..., under "...". */
  GMenu *menu = g_menu_new();
  g_menu_append(menu, mc_str_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU), "artist.delete");
  GtkWidget *more = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(more), "view-more-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(more), G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_tooltip_text(more, mc_str(MC_STR_LINUX_ARTIST_MENU));
  gtk_widget_add_css_class(more, "flat");
  gtk_widget_add_css_class(more, "circular");
  gtk_widget_set_valign(more, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header), more);
  return header;
}

/* The albums, in one row that scrolls sideways. */
static GtkWidget *build_albums(void) {
  v.albums = gtk_flow_box_new();
  gtk_orientable_set_orientation(GTK_ORIENTABLE(v.albums), GTK_ORIENTATION_VERTICAL); /* columns of one */
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(v.albums), 1);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(v.albums), 1);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(v.albums), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(v.albums, "search-albums");
  gtk_widget_set_margin_start(v.albums, 10);
  gtk_widget_set_margin_end(v.albums, 10);
  gtk_widget_set_halign(v.albums, GTK_ALIGN_START);
  gtk_accessible_update_property(GTK_ACCESSIBLE(v.albums), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_SECTION_ALBUMS), -1);
  g_signal_connect(v.albums, "child-activated", G_CALLBACK(on_album_activated), NULL);
  v.albums_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(v.albums_scroller), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(v.albums_scroller), v.albums);
  return v.albums_scroller;
}

static GtkWidget *build_tracks(void) {
  v.tracks = g_list_store_new(ARTIST_TYPE_TRACK);
  v.track_selection = gtk_single_selection_new(G_LIST_MODEL(v.tracks)); /* takes the store */
  gtk_single_selection_set_autoselect(v.track_selection, FALSE);
  gtk_single_selection_set_can_unselect(v.track_selection, TRUE);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(track_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(track_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(track_unbind), NULL);
  v.track_view = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(v.track_selection)), factory);
  gtk_widget_add_css_class(v.track_view, "navigation-sidebar");
  gtk_accessible_update_property(GTK_ACCESSIBLE(v.track_view), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_TRACKS_TITLE), -1);
  g_signal_connect(v.track_view, "activate", G_CALLBACK(on_track_activated), NULL);
  v.track_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(v.track_scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(v.track_scroller), v.track_view);
  gtk_widget_set_vexpand(v.track_scroller, TRUE);
  gtk_widget_set_margin_start(v.track_scroller, 6);
  gtk_widget_set_margin_end(v.track_scroller, 6);
  return v.track_scroller;
}

static GtkWidget *build_page(void) {
  v.page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(v.page), build_header());
  v.albums_heading = heading(mc_str(MC_STR_SECTION_ALBUMS));
  gtk_box_append(GTK_BOX(v.page), v.albums_heading);
  gtk_box_append(GTK_BOX(v.page), build_albums());
  gtk_box_append(GTK_BOX(v.page), heading(mc_str(MC_STR_TRACKS_TITLE)));
  gtk_box_append(GTK_BOX(v.page), build_tracks());

  /* The tracks' right-click menu, as the page's child so its "artist." action resolves. */
  GSimpleActionGroup *actions = g_simple_action_group_new();
  const GActionEntry entries[] = {{"add-to-playlist", on_add_to_playlist, NULL, NULL, NULL, {0}},
                                  {"delete-track", on_delete_track, NULL, NULL, NULL, {0}},
                                  {"delete", on_delete_artist, NULL, NULL, NULL, {0}}};
  g_action_map_add_action_entries(G_ACTION_MAP(actions), entries, G_N_ELEMENTS(entries), NULL);
  gtk_widget_insert_action_group(v.page, "artist", G_ACTION_GROUP(actions));
  g_object_unref(actions);
  GMenu *model = g_menu_new();
  GMenu *section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_ADD_TO_PLAYLIST_MENU), "artist.add-to-playlist");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU), "artist.delete-track");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  v.menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
  g_object_unref(model);
  gtk_popover_set_has_arrow(GTK_POPOVER(v.menu), FALSE);
  gtk_widget_set_halign(v.menu, GTK_ALIGN_START);
  gtk_widget_set_parent(v.menu, v.page);

  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Escape, 0), gtk_callback_action_new(on_back_key, NULL, NULL)));
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Left, GDK_ALT_MASK), gtk_callback_action_new(on_back_key, NULL, NULL)));
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_BackSpace, 0), gtk_callback_action_new(on_back_key, NULL, NULL)));
  gtk_widget_add_controller(v.page, keys);
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 8); /* back */
  g_signal_connect(click, "pressed", G_CALLBACK(on_back_click), NULL);
  gtk_widget_add_controller(v.page, GTK_EVENT_CONTROLLER(click));
  return v.page;
}

static void on_destroy(GtkWidget *widget, gpointer user) {
  (void)widget;
  (void)user;
  g_clear_object(&v.artist_selection);
  g_clear_object(&v.track_selection);
  g_clear_pointer(&v.rows, g_ptr_array_unref);
  memset(&v, 0, sizeof v);
}

GtkWidget *artist_view_new(app *a, const artist_view_callbacks *cb) {
  v.a = a;
  v.cb = *cb;
  v.rows = g_ptr_array_new();

  v.none = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(v.none), "avatar-default-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(v.none), mc_str(MC_STR_NO_ARTISTS));

  v.artists = g_list_store_new(ARTIST_TYPE_ITEM);
  v.artist_selection = gtk_single_selection_new(G_LIST_MODEL(v.artists)); /* takes the store */
  gtk_single_selection_set_autoselect(v.artist_selection, FALSE);
  gtk_single_selection_set_can_unselect(v.artist_selection, TRUE);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(artist_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(artist_bind), NULL);
  /* The view takes both; the selection stays ours too. */
  v.list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(v.artist_selection)), factory);
  gtk_list_view_set_single_click_activate(GTK_LIST_VIEW(v.list), TRUE);
  gtk_widget_add_css_class(v.list, "navigation-sidebar");
  gtk_widget_set_margin_start(v.list, 6);
  gtk_widget_set_margin_end(v.list, 6);
  gtk_accessible_update_property(GTK_ACCESSIBLE(v.list), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_SECTION_ARTISTS),
                                 -1);
  g_signal_connect(v.list, "activate", G_CALLBACK(on_artist_activated), NULL);
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), v.list);

  v.stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(v.stack), v.none, "none");
  gtk_stack_add_named(GTK_STACK(v.stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(v.stack), build_page(), "artist");
  g_signal_connect(v.stack, "destroy", G_CALLBACK(on_destroy), NULL);
  return v.stack;
}

void artist_view_reload(void) {
  if (!v.stack) return;
  reload_list();
  if (v.open[0] && !open_name()) v.open[0] = '\0'; /* it left the library */
  if (v.open[0]) load_page();
  else {
    g_list_store_remove_all(v.tracks);
    gtk_flow_box_remove_all(GTK_FLOW_BOX(v.albums));
  }
  gtk_stack_set_visible_child_name(GTK_STACK(v.stack), v.open[0]                                         ? "artist"
                                                       : g_list_model_get_n_items(G_LIST_MODEL(v.artists)) ? "list"
                                                                                                           : "none");
  changed();
}

gboolean artist_view_open(const char *uid) {
  if (!v.stack || !uid || strlen(uid) >= MC_UID_STRLEN) return FALSE;
  char was[MC_UID_STRLEN];
  memcpy(was, v.open, sizeof was);
  g_strlcpy(v.open, uid, sizeof v.open);
  if (!open_name()) {
    memcpy(v.open, was, sizeof v.open);
    return FALSE;
  }
  gtk_single_selection_set_selected(v.track_selection, GTK_INVALID_LIST_POSITION);
  load_page();
  gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(v.track_scroller)), 0);
  gtk_adjustment_set_value(gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(v.albums_scroller)), 0);
  gtk_stack_set_visible_child_name(GTK_STACK(v.stack), "artist");
  gtk_widget_grab_focus(v.track_view); /* so Escape and the arrow keys work from the start */
  /* Where the list will be on the way back. */
  for (guint i = 0, n = g_list_model_get_n_items(G_LIST_MODEL(v.artists)); i < n; i++)
    if (!strcmp(artist_at(i)->uid, v.open)) gtk_single_selection_set_selected(v.artist_selection, i);
  if (v.a->lib && mc_library_record_opened(v.a->lib, MC_RECENT_ARTIST, v.open) == MC_OK) recents_refresh(v.a);
  changed();
  return TRUE;
}

const char *artist_view_artist(void) {
  return v.open[0] ? v.open : NULL;
}

gboolean artist_view_playable(void) {
  if (!v.open[0]) return FALSE;
  for (guint i = 0, n = g_list_model_get_n_items(G_LIST_MODEL(v.tracks)); i < n; i++)
    if (track_at(i)->path) return TRUE;
  return FALSE;
}

void artist_view_play(int start) {
  guint n = v.open[0] ? g_list_model_get_n_items(G_LIST_MODEL(v.tracks)) : 0;
  if (!n) return;
  playback_track *queue = g_new0(playback_track, n);
  int count = 0, first = start < 0 ? -1 : 0;
  for (guint i = 0; i < n; i++) {
    ArtistTrack *t = track_at(i);
    if (!t->path) continue;
    if (start >= 0 && (int)i <= start) first = count;
    queue[count++] = (playback_track){t->uid, t->path, t->title, t->artist, t->album, t->cover_dir, t->duration_ms};
  }
  if (count) playback_play(queue, count, first);
  g_free(queue);
}

void artist_view_set_playing(const char *uid) {
  const char *now = uid ? uid : "";
  if (!v.rows || !strcmp(now, v.playing_uid) || strlen(now) >= MC_UID_STRLEN) return;
  strcpy(v.playing_uid, now);
  for (guint i = 0; i < v.rows->len; i++) track_row_update(g_ptr_array_index(v.rows, i));
}
