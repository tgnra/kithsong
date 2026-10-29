#include "search_view.h"

#include <string.h>

#include "album_grid.h"

enum {
  LIMIT_PER_KIND = 20,
  SEARCH_DELAY_MS = 100, /* the pause in typing before results change */
  KIND_COUNT = MC_SEARCH_ALBUM_GROUP + 1,
};

static struct {
  app *a;
  GtkWidget *entry, *results, *no_results;
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
  if (!uid) return;
  char copy[MC_UID_STRLEN], album_copy[MC_UID_STRLEN];
  g_strlcpy(copy, uid, sizeof copy);
  g_strlcpy(album_copy, album_uid ? album_uid : "", sizeof album_copy);
  switch ((mc_search_kind)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"))) {
    case MC_SEARCH_TRACK: app_open_track(v.a, album_copy, copy); break;
    case MC_SEARCH_ARTIST: app_open_artist(v.a, copy); break;
    case MC_SEARCH_ALBUM_GROUP: app_open_album_group(v.a, copy); break;
    case MC_SEARCH_PLAYLIST: app_open(v.a, TRUE, copy); break;
    default: app_open(v.a, FALSE, copy); break;
  }
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
    gtk_flow_box_child_set_child(GTK_FLOW_BOX_CHILD(child), album_tile_new(album));
    g_object_set_data_full(G_OBJECT(child), "uid", g_strdup(h->uid), g_free);
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

/* Whether this computer has a page to open the hit on. */
static gboolean hit_opens(const mc_search_hit *h) {
  switch (h->kind) {
    case MC_SEARCH_ARTIST: return TRUE;
    case MC_SEARCH_TRACK: return grid_has(v.a->album_grid, h->album_uid); /* gone since the index was built? */
    case MC_SEARCH_PLAYLIST: return grid_has(v.a->playlist_grid, h->uid); /* only playable ones */
    case MC_SEARCH_ALBUM_GROUP: return TRUE;
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

  gboolean opens = hit_opens(h);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), opens);
  if (opens) {
    g_object_set_data_full(G_OBJECT(row), "uid", g_strdup(h->uid), g_free);
    g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(h->kind));
    if (h->kind == MC_SEARCH_TRACK) g_object_set_data_full(G_OBJECT(row), "album-uid", g_strdup(h->album_uid), g_free);
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

GtkWidget *search_view_new(app *a) {
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

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_box_append(GTK_BOX(box), entry_clamp);
  gtk_box_append(GTK_BOX(box), results_clamp);
  gtk_box_append(GTK_BOX(box), v.no_results);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
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
