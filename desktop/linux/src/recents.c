#include "recents.h"

#include <string.h>

enum { MAX_RECENTS = 10 };

static struct {
  app *a;
  GtkWidget *heading, *list, *menu; /* menu: a row's, Remove from Recents and Clear Recents */
  char menu_uid[MC_UID_STRLEN];     /* the recent the menu is for */
} r;

/* The icon of the sidebar page it opens on. */
static const char *kind_icon(mc_recent_kind kind) {
  switch (kind) {
    case MC_RECENT_PLAYLIST: return "view-list-symbolic";
    case MC_RECENT_ARTIST: return "avatar-default-symbolic";
    case MC_RECENT_ALBUM_GROUP: return "folder-symbolic";
    default: return "media-optical-symbolic";
  }
}

/* A row like the pages above it: the icon and the name, cut off if it's long, with the
 * whole name and the album's artist (or what it is: "Playlist", "Artist", "Album group",
 * "Smart playlist", "Smart album group") in the tooltip. */
static GtkWidget *recent_row(const mc_recent_info *info) {
  const char *name = info->name && *info->name ? info->name : mc_str(MC_STR_UNTITLED);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(kind_icon(info->kind)));
  GtkWidget *label = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);

  const char *detail = info->kind == MC_RECENT_PLAYLIST && info->is_smart      ? mc_str(MC_STR_SMART_PLAYLIST)
                       : info->kind == MC_RECENT_PLAYLIST                      ? mc_str(MC_STR_PLAYLIST)
                       : info->kind == MC_RECENT_ARTIST                        ? mc_str(MC_STR_ARTIST)
                       : info->kind == MC_RECENT_ALBUM_GROUP && info->is_smart ? mc_str(MC_STR_SMART_ALBUM_GROUP)
                       : info->kind == MC_RECENT_ALBUM_GROUP                   ? mc_str(MC_STR_ALBUM_GROUP)
                       : info->artist                        ? info->artist
                                                             : mc_str(MC_STR_UNKNOWN_ARTIST);
  char *tip = g_strdup_printf("%s\n%s", name, detail);
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, tip);
  g_free(tip);
  g_object_set_data_full(G_OBJECT(row), "uid", g_strdup(info->uid), g_free);
  g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(info->kind));
  return row;
}

static void on_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  if (!uid) return;
  char copy[MC_UID_STRLEN];
  g_strlcpy(copy, uid, sizeof copy); /* the row goes if opening refreshes the section */
  switch ((mc_recent_kind)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"))) {
    case MC_RECENT_ARTIST: app_open_artist(user, copy); break;
    case MC_RECENT_ALBUM_GROUP: app_open_album_group(user, copy); break;
    case MC_RECENT_PLAYLIST: app_open(user, TRUE, copy); break;
    default: app_open(user, FALSE, copy); break;
  }
}

/* --- The rows' menu --- */

static void on_remove(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  if (!r.a->lib || !r.menu_uid[0]) return;
  mc_status st = mc_library_remove_recent(r.a->lib, r.menu_uid);
  if (st == MC_OK || st == MC_ERR_NOT_FOUND) recents_refresh(r.a); /* gone already: sync took it */
}

static void on_clear(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  if (r.a->lib && mc_library_clear_recents(r.a->lib) == MC_OK) recents_refresh(r.a);
}

/* row's menu, at x, y in widget. */
static void popup_menu(GtkListBoxRow *row, GtkWidget *widget, double x, double y) {
  const char *uid = row ? g_object_get_data(G_OBJECT(row), "uid") : NULL;
  graphene_point_t at;
  if (!uid || !gtk_widget_compute_point(widget, gtk_widget_get_parent(r.menu),
                                        &GRAPHENE_POINT_INIT((float)x, (float)y), &at))
    return;
  g_strlcpy(r.menu_uid, uid, sizeof r.menu_uid);
  gtk_popover_set_pointing_to(GTK_POPOVER(r.menu), &(GdkRectangle){(int)at.x, (int)at.y, 1, 1});
  gtk_popover_popup(GTK_POPOVER(r.menu));
}

/* Right-click, or a long press on a touchscreen, on a row. */
static void on_click(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  (void)user;
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(r.list), (int)y);
  if (!row) return;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  popup_menu(row, r.list, x, y);
}

