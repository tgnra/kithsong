#include "settings_view.h"

#include <string.h>

#include "musiccore/musiccore.h"

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "settings_view.c needs libadwaita 1.5 or later (AdwPreferencesDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct settings_view {
  char *settings_dir;
  settings_view_open_library_fn open_library;
  settings_view_move_library_fn move_library;
  settings_view_sync sync;
  gboolean has_sync;
  void *user;
  GtkWidget *dialog, *folder;
  GtkWidget *name, *address, *leave; /* the Sync group's rows */
} settings_view;

static void update_sync(settings_view *s);

static void settings_view_free(gpointer data) {
  settings_view *s = data;
  g_free(s->settings_dir);
  g_free(s);
}

/* The library folder settings.json names; g_free it. NULL if there's none. */
static char *load_library_dir(const char *settings_dir) {
  mc_settings *settings = NULL;
  if (mc_settings_load(settings_dir, &settings) != MC_OK) return NULL;
  char *dir = g_strdup(mc_settings_library_dir(settings));
  mc_settings_free(settings);
  return dir;
}

static gboolean save_library_dir(const char *settings_dir, const char *dir) {
  mc_settings *settings = NULL;
  if (mc_settings_load(settings_dir, &settings) != MC_OK) return FALSE;
  gboolean ok = mc_settings_set_library_dir(settings, dir) == MC_OK && mc_settings_save(settings) == MC_OK;
  mc_settings_free(settings);
  return ok;
}

static void show_library_dir(settings_view *s) {
  char *dir = load_library_dir(s->settings_dir);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(s->folder), dir ? dir : mc_str(MC_STR_LINUX_SETTINGS_NONE_CHOSEN));
  g_free(dir);
}

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  GtkWidget *dialog = data; /* referenced while the chooser was open */
  settings_view *s = g_object_get_data(G_OBJECT(dialog), "settings-view");
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
  char *dir = file ? g_file_get_path(file) : NULL;
  char *current = load_library_dir(s->settings_dir);

  if (dir && g_strcmp0(dir, current) != 0 && s->open_library && s->open_library(s->user, dir)) {
    if (!save_library_dir(s->settings_dir, dir))
      adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog),
                                       adw_toast_new(mc_str(MC_STR_SETTINGS_SAVE_FAILED)));
    /* Shown even if it wasn't saved: it's the library open now. */
    adw_action_row_set_subtitle(ADW_ACTION_ROW(s->folder), dir);
    update_sync(s); /* sync restarted with it */
  }
  g_free(current);
  g_free(dir);
  if (file) g_object_unref(file);
  g_object_unref(dialog);
}

static void on_open_activated(AdwActionRow *row, gpointer data) {
  (void)row;
  settings_view *s = data;
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, mc_str(MC_STR_LIBRARY_FOLDER_CHOOSE));
  char *current = load_library_dir(s->settings_dir);
  if (current) {
    GFile *folder = g_file_new_for_path(current);
    gtk_file_dialog_set_initial_folder(chooser, folder);
    g_object_unref(folder);
    g_free(current);
  }
  GtkRoot *root = gtk_widget_get_root(s->dialog);
  gtk_file_dialog_select_folder(chooser, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
                                on_folder_chosen, g_object_ref(s->dialog));
  g_object_unref(chooser);
}

static void on_move_activated(AdwActionRow *row, gpointer data) {
  (void)row;
  settings_view *s = data;
  settings_view_move_library_fn move_library = s->move_library;
  void *user = s->user;
  adw_dialog_close(ADW_DIALOG(s->dialog)); /* may free s */
  if (move_library) move_library(user);
}

static GtkWidget *nav_row(const char *title, const char *subtitle, gboolean chevron) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  if (chevron) adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
  return row;
}

/* --- Sync --- */

static void toast(settings_view *s, const char *text) {
  char *escaped = g_markup_escape_text(text, -1);
  adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(s->dialog), adw_toast_new(escaped));
  g_free(escaped);
}

/* Sets an entry row's text as the code's, not an edit: its apply button stays hidden. */
static void set_entry_text(GtkWidget *row, const char *text) {
  if (strcmp(gtk_editable_get_text(GTK_EDITABLE(row)), text)) gtk_editable_set_text(GTK_EDITABLE(row), text);
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), FALSE); /* forgets the change */
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
}

