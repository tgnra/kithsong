/* GTK4 + libadwaita frontend. Layout:
 *
 *   AdwToolbarView
 *   +- AdwHeaderBar: [prev][play][next]  ...  [import][settings]
 *   +- AdwOverlaySplitView (sidebar: Albums / Artists / Playlists / Devices)
 *      +- AdwOverlaySplitView, sidebar at the end (track list, hidden until an album is picked)
 *         +- GtkStack: empty-state page | GtkGridView of albums (virtualised)
 */
#include <adwaita.h>

#include "musiccore/musiccore.h"

typedef struct app {
  mc_library *lib;
} app;

static GtkWidget *nav_row(const char *icon, const char *label) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(icon));
  gtk_box_append(GTK_BOX(box), gtk_label_new(label));
  return box;
}

static GtkWidget *build_sidebar(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  GtkWidget *nav = gtk_list_box_new();
  gtk_widget_add_css_class(nav, "navigation-sidebar");
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("media-optical-symbolic", "Albums"));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("avatar-default-symbolic", "Artists"));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("view-list-symbolic", "Playlists"));
  gtk_list_box_select_row(GTK_LIST_BOX(nav), gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), 0));
  gtk_box_append(GTK_BOX(box), nav);

  GtkWidget *heading = gtk_label_new("Devices");
  gtk_widget_add_css_class(heading, "heading");
  gtk_widget_add_css_class(heading, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_set_margin_start(heading, 18);
  gtk_widget_set_margin_top(heading, 18);
  gtk_widget_set_margin_bottom(heading, 6);
  gtk_box_append(GTK_BOX(box), heading);

  /* Online peers, then paired peers that are currently unreachable. */
  GtkWidget *devices = gtk_list_box_new();
  gtk_widget_add_css_class(devices, "navigation-sidebar");
  GtkWidget *none = gtk_label_new("No devices found");
  gtk_widget_add_css_class(none, "dim-label");
  gtk_list_box_set_placeholder(GTK_LIST_BOX(devices), none);
  gtk_box_append(GTK_BOX(box), devices);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
  return scroller;
}

static void album_setup(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  gtk_list_item_set_child(item, gtk_label_new(NULL));
}

static void album_bind(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer user) {
  (void)factory;
  (void)user;
  GtkStringObject *obj = gtk_list_item_get_item(item);
  gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(item)), gtk_string_object_get_string(obj));
}

static GtkWidget *build_albums(app *a) {
  int64_t albums = 0;
  if (a->lib) mc_library_count(a->lib, MC_ENTITY_ALBUMS, &albums);

  /* Placeholder model until the library query API exists. */
  GtkStringList *model = gtk_string_list_new(NULL);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(album_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(album_bind), NULL);
  GtkWidget *grid = gtk_grid_view_new(
    GTK_SELECTION_MODEL(gtk_single_selection_new(G_LIST_MODEL(model))), factory);
  gtk_grid_view_set_max_columns(GTK_GRID_VIEW(grid), 12);
  GtkWidget *grid_scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(grid_scroller), grid);

  char *desc = g_strdup_printf(a->lib ? "Use Import to add music.\n\nmusiccore %s"
                                      : "Could not open the library.\n\nmusiccore %s",
                               mc_version());
  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "folder-music-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Albums");
  adw_status_page_set_description(ADW_STATUS_PAGE(empty), desc);
  g_free(desc);

  GtkWidget *stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(stack), empty, "empty");
  gtk_stack_add_named(GTK_STACK(stack), grid_scroller, "grid");
  gtk_stack_set_visible_child_name(GTK_STACK(stack), albums > 0 ? "grid" : "empty");
  return stack;
}

static GtkWidget *icon_button(const char *icon, const char *tooltip) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(b, tooltip);
  return b;
}

static void on_activate(GtkApplication *gapp, gpointer user) {
  app *a = user;

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            icon_button("media-skip-backward-symbolic", "Previous"));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            icon_button("media-playback-start-symbolic", "Play"));
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            icon_button("media-skip-forward-symbolic", "Next"));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), icon_button("emblem-system-symbolic", "Settings"));
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), icon_button("list-add-symbolic", "Import"));

  /* Right-hand track pane: hidden until an album is selected. */
  GtkWidget *tracks = adw_status_page_new();
  adw_status_page_set_title(ADW_STATUS_PAGE(tracks), "Tracks");
  GtkWidget *album_split = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar_position(ADW_OVERLAY_SPLIT_VIEW(album_split), GTK_PACK_END);
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(album_split), tracks);
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(album_split), build_albums(a));
  adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(album_split), FALSE);

  GtkWidget *nav_split = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(nav_split), build_sidebar());
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(nav_split), album_split);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), nav_split);

  GtkWidget *win = adw_application_window_new(gapp);
  gtk_window_set_title(GTK_WINDOW(win), "musicapp");
  gtk_window_set_default_size(GTK_WINDOW(win), 1200, 800);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);
  gtk_window_present(GTK_WINDOW(win));
}

int main(int argc, char **argv) {
  app a = {0};
  mc_init();

  char *dir = g_build_filename(g_get_user_data_dir(), "musicapp", NULL);
  g_mkdir_with_parents(dir, 0700);
  char *path = g_build_filename(dir, "library.db", NULL);
  if (mc_library_open(path, &a.lib) != MC_OK) a.lib = NULL;
  g_free(path);
  g_free(dir);

  AdwApplication *gapp = adw_application_new("app.musicapp.Musicapp", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
  int status = g_application_run(G_APPLICATION(gapp), argc, argv);
  g_object_unref(gapp);

  mc_library_close(a.lib);
  mc_shutdown();
  return status;
}