static void on_long_press(GtkGestureLongPress *press, double x, double y, gpointer user) {
  (void)user;
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(r.list), (int)y);
  if (!row) return;
  gtk_gesture_set_state(GTK_GESTURE(press), GTK_EVENT_SEQUENCE_CLAIMED); /* not opening it too */
  popup_menu(row, r.list, x, y);
}

/* The Menu key or Shift+F10: the focused row's menu, under it. */
static gboolean on_menu_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)widget;
  (void)args;
  (void)user;
  GtkRoot *root = gtk_widget_get_root(r.list);
  for (GtkWidget *w = root ? gtk_root_get_focus(root) : NULL; w && w != r.list; w = gtk_widget_get_parent(w)) {
    if (!GTK_IS_LIST_BOX_ROW(w)) continue;
    popup_menu(GTK_LIST_BOX_ROW(w), w, 0, gtk_widget_get_height(w));
    return TRUE;
  }
  return FALSE;
}

GtkWidget *recents_new(app *a) {
  r.a = a;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  r.heading = gtk_label_new(mc_str(MC_STR_SECTION_RECENTS));
  gtk_widget_add_css_class(r.heading, "heading");
  gtk_widget_add_css_class(r.heading, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(r.heading), 0);
  gtk_widget_set_margin_start(r.heading, 18);
  gtk_widget_set_margin_top(r.heading, 12);
  gtk_box_append(GTK_BOX(box), r.heading);

  /* Buttons rather than pages: nothing stays selected. */
  r.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(r.list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(r.list, "navigation-sidebar");
  g_signal_connect(r.list, "row-activated", G_CALLBACK(on_activated), a);
  gtk_box_append(GTK_BOX(box), r.list);

  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_click), NULL);
  gtk_widget_add_controller(r.list, GTK_EVENT_CONTROLLER(click));
  GtkGesture *press = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(press), TRUE);
  g_signal_connect(press, "pressed", G_CALLBACK(on_long_press), NULL);
  gtk_widget_add_controller(r.list, GTK_EVENT_CONTROLLER(press));
  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Menu, 0), gtk_callback_action_new(on_menu_key, NULL, NULL)));
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F10, GDK_SHIFT_MASK),
                     gtk_callback_action_new(on_menu_key, NULL, NULL)));
  gtk_widget_add_controller(r.list, keys);

  GMenu *model = g_menu_new();
  g_menu_append(model, mc_str_mnemonic(MC_STR_RECENT_REMOVE_MENU), "recents.remove");
  g_menu_append(model, mc_str_mnemonic(MC_STR_RECENTS_CLEAR_MENU), "recents.clear");
  r.menu = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
  g_object_unref(model);
  gtk_popover_set_has_arrow(GTK_POPOVER(r.menu), FALSE);
  gtk_widget_set_halign(r.menu, GTK_ALIGN_START);
  gtk_widget_set_parent(r.menu, box);
  GSimpleActionGroup *actions = g_simple_action_group_new();
  const GActionEntry entries[] = {{"remove", on_remove, NULL, NULL, NULL, {0}},
                                  {"clear", on_clear, NULL, NULL, NULL, {0}}};
  g_action_map_add_action_entries(G_ACTION_MAP(actions), entries, G_N_ELEMENTS(entries), NULL);
  gtk_widget_insert_action_group(box, "recents", G_ACTION_GROUP(actions));
  g_object_unref(actions);
  gtk_widget_set_visible(box, FALSE); /* until there are some */
  return box;
}

void recents_refresh(app *a) {
  if (!r.list) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(r.list))) gtk_list_box_remove(GTK_LIST_BOX(r.list), child);

  mc_recent_list *list = NULL;
  if (!a->lib || mc_library_list_recents(a->lib, 0, MC_LIST_PLAYABLE, &list) != MC_OK) list = NULL;
  int n = 0;
  for (int i = 0; list && i < list->count && n < MAX_RECENTS; i++) {
    const mc_recent_info *info = &list->items[i];
    gtk_list_box_append(GTK_LIST_BOX(r.list), recent_row(info));
    n++;
  }
  mc_recent_list_free(list);
  gtk_widget_set_visible(gtk_widget_get_parent(r.list), n > 0);
}
