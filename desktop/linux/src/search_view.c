#include "search_view.h"

#include <string.h>

#include "album_grid.h"
#include "album_groups.h"
#include "artist_view.h"
#include "drag.h"
#include "home_view.h"
#include "playback.h"
#include "track_menu.h"

enum {
  LIMIT_PER_KIND = 20,
  SEARCH_DELAY_MS = 100, /* the pause in typing before results change */
  KIND_COUNT = MC_SEARCH_ALBUM_GROUP + 1,
};

static struct {
  app *a;
  GtkWidget *entry, *results, *no_results;
  GtkWidget *box;         /* holds them, and shows the menu */
  GtkWidget *menu;        /* the results' */
  GtkWidget *menu_result; /* what it was opened on, while it lasts */
  track_list_callbacks cb;
  GSimpleAction *start_radio, *go_to_album, *go_to_artist;
  char menu_uid[MC_UID_STRLEN];    /* the track it was opened on */
  char menu_album[MC_UID_STRLEN];  /* and its album, or "" */
  char menu_artist[MC_UID_STRLEN]; /* and its artist, or "" */
  gboolean menu_here;              /* and whether it has a file here */
  char *query; /* last searched for, or NULL */
} v;

static const char *kind_heading(mc_search_kind kind) {
  switch (kind) {
    case MC_SEARCH_ARTIST: return mc_str(MC_STR_SECTION_ARTISTS);
    case MC_SEARCH_ALBUM: return mc_str(MC_STR_SECTION_ALBUMS);
    case MC_SEARCH_TRACK: return mc_str(MC_STR_TRACKS_TITLE);
    case MC_SEARCH_PLAYLIST: return mc_str(MC_STR_SECTION_PLAYLISTS);
    default: return mc_str(MC_STR_SECTION_ALBUM_GROUPS);
  }
}

/* As the sidebar's pages and Recents show them. */
static const char *kind_icon(mc_search_kind kind) {
  switch (kind) {
    case MC_SEARCH_ARTIST: return "avatar-default-symbolic";
    case MC_SEARCH_ALBUM: return "media-optical-symbolic";
    case MC_SEARCH_TRACK: return "audio-x-generic-symbolic";
    case MC_SEARCH_PLAYLIST: return "view-list-symbolic";
    default: return "folder-symbolic";
  }
}

static GtkWidget *section_heading(mc_search_kind kind) {
  GtkWidget *label = gtk_label_new(kind_heading(kind));
  gtk_widget_add_css_class(label, "title-4");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_margin_bottom(label, 6);
  return label;
}

/* --- Opening a result --- */

/* The uids are copied first: opening may refresh the results, and the row goes. */
static void on_album_activated(GtkFlowBox *box, GtkFlowBoxChild *child, gpointer user) {
  (void)box;
  (void)user;
  char uid[MC_UID_STRLEN];
  g_strlcpy(uid, g_object_get_data(G_OBJECT(child), "uid"), sizeof uid);
  app_open(v.a, FALSE, uid);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  (void)user;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  const char *album_uid = g_object_get_data(G_OBJECT(row), "album-uid");
  const char *artist_uid = g_object_get_data(G_OBJECT(row), "artist-uid");
  if (!uid) return;
  char copy[MC_UID_STRLEN], album_copy[MC_UID_STRLEN], artist_copy[MC_UID_STRLEN];
  g_strlcpy(copy, uid, sizeof copy);
  g_strlcpy(album_copy, album_uid ? album_uid : "", sizeof album_copy);
  g_strlcpy(artist_copy, artist_uid ? artist_uid : "", sizeof artist_copy);
  switch ((mc_search_kind)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"))) {
    case MC_SEARCH_TRACK:
      /* A single (or a track with no album) isn't on the Albums page: its artist's page has it. */
      if (album_copy[0]) app_open_track(v.a, album_copy, copy);
      else app_open_artist(v.a, artist_copy);
      break;
    case MC_SEARCH_ARTIST: app_open_artist(v.a, copy); break;
    case MC_SEARCH_ALBUM_GROUP: app_open_album_group(v.a, copy); break;
    case MC_SEARCH_PLAYLIST: app_open(v.a, TRUE, copy); break;
    default: app_open(v.a, FALSE, copy); break;
  }
}

/* --- Menus --- */

