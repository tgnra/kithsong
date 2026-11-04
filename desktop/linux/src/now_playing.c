#include "now_playing.h"

#include <string.h>

#include "app.h"
#include "art_accent.h"
#include "cover.h"
#include "musiccore/musiccore.h"
#include "playback.h"
#include "track_menu.h"

enum {
  ART = 36, /* the cover's square, as tall as the playback buttons */
  KEY_STEP_MS = 5000,
  FLASH_MS = 1500,
};

static struct {
  void (*seek)(void *user, int ms);
  void (*shuffle)(void *user, gboolean on);
  void *user;

  GtkWidget *box, *picture, *title, *cur, *left, *bar, *shuffle_button;
  GCancellable *cover_loading;
  gboolean playing; /* there's a track */
  int duration_ms;
  gboolean pressed;      /* a pointer is down on the bar */
  gboolean seek_pending; /* the bar moved during that press */
  gboolean updating;     /* we're moving the bar, not the user */
  char *markup;          /* the track's line, while a flash covers it */
  guint flash;           /* the flash's timeout */

  track_list_callbacks cb; /* what the track's menu asks for */
  mc_library *(*library)(void *user);
  GtkWidget *menu;
  GSimpleAction *go_to_album, *go_to_artist, *start_radio;
  char menu_uid[MC_UID_STRLEN]; /* the track it was opened on */
  char menu_album[MC_UID_STRLEN];
  char menu_artist[MC_UID_STRLEN];
  gboolean menu_here;   /* it has a file here */
  gboolean menu_by_key; /* opened from the bar, so the focus goes back there */
} s;

static void format_time(int ms, char *out, size_t n) {
  int sec = ms > 0 ? ms / 1000 : 0;
  if (sec >= 3600) g_snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else g_snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

/* "1:02" and "-5:21". Whole seconds, so the two always add up to the length. With no
 * length, both are "0:00". */
static void show_times(int pos_ms) {
  char cur[24], left[24] = "-";
  int total = (s.duration_ms + 500) / 1000 * 1000, pos = MIN(pos_ms, s.duration_ms) / 1000 * 1000;
  format_time(pos, cur, sizeof cur);
  format_time(total - pos, left + (total > 0), sizeof left - 1);
  gtk_label_set_text(GTK_LABEL(s.cur), cur);
  gtk_label_set_text(GTK_LABEL(s.left), left);
}

static gboolean seekable(void) {
  return s.playing && s.duration_ms > 0;
}

static void set_bar(int ms) {
  s.updating = TRUE;
  gtk_range_set_value(GTK_RANGE(s.bar), ms);
  s.updating = FALSE;
  show_times(ms);
}

/* The user moved the bar, by pointer or keyboard. A drag only seeks once it's let go. */
static gboolean on_change_value(GtkRange *range, GtkScrollType scroll, double value, gpointer user) {
  (void)range;
  (void)scroll;
  (void)user;
  if (s.updating || !seekable()) return TRUE;
  int ms = (int)CLAMP(value, 0, s.duration_ms);
  show_times(ms);
  if (s.pressed) s.seek_pending = TRUE;
  else if (s.seek) s.seek(s.user, ms);
  return FALSE; /* the bar moves as usual */
}

/* Sees presses and releases on the bar before its own gesture does. */
static gboolean on_bar_event(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer user) {
  (void)controller;
  (void)user;
  switch (gdk_event_get_event_type(event)) {
    case GDK_BUTTON_PRESS:
    case GDK_TOUCH_BEGIN:
      s.pressed = TRUE;
      s.seek_pending = FALSE;
      break;
    case GDK_BUTTON_RELEASE:
    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
      if (s.pressed && s.seek_pending && s.seek && seekable())
        s.seek(s.user, (int)gtk_range_get_value(GTK_RANGE(s.bar)));
      s.pressed = s.seek_pending = FALSE;
      break;
    default:
      break;
  }
  return FALSE;
}

static void on_shuffle_toggled(GtkToggleButton *button, gpointer user) {
  (void)user;
  gboolean on = gtk_toggle_button_get_active(button);
  app_set_tooltip(GTK_WIDGET(button), mc_str(on ? MC_STR_SHUFFLE_ON : MC_STR_SHUFFLE_OFF), "<Control>s");
  if (!s.updating && s.shuffle) s.shuffle(s.user, on);
}

/* --- The track's menu --- */

/* The track with uid, as a list of one, or NULL if there's no menu or the library hasn't it. */
static mc_track_list *find_track(const char *uid) {
  mc_library *lib = s.library ? s.library(s.cb.user) : NULL;
  mc_track_list *list = NULL;
  if (!lib || !uid[0] || mc_library_list_track(lib, uid, &list) != MC_OK) return NULL;
  if (list->count) return list;
  mc_track_list_free(list);
  return NULL;
}

/* Makes the menu act on the playing track. FALSE if there's no menu to open. */
static gboolean menu_for_playing(void) {
  mc_track_list *list = s.playing && s.menu ? find_track(playback_current_uid()) : NULL;
  if (!list) return FALSE;
  const mc_track_info *t = &list->items[0];
  memcpy(s.menu_uid, t->uid, sizeof s.menu_uid);
  memcpy(s.menu_album, t->album_uid, sizeof s.menu_album);
  memcpy(s.menu_artist, t->artist_uid, sizeof s.menu_artist);
  s.menu_here = t->path != NULL;
  g_simple_action_set_enabled(s.go_to_album, track_menu_album_has_page(s.library(s.cb.user), s.menu_album));
  g_simple_action_set_enabled(s.go_to_artist, s.menu_artist[0] != '\0');
  g_simple_action_set_enabled(s.start_radio, playback_can_radio(s.menu_uid));
  mc_track_list_free(list);
  return TRUE;
}

/* Opens the menu at x, y in the strip. */
static void popup_menu_at(float x, float y) {
  gtk_popover_set_pointing_to(GTK_POPOVER(s.menu), &(GdkRectangle){(int)x, (int)y, 1, 1});
  gtk_popover_popup(GTK_POPOVER(s.menu));
}

/* Right-click on the cover or the title. With no menu to open, the header bar gets the click
 * and offers its own. */
static void on_track_click(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  (void)user;
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click));
  graphene_point_t at;
  if ((widget == s.title && s.flash) || !menu_for_playing() ||
      !gtk_widget_compute_point(widget, s.box, &GRAPHENE_POINT_INIT((float)x, (float)y), &at)) {
    gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_DENIED);
    return;
  }
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  s.menu_by_key = FALSE;
  popup_menu_at(at.x, at.y);
}

