/* GTK4 + libadwaita frontend. Layout:
 *
 *   AdwToolbarView
 *   +- AdwHeaderBar: [prev][play][next]  [art] now playing + seekbar [shuffle]  [import][settings]
 *   +- AdwOverlaySplitView (sidebar: Home / Search, Albums / Artists / Playlists / Album groups
 *      |                    / Recents / Devices)
 *      +- GtkStack of pages: "library" | "artists" (artist_view.c) | "search" (search_view.c)
 *         | "import" (import_view.c) | "device" (device_view.c)
 *         +- AdwOverlaySplitView, sidebar at the end (track list, shown while an album is picked)
 *            +- GtkStack: empty-state page | GtkGridView of albums (virtualised)
 *                         | empty-state page | GtkGridView of playlists
 *                         | album groups and a group's albums (album_groups.c)
 *
 * The album and playlist grids are album_grid.c, the track list track_list.c, the
 * now-playing strip now_playing.c, the player playback.c, the desktop's media controls
 * mpris.c, the import page import_view.c, the Settings dialog settings_view.c, Settings' Move
 * Library move_library.c, the Add to Playlist dialog add_to_playlist.c and the Rename and
 * Delete Playlist dialogs playlist_dialogs.c, the Album groups page album_groups.c (its
 * dialogs album_group_dialogs.c), the Artists page artist_view.c and the Search page
 * search_view.c. The sidebar's
 * Recents section is recents.c, and
 * its Devices section and sync are devices.c, which adds a device's page (device_view.c) to
 * the stack of pages as "device". The playback buttons
 * are the app.previous, app.play-pause and app.next actions, so their sensitivity follows
 * the actions'. Shuffle, which the strip's toggle and the track list's Play and Shuffle set,
 * is kept in settings.json, as is the volume, which only the keyboard shortcuts (on_shortcut(),
 * listed in README.md) change for now.
 */
#include <adwaita.h>
#include <math.h>
#include <string.h>

#include "add_to_playlist.h"
#include "album_grid.h"
#include "album_groups.h"
#include "app.h"
#include "artist_view.h"
#include "cover.h"
#include "delete_dialog.h"
#include "devices.h"
#include "import_view.h"
#include "move_library.h"
#include "mpris.h"
#include "musiccore/musiccore.h"
#include "now_playing.h"
#include "playback.h"
#include "playlist_dialogs.h"
#include "recents.h"
#include "search_view.h"
#include "settings_view.h"
#include "track_list.h"

static const char css[] =
  ".album-cover { border-radius: 8px; background-color: alpha(currentColor, 0.08); }"
  ".now-playing .album-cover { border-radius: 4px; }"
  ".album-grid { padding: 12px; }"
  ".album-grid > child { padding: 8px; border-radius: 12px; }"
  ".track-list > row.drop-above { box-shadow: inset 0 2px @accent_bg_color; }"
  ".track-list > row.drop-below { box-shadow: inset 0 -2px @accent_bg_color; }"
  ".now-playing scale { padding-top: 2px; padding-bottom: 2px; }"
  ".now-playing .shuffle:checked { color: @accent_color; }"
  ".pair-code { font-size: 20pt; font-weight: 600; letter-spacing: 4px; font-feature-settings: 'tnum'; }"
  ".pair-code-shown { font-size: 32pt; font-weight: 600; letter-spacing: 4px; font-feature-settings: 'tnum'; }"
  ".sync-picker { background: none; }"
  ".search-large { font-size: 14pt; min-height: 44px; }"
  ".search-albums > flowboxchild { padding: 8px; border-radius: 12px; }";

static GtkWidget *play_button;
static guint save_volume_source; /* a timeout, so a held key saves the volume once */

enum {
  VOLUME_STEP = 5, /* per press of a volume shortcut */
  SEEK_STEP_MS = 5000,
  SAVE_VOLUME_MS = 1000,
};

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

/* The empty Albums page suggests pairing while this computer isn't in a group, with the Pair
 * with a device… button. */
static void set_empty_description(app *a) {
  gboolean alone = a->lib && devices_sync_state() == DEVICES_ALONE;
  enum mc_str_id text = !a->lib ? MC_STR_LIBRARY_OPEN_FAILED : alone ? MC_STR_LINUX_COMPUTER_EMPTY_TEXT_PAIR
                                                                     : MC_STR_EMPTY_TEXT;
  char *desc = mc_format_alloc(mc_str(MC_STR_LINUX_EMPTY_WITH_VERSION), MC_S(mc_str(text)), MC_S(mc_version()));
  adw_status_page_set_description(a->empty, desc);
  mc_free(desc);
  if (a->pair) gtk_widget_set_visible(a->pair, alone);
}

/* Choose what to sync shows under "none of them have tracks on this device" once this
 * computer is in a group. */
static void set_no_playlists(app *a) {
  gboolean hidden = a->hidden_playlists > 0;
  adw_status_page_set_title(a->no_playlists, mc_str(hidden ? MC_STR_NO_PLAYABLE_PLAYLISTS_TITLE
                                                           : MC_STR_NO_PLAYLISTS_TITLE));
  adw_status_page_set_description(a->no_playlists, mc_str(hidden ? MC_STR_NO_PLAYABLE_PLAYLISTS_TEXT
                                                                 : MC_STR_NO_PLAYLISTS_TEXT));
  gtk_widget_set_visible(a->choose_sync, hidden && devices_sync_state() == DEVICES_GROUPED);
}

void app_update_empty_pages(app *a) {
  if (!a->empty || !a->no_playlists) return; /* not built yet */
  set_empty_description(a);
  set_no_playlists(a);
}

GtkWidget *app_list_header(const char *title, const char *tooltip, GCallback clicked, gpointer user) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 18);
  gtk_widget_set_margin_top(header, 12);
  GtkWidget *label = gtk_label_new(title);
  gtk_widget_add_css_class(label, "title-2");
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(header), label);
  GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_set_tooltip_text(add, tooltip);
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_add_css_class(add, "circular");
  gtk_widget_set_valign(add, GTK_ALIGN_CENTER);
  g_signal_connect(add, "clicked", clicked, user);
  gtk_box_append(GTK_BOX(header), add);
  return header;
}

