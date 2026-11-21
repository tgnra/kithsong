#include "library_folder.h"

#include <string.h>

gboolean library_folder_sandboxed(void) {
  static int sandboxed = -1;
  if (sandboxed < 0) sandboxed = g_file_test("/.flatpak-info", G_FILE_TEST_EXISTS);
  return sandboxed;
}

char *library_folder_music(void) {
  const char *music = g_get_user_special_dir(G_USER_DIRECTORY_MUSIC);
  return music ? g_strdup(music) : g_build_filename(g_get_home_dir(), "Music", NULL);
}

char *library_folder_own(void) {
  return g_build_filename(g_get_user_data_dir(), mc_str(MC_STR_FIRST_RUN_FOLDER_NAME), NULL);
}

mc_folder_problem library_folder_check(const char *dir, mc_folder_use use) {
  if (!library_folder_sandboxed()) return mc_library_folder_check(dir, use, NULL);
  char *music = library_folder_music();
  const char *roots[] = {music, g_get_user_data_dir(), NULL};
  mc_folder_problem problem = mc_library_folder_check(dir, use, roots);
  g_free(music);
  return problem;
}

void library_folder_start_in(GtkFileDialog *chooser, const char *from) {
  char *music = library_folder_sandboxed() ? library_folder_music() : NULL;
  const char *start = music ? music : from;
  if (start) {
    GFile *folder = g_file_new_for_path(start);
    gtk_file_dialog_set_initial_folder(chooser, folder);
    g_object_unref(folder);
  }
  g_free(music);
}

static gboolean of_reach(mc_folder_problem problem) {
  return problem == MC_FOLDER_PORTAL || problem == MC_FOLDER_OUT_OF_REACH;
}

char *library_folder_why(mc_folder_problem problem, const char *dir, mc_folder_use use) {
  enum mc_str_id id = MC_STR_LIBRARY_MOVE_NOT_WRITABLE;
  switch (problem) {
    case MC_FOLDER_OK: return g_strdup("");
    case MC_FOLDER_PORTAL:
    case MC_FOLDER_OUT_OF_REACH: return g_strdup(mc_str(MC_STR_LINUX_LIBRARY_FOLDER_SANDBOX));
    case MC_FOLDER_NOT_EMPTY:
      if (use == MC_FOLDER_USE_NEW) return g_strdup(mc_str(MC_STR_LIBRARY_NOT_EMPTY));
      id = use == MC_FOLDER_USE_EMPTY ? MC_STR_LIBRARY_MOVE_NOT_EMPTY : MC_STR_FIRST_RUN_NOT_EMPTY;
      break;
    case MC_FOLDER_NOT_WRITABLE: id = MC_STR_LIBRARY_MOVE_NOT_WRITABLE; break;
    case MC_FOLDER_NO_FREE_SPACE: id = MC_STR_LINUX_LIBRARY_FOLDER_NO_FREE_SPACE; break;
  }
  char *text = mc_format_alloc(mc_str(id), MC_S(dir));
  char *why = g_strdup(text ? text : "");
  mc_free(text);
  return why;
}

typedef struct explaining {
  library_folder_answer_fn done;
  gpointer data;
  char *own;
} explaining;

static void on_explained(AdwAlertDialog *alert, const char *response, gpointer user) {
  (void)alert;
  explaining *e = user;
  library_folder_answer answer = !strcmp(response, "again") ? LIBRARY_FOLDER_AGAIN
                                 : !strcmp(response, "own") ? LIBRARY_FOLDER_OWN
                                                            : LIBRARY_FOLDER_CANCEL;
  if (e->done) e->done(e->data, answer, e->own);
  g_free(e->own);
  g_free(e);
}

void library_folder_explain(GtkWidget *parent, const char *heading, const char *body, mc_folder_problem problem,
                            const char *dir, mc_folder_use use, const char *again, library_folder_answer_fn done,
                            gpointer data) {
  explaining *e = g_new0(explaining, 1);
  e->done = done;
  e->data = data;
  if (library_folder_sandboxed() && problem != MC_FOLDER_NOT_EMPTY) {
    char *own = library_folder_own();
    if (g_strcmp0(own, dir) && library_folder_check(own, use) == MC_FOLDER_OK) e->own = own;
    else g_free(own);
  }
  char *why = body ? NULL : library_folder_why(problem, dir, use);
  AdwDialog *alert = adw_alert_dialog_new(heading ? heading : mc_str(MC_STR_LINUX_LIBRARY_FOLDER_CANT_TITLE),
                                          body ? body : why);
  g_free(why);
  AdwAlertDialog *a = ADW_ALERT_DIALOG(alert);
  adw_alert_dialog_add_response(a, "cancel", mc_str_mnemonic(MC_STR_CANCEL));
  if (e->own) adw_alert_dialog_add_response(a, "own", mc_str_mnemonic(MC_STR_LINUX_LIBRARY_FOLDER_USE_OWN));
  adw_alert_dialog_add_response(a, "again", again ? again
                                            : of_reach(problem) ? mc_str_mnemonic(MC_STR_LINUX_LIBRARY_FOLDER_CHOOSE_IN_MUSIC)
                                                                : mc_str(MC_STR_FIRST_RUN_CHOOSE));
  adw_alert_dialog_set_response_appearance(a, "again", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(a, "again");
  adw_alert_dialog_set_close_response(a, "cancel");
  g_signal_connect(alert, "response", G_CALLBACK(on_explained), e);
  adw_dialog_present(alert, parent);
}