/* The Menu key or Shift+F10 on the bar: the menu, under the cover. */
static gboolean on_menu_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  (void)user;
  graphene_rect_t at;
  if (!menu_for_playing() || !gtk_widget_compute_bounds(s.picture, s.box, &at)) return FALSE;
  s.menu_by_key = TRUE;
  popup_menu_at(at.origin.x, at.origin.y + at.size.height);
  return TRUE;
}

/* A closed menu gives the focus back to the bar, which GTK would hand elsewhere. */
static void on_menu_closed(GtkPopover *menu, gpointer user) {
  (void)menu;
  (void)user;
  if (s.menu_by_key) gtk_widget_grab_focus(s.bar);
}

/* What the menu's items do, to the track it was opened on, if that's still in the library:
 * the playing track may have changed since, or gone. */
static void on_menu_action(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)param;
  (void)user;
  const char *name = g_action_get_name(G_ACTION(action));
  mc_track_list *list = find_track(s.menu_uid);
  if (!list) return;
  if (!strcmp(name, "add-to-playlist")) {
    if (s.cb.add_to_playlist) s.cb.add_to_playlist(s.cb.user, s.menu_uid, s.menu_here);
  } else if (!strcmp(name, "edit")) {
    if (s.cb.edit_track) s.cb.edit_track(s.cb.user, s.menu_uid);
  } else if (!strcmp(name, "delete")) {
    const char *title = list->items[0].title;
    if (s.cb.delete_track) s.cb.delete_track(s.cb.user, s.menu_uid, title && *title ? title : mc_str(MC_STR_UNTITLED));
  } else if (action == s.start_radio) {
    playback_play_radio(s.menu_uid);
  } else if (action == s.go_to_album) {
    if (s.cb.go_to_album) s.cb.go_to_album(s.cb.user, s.menu_album, s.menu_uid);
  } else if (action == s.go_to_artist) {
    if (s.cb.go_to_artist) s.cb.go_to_artist(s.cb.user, s.menu_artist);
  }
  mc_track_list_free(list);
}

static GSimpleAction *add_action(GSimpleActionGroup *group, const char *name) {
  GSimpleAction *action = g_simple_action_new(name, NULL);
  g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), NULL);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  g_object_unref(action); /* the group keeps it */
  return action;
}

