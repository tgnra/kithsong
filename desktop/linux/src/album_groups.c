#include "album_groups.h"

#include <string.h>

#include "album_group_dialogs.h"
#include "delete_dialog.h"
#include "recents.h"

static struct {
  app *a;
  void (*album_changed)(void *user);
  void (*navigated)(void *user);
  GtkWidget *stack;        /* "none" status page | "groups" grid | "group" page */
  GtkWidget *groups;       /* the groups' grid */
  GtkWidget *albums;       /* the open group's albums' grid */
  GtkWidget *albums_stack; /* "empty" status page | "albums" */
  GtkWidget *name, *more;  /* the open group's header: its name and menu */
  GtkWidget *reshuffle;    /* in place of the menu, for a smart group in random order */
  char open[MC_UID_STRLEN];       /* the open group, or "" */
  gboolean smart;                 /* it's a smart group: nothing to rename, delete or remove */
  char menu_album[MC_UID_STRLEN]; /* the album a menu was opened on */
  char menu_group[MC_UID_STRLEN]; /* and the group it's in there, or "" */
  char *menu_title;               /* and the album's title */
  gboolean actions;               /* the "albumgroup." actions are on the window */
} v;

static void on_library_changed(void *user) {
  app_reload_library(user);
}

static void fail(const char *heading) {
  AdwDialog *alert = adw_alert_dialog_new(heading, NULL);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, GTK_WIDGET(v.a->win));
}

/* A name as a menu item's label, its underscores kept rather than taken as mnemonics.
 * g_free it. */
static char *menu_label(const char *name) {
  GString *s = g_string_new(NULL);
  for (const char *p = name && *name ? name : mc_str(MC_STR_UNTITLED); *p; p++) {
    if (*p == '_') g_string_append_c(s, '_');
    g_string_append_c(s, *p);
  }
  return g_string_free(s, FALSE);
}

/* The group's name as the groups' grid has it, or "". */
static char *group_name(const char *uid) {
  AlbumItem *group = album_grid_find(v.groups, uid);
  char *name = g_strdup(group ? group->title : "");
  if (group) g_object_unref(group);
  return name;
}

/* --- Actions: albumgroup.add(uid), .new, .remove, .rename(uid), .delete(uid), and
 * .delete-album, which deletes the menu's album from the library (delete_dialog.h) --- */

static void on_add(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  app *a = user;
  if (!a->lib || !v.menu_album[0]) return;
  if (mc_album_group_add(a->lib, g_variant_get_string(param, NULL), v.menu_album) != MC_OK)
    fail(mc_str(MC_STR_ALBUM_GROUP_ADD_FAILED));
  app_reload_library(a);
}

static void on_new(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  app *a = user;
  if (a->lib && v.menu_album[0])
    album_group_create_present(GTK_WIDGET(a->win), a->lib, v.menu_album, on_library_changed, a);
}

static void on_remove(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  app *a = user;
  if (!a->lib || !v.menu_album[0] || !v.menu_group[0]) return;
  if (mc_album_group_remove(a->lib, v.menu_group, v.menu_album) != MC_OK)
    fail(mc_str(MC_STR_ALBUM_GROUP_REMOVE_FAILED));
  app_reload_library(a);
}

static void on_rename(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  app *a = user;
  const char *uid = g_variant_get_string(param, NULL);
  char *name = group_name(uid);
  album_group_rename_present(GTK_WIDGET(a->win), a->lib, uid, name, on_library_changed, a);
  g_free(name);
}

static void on_delete(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  app *a = user;
  const char *uid = g_variant_get_string(param, NULL);
  char *name = group_name(uid);
  album_group_delete_present(GTK_WIDGET(a->win), a->lib, uid, name, on_library_changed, a);
  g_free(name);
}

static void on_delete_album(GSimpleAction *action, GVariant *param, gpointer user) {
  (void)action;
  (void)param;
  app *a = user;
  if (a->lib && v.menu_album[0])
    delete_dialog_present(GTK_WIDGET(a->win), a->lib, a->settings_dir, MC_DELETE_ALBUMS, v.menu_album,
                          v.menu_title, on_library_changed, a);
}