/* Deleted tracks whose files are back from the Trash come back into the library
 * (musiccore/delete.h). Cheap: run when a library opens and whenever the window becomes
 * active. TRUE if any came back. */
static gboolean restore_deleted(app *a) {
  int restored = 0;
  return a->lib && mc_delete_restore(a->lib, a->settings_dir, &restored) == MC_OK && restored;
}

/* Settings' Open Another Library. The import view lets go of the old library first. */
static gboolean open_library(void *user, const char *dir) {
  app *a = user;
  mc_library *lib = NULL;
  if (mc_library_open(dir, &lib) != MC_OK) {
    AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_LIBRARY_OPEN_FAILED), dir);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
    adw_dialog_present(alert, GTK_WIDGET(a->win));
    return FALSE;
  }
  devices_stop(a); /* sync reads the old library */
  import_view_set_library(a->import, lib);
  playback_set_library(lib, a->settings_dir);
  mc_library_close(a->lib);
  a->lib = lib;
  a->keep_playlist[0] = '\0';
  set_empty_description(a);
  restore_deleted(a);
  app_reload_library(a);
  devices_start(a);
  return TRUE;
}

gboolean app_open_library(app *a, const char *dir) {
  return open_library(a, dir);
}

static void on_settings_renamed(void *user, const char *name) {
  devices_renamed(user, name);
}

static void on_settings_group_changed(void *user) {
  devices_refresh(user);
}

/* Before Move Library moves the files: everything that reads the library or plays its files
 * lets go, and it closes. */
static void move_close_library(void *user) {
  app *a = user;
  devices_stop(a); /* sync reads the library */
  playback_clear();
  import_view_set_library(a->import, NULL);
  playback_set_library(NULL, NULL);
  album_grid_unselect(a->album_grid);
  album_grid_unselect(a->playlist_grid);
  mc_library_close(a->lib);
  a->lib = NULL;
  app_reload_library(a);
}

/* After the move: the library opens where it is now (settings.json already says so). */
static void move_reopen_library(void *user, const char *dir) {
  app *a = user;
  if (mc_library_open(dir, &a->lib) != MC_OK) a->lib = NULL; /* the empty page says so */
  import_view_set_library(a->import, a->lib);
  playback_set_library(a->lib, a->settings_dir);
  set_empty_description(a);
  restore_deleted(a);
  app_reload_library(a);
  devices_start(a);
  gtk_window_present(a->win);
}

static void move_library(void *user) {
  app *a = user;
  static move_library_host host = {move_close_library, move_reopen_library, NULL};
  host.user = a;
  move_library_start(a->win, a->settings_dir, &host);
}

static void on_settings(GtkButton *button, gpointer user) {
  (void)button;
  app *a = user;
  static const settings_view_sync sync = {devices_sync, devices_sync_off_text, on_settings_renamed,
                                          on_settings_group_changed};
  settings_view_present(GTK_WIDGET(a->win), a->settings_dir, open_library, move_library, &sync, a);
}

static void close_confirmed(void *user, gboolean closed) {
  app *a = user;
  if (!closed) return;
  devices_stop(a); /* while its dialogs and the device page are still there */
  gtk_window_destroy(a->win);
}

/* Closing the window while an import runs asks first. */
static gboolean on_close_request(GtkWindow *win, gpointer user) {
  (void)win;
  app *a = user;
  if (import_view_is_running(a->import)) {
    import_view_confirm_close(a->import, close_confirmed, a);
    return TRUE;
  }
  devices_stop(a);
  return FALSE;
}

/* --- Sidebar --- */

static void show_album(app *a);
static GtkWidget *shown_grid(app *a);
static void update_buttons(app *a);

/* Albums, Playlists and Album groups lead back to the library page, e.g. from Import, each
 * with its grid; Artists to the Artists page. The grid left loses its selection, so the
 * track list closes. */
static void on_nav_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  app *a = user;
  devices_unselect();
  gtk_list_box_unselect_all(a->top_nav);
  app_view view = (app_view)gtk_list_box_row_get_index(row);
  if (view != a->view) {
    GtkWidget *grid = shown_grid(a);
    if (grid) album_grid_unselect(grid);
    a->view = view;
    a->keep_playlist[0] = '\0'; /* a new playlist with nothing to play here is hidden again */
    app_reload_library(a);
  }
  show_page(a, view == APP_VIEW_ARTISTS ? "artists" : "library");
  update_buttons(a);
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

/* Search is the only row of top_nav that can be picked; Home isn't there yet. */
static void on_top_nav_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  (void)row;
  app *a = user;
  devices_unselect();
  gtk_list_box_unselect_all(a->nav);
  show_page(a, "search");
  search_view_focus();
}

static GtkWidget *build_top_nav(app *a) {
  GtkWidget *list = gtk_list_box_new();
  gtk_widget_add_css_class(list, "navigation-sidebar");
  GtkWidget *home = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(home), nav_row("go-home-symbolic", mc_str(MC_STR_SECTION_HOME)));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(home), FALSE);
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(home), FALSE);
  gtk_widget_set_sensitive(home, FALSE);
  gtk_list_box_append(GTK_LIST_BOX(list), home);
  gtk_list_box_append(GTK_LIST_BOX(list), nav_row("system-search-symbolic", mc_str(MC_STR_SEARCH)));
  g_signal_connect(list, "row-activated", G_CALLBACK(on_top_nav_activated), a);
  a->top_nav = GTK_LIST_BOX(list);
  return list;
}

/* The arrow keys stop at the ends of a list; past them, they go on to the next or previous
 * one (Recents, Devices), as Tab does. */
static gboolean on_sidebar_keynav_failed(GtkWidget *list, GtkDirectionType dir, gpointer user) {
  (void)user;
  if (dir != GTK_DIR_UP && dir != GTK_DIR_DOWN) return FALSE;
  gtk_widget_child_focus(GTK_WIDGET(gtk_widget_get_root(list)),
                         dir == GTK_DIR_DOWN ? GTK_DIR_TAB_FORWARD : GTK_DIR_TAB_BACKWARD);
  return TRUE;
}

