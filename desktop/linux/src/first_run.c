#include "first_run.h"

#include <string.h>

#include "brand_backdrop.h"
#include "library_folder.h"
#include "kithcore/kithcore.h"
#include "kithcore/source_run.h"

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
  char *suggested; /* Kithsong Library in it, or in Kithsong's own storage if that won't do */
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

static void on_picked(GtkCheckButton *check, GParamSpec *pspec, gpointer data) {
  (void)check;
  (void)pspec;
  first_run *f = data;
  gtk_widget_set_sensitive(f->start, gtk_check_button_get_active(GTK_CHECK_BUTTON(f->suggested_check)) ||
                                       gtk_check_button_get_active(GTK_CHECK_BUTTON(f->chosen_check)));
}

/* A folder that passed the check: its row, picked. */
static void use_folder(first_run *f, const char *lib) {
  if (!strcmp(lib, f->suggested) && gtk_widget_get_visible(f->suggested_row)) { /* the suggestion after all */
    gtk_check_button_set_active(GTK_CHECK_BUTTON(f->suggested_check), TRUE);
    return;
  }
  g_free(f->chosen);
  f->chosen = g_strdup(lib);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(f->chosen_row), lib);
  gtk_widget_set_visible(f->chosen_row, TRUE);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(f->chosen_check), TRUE);
}

static void on_choose(AdwActionRow *row, gpointer data);

static void on_explained(gpointer data, library_folder_answer answer, const char *own) {
  GtkWidget *dialog = data; /* referenced while the alert is up */
  first_run *f = g_object_get_data(G_OBJECT(dialog), "first-run");
  if (answer == LIBRARY_FOLDER_AGAIN) on_choose(NULL, f);
  else if (answer == LIBRARY_FOLDER_OWN) use_folder(f, own);
  g_object_unref(dialog);
}

/* The picker's answer: the folder's row, picked, or why it won't do. */
static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  GtkWidget *dialog = data; /* referenced while choosing */
  first_run *f = g_object_get_data(G_OBJECT(dialog), "first-run");
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
  char *dir = file ? g_file_get_path(file) : NULL;
  if (file) g_object_unref(file);
  mc_folder_problem problem = dir ? library_folder_check(dir, MC_FOLDER_USE_LIBRARY) : MC_FOLDER_OK;
  if (dir && problem != MC_FOLDER_OK)
    library_folder_explain(dialog, NULL, NULL, problem, dir, MC_FOLDER_USE_LIBRARY, NULL, on_explained,
                           g_object_ref(dialog));
  else if (dir)
    use_folder(f, dir);
  g_free(dir);
  g_object_unref(dialog);
}

static void on_choose(AdwActionRow *row, gpointer data) {
  (void)row;
  first_run *f = data;
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, mc_str(MC_STR_LIBRARY_FOLDER_CHOOSE));
  library_folder_start_in(chooser, f->chosen ? f->chosen : f->music);
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
  f->music = library_folder_music();
  f->suggested = g_build_filename(f->music, mc_str(MC_STR_FIRST_RUN_FOLDER_NAME), NULL);
  /* Never the Music folder itself, nor a folder of that name with other things in it, and
   * nothing in there from a checkout, which only asks if its own library couldn't be made.
   * A Flatpak that can't use it suggests Kithsong's own storage instead. */
  gboolean suggest = !mc_source_run() && library_folder_check(f->suggested, MC_FOLDER_USE_LIBRARY) == MC_FOLDER_OK;
  gboolean own = FALSE;
  if (!suggest && !mc_source_run() && library_folder_sandboxed()) {
    char *dir = library_folder_own();
    own = suggest = library_folder_check(dir, MC_FOLDER_USE_LIBRARY) == MC_FOLDER_OK;
    g_free(own ? f->suggested : dir);
    if (own) f->suggested = dir;
  }

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  f->suggested_row = folder_row(f, own ? MC_STR_LINUX_LIBRARY_FOLDER_OWN : MC_STR_FIRST_RUN_SUGGESTED, f->suggested,
                                &f->suggested_check);
  gtk_list_box_append(GTK_LIST_BOX(list), f->suggested_row);
  gtk_widget_set_visible(f->suggested_row, suggest);
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
  if (own) {
    GtkWidget *note = gtk_label_new(mc_str(MC_STR_LINUX_LIBRARY_FOLDER_OWN_NOTE));
    gtk_label_set_wrap(GTK_LABEL(note), TRUE);
    gtk_label_set_xalign(GTK_LABEL(note), 0);
    gtk_widget_add_css_class(note, "dim-label");
    gtk_widget_set_margin_top(note, -12); /* with the list, rather than the box's spacing apart */
    gtk_box_append(GTK_BOX(box), note);
  }
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
