#include "track_list.h"

#include <adwaita.h>
#include <string.h>

#include "app.h"
#include "cover.h"

enum { HEADER_COVER = 96 };

/* --- TrackItem: one row --- */

#define TRACK_TYPE_ITEM (track_item_get_type())
G_DECLARE_FINAL_TYPE(TrackItem, track_item, TRACK, ITEM, GObject)

struct _TrackItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char entry_uid[MC_UID_STRLEN]; /* its place in a playlist, else "" */
  char *title;
  char num[16];      /* "3", "2-5" with several discs, or ""; in a playlist, its place */
  char duration[16]; /* "4:44", or "" if unknown */
  int duration_ms;
  char *path;
  char *artist, *album, *cover_dir;
};

G_DEFINE_FINAL_TYPE(TrackItem, track_item, G_TYPE_OBJECT)

static void track_item_finalize(GObject *obj) {
  TrackItem *self = TRACK_ITEM(obj);
  g_free(self->title);
  g_free(self->path);
  g_free(self->artist);
  g_free(self->album);
  g_free(self->cover_dir);
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
  GtkListItem *item;
} row;

static struct {
  track_list_callbacks cb;

  GListStore *store;
  GtkSingleSelection *selection;
  GtkWidget *view, *scroller;
  GtkWidget *cover, *title, *artist, *summary;
  GCancellable *cover_loading;
  AlbumItem *album;
  GPtrArray *rows; /* bound rows, to mark the playing one */
  GtkWidget *pane, *menu;         /* menu: the tracks' right-click menu */
  GMenuModel *album_menu, *playlist_menu; /* what it shows */
  GSimpleAction *move_up, *move_down;
  char menu_uid[MC_UID_STRLEN];   /* the track it was opened on */
  char menu_entry[MC_UID_STRLEN]; /* and its playlist entry, or "" */
  guint menu_pos;
  GtkWidget *playlist_button;     /* the header's menu, shown for playlists */
  GtkWidget *reshuffle_button;    /* shown for smart playlists in random order */
  GtkWidget *actions;             /* Play and Shuffle */
  char playing_uid[MC_UID_STRLEN];
} s;

/* The shown track at position i (borrowed), or NULL. */
static TrackItem *track_at(guint i) {
  TrackItem *t = g_list_model_get_item(G_LIST_MODEL(s.store), i);
  if (t) g_object_unref(t); /* the store keeps it */
  return t;
}

/* A playlist whose entries can be moved, removed, renamed and deleted: not a smart one. */
static gboolean showing_playlist(void) {
  return s.album && s.album->is_playlist && !s.album->is_smart;
}

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

/* Makes the track menu (and the keys) act on the track at pos: in a playlist its items
 * include Move Up, Move Down and Remove from Playlist. FALSE if there's no track there. */
static gboolean menu_for(guint pos) {
  TrackItem *t = pos != GTK_INVALID_LIST_POSITION ? track_at(pos) : NULL;
  if (!t) return FALSE;
  memcpy(s.menu_uid, t->uid, sizeof s.menu_uid);
  memcpy(s.menu_entry, t->entry_uid, sizeof s.menu_entry);
  s.menu_pos = pos;
  gboolean playlist = showing_playlist() && s.menu_entry[0];
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(s.menu), playlist ? s.playlist_menu : s.album_menu);
  g_simple_action_set_enabled(s.move_up, playlist && s.menu_pos > 0);
  g_simple_action_set_enabled(s.move_down, playlist && s.menu_pos + 1 < g_list_model_get_n_items(G_LIST_MODEL(s.store)));
  return TRUE;
}

/* Opens the track menu at x, y in the pane. */
static void popup_menu_at(float x, float y) {
  gtk_popover_set_pointing_to(GTK_POPOVER(s.menu), &(GdkRectangle){(int)x, (int)y, 1, 1});
  gtk_popover_popup(GTK_POPOVER(s.menu));
}