/* The track menu's items, for the track it was opened on, if that's still in the library. */
static void on_track_action(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)param;
  (void)user;
  const char *name = g_action_get_name(G_ACTION(action));
  char uid[MC_UID_STRLEN], album[MC_UID_STRLEN], artist[MC_UID_STRLEN];
  memcpy(uid, v.menu_uid, sizeof uid); /* going to an album or artist can refresh the results */
  memcpy(album, v.menu_album, sizeof album);
  memcpy(artist, v.menu_artist, sizeof artist);
  mc_track_list *list = NULL;
  if (!v.a->lib || mc_library_list_track(v.a->lib, uid, &list) != MC_OK) return;
  const char *title = list->count ? list->items[0].title : NULL;
  if (list->count) {
    if (!strcmp(name, "add-to-playlist")) v.cb.add_to_playlist(v.cb.user, uid, v.menu_here);
    else if (!strcmp(name, "edit")) v.cb.edit_track(v.cb.user, uid);
    else if (!strcmp(name, "delete")) v.cb.delete_track(v.cb.user, uid, title && *title ? title : mc_str(MC_STR_UNTITLED));
    else if (action == v.start_radio) playback_play_radio(uid);
    else if (action == v.go_to_album) v.cb.go_to_album(v.cb.user, album, uid);
    else if (action == v.go_to_artist) v.cb.go_to_artist(v.cb.user, artist);
  }
  mc_track_list_free(list);
}

/* Makes the track menu act on track uid; FALSE if the library hasn't got it. */
static gboolean menu_for_track(const char *uid) {
  mc_track_list *list = NULL;
  if (!v.a->lib || mc_library_list_track(v.a->lib, uid, &list) != MC_OK) return FALSE;
  gboolean found = list->count > 0;
  if (found) {
    const mc_track_info *t = &list->items[0];
    memcpy(v.menu_uid, t->uid, sizeof v.menu_uid);
    memcpy(v.menu_album, t->album_uid, sizeof v.menu_album);
    memcpy(v.menu_artist, t->artist_uid, sizeof v.menu_artist);
    v.menu_here = t->path != NULL;
    g_simple_action_set_enabled(v.go_to_album, track_menu_album_has_page(v.a->lib, v.menu_album));
    g_simple_action_set_enabled(v.go_to_artist, v.menu_artist[0] != '\0');
    g_simple_action_set_enabled(v.start_radio, playback_can_radio(v.menu_uid));
  }
  mc_track_list_free(list);
  return found;
}

/* The menu result (a row or an album's tile) has wherever else it shows; NULL if it's gone. */
static GMenuModel *result_menu(GtkWidget *result) {
  const char *uid = g_object_get_data(G_OBJECT(result), "uid");
  const char *name = g_object_get_data(G_OBJECT(result), "name");
  gboolean smart = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(result), "smart"));
  switch ((mc_search_kind)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(result), "kind"))) {
    case MC_SEARCH_ARTIST:
      gtk_widget_insert_action_group(v.box, "artist", artist_view_actions());
      return artist_view_menu(uid);
    case MC_SEARCH_ALBUM: return album_groups_album_model(v.a, uid, name);
    case MC_SEARCH_TRACK: return menu_for_track(uid) ? track_menu_model(FALSE) : NULL;
    case MC_SEARCH_PLAYLIST:
      gtk_widget_insert_action_group(v.box, "home", home_view_actions());
      return home_view_playlist_menu(uid, name, smart);
    case MC_SEARCH_ALBUM_GROUP: return album_groups_group_model(uid, smart);
    default: return NULL;
  }
}

/* result's menu, at x, y in widget. */
static void popup_menu(GtkWidget *result, GtkWidget *widget, double x, double y) {
  graphene_point_t at;
  if (!v.a->lib || !gtk_widget_compute_point(widget, v.box, &GRAPHENE_POINT_INIT((float)x, (float)y), &at)) return;
  GMenuModel *menu = result_menu(result);
  if (!menu) return;
  g_set_weak_pointer(&v.menu_result, result);
  gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(v.menu), menu);
  g_object_unref(menu); /* the popover keeps it */
  gtk_popover_set_pointing_to(GTK_POPOVER(v.menu), &(GdkRectangle){(int)at.x, (int)at.y, 1, 1});
  gtk_popover_popup(GTK_POPOVER(v.menu));
}

/* A closed menu gives the focus back to its result, as GTK would hand it elsewhere, unless what it
 * chose opened a dialog or took the result away. */
static void on_menu_closed(GtkPopover *menu, gpointer user) {
  (void)menu;
  (void)user;
  if (v.menu_result && !adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(v.a->win)))
    gtk_widget_grab_focus(v.menu_result);
}

