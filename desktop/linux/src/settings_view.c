#include "settings_view.h"

#include "musiccore/musiccore.h"

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "settings_view.c needs libadwaita 1.5 or later (AdwPreferencesDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct settings_view {
  char *settings_dir;
  settings_view_open_library_fn open_library;
  settings_view_move_library_fn move_library;
  void *user;
  GtkWidget *dialog, *folder;
} settings_view;

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

void settings_view_present(GtkWidget *parent, const char *settings_dir,
                           settings_view_open_library_fn open_library,
                           settings_view_move_library_fn move_library, void *user) {
  settings_view *s = g_new0(settings_view, 1);
  s->settings_dir = g_strdup(settings_dir);
  s->open_library = open_library;
  s->move_library = move_library;
  s->user = user;

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

  s->dialog = GTK_WIDGET(adw_preferences_dialog_new());
  adw_dialog_set_title(ADW_DIALOG(s->dialog), mc_str(MC_STR_SETTINGS));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(s->dialog), ADW_PREFERENCES_PAGE(page));
  g_object_set_data_full(G_OBJECT(s->dialog), "settings-view", s, settings_view_free);
  adw_dialog_present(ADW_DIALOG(s->dialog), parent);
}
