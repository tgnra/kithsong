#include "move_library.h"

#include <stdlib.h>

#include "musiccore/musiccore.h"

static struct {
  GtkWindow *main, *win;
  GtkWidget *bar, *status, *file;
  const move_library_host *host;
  char *settings_dir;
  char *src, *dst;
  mc_settings *settings; /* the worker's copy, saved by the move */
  GThread *thread;
  guint inhibit; /* logging out, while the move runs */
  int delay_ms;  /* debug builds: KITHSONG_TEST_MOVE_DELAY_MS */
  gboolean running;
  void (*after_alert)(void); /* what the shown alert's Close leads to, or NULL */

  /* Shared with the worker, under lock. */
  GMutex lock;
  mc_library_move_progress p;
  char current[1024]; /* p.current, copied */
  gboolean posted;    /* a show_progress idle is queued */
  mc_status st;
  mc_library_move_result result;
} g_move;

static void reset(void) {
  g_clear_pointer(&g_move.settings_dir, g_free);
  g_clear_pointer(&g_move.src, g_free);
  g_clear_pointer(&g_move.dst, g_free);
  g_move.running = FALSE;
}

/* "12.5 MB", in the strings' units as Windows writes them. */
static char *fmt_size(int64_t bytes) {
  static const enum mc_str_id units[] = {MC_STR_COUNT, MC_STR_SIZE_KB, MC_STR_SIZE_MB, MC_STR_SIZE_GB,
                                         MC_STR_SIZE_TB};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 4) v /= 1024.0, u++;
  if (u == 0) return mc_format_alloc(mc_str_plural(MC_STR_SIZE_BYTES, (long)bytes), MC_D(bytes));
  char num[32];
  g_snprintf(num, sizeof num, v < 100.0 ? "%.1f" : "%.0f", v);
  return mc_format_alloc(mc_str(units[u]), MC_S(num));
}

static void alert(GtkWindow *parent, const char *heading, const char *body, void (*then)(void));

/* --- Choosing the folder --- */

static void choose(void);

/* Why dst won't take the library, as a message to mc_free(); NULL if it will. */
static char *problem_text(mc_status st, const mc_library_move_info *info, const char *dst) {
  if (st != MC_OK) return mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_CHECK_FAILED), MC_S(mc_status_str(st)));
  switch (info->problem) {
    case MC_MOVE_OK: return NULL;
    case MC_MOVE_SAME_FOLDER: return mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_SAME_FOLDER), MC_S(dst));
    case MC_MOVE_INSIDE_LIBRARY: return mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_INSIDE), MC_S(dst));
    case MC_MOVE_CONTAINS_LIBRARY: return mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_CONTAINS), MC_S(dst));
    case MC_MOVE_NOT_EMPTY: return mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_NOT_EMPTY), MC_S(dst));
    case MC_MOVE_NOT_WRITABLE: return mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_NOT_WRITABLE), MC_S(dst));
    case MC_MOVE_NO_SPACE: {
      char *needed = fmt_size(info->needed_bytes), *avail = fmt_size(info->free_bytes);
      char *text = mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_NO_SPACE), MC_S(needed), MC_S(avail));
      mc_free(needed);
      mc_free(avail);
      return text;
    }
  }
  return NULL;
}

/* Debug builds: KITHSONG_TEST_MOVE_TO in place of the picker. */
static const char *test_hook(void) {
#ifdef NDEBUG
  return NULL;
#else
  const char *to = g_getenv("KITHSONG_TEST_MOVE_TO");
  const char *delay = g_getenv("KITHSONG_TEST_MOVE_DELAY_MS");
  g_move.delay_ms = delay ? atoi(delay) : 0;
  return to && *to ? to : NULL;
#endif
}

static void begin(void);

/* Checks the chosen folder: starts the move, or says why not and asks again. */
static void chosen(char *dst) {
  mc_library_move_info info;
  mc_status st = mc_library_move_check(g_move.src, dst, &info);
  char *text = problem_text(st, &info, dst);
  if (!text) {
    g_move.dst = dst;
    begin();
    return;
  }
  /* The hook's folder won't change; nor will an unreadable library. */
  gboolean again = !test_hook() && st == MC_OK;
  alert(g_move.main, mc_str(MC_STR_LIBRARY_MOVE_CANT_TITLE), text, again ? choose : reset);
  mc_free(text);
  g_free(dst);
}

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
  char *dir = file ? g_file_get_path(file) : NULL;
  if (file) g_object_unref(file);
  if (dir) chosen(dir);
  else reset(); /* cancelled */
}

static void choose(void) {
  const char *hook = test_hook();
  if (hook) {
    chosen(g_strdup(hook));
    return;
  }
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, mc_str(MC_STR_LIBRARY_MOVE_CHOOSE));
  char *parent = g_path_get_dirname(g_move.src);
  GFile *folder = g_file_new_for_path(parent);
  gtk_file_dialog_set_initial_folder(chooser, folder);
  g_object_unref(folder);
  g_free(parent);
  gtk_file_dialog_select_folder(chooser, g_move.main, NULL, on_folder_chosen, NULL);
  g_object_unref(chooser);
}