/* Right-click: selects the row and opens the menu for its track where the pointer is. */
static void on_row_menu(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  row *r = user;
  if (!r->track || !r->item) return;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  guint pos = gtk_list_item_get_position(r->item);
  gtk_single_selection_set_selected(s.selection, pos);
  menu_for(pos);
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
  graphene_point_t at;
  if (gtk_widget_compute_point(widget, s.pane, &GRAPHENE_POINT_INIT((float)x, (float)y), &at)) popup_menu_at(at.x, at.y);
}

/* --- Dragging a playlist's tracks into another order --- */

/* A playlist's row drags its entry uid. */
static GdkContentProvider *on_drag_prepare(GtkDragSource *source, double x, double y, gpointer user) {
  row *r = user;
  if (!r->track || !r->track->entry_uid[0] || !showing_playlist()) return NULL;
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(source));
  GdkPaintable *icon = gtk_widget_paintable_new(widget);
  gtk_drag_source_set_icon(source, icon, (int)x, (int)y);
  g_object_unref(icon);
  return gdk_content_provider_new_typed(G_TYPE_STRING, r->track->entry_uid);
}

/* Whether a drop at y lands below the row's middle, i.e. after it. */
static gboolean drop_below(GtkDropTarget *target, double y) {
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target));
  return y > gtk_widget_get_height(widget) / 2.0;
}

/* The line where the track would go, above or below the row (the list's row widget). */
static void mark_drop(GtkDropTarget *target, int where) {
  GtkWidget *widget = gtk_widget_get_parent(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(target)));
  if (!widget) return;
  if (where < 0) gtk_widget_add_css_class(widget, "drop-above");
  else gtk_widget_remove_css_class(widget, "drop-above");
  if (where > 0) gtk_widget_add_css_class(widget, "drop-below");
  else gtk_widget_remove_css_class(widget, "drop-below");
}

static GdkDragAction on_drop_motion(GtkDropTarget *target, double x, double y, gpointer user) {
  (void)x;
  row *r = user;
  if (!r->track || !showing_playlist()) return 0;
  mark_drop(target, drop_below(target, y) ? 1 : -1);
  return GDK_ACTION_MOVE;
}

static void on_drop_leave(GtkDropTarget *target, gpointer user) {
  (void)user;
  mark_drop(target, 0);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer user) {
  (void)x;
  row *r = user;
  mark_drop(target, 0);
  if (!r->track || !r->item || !showing_playlist() || !G_VALUE_HOLDS_STRING(value)) return FALSE;
  const char *dragged = g_value_get_string(value);
  /* Only this playlist's entries: not text dragged in from elsewhere. */
  gboolean ours = FALSE;
  for (guint i = 0, n = g_list_model_get_n_items(G_LIST_MODEL(s.store)); dragged && i < n && !ours; i++)
    ours = !strcmp(track_at(i)->entry_uid, dragged);
  if (!ours) return FALSE;

  /* Copies: moving reloads the list, and these with it. */
  char entry[MC_UID_STRLEN], after[MC_UID_STRLEN] = "";
  g_strlcpy(entry, dragged, sizeof entry);
  guint to = gtk_list_item_get_position(r->item);
  gboolean below = drop_below(target, y);
  if (below) g_strlcpy(after, r->track->entry_uid, sizeof after);
  else if (to > 0) g_strlcpy(after, track_at(to - 1)->entry_uid, sizeof after);
  if (!strcmp(after, entry) || (!below && !strcmp(r->track->entry_uid, entry)))
    return FALSE; /* dropped where it already is */
  if (s.cb.move) s.cb.move(s.cb.user, entry, after[0] ? after : NULL);
  return TRUE;
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
  gtk_widget_set_hexpand(lead, FALSE); /* don't inherit the children's expand, or it takes the title's space */
  r->num = row_label(0);
  gtk_widget_set_hexpand(r->num, TRUE);
  gtk_widget_add_css_class(r->num, "dim-label");
  gtk_widget_add_css_class(r->num, "numeric");
  gtk_box_append(GTK_BOX(lead), r->num);
  r->speaker = gtk_image_new_from_icon_name("audio-volume-high-symbolic");
  gtk_widget_set_hexpand(r->speaker, TRUE);
  gtk_widget_set_halign(r->speaker, GTK_ALIGN_START);
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

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_row_menu), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(click));

  GtkDragSource *drag = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag, GDK_ACTION_MOVE);
  g_signal_connect(drag, "prepare", G_CALLBACK(on_drag_prepare), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drag));
  GtkDropTarget *drop = gtk_drop_target_new(G_TYPE_STRING, GDK_ACTION_MOVE);
  g_signal_connect(drop, "motion", G_CALLBACK(on_drop_motion), r);
  g_signal_connect(drop, "leave", G_CALLBACK(on_drop_leave), r);
  g_signal_connect(drop, "drop", G_CALLBACK(on_drop), r);
  gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(drop));

  g_object_set_data_full(G_OBJECT(box), "row", r, g_free);
  gtk_list_item_set_child(item, box);
}