static void connect_keynav(GtkWidget *widget) {
  if (GTK_IS_LIST_BOX(widget)) g_signal_connect(widget, "keynav-failed", G_CALLBACK(on_sidebar_keynav_failed), NULL);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) connect_keynav(c);
}

static GtkWidget *build_sidebar(app *a) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), build_top_nav(a));
  GtkWidget *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_start(separator, 12);
  gtk_widget_set_margin_end(separator, 12);
  gtk_widget_set_margin_top(separator, 3);
  gtk_widget_set_margin_bottom(separator, 3);
  gtk_box_append(GTK_BOX(box), separator);

  /* In app_view's order. */
  GtkWidget *nav = gtk_list_box_new();
  gtk_widget_add_css_class(nav, "navigation-sidebar");
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("media-optical-symbolic", mc_str(MC_STR_SECTION_ALBUMS)));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("avatar-default-symbolic", mc_str(MC_STR_SECTION_ARTISTS)));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("view-list-symbolic", mc_str(MC_STR_SECTION_PLAYLISTS)));
  gtk_list_box_append(GTK_LIST_BOX(nav), nav_row("folder-symbolic", mc_str(MC_STR_SECTION_ALBUM_GROUPS)));
  gtk_list_box_select_row(GTK_LIST_BOX(nav), gtk_list_box_get_row_at_index(GTK_LIST_BOX(nav), 0));
  g_signal_connect(nav, "row-activated", G_CALLBACK(on_nav_activated), a);
  gtk_box_append(GTK_BOX(box), nav);
  a->nav = GTK_LIST_BOX(nav);

  gtk_box_append(GTK_BOX(box), recents_new(a));
  gtk_box_append(GTK_BOX(box), devices_new(a));
  connect_keynav(box);

  /* It all scrolls when it's taller than the window, with a scrollbar only then; moving the
   * focus with the keyboard scrolls the focused row into view. Import and Settings are in
   * the header bar, so nothing is pinned below. */
  GtkWidget *viewport = gtk_viewport_new(NULL, NULL);
  gtk_viewport_set_scroll_to_focus(GTK_VIEWPORT(viewport), TRUE);
  gtk_viewport_set_child(GTK_VIEWPORT(viewport), box);
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                 GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), viewport);
  return scroller;
}

/* --- Playback buttons --- */

static void set_action_enabled(app *a, const char *name, gboolean enabled) {
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(a->gapp), name);
  if (action) g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

/* An artist's page is what's shown: Play plays the artist rather than the album. */
static gboolean artist_shown(app *a) {
  return !g_strcmp0(gtk_stack_get_visible_child_name(a->pages), "artists") && artist_view_artist();
}

/* Whether the shown album (or artist) has anything to play. */
static gboolean album_playable(app *a) {
  if (artist_shown(a)) return artist_view_playable();
  for (int i = 0, n = track_list_count(); i < n; i++)
    if (track_list_item_at(i).path) return TRUE;
  return FALSE;
}

/* Play turns into Pause while playing. With nothing queued, Play plays the album shown. */
static void update_buttons(app *a) {
  gboolean playing = playback_is_playing();
  gtk_button_set_icon_name(GTK_BUTTON(play_button),
                           playing ? "media-playback-pause-symbolic" : "media-playback-start-symbolic");
  gtk_widget_set_tooltip_text(play_button, mc_str(playing ? MC_STR_PAUSE : MC_STR_PLAY));
  set_action_enabled(a, "previous", playback_has_current());
  set_action_enabled(a, "play-pause", playback_has_current() || album_playable(a));
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
  if (playback_toggle()) return;
  if (artist_shown(user)) artist_view_play(-1); /* nothing queued yet: the artist or album that's shown */
  else app_play_album(user, -1);
}

static void on_next(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  (void)user;
  playback_next();
}

/* Space in an album grid or the track list. Those would take it to select the item with the
 * focus, but clicking or the arrow keys has selected it already, so Space plays there. */
static gboolean on_list_space(GtkWidget *win, GVariant *args, gpointer user) {
  (void)args;
  (void)user;
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(win));
  gboolean in_list = focus && (gtk_widget_get_ancestor(focus, GTK_TYPE_GRID_VIEW) ||
                                gtk_widget_get_ancestor(focus, GTK_TYPE_LIST_VIEW));
  if (!in_list) return FALSE;
  gtk_widget_activate_action(win, "app.play-pause", NULL);
  return TRUE;
}

static void on_seek(void *user, int ms) {
  (void)user;
  playback_seek(ms);
}

/* Shuffle on or off, kept in settings.json for next time. */
static void set_shuffle(app *a, gboolean on) {
  if (!on == !playback_shuffle()) return;
  playback_set_shuffle(on);
  mc_settings *settings = NULL;
  if (mc_settings_load(a->settings_dir, &settings) != MC_OK) return;
  if (mc_settings_set_shuffle(settings, on) == MC_OK) mc_settings_save(settings);
  mc_settings_free(settings);
}

static void on_shuffle(void *user, gboolean on) {
  set_shuffle(user, on);
}

/* --- Keyboard shortcuts --- */

static gboolean save_volume(gpointer user) {
  app *a = user;
  save_volume_source = 0;
  mc_settings *settings = NULL;
  if (mc_settings_load(a->settings_dir, &settings) != MC_OK) return G_SOURCE_REMOVE;
  if (mc_settings_set_volume(settings, playback_volume()) == MC_OK) mc_settings_save(settings);
  mc_settings_free(settings);
  return G_SOURCE_REMOVE;
}

/* Shows the volume (or Muted) for a moment in the now-playing strip, as there's no volume
 * control to show it. */
static void flash_volume(void) {
  if (playback_muted()) {
    now_playing_flash(mc_str(MC_STR_VOLUME_MUTED));
    return;
  }
  char *text = mc_format_alloc(mc_str(MC_STR_VOLUME_LEVEL), MC_D(playback_volume()));
  now_playing_flash(text);
  mc_free(text);
}