/* Right-click on a tile or a row, or a long press on a touchscreen; user is the result. */
static void on_result_click(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  popup_menu(user, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(click)), x, y);
}

static void on_result_long_press(GtkGestureLongPress *press, double x, double y, gpointer user) {
  gtk_gesture_set_state(GTK_GESTURE(press), GTK_EVENT_SEQUENCE_CLAIMED); /* not opening it too */
  popup_menu(user, gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(press)), x, y);
}

/* The Menu key or Shift+F10: the focused result's menu, under it. */
static gboolean on_menu_key(GtkWidget *widget, GVariant *args, gpointer user) {
  (void)args;
  (void)user;
  GtkRoot *root = gtk_widget_get_root(widget);
  for (GtkWidget *w = root ? gtk_root_get_focus(root) : NULL; w && w != v.results; w = gtk_widget_get_parent(w)) {
    if (!g_object_get_data(G_OBJECT(w), "uid")) continue;
    popup_menu(w, w, 0, gtk_widget_get_height(w));
    return TRUE;
  }
  return FALSE;
}

/* An album's tile drags its album and a track's row its track, as they do elsewhere; only the
 * sidebar's recents can take them from here. */
static gboolean on_result_drag(gpointer user, drag_item *item) {
  gboolean album = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(user), "kind")) == MC_SEARCH_ALBUM;
  item->kind = album ? DRAG_ALBUM : DRAG_TRACK;
  g_strlcpy(item->uid, g_object_get_data(G_OBJECT(user), "uid"), sizeof item->uid);
  return TRUE;
}

/* Makes result the hit h, with its menu and drag on widget: the result itself or what shows it. */
static void result_setup(GtkWidget *result, GtkWidget *widget, const mc_search_hit *h) {
  g_object_set_data_full(G_OBJECT(result), "uid", g_strdup(h->uid), g_free);
  g_object_set_data_full(G_OBJECT(result), "name", g_strdup(h->name && *h->name ? h->name : mc_str(MC_STR_UNTITLED)), g_free);
  g_object_set_data(G_OBJECT(result), "kind", GINT_TO_POINTER(h->kind));
  g_object_set_data(G_OBJECT(result), "smart", GINT_TO_POINTER(h->is_smart));
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "released", G_CALLBACK(on_result_click), result);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(click));
  GtkGesture *press = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(press), TRUE);
  g_signal_connect(press, "pressed", G_CALLBACK(on_result_long_press), result);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(press));
  if (h->kind == MC_SEARCH_ALBUM || h->kind == MC_SEARCH_TRACK) drag_source_new(widget, on_result_drag, result);
}

/* --- Sections --- */

/* The album hits that are still in the album grid, as tiles; NULL if none are. */
static GtkWidget *album_section(const mc_search_results *res) {
  GtkWidget *flow = NULL;
  for (int i = 0; i < res->count; i++) {
    const mc_search_hit *h = &res->items[i];
    if (h->kind != MC_SEARCH_ALBUM) continue;
    AlbumItem *album = album_grid_find(v.a->album_grid, h->uid);
    if (!album) continue; /* gone since the index was built */
    if (!flow) {
      flow = gtk_flow_box_new();
      gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), TRUE);
      gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
      gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
      gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 24);
      gtk_widget_add_css_class(flow, "search-albums");
      gtk_accessible_update_property(GTK_ACCESSIBLE(flow), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                     mc_str(MC_STR_SECTION_ALBUMS), -1);
      g_signal_connect(flow, "child-activated", G_CALLBACK(on_album_activated), NULL);
    }
    GtkWidget *child = gtk_flow_box_child_new();
    GtkWidget *tile = album_tile_new(album);
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), tile);
    result_setup(child, tile, h);
    gtk_flow_box_append(GTK_FLOW_BOX(flow), child);
    g_object_unref(album);
  }
  return flow;
}

/* A track's artist and album, or whichever is known; a playlist's "Playlist", a group's
 * "Album group" (or "Smart playlist", "Smart album group"). g_free it. */
static char *hit_subtitle(const mc_search_hit *h) {
  if (h->kind == MC_SEARCH_PLAYLIST) return g_strdup(mc_str(h->is_smart ? MC_STR_SMART_PLAYLIST : MC_STR_PLAYLIST));
  if (h->kind == MC_SEARCH_ALBUM_GROUP)
    return g_strdup(mc_str(h->is_smart ? MC_STR_SMART_ALBUM_GROUP : MC_STR_ALBUM_GROUP));
  if (h->kind != MC_SEARCH_TRACK) return NULL;
  if (h->artist && h->album) {
    char *s = mc_format_alloc(mc_str(MC_STR_TRACK_BYLINE), MC_S(h->artist), MC_S(h->album));
    char *copy = g_strdup(s);
    mc_free(s);
    return copy;
  }
  return g_strdup(h->artist ? h->artist : h->album);
}