static void row_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  TrackItem *t = gtk_list_item_get_item(item);
  row *r = g_object_get_data(G_OBJECT(gtk_list_item_get_child(item)), "row");
  r->track = t;
  r->item = item;
  gtk_label_set_text(GTK_LABEL(r->num), t->num);
  gtk_label_set_text(GTK_LABEL(r->title), t->title);
  gtk_label_set_text(GTK_LABEL(r->duration), t->duration);
  gtk_widget_set_tooltip_text(r->title, t->path ? NULL : mc_str(MC_STR_NOT_ON_DEVICE));
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
  r->item = NULL;
  g_ptr_array_remove_fast(s.rows, r);
}

static void on_activate(GtkListView *view, guint position, gpointer user) {
  (void)view;
  (void)user;
  if (s.cb.play) s.cb.play(s.cb.user, (int)position);
}

static void on_play_all(GtkButton *button, gpointer user) {
  (void)button;
  if (s.cb.play_all) s.cb.play_all(s.cb.user, GPOINTER_TO_INT(user));
}

/* A button with an icon and a label, half the row's width. */
static GtkWidget *action_button(const char *icon, const char *label, gboolean shuffled) {
  GtkWidget *content = adw_button_content_new();
  adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(content), icon);
  adw_button_content_set_label(ADW_BUTTON_CONTENT(content), label);
  GtkWidget *b = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(b), content);
  gtk_widget_set_hexpand(b, TRUE);
  g_signal_connect(b, "clicked", G_CALLBACK(on_play_all), GINT_TO_POINTER(shuffled));
  return b;
}

/* Play, the suggested action, and Shuffle, side by side under the header. */
static GtkWidget *build_actions(void) {
  s.actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_set_homogeneous(GTK_BOX(s.actions), TRUE);
  gtk_widget_set_margin_start(s.actions, 18);
  gtk_widget_set_margin_end(s.actions, 18);
  gtk_widget_set_margin_bottom(s.actions, 18);
  GtkWidget *play = action_button("media-playback-start-symbolic", mc_str(MC_STR_PLAY), FALSE);
  gtk_widget_add_css_class(play, "suggested-action");
  gtk_box_append(GTK_BOX(s.actions), play);
  gtk_box_append(GTK_BOX(s.actions), action_button("media-playlist-shuffle-symbolic", mc_str(MC_STR_SHUFFLE), TRUE));
  return s.actions;
}