/* Volume 0 to 100, kept in settings.json for next time. Changing it unmutes. */
static void set_volume(app *a, int volume) {
  playback_set_muted(FALSE);
  playback_set_volume(volume);
  flash_volume();
  if (save_volume_source) g_source_remove(save_volume_source);
  save_volume_source = g_timeout_add(SAVE_VOLUME_MS, save_volume, a);
}

/* The sidebar's row n of Albums, Artists, Playlists..., as clicking it does. */
static void activate_nav_row(app *a, int n) {
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(a->nav, n);
  if (!row) return;
  gtk_list_box_select_row(a->nav, row);
  g_signal_emit_by_name(a->nav, "row-activated", row);
}

typedef enum shortcut {
  SC_NEXT,
  SC_PREVIOUS,
  SC_SEEK_FORWARD,
  SC_SEEK_BACK,
  SC_VOLUME_UP,
  SC_VOLUME_DOWN,
  SC_MUTE,
  SC_SHUFFLE,
  SC_SEARCH,
  SC_PAGE_1, /* ...to SC_PAGE_1 + 3 */
  SC_SETTINGS = SC_PAGE_1 + 4,
} shortcut;

/* Whether the focus is in a text box, whose own keys (arrows, + and -) it keeps. */
static gboolean text_focused(GtkWidget *win) {
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(win));
  return focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus));
}

/* The window's keyboard shortcuts, besides Space (see README.md). Captured before the focused
 * widget sees them, so the lists' own arrow keys don't hide them; text boxes keep the keys
 * they use (the arrows, + and -), and a dialog over the window keeps them all. The pages and
 * Settings aren't reached while an import has the sidebar locked. */
static gboolean on_shortcut(GtkWidget *win, GVariant *args, gpointer user) {
  app *a = user;
  shortcut sc = (shortcut)g_variant_get_int32(args);
  GtkWidget *focus = gtk_root_get_focus(GTK_ROOT(win));
  if (focus && gtk_widget_get_ancestor(focus, ADW_TYPE_DIALOG)) return FALSE;
  gboolean text = text_focused(win);
  switch (sc) {
    case SC_NEXT:
    case SC_PREVIOUS:
      if (text) return FALSE; /* a word at a time */
      if (sc == SC_NEXT) playback_next();
      else playback_previous();
      return TRUE;
    case SC_SEEK_FORWARD:
    case SC_SEEK_BACK:
      if (text) return FALSE;
      playback_seek_by(sc == SC_SEEK_FORWARD ? SEEK_STEP_MS : -SEEK_STEP_MS);
      return TRUE;
    case SC_VOLUME_UP:
    case SC_VOLUME_DOWN:
      if (text) return FALSE;
      set_volume(a, playback_volume() + (sc == SC_VOLUME_UP ? VOLUME_STEP : -VOLUME_STEP));
      return TRUE;
    case SC_MUTE:
      playback_set_muted(!playback_muted());
      flash_volume();
      return TRUE;
    case SC_SHUFFLE:
      set_shuffle(a, !playback_shuffle());
      now_playing_flash(mc_str(playback_shuffle() ? MC_STR_SHUFFLE_ON : MC_STR_SHUFFLE_OFF));
      return TRUE;
    case SC_SEARCH:
      if (!gtk_widget_is_sensitive(a->sidebar)) return TRUE;
      gtk_list_box_select_row(a->top_nav, gtk_list_box_get_row_at_index(a->top_nav, 1));
      on_top_nav_activated(a->top_nav, NULL, a);
      return TRUE;
    case SC_SETTINGS:
      if (gtk_widget_is_sensitive(a->settings)) on_settings(NULL, a);
      return TRUE;
    default:
      if (gtk_widget_is_sensitive(a->sidebar)) activate_nav_row(a, sc - SC_PAGE_1);
      return TRUE;
  }
}

/* Volume and seeking also come from the plain keys, so held down they repeat. */
static void add_shortcuts(app *a, GtkWidget *win) {
  static const struct {
    guint key;
    GdkModifierType mods;
    shortcut sc;
  } keys[] = {
    {GDK_KEY_Right, GDK_CONTROL_MASK, SC_NEXT},
    {GDK_KEY_Left, GDK_CONTROL_MASK, SC_PREVIOUS},
    {GDK_KEY_Right, GDK_SHIFT_MASK, SC_SEEK_FORWARD},
    {GDK_KEY_Left, GDK_SHIFT_MASK, SC_SEEK_BACK},
    {GDK_KEY_Up, GDK_CONTROL_MASK, SC_VOLUME_UP},
    {GDK_KEY_Down, GDK_CONTROL_MASK, SC_VOLUME_DOWN},
    {GDK_KEY_plus, 0, SC_VOLUME_UP},
    {GDK_KEY_equal, 0, SC_VOLUME_UP},
    {GDK_KEY_KP_Add, 0, SC_VOLUME_UP},
    {GDK_KEY_minus, 0, SC_VOLUME_DOWN},
    {GDK_KEY_KP_Subtract, 0, SC_VOLUME_DOWN},
    {GDK_KEY_m, GDK_CONTROL_MASK, SC_MUTE},
    {GDK_KEY_s, GDK_CONTROL_MASK, SC_SHUFFLE},
    {GDK_KEY_f, GDK_CONTROL_MASK, SC_SEARCH},
    {GDK_KEY_1, GDK_CONTROL_MASK, SC_PAGE_1},
    {GDK_KEY_2, GDK_CONTROL_MASK, SC_PAGE_1 + 1},
    {GDK_KEY_3, GDK_CONTROL_MASK, SC_PAGE_1 + 2},
    {GDK_KEY_4, GDK_CONTROL_MASK, SC_PAGE_1 + 3},
    {GDK_KEY_comma, GDK_CONTROL_MASK, SC_SETTINGS},
  };
  GtkEventController *controller = gtk_shortcut_controller_new();
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  for (size_t i = 0; i < G_N_ELEMENTS(keys); i++)
    gtk_shortcut_controller_add_shortcut(
      GTK_SHORTCUT_CONTROLLER(controller),
      gtk_shortcut_new_with_arguments(gtk_keyval_trigger_new(keys[i].key, keys[i].mods),
                                      gtk_callback_action_new(on_shortcut, a, NULL), "i", (gint32)keys[i].sc));
  gtk_widget_add_controller(win, controller);
}

