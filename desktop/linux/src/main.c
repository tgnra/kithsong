/* GTK4 + libadwaita frontend. Layout:
 *
 *   AdwToolbarView
 *   +- AdwHeaderBar: [prev][play][next]  [art] now playing + seekbar  [import][settings]
 *   +- AdwOverlaySplitView (sidebar: Albums / Artists / Playlists / Devices)
 *      +- GtkStack of pages: "library" | "import" (import_view.c)
 *         +- AdwOverlaySplitView, sidebar at the end (track list, shown while an album is picked)
 *            +- GtkStack: empty-state page | GtkGridView of albums (virtualised)
 *
 * The album grid is album_grid.c, the track list track_list.c, the now-playing strip
 * now_playing.c, the player playback.c, the desktop's media controls mpris.c, the import
 * page import_view.c and the Settings dialog settings_view.c and the Add Device dialog add_device.c. The playback buttons are the app.previous, app.play-pause and app.next actions, so their
 * sensitivity follows the actions'.
 */
#include <adwaita.h>

#include "add_device.h"
#include "album_grid.h"
#include "app.h"
#include "cover.h"
#include "import_view.h"
#include "mpris.h"
#include "musiccore/musiccore.h"
#include "now_playing.h"
#include "playback.h"
#include "settings_view.h"
#include "track_list.h"

static const char css[] =
  ".album-cover { border-radius: 8px; background-color: alpha(currentColor, 0.08); }"
  ".now-playing .album-cover { border-radius: 4px; }"
  ".album-grid { padding: 12px; }"
  ".album-grid > child { padding: 8px; border-radius: 12px; }"
  ".now-playing scale { padding-top: 2px; padding-bottom: 2px; }";

static GtkWidget *play_button;

/* --- Import and Settings --- */

static void show_page(app *a, const char *name) {
  gtk_stack_set_visible_child_name(a->pages, name);
}

static void on_import(GtkButton *button, gpointer user) {
  (void)button;
  show_page(user, "import");
}

static void on_import_present(void *user) {
  show_page(user, "import");
}

static void on_import_library_changed(void *user) {
  app_reload_library(user);
}

/* While an import session is open the library mustn't change under it, so the sidebar and
 * Settings are locked, as on Windows. */
static void on_import_session(void *user, gboolean active) {
  app *a = user;
  gtk_widget_set_sensitive(a->sidebar, !active);
  gtk_widget_set_sensitive(a->settings, !active);
}

static void on_import_done(void *user) {
  show_page(user, "library");
}

static void set_empty_description(app *a) {
  char *desc = g_strdup_printf(a->lib ? "Use Import to add music.\n\nmusiccore %s"
                                      : "Could not open the library.\n\nmusiccore %s",
                               mc_version());
  adw_status_page_set_description(a->empty, desc);
  g_free(desc);
}

/* Settings' Open Another Library. The import view lets go of the old library first. */
static gboolean open_library(void *user, const char *dir) {
  app *a = user;
  mc_library *lib = NULL;
  if (mc_library_open(dir, &lib) != MC_OK) {
    AdwDialog *alert = adw_alert_dialog_new("Couldn't Open the Library", dir);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", "Close");
    adw_dialog_present(alert, GTK_WIDGET(a->win));
    return FALSE;
  }
  import_view_set_library(a->import, lib);
  mc_library_close(a->lib);
  a->lib = lib;
  set_empty_description(a);
  app_reload_library(a);
  return TRUE;
}

static void on_settings(GtkButton *button, gpointer user) {
  (void)button;
  app *a = user;
  settings_view_present(GTK_WIDGET(a->win), a->settings_dir, open_library, a);
}

static void close_confirmed(void *user, gboolean closed) {
  if (closed) gtk_window_destroy(user);
}

/* Closing the window while an import runs asks first. */
static gboolean on_close_request(GtkWindow *win, gpointer user) {
  app *a = user;
  if (!import_view_is_running(a->import)) return FALSE;
  import_view_confirm_close(a->import, close_confirmed, win);
  return TRUE;
}

/* --- Sidebar --- */

/* Albums, Artists and Playlists all lead back to the library page, e.g. from Import. */
static void on_nav_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  (void)row;
  show_page(user, "library");
}

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

/* TODO: start pairing with mc_sync_pair_request_address() once the sync API lands. */
static void on_add_device_connect(void *user, const char *host, guint16 port) {
  (void)user;
  g_message("Add device: %s port %u", host, (unsigned)port);
}

static void on_add_device(GtkButton *button, gpointer user) {
  add_device_present(GTK_WIDGET(button), on_add_device_connect, user);
}