static void on_close(GtkWidget *widget, gpointer user) {
  (void)widget;
  (void)user;
  if (s.cb.close) s.cb.close(s.cb.user);
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

static void on_reshuffle(GtkButton *button, gpointer user) {
  (void)button;
  (void)user;
  if (s.cb.reshuffle && s.album && s.album->is_smart) s.cb.reshuffle(s.cb.user);
}

/* A menu item that shows its key, as Windows' do. */
static void append_with_key(GMenu *menu, enum mc_str_id label, const char *action, const char *accel) {
  GMenuItem *item = g_menu_item_new(mc_str_mnemonic(label), action);
  g_menu_item_set_attribute(item, "accel", "s", accel);
  g_menu_append_item(menu, item);
  g_object_unref(item);
}

static GtkWidget *build_header(void) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 18);
  gtk_widget_set_margin_bottom(header, 16); /* then Play and Shuffle */
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

  GMenu *menu = g_menu_new();
  append_with_key(menu, MC_STR_PLAYLIST_RENAME_MENU, "playlist.rename", "F2");
  g_menu_append(menu, mc_str_mnemonic(MC_STR_PLAYLIST_DELETE_MENU), "playlist.delete");
  s.playlist_button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(s.playlist_button), "view-more-symbolic");
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(s.playlist_button), G_MENU_MODEL(menu));
  g_object_unref(menu);
  gtk_widget_set_tooltip_text(s.playlist_button, mc_str(MC_STR_PLAYLIST_MENU));
  gtk_widget_add_css_class(s.playlist_button, "flat");
  gtk_widget_add_css_class(s.playlist_button, "circular");
  gtk_widget_set_valign(s.playlist_button, GTK_ALIGN_START);
  gtk_widget_set_visible(s.playlist_button, FALSE);
  gtk_box_append(GTK_BOX(header), s.playlist_button);

  s.reshuffle_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(s.reshuffle_button, mc_str(MC_STR_RESHUFFLE));
  gtk_widget_add_css_class(s.reshuffle_button, "flat");
  gtk_widget_add_css_class(s.reshuffle_button, "circular");
  gtk_widget_set_valign(s.reshuffle_button, GTK_ALIGN_START);
  gtk_widget_set_visible(s.reshuffle_button, FALSE);
  g_signal_connect(s.reshuffle_button, "clicked", G_CALLBACK(on_reshuffle), NULL);
  gtk_box_append(GTK_BOX(header), s.reshuffle_button);

  GtkWidget *close = gtk_button_new_from_icon_name("window-close-symbolic");
  app_set_tooltip(close, mc_str(MC_STR_CLOSE), "Escape");
  gtk_widget_add_css_class(close, "flat");
  gtk_widget_add_css_class(close, "circular");
  gtk_widget_set_valign(close, GTK_ALIGN_START);
  g_signal_connect(close, "clicked", G_CALLBACK(on_close), NULL);
  gtk_box_append(GTK_BOX(header), close);
  return header;
}

static void on_add_to_playlist(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  if (s.cb.add_to_playlist && s.menu_uid[0]) s.cb.add_to_playlist(s.cb.user, s.menu_uid);
}

/* Move Up goes after the entry two above (or to the start); Move Down after the one below. */
static void on_move(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)param;
  (void)user;
  gboolean up = action == s.move_up;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(s.store));
  if (!s.cb.move || !s.menu_entry[0] || (up && s.menu_pos == 0) || (!up && s.menu_pos + 1 >= n)) return;
  char entry[MC_UID_STRLEN], after[MC_UID_STRLEN] = "";
  memcpy(entry, s.menu_entry, sizeof entry);
  if (!up) memcpy(after, track_at(s.menu_pos + 1)->entry_uid, sizeof after);
  else if (s.menu_pos >= 2) memcpy(after, track_at(s.menu_pos - 2)->entry_uid, sizeof after);
  s.cb.move(s.cb.user, entry, after[0] ? after : NULL);
}

static void on_remove(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  char entry[MC_UID_STRLEN];
  memcpy(entry, s.menu_entry, sizeof entry);
  if (s.cb.remove && entry[0]) s.cb.remove(s.cb.user, entry);
}