/* Plays the shown album or playlist in order, from track start on; or with start -1 from
 * the first track, which shuffled is any. Tracks with no file on this device are left out of
 * the queue. Playing it puts it first in the recents, as on Android and Windows. */
void app_play_album(app *a, int start) {
  int n = track_list_count(), count = 0, first = start < 0 ? -1 : 0;
  AlbumItem *album = track_list_album();
  if (!album || !n) return;
  playback_track *queue = g_new0(playback_track, (gsize)n);
  for (int i = 0; i < n; i++) {
    track_list_item t = track_list_item_at(i);
    if (!t.path) continue;
    if (start >= 0 && i <= start) first = count;
    queue[count++] = (playback_track){t.uid, t.path, t.title, t.artist, t.album, t.cover_dir, t.duration_ms};
  }
  if (count) playback_play(queue, count, first);
  g_free(queue);

  mc_recent_kind kind = album->is_playlist ? MC_RECENT_PLAYLIST : MC_RECENT_ALBUM;
  if (count && a->lib && mc_library_record_opened(a->lib, kind, album->uid) == MC_OK) recents_refresh(a);
}

/* --- Albums, playlists and tracks --- */

/* The grid whose selection the track list shows: on Album groups, the open group's albums
 * (NULL while the groups show); on Artists, none. */
static GtkWidget *shown_grid(app *a) {
  switch (a->view) {
    case APP_VIEW_PLAYLISTS: return a->playlist_grid;
    case APP_VIEW_ALBUM_GROUPS: return album_groups_album_grid();
    case APP_VIEW_ARTISTS: return NULL;
    default: return a->album_grid;
  }
}

/* Shows the selected album's or playlist's tracks, or hides the track list when none is
 * selected. */
static void show_album(app *a) {
  GtkWidget *grid = shown_grid(a);
  AlbumItem *album = grid ? album_grid_selection(grid) : NULL;
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

/* The track list's Play and Shuffle: the whole album or playlist, in order from the first
 * track, or shuffled. */
static void on_track_play_all(void *user, gboolean shuffled) {
  set_shuffle(user, shuffled);
  app_play_album(user, shuffled ? -1 : 0);
}

static void on_tracks_close(void *user) {
  GtkWidget *grid = shown_grid(user);
  if (grid) album_grid_unselect(grid);
  show_album(user);
}

/* An album clicked on an artist's page: it opens on the Albums page. */
static void on_artist_album(void *user, const char *album_uid) {
  app_open(user, FALSE, album_uid);
}

/* Opening an album group, or going back from one: the track list follows. */
static void on_group_navigated(void *user) {
  show_album(user);
}

/* Right-clicking an album in the album grid: Add to album group. */
static void on_album_menu(void *user, GtkWidget *grid, AlbumItem *album, double x, double y) {
  album_groups_album_menu(user, grid, album, NULL, x, y);
}

static void on_playlists_changed(void *user) {
  app_reload_library(user);
}

/* The Playlists header's +: the new playlist opens, and stays on the page while it has
 * nothing to play here until another page is picked. */
static void on_playlist_created(void *user, const char *uid) {
  app *a = user;
  g_strlcpy(a->keep_playlist, uid, sizeof a->keep_playlist);
  app_reload_library(a);
  app_open(a, TRUE, uid);
}

static void on_new_playlist(GtkButton *button, gpointer user) {
  app *a = user;
  playlist_create_present(GTK_WIDGET(button), a->lib, on_playlist_created, a);
}

static void on_choose_sync(GtkButton *button, gpointer user) {
  (void)button, (void)user;
  devices_open_self();
}

static void on_track_add_to_playlist(void *user, const char *track_uid) {
  app *a = user;
  add_to_playlist_present(GTK_WIDGET(a->win), a->lib, track_uid, on_playlists_changed, a);
}

static void playlist_failed(app *a, const char *heading) {
  AdwDialog *alert = adw_alert_dialog_new(heading, NULL);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, GTK_WIDGET(a->win));
}

static void on_track_remove(void *user, const char *entry_uid) {
  app *a = user;
  if (mc_playlist_remove(a->lib, entry_uid) != MC_OK)
    playlist_failed(a, mc_str(MC_STR_PLAYLIST_REMOVE_FAILED));
  app_reload_library(a);
}

static void on_track_move(void *user, const char *entry_uid, const char *after_entry_uid) {
  app *a = user;
  if (mc_playlist_move(a->lib, entry_uid, after_entry_uid) != MC_OK)
    playlist_failed(a, mc_str(MC_STR_PLAYLIST_MOVE_FAILED));
  app_reload_library(a);
}

static void on_playlist_rename(void *user) {
  app *a = user;
  AlbumItem *playlist = track_list_album();
  if (playlist && playlist->is_playlist && !playlist->is_smart)
    playlist_rename_present(GTK_WIDGET(a->win), a->lib, playlist->uid, playlist->title, on_playlists_changed, a);
}

static void on_playlist_delete(void *user) {
  app *a = user;
  AlbumItem *playlist = track_list_album();
  if (playlist && playlist->is_playlist && !playlist->is_smart)
    playlist_delete_present(GTK_WIDGET(a->win), a->lib, playlist->uid, playlist->title, on_playlists_changed, a);
}

/* A new order for the shown smart playlist, which is in random order. */
static void on_reshuffle(void *user) {
  app *a = user;
  AlbumItem *playlist = track_list_album();
  if (!playlist || !playlist->is_smart) return;
  if (mc_smart_reshuffle(a->lib, playlist->uid) != MC_OK) playlist_failed(a, mc_str(MC_STR_RESHUFFLE_FAILED));
  app_reload_library(a);
}