/* --- The worker --- */

static gboolean show_progress(gpointer data);

/* On the worker thread: keeps the latest progress, and queues an update if none is. */
static void on_progress(void *user, const mc_library_move_progress *p) {
  (void)user;
  g_mutex_lock(&g_move.lock);
  g_move.p = *p;
  g_snprintf(g_move.current, sizeof g_move.current, "%s", p->current ? p->current : "");
  gboolean post = !g_move.posted;
  g_move.posted = TRUE;
  g_mutex_unlock(&g_move.lock);
  if (post) g_idle_add(show_progress, NULL);
  if (g_move.delay_ms > 0 && p->current) g_usleep((gulong)g_move.delay_ms * 1000);
}

static gboolean finish(gpointer data);

static gpointer worker(gpointer data) {
  (void)data;
  mc_library_move_result result;
  mc_status st = mc_library_move(g_move.src, g_move.dst, g_move.settings, on_progress, NULL, &result);
  g_mutex_lock(&g_move.lock);
  g_move.st = st;
  g_move.result = result;
  g_mutex_unlock(&g_move.lock);
  g_idle_add(finish, NULL);
  return NULL;
}

/* --- The progress window --- */

static gboolean show_progress(gpointer data) {
  (void)data;
  g_mutex_lock(&g_move.lock);
  mc_library_move_progress p = g_move.p;
  char *file = g_strdup(g_move.current);
  g_move.posted = FALSE;
  g_mutex_unlock(&g_move.lock);
  if (!g_move.win) {
    g_free(file);
    return G_SOURCE_REMOVE;
  }

  char *counts = NULL;
  const char *status = "";
  switch (p.phase) {
    case MC_MOVE_PHASE_COPYING: {
      char *done = fmt_size(p.bytes_done), *total = fmt_size(p.bytes_total);
      status = counts = mc_format_alloc(mc_str_plural(MC_STR_LIBRARY_MOVE_PROGRESS, p.files_total),
                                        MC_D(p.files_done), MC_D(p.files_total), MC_S(done), MC_S(total));
      mc_free(done);
      mc_free(total);
      break;
    }
    case MC_MOVE_PHASE_CHECKING: status = mc_str(MC_STR_LIBRARY_MOVE_CHECKING); break;
    case MC_MOVE_PHASE_REMOVING: status = mc_str(MC_STR_LIBRARY_MOVE_REMOVING); break;
    case MC_MOVE_PHASE_UNDOING: status = mc_str(MC_STR_LIBRARY_MOVE_UNDOING); break;
  }
  gtk_label_set_text(GTK_LABEL(g_move.status), status ? status : "");
  gtk_label_set_text(GTK_LABEL(g_move.file), p.phase == MC_MOVE_PHASE_COPYING ? file : "");
  double fraction = p.bytes_total > 0 ? (double)p.bytes_done / (double)p.bytes_total
                    : p.files_total > 0 ? (double)p.files_done / p.files_total : 0;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_move.bar), p.phase == MC_MOVE_PHASE_COPYING ? fraction : 1.0);
  mc_free(counts);
  g_free(file);
  return G_SOURCE_REMOVE;
}

/* Alt+F4 and the like: not until it's done. */
static gboolean on_close_request(GtkWindow *win, gpointer data) {
  (void)win;
  (void)data;
  return g_move.running;
}

static GtkWidget *label(const char *text, const char *css) {
  GtkWidget *l = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  if (css) gtk_widget_add_css_class(l, css);
  return l;
}

static void create_window(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 24);

  char *to = mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_TO), MC_S(g_move.dst));
  GtkWidget *heading = label(to, "title-4");
  mc_free(to);
  gtk_label_set_wrap(GTK_LABEL(heading), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(heading), PANGO_WRAP_WORD_CHAR);
  gtk_widget_set_margin_bottom(heading, 12);
  gtk_box_append(GTK_BOX(box), heading);

  g_move.bar = gtk_progress_bar_new();
  gtk_widget_set_margin_bottom(g_move.bar, 6);
  gtk_box_append(GTK_BOX(box), g_move.bar);
  g_move.status = label("", NULL);
  gtk_box_append(GTK_BOX(box), g_move.status);
  g_move.file = label("", "dim-label");
  gtk_label_set_ellipsize(GTK_LABEL(g_move.file), PANGO_ELLIPSIZE_MIDDLE);
  gtk_box_append(GTK_BOX(box), g_move.file);
  GtkWidget *note = label(mc_str(MC_STR_LIBRARY_MOVE_WAIT), "dim-label");
  gtk_label_set_wrap(GTK_LABEL(note), TRUE);
  gtk_widget_set_margin_top(note, 12);
  gtk_box_append(GTK_BOX(box), note);

  /* No close button: the header bar shows the title only. */
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);

  GtkWidget *win = adw_window_new();
  g_move.win = GTK_WINDOW(win);
  gtk_window_set_application(g_move.win, gtk_window_get_application(g_move.main));
  gtk_window_set_title(g_move.win, mc_str(MC_STR_LIBRARY_MOVE_TITLE));
  gtk_window_set_deletable(g_move.win, FALSE);
  gtk_window_set_resizable(g_move.win, FALSE);
  gtk_window_set_modal(g_move.win, TRUE);
  gtk_window_set_default_size(g_move.win, 520, -1);
  adw_window_set_content(ADW_WINDOW(win), toolbar);
  g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), NULL);
  show_progress(NULL);
  gtk_window_present(g_move.win);
}