static void on_rename_playlist(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  if (s.cb.rename_playlist && showing_playlist()) s.cb.rename_playlist(s.cb.user);
}

static void on_delete_playlist(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  if (s.cb.delete_playlist && showing_playlist()) s.cb.delete_playlist(s.cb.user);
}

/* The track menu's Delete from Library... */
static void on_delete_track(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  TrackItem *t = s.menu_uid[0] ? track_at(s.menu_pos) : NULL;
  if (s.cb.delete_track && t && !strcmp(t->uid, s.menu_uid)) s.cb.delete_track(s.cb.user, t->uid, t->title);
}

/* --- Keys: Delete, Alt+Up/Down, F2 and the Menu key, as on Windows (Enter is the view's) --- */

static guint selected(void) {
  return gtk_single_selection_get_selected(s.selection);
}

/* After Delete or Alt+Up/Down has reloaded the list: the focus back on the selected track,
 * or after Delete on the one that took its place, so the keys can go on. Not if the
 * playlist has gone (its last playable track removed). */
static void refocus(const char *playlist_uid, guint pos) {
  guint n = g_list_model_get_n_items(G_LIST_MODEL(s.store));
  if (!n || !s.album || strcmp(s.album->uid, playlist_uid)) return;
  guint sel = selected();
  if (sel == GTK_INVALID_LIST_POSITION) sel = pos < n ? pos : n - 1;
  gtk_list_view_scroll_to(GTK_LIST_VIEW(s.view), sel, GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS, NULL);
}

/* Delete: Remove from Playlist, for a playlist's selected track. An album's tracks stay. */
static gboolean on_delete_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  (void)user;
  guint pos = selected();
  if (!showing_playlist() || !menu_for(pos) || !s.menu_entry[0]) return FALSE;
  char playlist[MC_UID_STRLEN];
  memcpy(playlist, s.album->uid, sizeof playlist);
  on_remove(NULL, NULL, NULL);
  refocus(playlist, pos);
  return TRUE;
}

/* Alt+Up and Alt+Down: Move Up and Move Down, for a playlist's selected track. */
static gboolean on_move_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  guint pos = selected();
  if (!showing_playlist() || !menu_for(pos) || !s.menu_entry[0]) return FALSE;
  char playlist[MC_UID_STRLEN];
  memcpy(playlist, s.album->uid, sizeof playlist);
  on_move(GPOINTER_TO_INT(user) ? s.move_up : s.move_down, NULL, NULL);
  refocus(playlist, pos);
  return TRUE;
}

/* F2: the header menu's Rename... */
static gboolean on_rename_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  (void)user;
  if (!showing_playlist()) return FALSE;
  on_rename_playlist(NULL, NULL, NULL);
  return TRUE;
}

/* The Menu key or Shift+F10: the selected track's menu, under its title; with no track
 * selected, a playlist's header menu. A selected track scrolled out of sight is scrolled
 * back first, and the menu hangs from the top of the list. */
static gboolean on_menu_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  (void)user;
  guint pos = selected();
  if (menu_for(pos)) {
    graphene_rect_t at;
    for (guint i = 0; i < s.rows->len; i++) {
      row *r = g_ptr_array_index(s.rows, i);
      if (r->item && gtk_list_item_get_position(r->item) == pos &&
          gtk_widget_compute_bounds(r->title, s.pane, &at)) {
        popup_menu_at(at.origin.x, at.origin.y + at.size.height);
        return TRUE;
      }
    }
    gtk_list_view_scroll_to(GTK_LIST_VIEW(s.view), pos, GTK_LIST_SCROLL_FOCUS, NULL);
    if (gtk_widget_compute_bounds(s.scroller, s.pane, &at)) popup_menu_at(at.origin.x + 18, at.origin.y);
    return TRUE;
  }
  if (!gtk_widget_get_visible(s.playlist_button)) return FALSE;
  gtk_menu_button_popup(GTK_MENU_BUTTON(s.playlist_button));
  return TRUE;
}

