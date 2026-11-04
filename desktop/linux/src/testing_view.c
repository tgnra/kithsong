#include "testing_view.h"

#include <string.h>

#include "perf_ui.h"

enum { JOB_POLL_MS = 150, REPORT_MS = 2000 };

/* What outlives the dialog. */
static struct {
  mc_stress_job *job;
  char *dir;
  guint poll;
  GThread *deleting;
  gboolean open_next;
} g_work;

/* The open group's widgets, while there is one. */
typedef struct testing {
  app *a;
  GtkWidget *group, *expander, *enable, *run, *copy, *save, *clear, *preset, *generate, *back, *del, *report;
  guint report_timer;
} testing;

static testing *g_shown;

static void update(testing *t) {
  gboolean on = mc_perf_enabled(), busy = g_work.job || g_work.deleting, stress = mc_library_is_stress(t->a->lib);
  gtk_widget_set_sensitive(t->run, on && !mc_perf_auto_running());
  gtk_widget_set_sensitive(t->copy, on);
  gtk_widget_set_sensitive(t->save, on);
  gtk_widget_set_sensitive(t->clear, on);
  gtk_widget_set_sensitive(t->preset, !busy);
  gtk_widget_set_sensitive(t->generate, !busy || g_work.job); /* it's Cancel while it runs */
  gtk_widget_set_sensitive(t->back, stress && !busy);
  gtk_widget_set_sensitive(t->del, !stress && !busy && mc_stress_count(t->a->settings_dir) > 0);
  if (!g_work.job) {
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(t->generate), mc_str(MC_STR_STRESS_GENERATE));
    if (!g_work.deleting) adw_action_row_set_subtitle(ADW_ACTION_ROW(t->generate), mc_str(MC_STR_STRESS_TEXT));
  }
}

static void update_report(testing *t) {
  if (!mc_perf_enabled()) {
    gtk_label_set_text(GTK_LABEL(t->report), mc_str(MC_STR_PERF_OFF));
    return;
  }
  char *report = mc_perf_report();
  if (report && strcmp(gtk_label_get_text(GTK_LABEL(t->report)), report))
    gtk_label_set_text(GTK_LABEL(t->report), report);
  mc_free(report);
}

static gboolean on_report_tick(gpointer data) {
  update_report(data);
  return G_SOURCE_CONTINUE;
}

/* Closes Settings, for what changes the library or wants the main window seen. */
static void close_dialog(testing *t) {
  GtkWidget *dialog = gtk_widget_get_ancestor(t->group, ADW_TYPE_DIALOG);
  if (dialog) adw_dialog_close(ADW_DIALOG(dialog));
}

static gboolean poll_job(gpointer data) {
  app *a = data;
  mc_status st = MC_OK;
  if (!mc_stress_job_done(g_work.job, &st)) {
    if (g_shown) {
      char *text = mc_format_alloc(mc_str(MC_STR_STRESS_GENERATING), MC_D(mc_stress_job_percent(g_work.job)));
      adw_action_row_set_subtitle(ADW_ACTION_ROW(g_shown->generate), text);
      mc_free(text);
    }
    return G_SOURCE_CONTINUE;
  }
  mc_stress_job_free(g_work.job);
  g_work.job = NULL;
  g_work.poll = 0;
  char *dir = g_work.dir;
  g_work.dir = NULL;
  if (st == MC_OK) {
    if (g_shown) close_dialog(g_shown);
    app_open_stress(a, dir);
  } else if (st != MC_ERR_NOT_FOUND) { /* not cancelled */
    char *text = mc_format_alloc(mc_str(MC_STR_STRESS_FAILED), MC_S(mc_status_str(st)));
    AdwToast *toast = adw_toast_new(text);
    adw_toast_overlay_add_toast(a->toasts, toast);
    mc_free(text);
  }
  g_free(dir);
  if (g_shown) update(g_shown);
  return G_SOURCE_REMOVE;
}