void now_playing_set_track_menu(const track_list_callbacks *cb, mc_library *(*library)(void *user)) {
  s.cb = *cb;
  s.library = library;
  GSimpleActionGroup *actions = g_simple_action_group_new();
  add_action(actions, "add-to-playlist");
  add_action(actions, "edit");
  add_action(actions, "delete");
  s.start_radio = add_action(actions, "start-radio");
  s.go_to_album = add_action(actions, "go-to-album");
  s.go_to_artist = add_action(actions, "go-to-artist");
  gtk_widget_insert_action_group(s.box, "track", G_ACTION_GROUP(actions));
  g_object_unref(actions);

  GMenuModel *model = track_menu_model(FALSE);
  s.menu = gtk_popover_menu_new_from_model(model);
  g_object_unref(model);
  gtk_popover_set_has_arrow(GTK_POPOVER(s.menu), FALSE);
  gtk_widget_set_halign(s.menu, GTK_ALIGN_START);
  gtk_widget_set_parent(s.menu, s.box);
  g_signal_connect(s.menu, "closed", G_CALLBACK(on_menu_closed), NULL);
}

/* Right-clicks on widget open the track's menu. */
static void add_track_click(GtkWidget *widget) {
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_track_click), NULL);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
}

static GtkWidget *time_label(void) {
  GtkWidget *l = gtk_label_new("0:00");
  gtk_widget_add_css_class(l, "numeric");
  gtk_widget_add_css_class(l, "caption");
  gtk_widget_add_css_class(l, "dim-label");
  return l;
}

GtkWidget *now_playing_new(void (*seek)(void *user, int ms), void (*shuffle)(void *user, gboolean on), void *user) {
  memset(&s, 0, sizeof s);
  s.seek = seek;
  s.shuffle = shuffle;
  s.user = user;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(box, "now-playing");
  s.box = box;
  GtkWidget *cover = cover_frame_new(ART, 16, &s.picture);
  add_track_click(cover);
  gtk_box_append(GTK_BOX(box), cover);

  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(right, TRUE);
  gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
  s.title = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(s.title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(s.title, "caption");
  gtk_widget_set_halign(s.title, GTK_ALIGN_CENTER); /* as wide as the text, which right-clicks open the menu on */
  add_track_click(s.title);
  gtk_box_append(GTK_BOX(right), s.title);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  s.cur = time_label();
  gtk_label_set_xalign(GTK_LABEL(s.cur), 1);
  gtk_box_append(GTK_BOX(row), s.cur);
  s.bar = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, KEY_STEP_MS);
  gtk_range_set_increments(GTK_RANGE(s.bar), KEY_STEP_MS, 2 * KEY_STEP_MS);
  gtk_scale_set_draw_value(GTK_SCALE(s.bar), FALSE);
  gtk_widget_set_hexpand(s.bar, TRUE);
  gtk_widget_set_sensitive(s.bar, FALSE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.bar), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_SEEK), -1);
  g_signal_connect(s.bar, "change-value", G_CALLBACK(on_change_value), NULL);
  GtkEventController *legacy = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(legacy, GTK_PHASE_CAPTURE);
  g_signal_connect(legacy, "event", G_CALLBACK(on_bar_event), NULL);
  gtk_widget_add_controller(s.bar, legacy);
  GtkEventController *keys = gtk_shortcut_controller_new();
  const guint menu_keys[][2] = {{GDK_KEY_Menu, 0}, {GDK_KEY_F10, GDK_SHIFT_MASK}};
  for (size_t i = 0; i < G_N_ELEMENTS(menu_keys); i++)
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(keys),
                                         gtk_shortcut_new(gtk_keyval_trigger_new(menu_keys[i][0], menu_keys[i][1]),
                                                          gtk_callback_action_new(on_menu_key, NULL, NULL)));
  gtk_widget_add_controller(s.bar, keys);
  gtk_box_append(GTK_BOX(row), s.bar);
  s.left = time_label();
  gtk_label_set_xalign(GTK_LABEL(s.left), 0);
  gtk_box_append(GTK_BOX(row), s.left);
  gtk_box_append(GTK_BOX(right), row);
  gtk_box_append(GTK_BOX(box), right);

  /* Doesn't take the focus when clicked, like the playback buttons, so Space still plays. */
  s.shuffle_button = gtk_toggle_button_new();
  gtk_button_set_icon_name(GTK_BUTTON(s.shuffle_button), "media-playlist-shuffle-symbolic");
  gtk_widget_add_css_class(s.shuffle_button, "flat");
  gtk_widget_add_css_class(s.shuffle_button, "shuffle");
  gtk_widget_set_valign(s.shuffle_button, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click(s.shuffle_button, FALSE);
  app_set_tooltip(s.shuffle_button, mc_str(MC_STR_SHUFFLE_OFF), "<Control>s");
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.shuffle_button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_SHUFFLE), -1);
  g_signal_connect(s.shuffle_button, "toggled", G_CALLBACK(on_shuffle_toggled), NULL);
  gtk_box_append(GTK_BOX(box), s.shuffle_button);

  now_playing_show(NULL, NULL, 0, NULL);
  return box;
}