static void add_key(GtkEventController *keys, guint key, GdkModifierType mods, GtkShortcutFunc func, gpointer user) {
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys), gtk_shortcut_new(gtk_keyval_trigger_new(key, mods), gtk_callback_action_new(func, user, NULL)));
}

static GSimpleAction *add_action(GSimpleActionGroup *group, const char *name,
                                 void (*activate)(GSimpleAction *, GVariant *, gpointer)) {
  GSimpleAction *action = g_simple_action_new(name, NULL);
  g_signal_connect(action, "activate", G_CALLBACK(activate), NULL);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  g_object_unref(action); /* the group keeps it */
  return action;
}

/* A closed menu gives the focus back to the list, as GTK would hand it elsewhere (the
 * sidebar, under Xvfb), and the keys should go on where they were. */
static void on_menu_closed(GtkPopover *menu, gpointer list) {
  (void)menu;
  gtk_widget_grab_focus(list);
}

/* The right-click menu, as the pane's child so its "track." actions resolve; and the
 * header menu's "playlist." actions. */
static void build_menu(GtkWidget *pane) {
  GSimpleActionGroup *actions = g_simple_action_group_new();
  add_action(actions, "add-to-playlist", on_add_to_playlist);
  s.move_up = add_action(actions, "move-up", on_move);
  s.move_down = add_action(actions, "move-down", on_move);
  add_action(actions, "remove", on_remove);
  add_action(actions, "delete", on_delete_track);
  gtk_widget_insert_action_group(pane, "track", G_ACTION_GROUP(actions));
  g_object_unref(actions);
  actions = g_simple_action_group_new();
  add_action(actions, "rename", on_rename_playlist);
  add_action(actions, "delete", on_delete_playlist);
  gtk_widget_insert_action_group(pane, "playlist", G_ACTION_GROUP(actions));
  g_object_unref(actions);

  GMenu *model = g_menu_new();
  GMenu *section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_ADD_TO_PLAYLIST_MENU), "track.add-to-playlist");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU), "track.delete");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  s.album_menu = G_MENU_MODEL(model);
  model = g_menu_new();
  section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_ADD_TO_PLAYLIST_MENU), "track.add-to-playlist");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  section = g_menu_new();
  append_with_key(section, MC_STR_MOVE_UP, "track.move-up", "<Alt>Up");
  append_with_key(section, MC_STR_MOVE_DOWN, "track.move-down", "<Alt>Down");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  section = g_menu_new();
  append_with_key(section, MC_STR_REMOVE_FROM_PLAYLIST, "track.remove", "Delete");
  g_menu_append(section, mc_str_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU), "track.delete");
  g_menu_append_section(model, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  s.playlist_menu = G_MENU_MODEL(model);

  s.menu = gtk_popover_menu_new_from_model(s.album_menu);
  gtk_popover_set_has_arrow(GTK_POPOVER(s.menu), FALSE);
  gtk_widget_set_halign(s.menu, GTK_ALIGN_START);
  gtk_widget_set_parent(s.menu, pane);
  g_signal_connect(s.menu, "closed", G_CALLBACK(on_menu_closed), s.view);
}