static GtkWidget *build_sidebar(app *a) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* Albums, Artists and Playlists all show the album grid for now, as on Windows. */
  GtkWidget *nav = gtk_list_box_new();
  gtk_widget_add_css_class(nav, "navigation-sidebar");
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("media-optical-symbolic", "Albums"));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("avatar-default-symbolic", "Artists"));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("view-list-symbolic", "Playlists"));
  gtk_list_box_select_row(GTK_LIST_BOX(nav), gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), 0));
  g_signal_connect(nav, "row-activated", G_CALLBACK(on_nav_activated), a);
  gtk_box_append(GTK_BOX(box), nav);

  /* The Devices heading, with + at the end to add a device by address. */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 12);
  GtkWidget *heading = gtk_label_new("Devices");
  gtk_widget_add_css_class(heading, "heading");
  gtk_widget_add_css_class(heading, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_set_hexpand(heading, TRUE);
  gtk_box_append(GTK_BOX(header), heading);
  GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_set_tooltip_text(add, "Add Device by Address");
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_device), a);
  gtk_box_append(GTK_BOX(header), add);
  gtk_box_append(GTK_BOX(box), header);

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

/* --- Playback buttons --- */

static void set_action_enabled(app *a, const char *name, gboolean enabled) {
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(a->gapp), name);
  if (action) g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

/* Whether the shown album has anything to play. */
static gboolean album_playable(void) {
  for (int i = 0, n = track_list_count(); i < n; i++)
    if (track_list_item_at(i).path) return TRUE;
  return FALSE;
}

/* Play turns into Pause while playing. With nothing queued, Play plays the album shown. */
static void update_buttons(app *a) {
  gboolean playing = playback_is_playing();
  gtk_button_set_icon_name(GTK_BUTTON(play_button),
                           playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text(play_button, playing ? "Pause" : "Play");
  set_action_enabled(a, "previous", playback_has_current());
  set_action_enabled(a, "play-pause", playback_has_current() || album_playable());
  set_action_enabled(a, "next", playback_has_next());
}

static void playback_changed(void *user) {
  update_buttons(user);
}

static void on_previous(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  playback_previous();
}

static void on_play_pause(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  if (!playback_toggle()) app_play_album(user, 0); /* nothing queued yet: the album that's shown */
}

static void on_next(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  playback_next();
}

static void on_seek(void *user, int ms) {
  (void)user;
  playback_seek(ms);
}

/* Plays the shown album, from track start on. Tracks with no file on this device are left
 * out of the queue. */
void app_play_album(app *a, int start) {
  (void)a;
  AlbumItem *album = track_list_album();
  int n = track_list_count(), count = 0, first = 0;
  if (!album || !n) return;
  playback_track *queue = g_new0(playback_track, (gsize)n);
  for (int i = 0; i < n; i++) {
    track_list_item t = track_list_item_at(i);
    if (!t.path) continue;
    if (i <= start) first = count;
    queue[count++] = (playback_track){t.uid, t.path, t.title, album->artist, album->title, album->cover_dir,
                                      t.duration_ms};
  }
  if (count) playback_play(queue, count, first);
  g_free(queue);
}

/* --- Albums and tracks --- */

/* Shows the selected album's tracks, or hides the track list when none is selected. */
static void show_album(app *a) {
  AlbumItem *album = album_grid_selection();
  track_list_show(a->lib, album);
  adw_overlay_split_view_set_show_sidebar(a->album_split, album != NULL);
  update_buttons(a);
}

static void on_album_changed(void *user) {
  show_album(user);
}

static void on_track_play(void *user, int index) {
  app_play_album(user, index);
}

static void on_tracks_close(void *user) {
  album_grid_unselect();
  show_album(user);
}

void app_reload_library(app *a) {
  int albums = album_grid_reload(a->lib);
  gtk_stack_set_visible_child_name(a->albums, albums > 0 ? "grid" : "empty");
  show_album(a); /* if the album went, the selection did too */
}

static GtkWidget *build_albums(app *a) {
  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "folder-music-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), "No Albums");
  a->empty = ADW_STATUS_PAGE(empty);
  set_empty_description(a);

  GtkWidget *stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(stack), empty, "empty");
  gtk_stack_add_named(GTK_STACK(stack), album_grid_new(on_album_changed, a), "grid");
  a->albums = GTK_STACK(stack);
  return stack;
}

/* --- Window --- */

static GtkWidget *icon_button(const char *icon, const char *tooltip) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(b, tooltip);
  return b;
}

static GtkWidget *action_button(const char *icon, const char *tooltip, const char *action) {
  GtkWidget *b = icon_button(icon, tooltip);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
  return b;
}

static void on_shutdown(GApplication *gapp, gpointer user) {
  (void)gapp;
  (void)user;
  playback_free();
  mpris_free();
  cover_cache_free();
}

