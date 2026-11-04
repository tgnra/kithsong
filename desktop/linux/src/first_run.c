#include "first_run.h"

#include <string.h>

#include "brand_backdrop.h"
#include "musiccore/musiccore.h"

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "first_run.c needs libadwaita 1.5 or later (AdwDialog)"
#endif

enum {
  WIDTH = 520,
  HEIGHT = 640, /* the logo, the text and the rows with a chosen folder, without scrolling */
};

/* Owned by the dialog: freed with it. */
typedef struct first_run {
  first_run_done_fn done;
  void *user;
  GtkWidget *dialog;
  GtkWidget *suggested_row, *chosen_row;
  GtkWidget *suggested_check, *chosen_check;
  GtkWidget *start;
  char *music;     /* the user's Music folder */
  char *suggested; /* Kithsong Library in it */
  char *chosen;    /* the folder the user chose, once there is one */
  char *result;    /* what Start picked */
} first_run;

static void first_run_free(gpointer data) {
  first_run *f = data;
  g_free(f->music);
  g_free(f->suggested);
  g_free(f->chosen);
  g_free(f->result);
  g_free(f);
}

/* The user's Music folder, whether or not it exists yet. */
static char *music_folder(void) {
  const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
  return music ? g_strdup(music) : g_build_filename(g_get_home_dir(), "Music", NULL);
}

static void on_picked(GtkCheckButton *check, GParamSpec *pspec, gpointer data) {
  (void)check;
  (void)pspec;
  first_run *f = data;
  gtk_widget_set_sensitive(f->start, gtk_check_button_get_active(GTK_CHECK_BUTTON(f->suggested_check)) ||
                                       gtk_check_button_get_active(GTK_CHECK_BUTTON(f->chosen_check)));
}

/* The picker's answer: the folder's row, picked. */
static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  GtkWidget *dialog = data; /* referenced while choosing */
  first_run *f = g_object_get_data(G_OBJECT(dialog), "first-run");
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
  char *dir = file ? g_file_get_path(file) : NULL;
  if (file) g_object_unref(file);
  char *lib = dir && mc_folder_fits_library(dir) ? g_strdup(dir) : NULL;
  if (dir && !lib) { /* they can choose again */
    char *text = mc_format_alloc(mc_str(MC_STR_FIRST_RUN_NOT_EMPTY), MC_S(dir));
    AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_LIBRARY_FOLDER), text);
    mc_free(text);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
    adw_dialog_present(alert, dialog);
  } else if (lib && !strcmp(lib, f->suggested)) { /* the suggestion after all */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(f->suggested_check), TRUE);
  } else if (lib) {
    g_free(f->chosen);
    f->chosen = g_strdup(lib);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(f->chosen_row), lib);
    gtk_widget_set_visible(f->chosen_row, TRUE);
    gtk_check_button_set_active(GTK_CHECK_BUTTON(f->chosen_check), TRUE);
  }
  g_free(lib);
  g_free(dir);
  g_object_unref(dialog);
}

static void on_choose(AdwActionRow *row, gpointer data) {
  (void)row;
  first_run *f = data;
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, mc_str(MC_STR_LIBRARY_FOLDER_CHOOSE));
  GFile *start = g_file_new_for_path(f->chosen ? f->chosen : f->music);
  gtk_file_dialog_set_initial_folder(chooser, start);
  g_object_unref(start);
  GtkRoot *root = gtk_widget_get_root(f->dialog);
  gtk_file_dialog_select_folder(chooser, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, on_folder_chosen,
                                g_object_ref(f->dialog));
  g_object_unref(chooser);
}

static void on_start(GtkButton *button, gpointer data) {
  (void)button;
  first_run *f = data;
  gboolean suggested = gtk_check_button_get_active(GTK_CHECK_BUTTON(f->suggested_check));
  f->result = g_strdup(suggested ? f->suggested : f->chosen);
  adw_dialog_force_close(ADW_DIALOG(f->dialog));
}

static void on_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  first_run *f = data;
  if (f->done) f->done(f->user, f->result);
}