static void cover_loaded(GdkTexture *texture, gpointer user) {
  (void)user;
  g_clear_object(&s.cover_loading);
  gtk_picture_set_paintable(GTK_PICTURE(s.picture), GDK_PAINTABLE(texture));
}

/* The cover, for a track playing or with cover_dir NULL nothing, and the accent from it: the
 * bar's fill and shuffle. */
static void show_cover(const char *cover_dir) {
  if (s.cover_loading) g_cancellable_cancel(s.cover_loading);
  g_clear_object(&s.cover_loading);
  GdkTexture *texture = NULL;
  if (cover_dir) {
    s.cover_loading = g_cancellable_new();
    texture = cover_get(cover_dir, ART * gtk_widget_get_scale_factor(s.picture), s.cover_loading, cover_loaded, NULL);
  }
  gtk_picture_set_paintable(GTK_PICTURE(s.picture), GDK_PAINTABLE(texture));
  g_clear_object(&texture);
  char *cover = cover_path(cover_dir);
  art_accent_set_file(s.box, cover);
  g_free(cover);
}

void now_playing_art_changed(const char *cover_dir) {
  if (s.playing) show_cover(cover_dir);
}

void now_playing_show(const char *title, const char *artist, int duration_ms, const char *cover_dir) {
  s.playing = title != NULL;
  s.duration_ms = title && duration_ms > 0 ? duration_ms : 0;
  s.pressed = s.seek_pending = FALSE;

  char *byline = artist ? mc_format_alloc(mc_str(MC_STR_NOW_PLAYING_ARTIST), MC_S(artist)) : NULL;
  char *markup = !title ? g_strdup("")
                 : byline ? g_markup_printf_escaped("<b>%s</b> <span alpha=\"60%%\">%s</span>", title, byline)
                          : g_markup_printf_escaped("<b>%s</b>", title);
  if (s.flash) { /* shown once the flash is over */
    g_free(s.markup);
    s.markup = markup;
  } else {
    gtk_label_set_markup(GTK_LABEL(s.title), markup);
    g_free(markup);
  }
  mc_free(byline);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.title), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 title ? title : mc_str(MC_STR_NOTHING_PLAYING), -1);

  /* Both times get room for the longest the track can show, so the bar doesn't move. */
  char widest[24] = "-";
  format_time(s.duration_ms, widest + 1, sizeof widest - 1);
  int chars = MAX((int)strlen(widest), 5);
  gtk_label_set_width_chars(GTK_LABEL(s.cur), chars);
  gtk_label_set_width_chars(GTK_LABEL(s.left), chars);

  s.updating = TRUE;
  gtk_range_set_range(GTK_RANGE(s.bar), 0, s.duration_ms > 0 ? s.duration_ms : 1);
  s.updating = FALSE;
  gtk_widget_set_sensitive(s.bar, seekable());
  set_bar(0);
  show_cover(title ? cover_dir : NULL);
}

void now_playing_set_shuffle(gboolean on) {
  if (!s.shuffle_button) return;
  s.updating = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s.shuffle_button), on);
  s.updating = FALSE;
}

void now_playing_set_position(int ms) {
  if (!s.pressed) set_bar(MAX(0, MIN(ms, s.duration_ms)));
}

static gboolean end_flash(gpointer user) {
  (void)user;
  s.flash = 0;
  gtk_label_set_markup(GTK_LABEL(s.title), s.markup ? s.markup : "");
  g_clear_pointer(&s.markup, g_free);
  return G_SOURCE_REMOVE;
}

void now_playing_flash(const char *text) {
  if (!s.title) return;
  if (s.flash) g_source_remove(s.flash);
  else s.markup = g_strdup(gtk_label_get_label(GTK_LABEL(s.title)));
  gtk_label_set_text(GTK_LABEL(s.title), text ? text : "");
  s.flash = g_timeout_add(FLASH_MS, end_flash, NULL);
}