/* The track menu's Delete from library... (the album tiles' is in album_groups_album_menu().) */
static void on_track_delete(void *user, const char *track_uid, const char *title) {
  app *a = user;
  delete_dialog_present(GTK_WIDGET(a->win), a->lib, a->settings_dir, MC_DELETE_TRACKS, track_uid, title,
                        on_playlists_changed, a);
}

static void on_active_changed(GObject *win, GParamSpec *pspec, gpointer user) {
  (void)pspec;
  if (gtk_window_is_active(GTK_WINDOW(win)) && restore_deleted(user)) app_reload_library(user);
}

void app_reload_library(app *a) {
  int albums = album_grid_reload(a->album_grid, a->lib);
  int playlists = album_grid_reload_playlists(a->playlist_grid, a->lib, a->art_dir, a->keep_playlist,
                                              &a->hidden_playlists);
  /* Playlists with nothing to play here are hidden; say so rather than "No playlists yet". */
  set_no_playlists(a);
  gtk_stack_set_visible_child_name(a->playlists, playlists > 0 ? "grid" : "none");
  album_groups_reload(a); /* if the open group went, back to the groups */
  artist_view_reload();
  const char *page = a->view == APP_VIEW_PLAYLISTS      ? "playlists"
                     : a->view == APP_VIEW_ALBUM_GROUPS ? "groups"
                                                        : (albums > 0 ? "grid" : "empty");
  gtk_stack_set_visible_child_name(a->albums, page);
  show_album(a); /* if the album went, the selection did too */
  recents_refresh(a);
  if (a->lib) mc_library_search_prepare(a->lib); /* the index is ready before the user types */
  search_view_refresh();
}

/* Picks view in the sidebar, as clicking it does. */
static void open_view(app *a, app_view view) {
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(a->nav, (int)view);
  gtk_list_box_select_row(a->nav, row);
  on_nav_activated(a->nav, row, a);
}

void app_open(app *a, gboolean playlist, const char *uid) {
  open_view(a, playlist ? APP_VIEW_PLAYLISTS : APP_VIEW_ALBUMS);
  if (album_grid_select(shown_grid(a), uid)) show_album(a);
}

void app_open_artist(app *a, const char *uid) {
  open_view(a, APP_VIEW_ARTISTS);
  artist_view_open(uid);
}

void app_open_album_group(app *a, const char *uid) {
  open_view(a, APP_VIEW_ALBUM_GROUPS);
  album_groups_open(a, uid);
}

void app_open_track(app *a, const char *album_uid, const char *track_uid) {
  app_open(a, FALSE, album_uid);
  AlbumItem *album = track_list_album();
  if (album && !strcmp(album->uid, album_uid)) track_list_select(track_uid);
}

/* --- SteadyBin --- */

/* Holds the album grids while the track list slides in and out. The split view shrinks its
 * content a little each frame, which would reflow the grid every frame; this lays the child
 * out at the width it will end at, at once, and clips what the sliding pane will cover. */
#define STEADY_TYPE_BIN (steady_bin_get_type())
G_DECLARE_FINAL_TYPE(SteadyBin, steady_bin, STEADY, BIN, GtkWidget)

struct _SteadyBin {
  GtkWidget parent;
  GtkWidget *child;
  AdwOverlaySplitView *split;
};

G_DEFINE_FINAL_TYPE(SteadyBin, steady_bin, GTK_TYPE_WIDGET)

/* The content's width once the split view settles; mirrors its get_sidebar_width(). */
static int steady_bin_target_width(SteadyBin *self, int width) {
  GtkWidget *split = GTK_WIDGET(self->split);
  int split_width = gtk_widget_get_width(split);
  if (adw_overlay_split_view_get_collapsed(self->split)) return width; /* the pane overlays */
  if (!adw_overlay_split_view_get_show_sidebar(self->split)) return split_width;

  GtkSettings *settings = gtk_widget_get_settings(split);
  AdwLengthUnit unit = adw_overlay_split_view_get_sidebar_width_unit(self->split);
  int sidebar_min, content_min;
  gtk_widget_measure(adw_overlay_split_view_get_sidebar(self->split), GTK_ORIENTATION_HORIZONTAL, -1,
                     &sidebar_min, NULL, NULL, NULL);
  sidebar_min = MAX(sidebar_min, (int)ceil(adw_length_unit_to_px(
                                     unit, adw_overlay_split_view_get_min_sidebar_width(self->split), settings)));
  int sidebar_max = MAX(sidebar_min, (int)ceil(adw_length_unit_to_px(
                                         unit, adw_overlay_split_view_get_max_sidebar_width(self->split), settings)));
  gtk_widget_measure(GTK_WIDGET(self), GTK_ORIENTATION_HORIZONTAL, -1, &content_min, NULL, NULL, NULL);
  int sidebar = MIN((int)(split_width * adw_overlay_split_view_get_sidebar_width_fraction(self->split)),
                    split_width - content_min);
  return split_width - CLAMP(sidebar, sidebar_min, sidebar_max);
}

static void steady_bin_measure(GtkWidget *widget, GtkOrientation orientation, int for_size, int *minimum,
                               int *natural, int *minimum_baseline, int *natural_baseline) {
  SteadyBin *self = STEADY_BIN(widget);
  gtk_widget_measure(self->child, orientation, for_size, minimum, natural, minimum_baseline, natural_baseline);
}

static void steady_bin_size_allocate(GtkWidget *widget, int width, int height, int baseline) {
  SteadyBin *self = STEADY_BIN(widget);
  int min;
  gtk_widget_measure(self->child, GTK_ORIENTATION_HORIZONTAL, height, &min, NULL, NULL, NULL);
  gtk_widget_allocate(self->child, MAX(min, steady_bin_target_width(self, width)), height, baseline, NULL);
}

static void steady_bin_dispose(GObject *obj) {
  SteadyBin *self = STEADY_BIN(obj);
  g_clear_pointer(&self->child, gtk_widget_unparent);
  G_OBJECT_CLASS(steady_bin_parent_class)->dispose(obj);
}