GtkWidget *track_list_new(const track_list_callbacks *cb) {
  s.cb = *cb;
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
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.view), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_TRACKS_TITLE),
                                 -1);
  g_signal_connect(s.view, "activate", G_CALLBACK(on_activate), NULL);
  s.scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s.scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s.scroller), s.view);
  gtk_widget_set_vexpand(s.scroller, TRUE);

  GtkWidget *pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(pane), build_header());
  gtk_box_append(GTK_BOX(pane), build_actions());
  gtk_box_append(GTK_BOX(pane), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(pane), s.scroller);
  s.pane = pane;
  build_menu(pane);

  GtkEventController *keys = gtk_shortcut_controller_new();
  add_key(keys, GDK_KEY_Escape, 0, on_escape, NULL);
  add_key(keys, GDK_KEY_F2, 0, on_rename_key, NULL);
  add_key(keys, GDK_KEY_Menu, 0, on_menu_key, NULL);
  add_key(keys, GDK_KEY_F10, GDK_SHIFT_MASK, on_menu_key, NULL);
  gtk_widget_add_controller(pane, keys);
  /* The rest act on the selected track, so only from the list. */
  keys = gtk_shortcut_controller_new();
  add_key(keys, GDK_KEY_Delete, 0, on_delete_key, NULL);
  add_key(keys, GDK_KEY_KP_Delete, 0, on_delete_key, NULL);
  add_key(keys, GDK_KEY_Up, GDK_ALT_MASK, on_move_key, GINT_TO_POINTER(TRUE));
  add_key(keys, GDK_KEY_Down, GDK_ALT_MASK, on_move_key, GINT_TO_POINTER(FALSE));
  gtk_widget_add_controller(s.view, keys);
  return pane;
}

/* "1997 · 12 tracks · 47 min" */
static void set_summary(int year, int count, int total_ms) {
  GString *str = g_string_new(NULL);
  char part[64];
  if (year > 0) g_string_append_printf(str, "%d%s", year, mc_str(MC_STR_SEPARATOR_DOT));
  int min = (total_ms + 30000) / 60000;
  if (!count) {
    g_string_append(str, mc_str(MC_STR_NO_TRACKS));
  } else {
    mc_format(part, sizeof part, mc_str_plural(MC_STR_TRACKS, count), MC_D(count));
    g_string_append(str, part);
    g_string_append(str, mc_str(MC_STR_SEPARATOR_DOT));
    if (min >= 60) mc_format(part, sizeof part, mc_str(MC_STR_DURATION_HOURS_MINUTES), MC_D(min / 60), MC_D(min % 60));
    else mc_format(part, sizeof part, mc_str(MC_STR_DURATION_MINUTES), MC_D(min));
    g_string_append(str, part);
  }
  gtk_label_set_text(GTK_LABEL(s.summary), str->str);
  g_string_free(str, TRUE);
}

