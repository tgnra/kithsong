/* See analysis_ui.h. */
#include "analysis_ui.h"

#include <adwaita.h>

#include "kithcore/analysis.h"
#include "kithcore/kithcore.h"

#define SETTING_BACKGROUND "analysis_background"
#define SETTING_STOPPED "analysis_stopped"
#define POLL_MS 500

static struct {
  app *a;
  mc_analysis_scanner *scanner;
  guint poll;
  AdwDialog *dialog; /* while it's open */
  GtkWidget *bar, *status;
  gboolean remembered; /* SETTING_BACKGROUND is set */
  gboolean stopped;    /* SETTING_STOPPED is set */
  gboolean active;     /* it was at the last poll */
  int left;            /* the tracks waiting, as last counted: -1 to count them */
} g;

static void remember(app *a, gboolean background, gboolean stopped) {
  mc_settings *settings = NULL;
  if (mc_settings_load(a->settings_dir, &settings) == MC_OK) {
    mc_settings_set_bool(settings, SETTING_BACKGROUND, background);
    mc_settings_set_bool(settings, SETTING_STOPPED, stopped);
    mc_settings_save(settings); /* failing, it asks again at the next start */
  }
  mc_settings_free(settings);
  g.remembered = background;
  g.stopped = stopped;
}

static void show_progress(int done, int total) {
  if (!g.dialog) return;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g.bar), total ? (double)done / total : 0.0);
  char *text = mc_format_alloc(mc_str(MC_STR_ANALYSIS_PROGRESS), MC_D(done), MC_D(total));
  gtk_label_set_text(GTK_LABEL(g.status), text);
  mc_free(text);
}

static void on_response(AdwAlertDialog *dialog, const char *response, gpointer user) {
  (void)dialog;
  app *a = user;
  g.dialog = NULL;
  if (!g_strcmp0(response, "cancel")) {
    mc_analysis_scanner_stop(g.scanner); /* until Settings' Resume analysis */
    g.scanner = NULL;
    g.left = -1;
    gtk_widget_set_visible(a->analysing, FALSE);
    remember(a, FALSE, TRUE);
  } else {
    mc_analysis_scanner_set_paced(g.scanner, 1);
    remember(a, TRUE, FALSE);
  }
}

static void open_dialog(app *a) {
  if (g.dialog) return;
  AdwDialog *dialog = adw_alert_dialog_new(mc_str(MC_STR_ANALYSIS_TITLE), mc_str(MC_STR_ANALYSIS_TEXT));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  g.bar = gtk_progress_bar_new();
  gtk_box_append(GTK_BOX(box), g.bar);
  g.status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g.status), 0.0f);
  gtk_widget_add_css_class(g.status, "dim-label");
  gtk_box_append(GTK_BOX(box), g.status);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), box);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "background",
                                 mc_str_mnemonic(MC_STR_ANALYSIS_BACKGROUND), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "background");
  /* Closing it isn't Cancel: the work goes on, and Cancel is there to stop it. */
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "background");
  g_signal_connect(dialog, "response", G_CALLBACK(on_response), a);
  g.dialog = dialog;
  adw_dialog_present(dialog, GTK_WIDGET(a->win));
}

static gboolean on_poll(gpointer user) {
  (void)user;
  app *a = g.a;
  mc_analysis_status st;
  mc_analysis_scanner_status(g.scanner, &st);
  if (g.remembered && g.scanner && !st.paced) remember(a, FALSE, FALSE); /* it finished */
  if (g.active && !st.active) g.left = -1;
  g.active = st.active;
  /* After a track is done: a pass of files that are all missing is over in a moment. */
  if (st.active && st.large && !st.paced && st.done > 0) open_dialog(a);
  if (st.active) {
    show_progress(st.done, st.total);
  } else if (g.dialog) {
    AdwDialog *dialog = g.dialog;
    g.dialog = NULL;
    g_signal_handlers_disconnect_by_func(dialog, G_CALLBACK(on_response), a);
    adw_dialog_force_close(dialog);
  }
  gtk_widget_set_visible(a->analysing, st.active && st.paced);
  return G_SOURCE_CONTINUE;
}

void analysis_ui_start(app *a) {
  if (!a->lib || g.scanner) return;
  g.a = a;
  g.left = -1;
  mc_settings *settings = NULL;
  if (mc_settings_load(a->settings_dir, &settings) == MC_OK) {
    g.remembered = mc_settings_bool(settings, SETTING_BACKGROUND, 0);
    g.stopped = mc_settings_bool(settings, SETTING_STOPPED, 0);
  }
  mc_settings_free(settings);
  if (!g.stopped) mc_analysis_scanner_start_sized(mc_library_dir(a->lib), g.remembered, &g.scanner);
  if (!g.poll) g.poll = g_timeout_add(POLL_MS, on_poll, NULL);
}

void analysis_ui_stop(void) {
  mc_analysis_scanner_stop(g.scanner); /* it reads the files, which may be about to move */
  g.scanner = NULL;
  if (g.poll) g_source_remove(g.poll);
  g.poll = 0;
  if (g.dialog) {
    AdwDialog *dialog = g.dialog;
    g.dialog = NULL;
    g_signal_handlers_disconnect_by_func(dialog, G_CALLBACK(on_response), g.a);
    adw_dialog_force_close(dialog);
  }
  if (g.a && g.a->analysing) gtk_widget_set_visible(g.a->analysing, FALSE); /* it goes with the window */
}

gboolean analysis_ui_stopped(void) {
  return g.stopped;
}

int analysis_ui_left(gboolean count_again) {
  mc_analysis_status st;
  mc_analysis_scanner_status(g.scanner, &st);
  if (st.active) return st.total - st.done;
  if (count_again) g.left = -1;
  if (g.left < 0) {
    int64_t ms;
    if (!g.a || !g.a->lib || mc_analysis_pending(g.a->lib, &g.left, &ms) != MC_OK) g.left = 0;
  }
  return g.left;
}

void analysis_ui_resume(void) {
  app *a = g.a;
  if (!a || !a->lib || g.scanner) return;
  remember(a, TRUE, FALSE);
  mc_analysis_scanner_start_resumed(mc_library_dir(a->lib), &g.scanner);
}

void analysis_ui_present(app *a) {
  if (!g.scanner) return;
  open_dialog(a);
  mc_analysis_status st;
  mc_analysis_scanner_status(g.scanner, &st);
  show_progress(st.done, st.total);
}