static void steady_bin_class_init(SteadyBinClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = steady_bin_dispose;
  GTK_WIDGET_CLASS(klass)->measure = steady_bin_measure;
  GTK_WIDGET_CLASS(klass)->size_allocate = steady_bin_size_allocate;
}

static void steady_bin_init(SteadyBin *self) {
  gtk_widget_set_overflow(GTK_WIDGET(self), GTK_OVERFLOW_HIDDEN);
}

static GtkWidget *steady_bin_new(AdwOverlaySplitView *split, GtkWidget *child) {
  SteadyBin *self = g_object_new(STEADY_TYPE_BIN, NULL);
  self->split = split;
  self->child = child;
  gtk_widget_set_parent(child, GTK_WIDGET(self));
  return GTK_WIDGET(self);
}

static GtkWidget *build_albums(app *a) {
  GtkWidget *empty = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), "folder-music-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(empty), mc_str(MC_STR_EMPTY_TITLE));
  a->empty = ADW_STATUS_PAGE(empty);
  a->pair = devices_pair_button();
  gtk_widget_set_visible(a->pair, FALSE);
  adw_status_page_set_child(a->empty, a->pair);
  set_empty_description(a);

  GtkWidget *no_playlists = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(no_playlists), "view-list-symbolic");
  a->no_playlists = ADW_STATUS_PAGE(no_playlists);
  a->choose_sync = gtk_button_new_with_label(mc_str(MC_STR_CHOOSE_WHAT_TO_SYNC));
  gtk_widget_add_css_class(a->choose_sync, "pill");
  gtk_widget_set_halign(a->choose_sync, GTK_ALIGN_CENTER);
  g_signal_connect(a->choose_sync, "clicked", G_CALLBACK(on_choose_sync), a);
  adw_status_page_set_child(a->no_playlists, a->choose_sync);
  set_no_playlists(a);

  /* The playlists under a header with the page's name and New playlist (+). */
  GtkWidget *playlists = gtk_stack_new();
  gtk_widget_set_vexpand(playlists, TRUE);
  a->playlists = GTK_STACK(playlists);
  gtk_stack_add_named(a->playlists, no_playlists, "none");
  a->playlist_grid = album_grid_new(mc_str(MC_STR_SECTION_PLAYLISTS), on_album_changed, a);
  gtk_stack_add_named(a->playlists, a->playlist_grid, "grid");
  GtkWidget *playlists_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(playlists_page, "view");
  gtk_box_append(GTK_BOX(playlists_page), app_list_header(mc_str(MC_STR_SECTION_PLAYLISTS), mc_str(MC_STR_NEW_PLAYLIST),
                                                          G_CALLBACK(on_new_playlist), a));
  gtk_box_append(GTK_BOX(playlists_page), playlists);

  GtkWidget *stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(stack), empty, "empty");
  a->album_grid = album_grid_new(mc_str(MC_STR_SECTION_ALBUMS), on_album_changed, a);
  album_grid_set_menu(a->album_grid, on_album_menu, a);
  gtk_stack_add_named(GTK_STACK(stack), a->album_grid, "grid");
  gtk_stack_add_named(GTK_STACK(stack), playlists_page, "playlists");
  gtk_stack_add_named(GTK_STACK(stack), album_groups_new(a, on_album_changed, on_group_navigated), "groups");
  a->albums = GTK_STACK(stack);
  return stack;
}

/* --- Window --- */

static GtkWidget *icon_button(const char *icon, const char *tooltip) {
  GtkWidget *b = gtk_button_new_from_icon_name(icon);
  gtk_widget_set_tooltip_text(b, tooltip);
  return b;
}

/* The playback buttons don't take the focus when clicked, so Space after clicking Next still
 * plays or pauses rather than skipping again. */
static GtkWidget *action_button(const char *icon, const char *tooltip, const char *action) {
  GtkWidget *b = icon_button(icon, tooltip);
  gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
  gtk_widget_set_focus_on_click(b, FALSE);
  return b;
}

/* Clears playlists' pictures out of the cache when their playlists have gone, on a worker
 * thread with its own connection, since it looks each one up. data is the library folder
 * and the cache folder. */
static void prune_art_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  (void)cancellable;
  char **dirs = data;
  mc_library *lib = NULL;
  if (mc_library_open(dirs[0], &lib) == MC_OK) {
    mc_playlist_art_prune(lib, dirs[1]);
    mc_library_close(lib);
  }
  g_task_return_boolean(task, TRUE);
}

static void prune_art(app *a) {
  if (!a->lib) return;
  char **dirs = g_new0(char *, 3);
  dirs[0] = g_strdup(mc_library_dir(a->lib));
  dirs[1] = g_strdup(a->art_dir);
  GTask *task = g_task_new(NULL, NULL, NULL, NULL);
  g_task_set_task_data(task, dirs, (GDestroyNotify)g_strfreev);
  g_task_run_in_thread(task, prune_art_thread);
  g_object_unref(task);
}

/* Sync goes with the window, before the library closes, if closing it didn't stop sync
 * already (it does, while the pairing dialogs and the device page are still there). */
static void on_window_destroy(GtkWidget *win, gpointer user) {
  (void)win;
  devices_stop(user);
}