/* Whether grid has the item with uid. */
static gboolean grid_has(GtkWidget *grid, const char *uid) {
  AlbumItem *item = uid[0] ? album_grid_find(grid, uid) : NULL;
  if (item) g_object_unref(item);
  return item != NULL;
}

/* The artist page to open a track hit on when its album isn't in the album grid: a single's,
 * or a track's with no album. NULL if there's none to open (borrowed). */
static const char *track_artist(const mc_search_hit *h) {
  if (grid_has(v.a->album_grid, h->album_uid)) return NULL;
  return artist_view_find(h->artist ? h->artist : mc_str(MC_STR_UNKNOWN_ARTIST));
}

/* Whether this computer has a page to open the hit on. */
static gboolean hit_opens(const mc_search_hit *h) {
  switch (h->kind) {
    case MC_SEARCH_ARTIST: return TRUE;
    case MC_SEARCH_TRACK: /* gone since the index was built? */
      return grid_has(v.a->album_grid, h->album_uid) || track_artist(h);
    case MC_SEARCH_PLAYLIST: return h->is_smart || grid_has(v.a->playlist_grid, h->uid); /* only playable ones */
    case MC_SEARCH_ALBUM_GROUP: return h->is_smart || album_groups_shows(h->uid); /* the same */
    default: return FALSE;
  }
}

static GtkWidget *hit_row(const mc_search_hit *h) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), h->name && *h->name ? h->name : mc_str(MC_STR_UNTITLED));
  char *subtitle = hit_subtitle(h);
  if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  g_free(subtitle);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name(kind_icon(h->kind)));

  result_setup(row, row, h);
  gboolean opens = hit_opens(h);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), opens);
  if (opens) {
    const char *artist = h->kind == MC_SEARCH_TRACK ? track_artist(h) : NULL;
    if (artist) g_object_set_data_full(G_OBJECT(row), "artist-uid", g_strdup(artist), g_free);
    else if (h->kind == MC_SEARCH_TRACK)
      g_object_set_data_full(G_OBJECT(row), "album-uid", g_strdup(h->album_uid), g_free);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
  }
  return row;
}

static GtkWidget *list_section(const mc_search_results *res, mc_search_kind kind) {
  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  gtk_accessible_update_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL, kind_heading(kind), -1);
  g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  for (int i = 0; i < res->count; i++)
    if (res->items[i].kind == kind) gtk_list_box_append(GTK_LIST_BOX(list), hit_row(&res->items[i]));
  return list;
}

/* --- Searching --- */

static void show_results(const char *query) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(v.results))) gtk_box_remove(GTK_BOX(v.results), child);
  gtk_widget_set_visible(v.no_results, FALSE);
  if (!query || !*query) return;

  mc_search_results *res = NULL;
  if (!v.a->lib || mc_library_search(v.a->lib, query, LIMIT_PER_KIND, &res) != MC_OK) res = NULL;

  /* Hits come best first, so the order kinds first turn up in is their sections' order. */
  mc_search_kind order[KIND_COUNT];
  gboolean seen[KIND_COUNT] = {FALSE};
  int kinds = 0;
  for (int i = 0; res && i < res->count; i++) {
    mc_search_kind k = res->items[i].kind;
    if ((unsigned)k < KIND_COUNT && !seen[k]) seen[k] = TRUE, order[kinds++] = k; /* a newer core's kinds are skipped */
  }

  int shown = 0;
  for (int i = 0; i < kinds; i++) {
    GtkWidget *body = order[i] == MC_SEARCH_ALBUM ? album_section(res) : list_section(res, order[i]);
    if (!body) continue;
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(section), section_heading(order[i]));
    gtk_box_append(GTK_BOX(section), body);
    gtk_box_append(GTK_BOX(v.results), section);
    shown++;
  }
  mc_search_results_free(res);

  if (!shown) {
    char *desc = mc_format_alloc(mc_str(MC_STR_SEARCH_NO_RESULTS_TEXT), MC_S(query));
    adw_status_page_set_description(ADW_STATUS_PAGE(v.no_results), desc);
    mc_free(desc);
    gtk_widget_set_visible(v.no_results, TRUE);
  }
}