/* This computer's name as sync has it, else as saved, else the host name; g_free it. */
static char *own_name(settings_view *s) {
  mc_sync *sync = s->sync.get();
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  if (sync && mc_sync_get_device_name(sync, name, sizeof name) == MC_OK && name[0]) return g_strdup(name);
  mc_settings *settings = NULL;
  char *saved = NULL;
  if (mc_settings_load(s->settings_dir, &settings) == MC_OK) {
    saved = g_strdup(mc_settings_device_name(settings));
    mc_settings_free(settings);
  }
  return saved ? saved : g_strdup(g_get_host_name());
}

/* The other members' names, "Phone, Laptop", or NULL when not in a group; g_free it. */
static char *other_members(mc_sync *sync) {
  mc_sync_device_list *list = NULL;
  if (!sync || mc_sync_list_devices(sync, &list) != MC_OK || !list) return NULL;
  GString *names = list->count ? g_string_new(NULL) : NULL;
  for (int i = 0; names && i < list->count; i++) {
    const mc_sync_device *d = &list->items[i];
    if (d->is_self || d->removed || !d->name) continue;
    if (names->len) g_string_append(names, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
    g_string_append(names, d->name);
  }
  mc_sync_device_list_free(list);
  return names ? g_string_free(names, FALSE) : NULL;
}

static void update_sync(settings_view *s) {
  if (!s->has_sync) return;
  char *name = own_name(s);
  set_entry_text(s->name, name);
  g_free(name);

  mc_sync *sync = s->sync.get();
  if (!sync) {
    char *off = s->sync.off_text();
    adw_action_row_set_subtitle(ADW_ACTION_ROW(s->address), off ? off : "");
    g_free(off);
  } else {
    char addrs[8][MC_SYNC_ADDR_STRLEN];
    unsigned port = mc_sync_port(sync);
    int n = mc_sync_local_addresses(addrs, 8);
    GString *text = g_string_new(NULL);
    for (int i = 0; i < n; i++) {
      if (i) g_string_append(text, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
      g_string_append_printf(text, strchr(addrs[i], ':') ? "[%s]:%u" : "%s:%u", addrs[i], port);
    }
    adw_action_row_set_subtitle(ADW_ACTION_ROW(s->address), n > 0 ? text->str : mc_str(MC_STR_NO_NETWORK));
    g_string_free(text, TRUE);
  }
  char *others = other_members(sync);
  gtk_widget_set_sensitive(s->leave, others != NULL);
  g_free(others);
}

static void on_name_apply(AdwEntryRow *row, gpointer data) {
  settings_view *s = data;
  char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
  mc_sync *sync = s->sync.get();
  mc_status st = !*name ? MC_ERR_INVALID_ARG : sync ? mc_sync_set_device_name(sync, name) : MC_OK;
  if (st != MC_OK) {
    char text[256];
    mc_format(text, sizeof text, mc_str(MC_STR_DEVICE_RENAME_FAILED), MC_S(mc_status_str(st)));
    toast(s, text);
  } else {
    /* Sync may have cut it to fit: save what it now goes by. */
    char stored[MC_SYNC_DEVICE_NAME_MAX + 1];
    if (sync && mc_sync_get_device_name(sync, stored, sizeof stored) == MC_OK && stored[0])
      s->sync.renamed(s->user, stored);
    else
      s->sync.renamed(s->user, name);
  }
  g_free(name);
  update_sync(s);
}

static void on_leave_response(AdwAlertDialog *alert, const char *response, gpointer data) {
  (void)alert;
  settings_view *s = data;
  mc_sync *sync = s->sync.get();
  if (strcmp(response, "leave") || !sync) return;
  mc_status st = mc_sync_leave_group(sync); /* blocks for a few seconds at most */
  if (st == MC_ERR_LOCKED) {
    toast(s, mc_str(MC_STR_LINUX_COMPUTER_LEAVE_ONLY_ARCHIVE));
  } else if (st != MC_OK) {
    char text[256];
    mc_format(text, sizeof text, mc_str(MC_STR_LEAVE_GROUP_FAILED), MC_S(mc_status_str(st)));
    toast(s, text);
  }
  s->sync.group_changed(s->user);
  update_sync(s);
}

static void on_leave_activated(GtkWidget *row, gpointer data) {
  (void)row;
  settings_view *s = data;
  char *others = other_members(s->sync.get());
  if (!others) return;
  char *body = mc_format_alloc(mc_str(MC_STR_LINUX_COMPUTER_LEAVE_GROUP_CONFIRM),
                               MC_S(*others ? others : mc_str(MC_STR_OTHER_DEVICES)));
  AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_LEAVE_GROUP_TITLE), body);
  mc_free(body);
  g_free(others);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "leave",
                                 mc_str_mnemonic(MC_STR_LEAVE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "leave", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  g_signal_connect(alert, "response", G_CALLBACK(on_leave_response), s);
  adw_dialog_present(alert, s->dialog);
}

static GtkWidget *nav_row(const char *title, const char *subtitle, gboolean chevron);

static GtkWidget *build_sync_group(settings_view *s) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_SYNC));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_LINUX_COMPUTER_SYNC_SETTINGS_NOTE));

  s->name = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->name), mc_str(MC_STR_LINUX_THIS_COMPUTER_NAME));
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(s->name), TRUE);
  g_signal_connect(s->name, "apply", G_CALLBACK(on_name_apply), s);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->name);

  s->address = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(s->address), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->address), mc_str(MC_STR_LINUX_THIS_COMPUTER_ADDRESS));
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(s->address), TRUE);
  gtk_widget_add_css_class(s->address, "property");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->address);