static void on_activate(GtkApplication *gapp, gpointer user) {
  app *a = user;
  if (a->win) { /* launched again: the app was already running */
    gtk_window_present(a->win);
    return;
  }
  a->gapp = gapp;

  GtkCssProvider *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, css);
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(provider);

  const GActionEntry actions[] = {
    {"previous", on_previous, NULL, NULL, NULL, {0}},
    {"play-pause", on_play_pause, NULL, NULL, NULL, {0}},
    {"next", on_next, NULL, NULL, NULL, {0}},
  };
  g_action_map_add_action_entries(G_ACTION_MAP(gapp), actions, G_N_ELEMENTS(actions), a);

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            action_button("media-skip-backward-symbolic", "Previous", "app.previous"));
  play_button = action_button("media-playback-start-symbolic", "Play", "app.play-pause");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), play_button);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            action_button("media-skip-forward-symbolic", "Next", "app.next"));
  a->settings = icon_button("emblem-system-symbolic", "Settings");
  g_signal_connect(a->settings, "clicked", G_CALLBACK(on_settings), a);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), a->settings);
  GtkWidget *import = icon_button("list-add-symbolic", "Import");
  g_signal_connect(import, "clicked", G_CALLBACK(on_import), a);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), import);

  /* The now-playing strip takes the title's place, up to a point. */
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 640);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 640);
  gtk_widget_set_hexpand(clamp, TRUE);
  gtk_widget_set_margin_start(clamp, 12);
  gtk_widget_set_margin_end(clamp, 12);
  adw_clamp_set_child(ADW_CLAMP(clamp), now_playing_new(on_seek, a));
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), clamp);

  /* Right-hand track pane: hidden until an album is selected. */
  GtkWidget *album_split = adw_overlay_split_view_new();
  a->album_split = ADW_OVERLAY_SPLIT_VIEW(album_split);
  adw_overlay_split_view_set_sidebar_position(a->album_split, GTK_PACK_END);
  adw_overlay_split_view_set_min_sidebar_width(a->album_split, 320);
  adw_overlay_split_view_set_max_sidebar_width(a->album_split, 420);
  adw_overlay_split_view_set_sidebar(a->album_split, track_list_new(on_track_play, on_tracks_close, a));
  adw_overlay_split_view_set_content(a->album_split, build_albums(a));
  adw_overlay_split_view_set_show_sidebar(a->album_split, FALSE);

  GtkWidget *pages = gtk_stack_new();
  a->pages = GTK_STACK(pages);
  gtk_stack_add_named(a->pages, album_split, "library");
  const import_view_callbacks import_cb = {on_import_library_changed, on_import_session,
                                           on_import_done, on_import_present, a};
  a->import = import_view_new(a->lib, &import_cb);
  gtk_stack_add_named(a->pages, a->import, "import");

  a->sidebar = build_sidebar(a);
  GtkWidget *nav_split = adw_overlay_split_view_new();
  adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(nav_split), a->sidebar);
  adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(nav_split), pages);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_top_bar_style(ADW_TOOLBAR_VIEW(toolbar), ADW_TOOLBAR_RAISED_BORDER);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), nav_split);
  import_view_add_drop_target(a->import, toolbar); /* folders dropped anywhere are imported */

  GtkWidget *win = adw_application_window_new(gapp);
  a->win = GTK_WINDOW(win);
  gtk_window_set_title(a->win, "musicapp");
  gtk_window_set_default_size(a->win, 1200, 800);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);
  g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), a);

  playback_init(a->win, playback_changed, a);
  mpris_init(gapp, a->win);
  app_reload_library(a);
  gtk_window_present(a->win);
}

int main(int argc, char **argv) {
  app a = {0};
  mc_init();

  /* Settings in ~/.config/musicapp say where the library folder is. Until there's a
   * first-run dialog here, the Music folder is used and remembered. */
  char *settings_dir = g_build_filename(g_get_user_config_dir(), "musicapp", NULL);
  mc_settings *settings = NULL;
  if (mc_settings_load(settings_dir, &settings) == MC_OK && !mc_settings_library_dir(settings)) {
    const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    char *fallback = music ? NULL : g_build_filename(g_get_home_dir(), "Music", NULL);
    mc_settings_set_library_dir(settings, music ? music : fallback);
    mc_settings_save(settings);
    g_free(fallback);
  }
  a.settings_dir = settings_dir;
  const char *library_dir = mc_settings_library_dir(settings);
  if (!library_dir || mc_library_open(library_dir, &a.lib) != MC_OK) a.lib = NULL;
  mc_settings_free(settings);

  AdwApplication *gapp = adw_application_new("app.musicapp.Musicapp", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
  g_signal_connect(gapp, "shutdown", G_CALLBACK(on_shutdown), &a);
  int status = g_application_run(G_APPLICATION(gapp), argc, argv);
  g_object_unref(gapp);

  /* The window, and the import view with it, is gone by now, so the library can close. */
  mc_library_close(a.lib);
  g_free(a.settings_dir);
  mc_shutdown();
  return status;
}