static void on_generate(GtkWidget *row, gpointer data) {
  (void)row;
  testing *t = data;
  if (g_work.job) {
    mc_stress_job_cancel(g_work.job);
    return;
  }
  mc_stress_preset preset = (mc_stress_preset)adw_combo_row_get_selected(ADW_COMBO_ROW(t->preset));
  char *dir = mc_stress_dir(t->a->settings_dir, preset);
  if (!dir) return;
  if (mc_stress_is_complete(dir)) { /* made before: the same library, so it's used again */
    close_dialog(t);
    app_open_stress(t->a, dir);
    mc_free(dir);
    return;
  }
  mc_status st = mc_stress_start(dir, preset, &g_work.job);
  if (st != MC_OK) {
    char *text = mc_format_alloc(mc_str(MC_STR_STRESS_FAILED), MC_S(mc_status_str(st)));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(t->generate), text);
    mc_free(text);
    mc_free(dir);
    return;
  }
  g_work.dir = g_strdup(dir);
  mc_free(dir);
  g_work.poll = g_timeout_add(JOB_POLL_MS, poll_job, t->a);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(t->generate), mc_str(MC_STR_CANCEL));
  update(t);
}

static void on_back(GtkWidget *row, gpointer data) {
  (void)row;
  testing *t = data;
  close_dialog(t);
  app_back_to_library(t->a);
}

typedef struct delete_job {
  char *config_dir, *open_dir;
  mc_status status;
  int deleted;
  app *a;
} delete_job;

static gboolean delete_finished(gpointer data) {
  delete_job *job = data;
  g_thread_join(g_work.deleting);
  g_work.deleting = NULL;
  char *text = job->status == MC_OK ? mc_format_alloc(mc_str_plural(MC_STR_STRESS_DELETED, job->deleted), MC_D(job->deleted))
                                    : mc_format_alloc(mc_str(MC_STR_STRESS_FAILED), MC_S(mc_status_str(job->status)));
  adw_toast_overlay_add_toast(job->a->toasts, adw_toast_new(text));
  mc_free(text);
  g_free(job->config_dir);
  g_free(job->open_dir);
  g_free(job);
  if (g_shown) update(g_shown);
  return G_SOURCE_REMOVE;
}

static gpointer delete_main(gpointer data) {
  delete_job *job = data;
  job->status = mc_stress_delete_all(job->config_dir, job->open_dir, &job->deleted);
  g_idle_add(delete_finished, job);
  return NULL;
}

static void on_delete_response(AdwAlertDialog *alert, const char *response, gpointer data) {
  (void)alert;
  testing *t = data;
  if (strcmp(response, "delete") || g_work.deleting) return;
  delete_job *job = g_new0(delete_job, 1);
  job->a = t->a;
  job->config_dir = g_strdup(t->a->settings_dir);
  job->open_dir = g_strdup(t->a->lib ? mc_library_dir(t->a->lib) : NULL);
  g_work.deleting = g_thread_new("stress-delete", delete_main, job);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(t->generate), mc_str(MC_STR_STRESS_DELETING));
  update(t);
}

static void on_delete(GtkWidget *row, gpointer data) {
  (void)row;
  testing *t = data;
  AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_STRESS_HEADING), mc_str(MC_STR_STRESS_DELETE_CONFIRM));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "delete",
                                 mc_str_mnemonic(MC_STR_DELETE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  g_signal_connect(alert, "response", G_CALLBACK(on_delete_response), t);
  adw_dialog_present(alert, t->group);
}

static void on_enable(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  testing *t = data;
  gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  mc_perf_set_enabled(on);
  mc_settings *settings = NULL;
  if (mc_settings_load(t->a->settings_dir, &settings) == MC_OK) {
    mc_settings_set_bool(settings, MC_PERF_SETTING, on);
    mc_settings_save(settings);
  }
  mc_settings_free(settings);
  update(t);
  update_report(t);
}

static void on_run(GtkWidget *row, gpointer data) {
  (void)row;
  testing *t = data;
  close_dialog(t); /* the test scrolls what's behind it */
  perf_ui_run_scroll_test();
}

static void on_copy(GtkWidget *row, gpointer data) {
  (void)row;
  testing *t = data;
  char *report = mc_perf_report();
  if (report) gdk_clipboard_set_text(gtk_widget_get_clipboard(t->group), report);
  mc_free(report);
}

static void on_saved(GObject *source, GAsyncResult *result, gpointer data) {
  app *a = data;
  GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
  if (!file) return;
  char *path = g_file_get_path(file);
  mc_status st = path ? mc_perf_save(path) : MC_ERR_INVALID_ARG;
  if (st != MC_OK) {
    char *text = mc_format_alloc(mc_str(MC_STR_PERF_SAVE_FAILED), MC_S(mc_status_str(st)));
    adw_toast_overlay_add_toast(a->toasts, adw_toast_new(text));
    mc_free(text);
  }
  g_free(path);
  g_object_unref(file);
}