#if ADW_CHECK_VERSION(1, 6, 0)
  s->leave = adw_button_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->leave), mc_str(MC_STR_LEAVE_GROUP_BUTTON));
  gtk_widget_add_css_class(s->leave, "destructive-action");
#else
  s->leave = nav_row(mc_str(MC_STR_LEAVE_GROUP_BUTTON), NULL, FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(s->leave), TRUE);
#endif
  g_signal_connect(s->leave, "activated", G_CALLBACK(on_leave_activated), s);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->leave);
  return group;
}

void settings_view_present(GtkWidget *parent, const char *settings_dir,
                           settings_view_open_library_fn open_library,
                           settings_view_move_library_fn move_library, const settings_view_sync *sync,
                           void *user) {
  settings_view *s = g_new0(settings_view, 1);
  s->settings_dir = g_strdup(settings_dir);
  s->open_library = open_library;
  s->move_library = move_library;
  s->user = user;
  if (sync) {
    s->sync = *sync;
    s->has_sync = TRUE;
  }

  /* The folder is plain text (not markup) and selectable, so it can be copied. */
  s->folder = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(s->folder), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->folder), mc_str(MC_STR_LIBRARY_FOLDER));
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(s->folder), TRUE);
  gtk_widget_add_css_class(s->folder, "property");
  show_library_dir(s);

  GtkWidget *open = nav_row(mc_str(MC_STR_LIBRARY_OPEN_ANOTHER), NULL, TRUE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(open), TRUE);
  g_signal_connect(open, "activated", G_CALLBACK(on_open_activated), s);

  GtkWidget *move = nav_row(mc_str(MC_STR_LIBRARY_MOVE), NULL, TRUE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(move), TRUE);
  g_signal_connect(move, "activated", G_CALLBACK(on_move_activated), s);

  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_LIBRARY_LOCATION));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_LIBRARY_LOCATION_TEXT));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->folder);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), open);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), move);

  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_set_title(ADW_PREFERENCES_PAGE(page), mc_str(MC_STR_LINUX_SETTINGS_GENERAL));
  adw_preferences_page_set_icon_name(ADW_PREFERENCES_PAGE(page), "emblem-system-symbolic");
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(group));
  if (s->has_sync) adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(build_sync_group(s)));

  s->dialog = GTK_WIDGET(adw_preferences_dialog_new());
  adw_dialog_set_title(ADW_DIALOG(s->dialog), mc_str(MC_STR_SETTINGS));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(s->dialog), ADW_PREFERENCES_PAGE(page));
  g_object_set_data_full(G_OBJECT(s->dialog), "settings-view", s, settings_view_free);
  update_sync(s);
  adw_dialog_present(ADW_DIALOG(s->dialog), parent);
}