static void search(void) {
  g_free(v.query);
  v.query = g_strdup(gtk_editable_get_text(GTK_EDITABLE(v.entry)));
  show_results(v.query);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user) {
  (void)entry;
  (void)user;
  search();
}

/* Enter doesn't wait for the pause. */
static void on_entry_activate(GtkSearchEntry *entry, gpointer user) {
  (void)entry;
  (void)user;
  search();
}

static void on_destroy(GtkWidget *widget, gpointer user) {
  (void)widget;
  (void)user;
  g_clear_pointer(&v.query, g_free);
  memset(&v, 0, sizeof v);
}

static GSimpleAction *add_track_action(GSimpleActionGroup *group, const char *name) {
  GSimpleAction *action = g_simple_action_new(name, NULL);
  g_signal_connect(action, "activate", G_CALLBACK(on_track_action), NULL);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
  g_object_unref(action); /* the group keeps it */
  return action;
}

GtkWidget *search_view_new(app *a, const track_list_callbacks *cb) {
  v.cb = *cb;
  v.a = a;
  v.entry = gtk_search_entry_new();
  gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(v.entry), mc_str(MC_STR_SEARCH_PLACEHOLDER));
  gtk_search_entry_set_search_delay(GTK_SEARCH_ENTRY(v.entry), SEARCH_DELAY_MS);
  gtk_widget_add_css_class(v.entry, "search-large");
  gtk_widget_set_hexpand(v.entry, TRUE);
  g_signal_connect(v.entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
  g_signal_connect(v.entry, "activate", G_CALLBACK(on_entry_activate), NULL);
  GtkWidget *entry_clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(entry_clamp), 560);
  adw_clamp_set_child(ADW_CLAMP(entry_clamp), v.entry);
  gtk_widget_set_margin_top(entry_clamp, 36);

  v.results = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
  GtkWidget *results_clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(results_clamp), 1100);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(results_clamp), 1100);
  adw_clamp_set_child(ADW_CLAMP(results_clamp), v.results);
  gtk_widget_set_margin_top(results_clamp, 24);

  v.no_results = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(v.no_results), "system-search-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(v.no_results), mc_str(MC_STR_SEARCH_NO_RESULTS_TITLE));
  gtk_widget_set_visible(v.no_results, FALSE);

  GtkWidget *box = v.box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_box_append(GTK_BOX(box), entry_clamp);
  gtk_box_append(GTK_BOX(box), results_clamp);
  gtk_box_append(GTK_BOX(box), v.no_results);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
  v.menu = gtk_popover_menu_new_from_model(NULL);
  gtk_popover_set_has_arrow(GTK_POPOVER(v.menu), FALSE);
  gtk_widget_set_halign(v.menu, GTK_ALIGN_START);
  gtk_widget_set_parent(v.menu, box);
  g_signal_connect(v.menu, "closed", G_CALLBACK(on_menu_closed), NULL);
  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Menu, 0), gtk_callback_action_new(on_menu_key, NULL, NULL)));
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_F10, GDK_SHIFT_MASK),
                     gtk_callback_action_new(on_menu_key, NULL, NULL)));
  gtk_widget_add_controller(v.results, keys);

  /* The track menu's items, which act on the track it was opened on. */
  GSimpleActionGroup *actions = g_simple_action_group_new();
  add_track_action(actions, "add-to-playlist");
  add_track_action(actions, "edit");
  add_track_action(actions, "delete");
  v.start_radio = add_track_action(actions, "start-radio");
  v.go_to_album = add_track_action(actions, "go-to-album");
  v.go_to_artist = add_track_action(actions, "go-to-artist");
  gtk_widget_insert_action_group(box, "track", G_ACTION_GROUP(actions));
  g_object_unref(actions);

  g_signal_connect(scroller, "destroy", G_CALLBACK(on_destroy), NULL);
  return scroller;
}

void search_view_focus(void) {
  if (v.entry) gtk_widget_grab_focus(v.entry);
}

void search_view_start(const char *text) {
  if (!v.entry) return;
  gtk_widget_grab_focus(v.entry);
  gtk_editable_set_text(GTK_EDITABLE(v.entry), text);
  gtk_editable_set_position(GTK_EDITABLE(v.entry), -1);
}

void search_view_refresh(void) {
  if (!v.results) return;
  char *query = g_strdup(v.query);
  show_results(query);
  g_free(query);
}