void track_list_show(mc_library *lib, AlbumItem *album) {
  gboolean same = album && s.album && !strcmp(album->uid, s.album->uid);
  /* The selected track; in a playlist, its entry, as a track can be in it twice. */
  char keep[MC_UID_STRLEN] = "";
  TrackItem *sel = gtk_single_selection_get_selected_item(s.selection);
  gboolean entries = album && album->is_playlist && !album->is_smart; /* else by track */
  if (same && sel) memcpy(keep, entries ? sel->entry_uid : sel->uid, sizeof keep);
  /* A playlist's picture changes with its tracks, and an album's cover may have arrived. */
  gboolean new_cover = !same || g_strcmp0(album->cover_file, s.album->cover_file);
  if (album) g_object_ref(album);
  g_clear_object(&s.album);
  s.album = album;

  mc_track_list *list = NULL;
  mc_status st = MC_OK;
  if (lib && album)
    st = album->is_smart      ? mc_library_list_smart_tracks(lib, album->uid, &list)
         : album->is_playlist ? mc_library_list_playlist_tracks(lib, album->uid, &list)
                              : mc_library_list_album_tracks(lib, album->uid, &list);
  if (st != MC_OK) list = NULL;
  int n = list ? list->count : 0, total_ms = 0;
  gboolean discs = FALSE, playable = FALSE;
  for (int i = 0; i < n; i++) discs |= list->items[i].disc_no > 1;
  TrackItem **items = g_new0(TrackItem *, (gsize)n + 1);
  guint selected = GTK_INVALID_LIST_POSITION;
  for (int i = 0; i < n; i++) {
    const mc_track_info *t = &list->items[i];
    TrackItem *it = items[i] = g_object_new(TRACK_TYPE_ITEM, NULL);
    memcpy(it->uid, t->uid, sizeof it->uid);
    memcpy(it->entry_uid, t->entry_uid, sizeof it->entry_uid);
    it->title = g_strdup(t->title && *t->title ? t->title : mc_str(MC_STR_UNTITLED));
    if (album->is_playlist) g_snprintf(it->num, sizeof it->num, "%d", i + 1);
    else if (t->track_no && discs) g_snprintf(it->num, sizeof it->num, "%d-%d", t->disc_no ? t->disc_no : 1, t->track_no);
    else if (t->track_no) g_snprintf(it->num, sizeof it->num, "%d", t->track_no);
    format_duration(t->duration_ms, it->duration, sizeof it->duration);
    it->duration_ms = t->duration_ms;
    it->path = g_strdup(t->path);
    playable |= t->path != NULL;
    it->artist = g_strdup(t->artist);
    it->album = g_strdup(t->album);
    it->cover_dir = g_strdup(t->cover_dir);
    total_ms += t->duration_ms > 0 ? t->duration_ms : 0;
    if (keep[0] && selected == GTK_INVALID_LIST_POSITION &&
        !strcmp(entries ? it->entry_uid : it->uid, keep))
      selected = (guint)i;
  }
  mc_track_list_free(list);
  g_list_store_splice(s.store, 0, g_list_model_get_n_items(G_LIST_MODEL(s.store)), (gpointer *)items, (guint)n);
  gtk_single_selection_set_selected(s.selection, selected);
  for (int i = 0; i < n; i++) g_object_unref(items[i]);
  g_free(items);

  gtk_label_set_text(GTK_LABEL(s.title), album ? album->title : "");
  gtk_label_set_text(GTK_LABEL(s.artist), album && album->is_smart      ? mc_str(MC_STR_SMART_PLAYLIST)
                                          : album && album->is_playlist ? mc_str(MC_STR_PLAYLIST)
                                          : album && album->artist      ? album->artist
                                                                        : mc_str(MC_STR_UNKNOWN_ARTIST));
  set_summary(album ? album->year : 0, n, total_ms);
  gtk_widget_set_visible(s.playlist_button, entries);
  gtk_widget_set_visible(s.reshuffle_button, album && album->is_smart && album->random);
  gtk_widget_set_sensitive(s.actions, playable);
  if (!same) gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s.scroller)), 0);
  if (new_cover) {
    if (s.cover_loading) g_cancellable_cancel(s.cover_loading);
    g_clear_object(&s.cover_loading);
    GdkTexture *texture = NULL;
    int size = HEADER_COVER * gtk_widget_get_scale_factor(s.cover);
    if (album && album->cover_file) {
      s.cover_loading = g_cancellable_new();
      texture = cover_get_file(album->cover_file, size, s.cover_loading, cover_loaded, NULL);
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
  TrackItem *t = track_at((guint)i);
  item.uid = t->uid;
  item.title = t->title;
  item.duration_ms = t->duration_ms;
  item.path = t->path;
  item.artist = t->artist;
  item.album = t->album;
  item.cover_dir = t->cover_dir;
  return item;
}

void track_list_select(const char *uid) {
  for (guint i = 0, n = (guint)track_list_count(); i < n; i++) {
    if (strcmp(track_at(i)->uid, uid)) continue;
    gtk_list_view_scroll_to(GTK_LIST_VIEW(s.view), i, GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS, NULL);
    return;
  }
}

void track_list_set_playing(const char *uid) {
  const char *now = uid ? uid : "";
  if (!strcmp(now, s.playing_uid) || strlen(now) >= MC_UID_STRLEN) return;
  strcpy(s.playing_uid, now);
  for (guint i = 0; s.rows && i < s.rows->len; i++) row_update(g_ptr_array_index(s.rows, i));
}