static void on_shutdown(GApplication *gapp, gpointer user) {
  (void)gapp;
  if (save_volume_source) { /* a change not saved yet */
    g_source_remove(save_volume_source);
    save_volume(user);
  }
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
                            action_button("media-skip-backward-symbolic", mc_str(MC_STR_PREVIOUS_TRACK),
                                          "app.previous"));
  play_button = action_button("media-playback-start-symbolic", mc_str(MC_STR_PLAY), "app.play-pause");
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), play_button);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header),
                            action_button("media-skip-forward-symbolic", mc_str(MC_STR_NEXT_TRACK), "app.next"));
  a->settings = icon_button("emblem-system-symbolic", mc_str(MC_STR_SETTINGS));
  g_signal_connect(a->settings, "clicked", G_CALLBACK(on_settings), a);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), a->settings);
  GtkWidget *import = icon_button("list-add-symbolic", mc_str(MC_STR_IMPORT_MUSIC));
  g_signal_connect(import, "clicked", G_CALLBACK(on_import), a);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), import);

  /* The now-playing strip takes the title's place, up to a point. */
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 640);
  adw_clamp_set_tightening_threshold(ADW_CLAMP(clamp), 640);
  gtk_widget_set_hexpand(clamp, TRUE);
  gtk_widget_set_margin_start(clamp, 12);
  gtk_widget_set_margin_end(clamp, 12);
  adw_clamp_set_child(ADW_CLAMP(clamp), now_playing_new(on_seek, on_shuffle, a));
  adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), clamp);

  /* Right-hand track pane: hidden until an album is selected. */
  GtkWidget *album_split = adw_overlay_split_view_new();
  a->album_split = ADW_OVERLAY_SPLIT_VIEW(album_split);
  adw_overlay_split_view_set_sidebar_position(a->album_split, GTK_PACK_END);
  adw_overlay_split_view_set_min_sidebar_width(a->album_split, 320);
  adw_overlay_split_view_set_max_sidebar_width(a->album_split, 420);
  const track_list_callbacks tracks_cb = {on_track_play, on_track_play_all, on_tracks_close, on_track_add_to_playlist,
                                          on_track_remove, on_track_move, on_playlist_rename, on_playlist_delete,
                                          on_track_delete, on_reshuffle, a};
  adw_overlay_split_view_set_sidebar(a->album_split, track_list_new(&tracks_cb));
  adw_overlay_split_view_set_content(a->album_split, steady_bin_new(a->album_split, build_albums(a)));
  adw_overlay_split_view_set_show_sidebar(a->album_split, FALSE);

  GtkWidget *pages = gtk_stack_new();
  a->pages = GTK_STACK(pages);
  gtk_stack_add_named(a->pages, album_split, "library");
  const artist_view_callbacks artists_cb = {on_shuffle, on_track_add_to_playlist, on_artist_album, playback_changed, a};
  a->artists = artist_view_new(a, &artists_cb);
  gtk_stack_add_named(a->pages, a->artists, "artists");
  a->search = search_view_new(a);
  gtk_stack_add_named(a->pages, a->search, "search");
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
  gtk_window_set_title(a->win, mc_str(MC_STR_APP_NAME));
  gtk_window_set_default_size(a->win, 1200, 800);
  adw_application_window_set_content(ADW_APPLICATION_WINDOW(win), toolbar);
  g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), a);

  /* Space plays or pauses. The shortcut is in the bubble phase, so entries, buttons and
   * switches still get Space first; the album grids and the track list don't (on_list_space()). */
  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_space, 0), gtk_named_action_new("app.play-pause")));
  gtk_widget_add_controller(win, keys);
  GtkEventController *list_keys = gtk_shortcut_controller_new();
  gtk_event_controller_set_propagation_phase(list_keys, GTK_PHASE_CAPTURE);
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(list_keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_space, 0), gtk_callback_action_new(on_list_space, NULL, NULL)));
  gtk_widget_add_controller(win, list_keys);
  add_shortcuts(a, win);

  playback_init(a->win, playback_changed, a);
  playback_set_library(a->lib, a->settings_dir);
  mc_settings *settings = NULL;
  if (mc_settings_load(a->settings_dir, &settings) == MC_OK) { /* shuffle and volume as they were left */
    playback_set_shuffle(mc_settings_shuffle(settings));
    playback_set_volume(mc_settings_volume(settings));
    playback_set_scrobble(mc_settings_scrobble(settings));
  }
  mc_settings_free(settings);

  mpris_init(gapp, a->win);
  prune_art(a);
  restore_deleted(a);
  app_reload_library(a);
  devices_start(a);
  g_signal_connect(win, "notify::is-active", G_CALLBACK(on_active_changed), a);
  g_signal_connect(win, "destroy", G_CALLBACK(on_window_destroy), a);
  gtk_window_present(a->win);
}

/* The UI language: LANGUAGE's first choice, else LC_ALL, LC_MESSAGES or LANG, as GLib
 * reads them for gettext. With none set it's "C", which is English. */
static void set_strings_locale(void) {
  const char *const *names = g_get_language_names();
  mc_strings_set_locale(names[0]);
}

/* Settings from before the app was called Kithsong move over once. If the move fails, the
 * new folder starts out empty as on a first run. */
int main(int argc, char **argv) {
  app a = {0};
  mc_init();
  set_strings_locale(); /* before the player's and import's threads look anything up */

  /* Settings in ~/.config/kithsong say where the library folder is. Until there's a
   * first-run dialog here, the Music folder is used and remembered. */
  char *settings_dir = g_build_filename(g_get_user_config_dir(), "kithsong", NULL);
  char *old_dir = g_build_filename(g_get_user_config_dir(), "musicapp", NULL);
  mc_settings_move_old_dir(old_dir, settings_dir);
  g_free(old_dir);
  mc_settings *settings = NULL;
  if (mc_settings_load(settings_dir, &settings) == MC_OK && !mc_settings_library_dir(settings)) {
    const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
    char *fallback = music ? NULL : g_build_filename(g_get_home_dir(), "Music", NULL);
    mc_settings_set_library_dir(settings, music ? music : fallback);
    mc_settings_save(settings);
    g_free(fallback);
  }
  a.settings_dir = settings_dir;
  a.art_dir = g_build_filename(settings_dir, MC_PLAYLIST_ART_CACHE_DIR, NULL);
  const char *library_dir = mc_settings_library_dir(settings);
  if (!library_dir || mc_library_open(library_dir, &a.lib) != MC_OK) a.lib = NULL;
  mc_settings_free(settings);

  AdwApplication *gapp = adw_application_new("app.kithsong.Kithsong", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), &a);
  g_signal_connect(gapp, "shutdown", G_CALLBACK(on_shutdown), &a);
  int status = g_application_run(G_APPLICATION(gapp), argc, argv);
  g_object_unref(gapp);

  /* The window, and the import view with it, is gone by now, so the library can close. */
  mc_library_close(a.lib);
  g_free(a.settings_dir);
  g_free(a.art_dir);
  mc_shutdown();
  return status;
}