/* On the window, so the album grids' menus and the header's find them. */
static void ensure_actions(void) {
  if (v.actions || !v.a->win) return;
  const GActionEntry entries[] = {
    {"add", on_add, "s", NULL, NULL, {0}},
    {"new", on_new, NULL, NULL, NULL, {0}},
    {"remove", on_remove, NULL, NULL, NULL, {0}},
    {"rename", on_rename, "s", NULL, NULL, {0}},
    {"delete", on_delete, "s", NULL, NULL, {0}},
    {"delete-album", on_delete_album, NULL, NULL, NULL, {0}},
  };
  GSimpleActionGroup *group = g_simple_action_group_new();
  g_action_map_add_action_entries(G_ACTION_MAP(group), entries, G_N_ELEMENTS(entries), v.a);
  gtk_widget_insert_action_group(GTK_WIDGET(v.a->win), "albumgroup", G_ACTION_GROUP(group));
  g_object_unref(group);
  v.actions = TRUE;
}

/* Rename... and Delete album group... for group uid. */
static GMenuModel *group_menu(const char *uid) {
  GMenu *menu = g_menu_new();
  GMenuItem *item = g_menu_item_new(mc_str_mnemonic(MC_STR_PLAYLIST_RENAME_MENU), NULL);
  g_menu_item_set_action_and_target_value(item, "albumgroup.rename", g_variant_new_string(uid));
  g_menu_append_item(menu, item);
  g_object_unref(item);
  item = g_menu_item_new(mc_str_mnemonic(MC_STR_ALBUM_GROUP_DELETE_MENU), NULL);
  g_menu_item_set_action_and_target_value(item, "albumgroup.delete", g_variant_new_string(uid));
  g_menu_append_item(menu, item);
  g_object_unref(item);
  return G_MENU_MODEL(menu);
}

void album_groups_album_menu(app *a, GtkWidget *grid, AlbumItem *album, const char *in_group, double x, double y) {
  ensure_actions();
  if (!a->lib || !album || album->is_playlist || album->is_group) return;
  g_strlcpy(v.menu_album, album->uid, sizeof v.menu_album);
  g_strlcpy(v.menu_group, in_group ? in_group : "", sizeof v.menu_group);
  g_free(v.menu_title);
  v.menu_title = g_strdup(album->title);

  GMenu *menu = g_menu_new(), *add = g_menu_new();
  mc_album_group_list *groups = NULL;
  if (mc_library_list_album_groups(a->lib, &groups) == MC_OK && groups->count) {
    GMenu *section = g_menu_new();
    for (int i = 0; i < groups->count; i++) {
      char *label = menu_label(groups->items[i].name);
      GMenuItem *item = g_menu_item_new(label, NULL);
      g_menu_item_set_action_and_target_value(item, "albumgroup.add", g_variant_new_string(groups->items[i].uid));
      g_menu_append_item(section, item);
      g_object_unref(item);
      g_free(label);
    }
    g_menu_append_section(add, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  mc_album_group_list_free(groups);
  GMenu *section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_NEW_ALBUM_GROUP_MENU), "albumgroup.new");
  g_menu_append_section(add, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  g_menu_append_submenu(menu, mc_str_mnemonic(MC_STR_ADD_TO_ALBUM_GROUP), G_MENU_MODEL(add));
  g_object_unref(add);
  if (in_group) {
    section = g_menu_new();
    g_menu_append(section, mc_str_mnemonic(MC_STR_REMOVE_FROM_ALBUM_GROUP), "albumgroup.remove");
    g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
  }
  section = g_menu_new();
  g_menu_append(section, mc_str_mnemonic(MC_STR_DELETE_ALBUM_MENU), "albumgroup.delete-album");
  g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
  g_object_unref(section);
  album_grid_popup(grid, G_MENU_MODEL(menu), x, y);
  g_object_unref(menu); /* the popover keeps it */
}

/* --- Opening a group and going back --- */

static void show_open(void) {
  gtk_stack_set_visible_child_name(GTK_STACK(v.stack), "group");
}

/* Empties the albums' grid, so the next group's start at the top with nothing selected. */
static void clear_albums(void) {
  gboolean found;
  album_grid_unselect(v.albums);
  album_grid_reload_group_albums(v.albums, NULL, NULL, &found);
}

/* Lists the open group's albums again; FALSE if it has gone. */
static gboolean reload_open(app *a) {
  gboolean found = FALSE;
  int n = album_grid_reload_group_albums(v.albums, a->lib, v.open, &found);
  if (!found) return FALSE;
  gtk_stack_set_visible_child_name(GTK_STACK(v.albums_stack), n ? "albums" : v.smart ? "smart-empty" : "empty");
  char *name = group_name(v.open);
  if (*name) gtk_label_set_text(GTK_LABEL(v.name), name); /* renamed, maybe */
  g_free(name);
  return TRUE;
}

gboolean album_groups_open(app *a, const char *uid) {
  AlbumItem *group = album_grid_find(v.groups, uid);
  if (!group) return FALSE;
  ensure_actions();
  g_strlcpy(v.open, group->uid, sizeof v.open);
  gtk_label_set_text(GTK_LABEL(v.name), group->title);
  v.smart = group->is_smart;
  gtk_widget_set_visible(v.more, !group->is_smart);
  gtk_widget_set_visible(v.reshuffle, group->is_smart && group->random);
  g_object_unref(group);
  GMenuModel *menu = group_menu(v.open);
  gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(v.more), menu);
  g_object_unref(menu);
  clear_albums();
  reload_open(a);
  show_open();
  album_grid_focus(v.albums); /* so Alt+Left and the arrow keys work from the start */
  if (a->lib && mc_library_record_opened(a->lib, MC_RECENT_ALBUM_GROUP, v.open) == MC_OK) recents_refresh(a);
  if (v.navigated) v.navigated(a);
  return TRUE;
}