/* A folder's row: its name over its path, with a radio button. Paths aren't markup. */
static GtkWidget *folder_row(first_run *f, enum mc_str_id title, const char *path, GtkWidget **check) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mc_str(title));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), path);
  *check = gtk_check_button_new();
  gtk_widget_set_valign(*check, GTK_ALIGN_CENTER);
  gtk_accessible_update_relation(GTK_ACCESSIBLE(*check), GTK_ACCESSIBLE_RELATION_LABELLED_BY, row, NULL, -1);
  g_signal_connect(*check, "notify::active", G_CALLBACK(on_picked), f);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), *check);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), *check);
  return row;
}

void first_run_present(GtkWidget *parent, first_run_done_fn done, void *user) {
  first_run *f = g_new0(first_run, 1);
  f->done = done;
  f->user = user;
  f->music = music_folder();
  f->suggested = g_build_filename(f->music, mc_str(MC_STR_FIRST_RUN_FOLDER_NAME), NULL);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  f->suggested_row = folder_row(f, MC_STR_FIRST_RUN_SUGGESTED, f->suggested, &f->suggested_check);
  gtk_list_box_append(GTK_LIST_BOX(list), f->suggested_row);
  /* Never the Music folder itself, nor a folder of that name with other things in it. */
  gtk_widget_set_visible(f->suggested_row, mc_folder_fits_library(f->suggested));
  f->chosen_row = folder_row(f, MC_STR_FIRST_RUN_CHOSEN, "", &f->chosen_check);
  gtk_check_button_set_group(GTK_CHECK_BUTTON(f->chosen_check), GTK_CHECK_BUTTON(f->suggested_check));
  gtk_widget_set_visible(f->chosen_row, FALSE);
  gtk_list_box_append(GTK_LIST_BOX(list), f->chosen_row);
  GtkWidget *choose = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(choose), mc_str(MC_STR_FIRST_RUN_CHOOSE));
  adw_action_row_add_prefix(ADW_ACTION_ROW(choose), gtk_image_new_from_icon_name("folder-open-symbolic"));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(choose), TRUE);
  g_signal_connect(choose, "activated", G_CALLBACK(on_choose), f);
  gtk_list_box_append(GTK_LIST_BOX(list), choose);

  f->start = gtk_button_new_with_label(mc_str(MC_STR_FIRST_RUN_START));
  gtk_widget_add_css_class(f->start, "pill");
  gtk_widget_add_css_class(f->start, "suggested-action");
  gtk_widget_set_halign(f->start, GTK_ALIGN_CENTER);
  gtk_widget_set_sensitive(f->start, FALSE);
  g_signal_connect(f->start, "clicked", G_CALLBACK(on_start), f);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
  gtk_box_append(GTK_BOX(box), list);
  gtk_box_append(GTK_BOX(box), f->start);

  GtkWidget *page = adw_status_page_new();
  gtk_widget_add_css_class(page, "compact");
  adw_status_page_set_paintable(ADW_STATUS_PAGE(page), GDK_PAINTABLE(brand_logo()));
  adw_status_page_set_title(ADW_STATUS_PAGE(page), mc_str(MC_STR_FIRST_RUN_HEADING));
  adw_status_page_set_description(ADW_STATUS_PAGE(page), mc_str(MC_STR_FIRST_RUN_TEXT));
  adw_status_page_set_child(ADW_STATUS_PAGE(page), box);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);

  f->dialog = GTK_WIDGET(adw_dialog_new());
  g_object_set_data_full(G_OBJECT(f->dialog), "first-run", f, first_run_free);
  adw_dialog_set_title(ADW_DIALOG(f->dialog), mc_str(MC_STR_WELCOME_TITLE));
  adw_dialog_set_content_width(ADW_DIALOG(f->dialog), WIDTH);
  adw_dialog_set_content_height(ADW_DIALOG(f->dialog), HEIGHT);
  adw_dialog_set_child(ADW_DIALOG(f->dialog), toolbar);
  g_signal_connect(f->dialog, "closed", G_CALLBACK(on_closed), f);
  adw_dialog_set_focus(ADW_DIALOG(f->dialog), f->suggested_row);
  adw_dialog_present(ADW_DIALOG(f->dialog), parent);
}