static void on_save(GtkWidget *row, gpointer data) {
  (void)row;
  testing *t = data;
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_initial_name(chooser, "kithsong-profile.txt");
  gtk_file_dialog_save(chooser, t->a->win, NULL, on_saved, t->a);
  g_object_unref(chooser);
}

static void on_clear(GtkWidget *row, gpointer data) {
  (void)row;
  mc_perf_clear();
  update_report(data);
}

static GtkWidget *action_row(testing *t, enum mc_str_id title, GCallback clicked) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mc_str(title));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_signal_connect(row, "activated", clicked, t);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(t->expander), row);
  return row;
}

static void on_group_destroy(GtkWidget *group, gpointer data) {
  (void)group;
  testing *t = data;
  if (t->report_timer) g_source_remove(t->report_timer);
  if (g_shown == t) g_shown = NULL;
  g_free(t);
}

void testing_view_open_next(void) {
  g_work.open_next = TRUE;
}

GtkWidget *testing_view_group(app *a) {
  testing *t = g_new0(testing, 1);
  t->a = a;
  t->group = adw_preferences_group_new();
  t->expander = adw_expander_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(t->expander), mc_str(MC_STR_TESTING_HEADING));
  gboolean stress = mc_library_is_stress(a->lib);
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(t->expander), g_work.open_next || stress);
  g_work.open_next = FALSE;
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(t->group), t->expander);

  t->enable = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(t->enable), mc_str(MC_STR_PERF_ENABLE));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(t->enable), mc_str(MC_STR_PERF_ENABLE_TEXT));
  adw_switch_row_set_active(ADW_SWITCH_ROW(t->enable), mc_perf_enabled());
  g_signal_connect(t->enable, "notify::active", G_CALLBACK(on_enable), t);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(t->expander), t->enable);

  t->run = action_row(t, MC_STR_PERF_RUN_SCROLL, G_CALLBACK(on_run));
  t->copy = action_row(t, MC_STR_PERF_COPY, G_CALLBACK(on_copy));
  t->save = action_row(t, MC_STR_PERF_SAVE, G_CALLBACK(on_save));
  t->clear = action_row(t, MC_STR_PERF_CLEAR, G_CALLBACK(on_clear));

  GtkStringList *presets = gtk_string_list_new(NULL);
  for (int i = 0; i < MC_STRESS_PRESETS; i++) {
    char *text = mc_format_alloc(mc_str(MC_STR_STRESS_PRESET), MC_D(mc_stress_preset_tracks((mc_stress_preset)i)));
    gtk_string_list_append(presets, text);
    mc_free(text);
  }
  t->preset = adw_combo_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(t->preset), mc_str(MC_STR_STRESS_HEADING));
  adw_combo_row_set_model(ADW_COMBO_ROW(t->preset), G_LIST_MODEL(presets));
  adw_combo_row_set_selected(ADW_COMBO_ROW(t->preset), MC_STRESS_10K);
  g_object_unref(presets);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(t->expander), t->preset);

  t->generate = action_row(t, MC_STR_STRESS_GENERATE, G_CALLBACK(on_generate));
  t->back = action_row(t, MC_STR_STRESS_BACK, G_CALLBACK(on_back));
  t->del = action_row(t, MC_STR_STRESS_DELETE, G_CALLBACK(on_delete));

  t->report = gtk_label_new("");
  gtk_label_set_selectable(GTK_LABEL(t->report), TRUE);
  gtk_label_set_xalign(GTK_LABEL(t->report), 0);
  gtk_widget_add_css_class(t->report, "monospace");
  gtk_widget_set_margin_start(t->report, 12);
  gtk_widget_set_margin_end(t->report, 12);
  gtk_widget_set_margin_top(t->report, 12);
  gtk_widget_set_margin_bottom(t->report, 12);
  GtkWidget *report_row = gtk_list_box_row_new();
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(report_row), FALSE);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(report_row), t->report);
  adw_expander_row_add_row(ADW_EXPANDER_ROW(t->expander), report_row);

  g_shown = t;
  g_signal_connect(t->group, "destroy", G_CALLBACK(on_group_destroy), t);
  t->report_timer = g_timeout_add(REPORT_MS, on_report_tick, t);
  update(t);
  update_report(t);
  return t->group;
}