static void go_back(void) {
  if (!v.open[0]) return;
  char was[MC_UID_STRLEN];
  memcpy(was, v.open, sizeof was);
  v.open[0] = '\0';
  clear_albums();
  album_grid_select(v.groups, was);
  album_groups_reload(v.a);
  if (v.navigated) v.navigated(v.a);
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
  go_back();
  return TRUE;
}

/* The mouse's back button. */
static void on_back_click(GtkGestureClick *click, int n_press, double x, double y, gpointer user) {
  (void)n_press;
  (void)x;
  (void)y;
  (void)user;
  gtk_gesture_set_state(GTK_GESTURE(click), GTK_EVENT_SEQUENCE_CLAIMED);
  go_back();
}

static void on_group_activated(void *user, AlbumItem *group) {
  char uid[MC_UID_STRLEN];
  g_strlcpy(uid, group->uid, sizeof uid); /* opening reloads the grid, and the item may go */
  album_groups_open(user, uid);
}

static void on_group_menu(void *user, GtkWidget *grid, AlbumItem *group, double x, double y) {
  (void)user;
  if (group->is_smart) return; /* nothing to rename or delete */
  ensure_actions();
  GMenuModel *menu = group_menu(group->uid);
  album_grid_popup(grid, menu, x, y);
  g_object_unref(menu);
}

static void on_album_menu(void *user, GtkWidget *grid, AlbumItem *album, double x, double y) {
  album_groups_album_menu(user, grid, album, v.open[0] && !v.smart ? v.open : NULL, x, y);
}

/* The header's Reshuffle, for a smart group in random order: its albums in a new order. */
static void on_reshuffle(GtkButton *button, gpointer user) {
  (void)button;
  app *a = user;
  if (!a->lib || !v.open[0] || !v.smart) return;
  if (mc_smart_reshuffle(a->lib, v.open) != MC_OK) fail(mc_str(MC_STR_RESHUFFLE_FAILED));
  app_reload_library(a);
}

static void on_album_changed(void *user) {
  if (v.album_changed) v.album_changed(user);
}

/* --- The page --- */

static GtkWidget *status_page(const char *icon, enum mc_str_id title, enum mc_str_id text) {
  GtkWidget *page = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), icon);
  adw_status_page_set_title(ADW_STATUS_PAGE(page), mc_str(title));
  adw_status_page_set_description(ADW_STATUS_PAGE(page), mc_str(text));
  return page;
}

static GtkWidget *round_button(GtkWidget *b, const char *tooltip) {
  gtk_widget_set_tooltip_text(b, tooltip);
  gtk_widget_add_css_class(b, "flat");
  gtk_widget_add_css_class(b, "circular");
  gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
  return b;
}