/* --- Starting and finishing --- */

static void begin(void) {
  g_move.settings = NULL;
  if (mc_settings_load(g_move.settings_dir, &g_move.settings) != MC_OK) {
    char *text = mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_FAILED), MC_S(mc_status_str(MC_ERR_IO)),
                                 MC_S(g_move.src));
    alert(g_move.main, mc_str(MC_STR_LIBRARY_MOVE_TITLE), text, reset);
    mc_free(text);
    return;
  }
  memset(&g_move.p, 0, sizeof g_move.p);
  g_move.current[0] = '\0';
  g_move.posted = FALSE;

  g_move.host->close_library(g_move.host->user);
  gtk_widget_set_visible(GTK_WIDGET(g_move.main), FALSE);
  create_window();
  GtkApplication *app = gtk_window_get_application(g_move.main);
  if (app)
    g_move.inhibit = gtk_application_inhibit(app, g_move.win,
                                             GTK_APPLICATION_INHIBIT_LOGOUT | GTK_APPLICATION_INHIBIT_SUSPEND,
                                             mc_str(MC_STR_LIBRARY_MOVE_TITLE));
  g_move.thread = g_thread_new("library-move", worker, NULL);
}

/* The worker is done: the library opens where it is now, and the main window comes back. */
static gboolean finish(gpointer data) {
  (void)data;
  g_thread_join(g_move.thread);
  g_move.thread = NULL;
  mc_status st = g_move.st;
  mc_library_move_result result = g_move.result;
  mc_settings_free(g_move.settings);
  g_move.settings = NULL;

  GtkApplication *app = gtk_window_get_application(g_move.main);
  if (app && g_move.inhibit) gtk_application_uninhibit(app, g_move.inhibit);
  g_move.inhibit = 0;
  g_move.running = FALSE;
  gtk_window_destroy(g_move.win);
  g_move.win = NULL;

  g_move.host->reopen_library(g_move.host->user, st == MC_OK ? g_move.dst : g_move.src);

  char *text = NULL;
  if (st != MC_OK && result.failed_path)
    text = mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_FAILED_FILE), MC_S(result.failed_path),
                           MC_S(mc_status_str(st)), MC_S(g_move.src));
  else if (st != MC_OK)
    text = mc_format_alloc(mc_str(MC_STR_LIBRARY_MOVE_FAILED), MC_S(mc_status_str(st)), MC_S(g_move.src));
  else if (result.left_behind)
    text = mc_format_alloc(mc_str_plural(MC_STR_LIBRARY_MOVE_LEFT_BEHIND, result.left_behind),
                           MC_D(result.left_behind), MC_S(g_move.src));
  if (text) alert(g_move.main, mc_str(MC_STR_LIBRARY_MOVE_TITLE), text, NULL);
  mc_free(text);
  mc_library_move_result_free(&result);
  reset();
  return G_SOURCE_REMOVE;
}

static void on_alert_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog;
  (void)response;
  (void)data;
  void (*then)(void) = g_move.after_alert;
  g_move.after_alert = NULL;
  if (then) then();
}

/* A message over parent with a Close button; then (may be NULL) runs once it's closed. */
static void alert(GtkWindow *parent, const char *heading, const char *body, void (*then)(void)) {
  AdwDialog *dialog = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", mc_str_mnemonic(MC_STR_CLOSE));
  g_move.after_alert = then;
  g_signal_connect(dialog, "response", G_CALLBACK(on_alert_response), NULL);
  adw_dialog_present(dialog, GTK_WIDGET(parent));
}

void move_library_start(GtkWindow *win, const char *settings_dir, const move_library_host *host) {
  if (g_move.running || g_move.thread) return;
  mc_settings *settings = NULL;
  if (mc_settings_load(settings_dir, &settings) != MC_OK) return;
  const char *src = mc_settings_library_dir(settings);
  if (src) {
    g_move.main = win;
    g_move.host = host;
    g_move.settings_dir = g_strdup(settings_dir);
    g_move.src = g_strdup(src);
    g_move.running = TRUE;
  }
  mc_settings_free(settings);
  if (src) choose();
}