static GtkWidget *build_group_page(app *a) {
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 18);
  gtk_widget_set_margin_top(header, 12);
  GtkWidget *back = round_button(gtk_button_new_from_icon_name("go-previous-symbolic"), mc_str(MC_STR_BACK));
  g_signal_connect(back, "clicked", G_CALLBACK(on_back), NULL);
  gtk_box_append(GTK_BOX(header), back);
  v.name = gtk_label_new(NULL);
  gtk_widget_add_css_class(v.name, "title-2");
  gtk_label_set_xalign(GTK_LABEL(v.name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(v.name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(v.name, TRUE);
  gtk_box_append(GTK_BOX(header), v.name);
  v.more = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(v.more), "view-more-symbolic");
  gtk_box_append(GTK_BOX(header), round_button(v.more, mc_str(MC_STR_LINUX_ALBUM_GROUP_MENU)));
  v.reshuffle = round_button(gtk_button_new_from_icon_name("view-refresh-symbolic"), mc_str(MC_STR_RESHUFFLE));
  gtk_widget_set_visible(v.reshuffle, FALSE);
  g_signal_connect(v.reshuffle, "clicked", G_CALLBACK(on_reshuffle), a);
  gtk_box_append(GTK_BOX(header), v.reshuffle);

  v.albums = album_grid_new(mc_str(MC_STR_ALBUM_GROUP), on_album_changed, a);
  album_grid_set_menu(v.albums, on_album_menu, a);
  v.albums_stack = gtk_stack_new();
  gtk_widget_set_vexpand(v.albums_stack, TRUE);
  gtk_stack_add_named(GTK_STACK(v.albums_stack),
                      status_page("media-optical-symbolic", MC_STR_ALBUM_GROUP_EMPTY_TITLE, MC_STR_ALBUM_GROUP_EMPTY_TEXT),
                      "empty");
  gtk_stack_add_named(GTK_STACK(v.albums_stack),
                      status_page("media-optical-symbolic", MC_STR_SMART_LIST_EMPTY_TITLE, MC_STR_SMART_LIST_EMPTY_TEXT),
                      "smart-empty");
  gtk_stack_add_named(GTK_STACK(v.albums_stack), v.albums, "albums");

  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_add_css_class(page, "view"); /* the header on the grid's background */
  gtk_box_append(GTK_BOX(page), header);
  gtk_box_append(GTK_BOX(page), v.albums_stack);

  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_Left, GDK_ALT_MASK), gtk_callback_action_new(on_back_key, NULL, NULL)));
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_keyval_trigger_new(GDK_KEY_BackSpace, 0), gtk_callback_action_new(on_back_key, NULL, NULL)));
  gtk_widget_add_controller(page, keys);
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), 8); /* back */
  g_signal_connect(click, "pressed", G_CALLBACK(on_back_click), NULL);
  gtk_widget_add_controller(page, GTK_EVENT_CONTROLLER(click));
  return page;
}

GtkWidget *album_groups_new(app *a, void (*album_changed)(void *user), void (*navigated)(void *user)) {
  v.a = a;
  v.album_changed = album_changed;
  v.navigated = navigated;
  v.groups = album_grid_new(mc_str(MC_STR_SECTION_ALBUM_GROUPS), NULL, NULL);
  album_grid_set_activate(v.groups, TRUE, on_group_activated, a);
  album_grid_set_menu(v.groups, on_group_menu, a);

  v.stack = gtk_stack_new();
  gtk_stack_add_named(GTK_STACK(v.stack),
                      status_page("folder-symbolic", MC_STR_NO_ALBUM_GROUPS_TITLE, MC_STR_NO_ALBUM_GROUPS_TEXT), "none");
  gtk_stack_add_named(GTK_STACK(v.stack), v.groups, "groups");
  gtk_stack_add_named(GTK_STACK(v.stack), build_group_page(a), "group");
  return v.stack;
}

int album_groups_reload(app *a) {
  ensure_actions();
  int n = album_grid_reload_groups(v.groups, a->lib, a->art_dir);
  if (v.open[0] && !reload_open(a)) { /* deleted, maybe on another device: back to the groups */
    v.open[0] = '\0';
    clear_albums();
  }
  gtk_stack_set_visible_child_name(GTK_STACK(v.stack), v.open[0] ? "group" : n ? "groups" : "none");
  return n;
}

const char *album_groups_open_group(void) {
  return v.open[0] ? v.open : NULL;
}

GtkWidget *album_groups_album_grid(void) {
  return v.open[0] ? v.albums : NULL;
}
