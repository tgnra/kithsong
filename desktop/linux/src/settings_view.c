#include "settings_view.h"

#include <string.h>

#include "musiccore/musiccore.h"
#include "playback.h"

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
  GtkWidget *lfm_status, *lfm_button, *lfm_switch; /* the Scrobbling group's */
  guint lfm_timer;
} settings_view;

static void update_sync(settings_view *s);

/* Signing in to Last.fm outlives the dialog: the user may close it while the browser is open.
 * One sign-in at a time, on a thread that gets the token, has the page opened, then polls
 * mc_scrobbler_auth_finish() every AUTH_POLL_MS until approved, cancelled or AUTH_LIMIT_MS. */
enum { AUTH_POLL_MS = 3000, AUTH_LIMIT_MS = 10 * 60 * 1000, STATUS_MS = 2000 };
static struct {
  GThread *thread;
  GMutex lock;
  GCond wake;
  gboolean cancel;        /* under lock */
  unsigned generation;    /* bumped by each stop, so a stopped thread's news is dropped */
  mc_status result;       /* of the last sign-in, shown until the next action */
  gboolean timed_out;     /* it wasn't approved in time */
  settings_view *shown;   /* the open dialog, or NULL */
} g_auth;

static void update_scrobble(settings_view *s);
static void stop_auth(void);

static void settings_view_free(gpointer data) {
  settings_view *s = data;
  if (s->lfm_timer) g_source_remove(s->lfm_timer);
  if (g_auth.shown == s) g_auth.shown = NULL;
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

/* Open Another Library's steps, while the folder picker is open. */
typedef struct choosing {
  GtkWidget *parent; /* referenced */
  char *settings_dir;
  settings_view_open_library_fn open_library;
  void *user;
  gboolean empty_only;
  settings_view_chosen_fn done;
  gpointer data;
  GDestroyNotify free_data;
} choosing;

static void choosing_free(choosing *c, const char *dir, gboolean saved) {
  if (c->done) c->done(c->data, dir, saved);
  if (c->free_data) c->free_data(c->data);
  g_object_unref(c->parent);
  g_free(c->settings_dir);
  g_free(c);
}

static void choose_folder(choosing *c);

/* Asks again after a folder that isn't empty, once the alert is dismissed. */
static void on_not_empty_closed(AdwAlertDialog *alert, const char *response, gpointer data) {
  (void)alert, (void)response;
  choose_folder(data);
}

static void on_folder_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  choosing *c = data;
  GFile *file = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, NULL);
  char *dir = file ? g_file_get_path(file) : NULL;
  if (file) g_object_unref(file);
  char *current = load_library_dir(c->settings_dir);
  gboolean switched = FALSE, saved = FALSE;

  if (dir && c->empty_only && !mc_folder_is_empty(dir)) {
    AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_LIBRARY_FOLDER), mc_str(MC_STR_LIBRARY_NOT_EMPTY));
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
    g_signal_connect(alert, "response", G_CALLBACK(on_not_empty_closed), c);
    adw_dialog_present(alert, c->parent);
    g_free(current);
    g_free(dir);
    return;
  }
  if (dir && g_strcmp0(dir, current) != 0 && c->open_library && c->open_library(c->user, dir)) {
    switched = TRUE;
    saved = save_library_dir(c->settings_dir, dir);
  }
  g_free(current);
  choosing_free(c, switched ? dir : NULL, saved);
  g_free(dir);
}

static void choose_folder(choosing *c) {
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, mc_str(c->empty_only ? MC_STR_LIBRARY_FOLDER_NEW : MC_STR_LIBRARY_FOLDER_CHOOSE));
  char *current = load_library_dir(c->settings_dir);
  if (current) {
    GFile *folder = g_file_new_for_path(current);
    gtk_file_dialog_set_initial_folder(chooser, folder);
    g_object_unref(folder);
    g_free(current);
  }
  GtkRoot *root = gtk_widget_get_root(c->parent);
  gtk_file_dialog_select_folder(chooser, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, on_folder_chosen, c);
  g_object_unref(chooser);
}

void settings_view_choose_library(GtkWidget *parent, const char *settings_dir,
                                  settings_view_open_library_fn open_library, void *user, gboolean empty_only,
                                  settings_view_chosen_fn done, gpointer data, GDestroyNotify free_data) {
  choosing *c = g_new0(choosing, 1);
  c->parent = g_object_ref(parent);
  c->settings_dir = g_strdup(settings_dir);
  c->open_library = open_library;
  c->user = user;
  c->empty_only = empty_only;
  c->done = done;
  c->data = data;
  c->free_data = free_data;
  choose_folder(c);
}

/* Open Another Library from the dialog: data is the dialog, referenced while choosing. */
static void on_library_chosen(gpointer data, const char *dir, gboolean saved) {
  GtkWidget *dialog = data;
  settings_view *s = g_object_get_data(G_OBJECT(dialog), "settings-view");
  if (!dir || !s) return;
  if (!saved)
    adw_preferences_dialog_add_toast(ADW_PREFERENCES_DIALOG(dialog), adw_toast_new(mc_str(MC_STR_SETTINGS_SAVE_FAILED)));
  /* Shown even if it wasn't saved: it's the library open now. */
  adw_action_row_set_subtitle(ADW_ACTION_ROW(s->folder), dir);
  update_sync(s); /* sync restarted with it */
}

static void on_open_activated(AdwActionRow *row, gpointer data) {
  (void)row;
  settings_view *s = data;
  settings_view_choose_library(s->dialog, s->settings_dir, s->open_library, s->user, FALSE, on_library_chosen,
                               g_object_ref(s->dialog), g_object_unref);
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

/* --- Scrobbling --- */

typedef struct auth_news {
  unsigned generation;
  char *url;    /* the page to open (mc_free it), or NULL when it's the result */
  mc_status st;
  gboolean timed_out;
} auth_news;

static void on_uri_launched(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  gtk_uri_launcher_launch_finish(GTK_URI_LAUNCHER(source), result, NULL);
}

/* On the main thread: the auth thread's page to open, or its result. */
static gboolean auth_news_arrived(gpointer data) {
  auth_news *n = data;
  if (n->generation == g_auth.generation) {
    if (n->url) {
      GtkUriLauncher *launcher = gtk_uri_launcher_new(n->url);
      GtkRoot *root = g_auth.shown ? gtk_widget_get_root(g_auth.shown->dialog) : NULL;
      gtk_uri_launcher_launch(launcher, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL, on_uri_launched, NULL);
      g_object_unref(launcher);
    } else {
      g_thread_join(g_auth.thread); /* it's just sent this */
      g_auth.thread = NULL;
      g_auth.result = n->st;
      g_auth.timed_out = n->timed_out;
      if (g_auth.shown) update_scrobble(g_auth.shown);
    }
  }
  mc_free(n->url);
  g_free(n);
  return G_SOURCE_REMOVE;
}

static void send_news(unsigned generation, char *url, mc_status st, gboolean timed_out) {
  auth_news *n = g_new0(auth_news, 1);
  *n = (auth_news){generation, url, st, timed_out};
  g_idle_add(auth_news_arrived, n);
}

static gpointer auth_main(gpointer data) {
  mc_scrobbler *sc = data;
  g_mutex_lock(&g_auth.lock);
  unsigned generation = g_auth.generation;
  g_mutex_unlock(&g_auth.lock);
  char *url = NULL;
  mc_status st = mc_scrobbler_auth_begin(sc, &url);
  gboolean polled = FALSE;
  if (st == MC_OK) {
    polled = TRUE;
    send_news(generation, url, MC_OK, FALSE);
    gint64 end = g_get_monotonic_time() + (gint64)AUTH_LIMIT_MS * 1000;
    do {
      g_mutex_lock(&g_auth.lock);
      gint64 until = g_get_monotonic_time() + (gint64)AUTH_POLL_MS * 1000;
      while (!g_auth.cancel && g_cond_wait_until(&g_auth.wake, &g_auth.lock, until)) {
      }
      gboolean cancel = g_auth.cancel;
      g_mutex_unlock(&g_auth.lock);
      if (cancel) return NULL; /* stop_auth() joins it */
      st = mc_scrobbler_auth_finish(sc);
    } while (st == MC_ERR_AUTH && g_get_monotonic_time() < end);
  }
  send_news(generation, NULL, st, polled && st == MC_ERR_AUTH);
  return NULL;
}

static void start_auth(settings_view *s) {
  mc_scrobbler *sc = playback_scrobbler();
  if (!sc || g_auth.thread) return;
  g_auth.cancel = FALSE;
  g_auth.result = MC_OK;
  g_auth.timed_out = FALSE;
  g_auth.thread = g_thread_new("scrobble-auth", auth_main, sc);
  update_scrobble(s);
}

/* Cancels a sign-in in progress and waits for its thread, which may be in a call to the
 * service (cut short by mc_scrobbler_auth_cancel()). Also called just before the scrobbler
 * is freed. */
static void stop_auth(void) {
  if (!g_auth.thread) return;
  g_mutex_lock(&g_auth.lock);
  g_auth.cancel = TRUE;
  g_auth.generation++;
  g_cond_broadcast(&g_auth.wake);
  g_mutex_unlock(&g_auth.lock);
  mc_scrobbler_auth_cancel(playback_scrobbler());
  g_thread_join(g_auth.thread);
  g_auth.thread = NULL;
  if (g_auth.shown) update_scrobble(g_auth.shown);
}

/* Brings the Scrobbling group up to date: signed in or not, whether the build can sign in at
 * all, plays waiting and the last error. */
static void update_scrobble(settings_view *s) {
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st = {0};
  gboolean have = sc && mc_scrobbler_status_get(sc, &st) == MC_OK;
  GString *text = g_string_new(NULL);
  enum mc_str_id button = MC_STR_SCROBBLE_SIGN_IN;
  gboolean enabled = have;
  char line[256];

  if (g_auth.thread) {
    g_string_append(text, mc_str(MC_STR_SCROBBLE_WAITING));
    button = MC_STR_SCROBBLE_CANCEL_SIGN_IN;
    enabled = TRUE;
  } else if (have && st.username) {
    mc_format(line, sizeof line, mc_str(MC_STR_SCROBBLE_SIGNED_IN), MC_S(st.username));
    g_string_append(text, line);
    button = MC_STR_SCROBBLE_SIGN_OUT;
    line[0] = '\0';
    if (st.last_error == MC_ERR_AUTH) g_strlcpy(line, mc_str(MC_STR_SCROBBLE_REFUSED), sizeof line);
    else if (st.last_error == MC_ERR_NET) g_strlcpy(line, mc_str(MC_STR_SCROBBLE_OFFLINE), sizeof line);
    else if (st.pending > 0) mc_format(line, sizeof line, mc_str_plural(MC_STR_SCROBBLE_PENDING, st.pending), MC_D(st.pending));
    if (line[0]) g_string_append_printf(text, "\n%s", line);
  } else if (have && !st.has_key) {
    g_string_append(text, mc_str(MC_STR_SCROBBLE_NO_KEY));
    enabled = FALSE;
  } else {
    g_string_append(text, mc_str(MC_STR_SCROBBLE_SIGNED_OUT));
  }
  if (!g_auth.thread && g_auth.result != MC_OK) {
    if (g_auth.timed_out)
      g_strlcpy(line, mc_str(MC_STR_SCROBBLE_SIGN_IN_TIMED_OUT), sizeof line);
    else
      mc_format(line, sizeof line, mc_str(MC_STR_SCROBBLE_SIGN_IN_FAILED), MC_S(mc_status_str(g_auth.result)));
    g_string_append_printf(text, "\n%s", line);
  }
  if (have) mc_scrobbler_status_clear(&st);

  adw_action_row_set_subtitle(ADW_ACTION_ROW(s->lfm_status), text->str);
  g_string_free(text, TRUE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->lfm_button), mc_str(button));
  gtk_widget_set_sensitive(s->lfm_button, enabled);
}

static gboolean on_scrobble_tick(gpointer data) {
  update_scrobble(data);
  return G_SOURCE_CONTINUE;
}

static void on_sign_out_response(AdwAlertDialog *alert, const char *response, gpointer data) {
  (void)alert;
  settings_view *s = data;
  mc_scrobbler *sc = playback_scrobbler();
  if (strcmp(response, "sign-out") || !sc) return;
  g_auth.result = mc_scrobbler_sign_out(sc);
  g_auth.timed_out = FALSE;
  update_scrobble(s);
}

static void on_scrobble_button(GtkWidget *row, gpointer data) {
  (void)row;
  settings_view *s = data;
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st;
  if (g_auth.thread) {
    stop_auth();
    return;
  }
  if (!sc || mc_scrobbler_status_get(sc, &st) != MC_OK) return;
  gboolean signed_in = st.username != NULL;
  mc_scrobbler_status_clear(&st);
  if (!signed_in) {
    start_auth(s);
    return;
  }
  AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_SCROBBLE_SIGN_OUT_TITLE), mc_str(MC_STR_SCROBBLE_SIGN_OUT_TEXT));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "sign-out",
                                 mc_str_mnemonic(MC_STR_SCROBBLE_SIGN_OUT), NULL);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  g_signal_connect(alert, "response", G_CALLBACK(on_sign_out_response), s);
  adw_dialog_present(alert, s->dialog);
}

static void on_scrobble_switch(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec;
  settings_view *s = data;
  gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  playback_set_scrobble(on);
  mc_settings *settings = NULL;
  if (mc_settings_load(s->settings_dir, &settings) == MC_OK) {
    if (mc_settings_set_scrobble(settings, on) != MC_OK || mc_settings_save(settings) != MC_OK)
      toast(s, mc_str(MC_STR_SETTINGS_SAVE_FAILED));
    mc_settings_free(settings);
  }
}

static GtkWidget *build_scrobble_group(settings_view *s) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_SCROBBLING));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_SCROBBLE_NOTE));

  s->lfm_status = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(s->lfm_status), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->lfm_status), mc_str(MC_STR_SCROBBLE_SERVICE));
  gtk_widget_add_css_class(s->lfm_status, "property");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->lfm_status);

#if ADW_CHECK_VERSION(1, 6, 0)
  s->lfm_button = adw_button_row_new();
#else
  s->lfm_button = nav_row(mc_str(MC_STR_SCROBBLE_SIGN_IN), NULL, FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(s->lfm_button), TRUE);
#endif
  g_signal_connect(s->lfm_button, "activated", G_CALLBACK(on_scrobble_button), s);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->lfm_button);

  s->lfm_switch = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(s->lfm_switch), mc_str(MC_STR_SCROBBLE_SWITCH));
  mc_settings *settings = NULL;
  gboolean on = TRUE;
  if (mc_settings_load(s->settings_dir, &settings) == MC_OK) on = mc_settings_scrobble(settings);
  mc_settings_free(settings);
  adw_switch_row_set_active(ADW_SWITCH_ROW(s->lfm_switch), on);
  g_signal_connect(s->lfm_switch, "notify::active", G_CALLBACK(on_scrobble_switch), s);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), s->lfm_switch);

  g_auth.shown = s;
  playback_on_scrobbler_closing(stop_auth);
  update_scrobble(s);
  s->lfm_timer = g_timeout_add(STATUS_MS, on_scrobble_tick, s);
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
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(build_scrobble_group(s)));

  s->dialog = GTK_WIDGET(adw_preferences_dialog_new());
  adw_dialog_set_title(ADW_DIALOG(s->dialog), mc_str(MC_STR_SETTINGS));
  adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(s->dialog), ADW_PREFERENCES_PAGE(page));
  g_object_set_data_full(G_OBJECT(s->dialog), "settings-view", s, settings_view_free);
  update_sync(s);
  adw_dialog_present(ADW_DIALOG(s->dialog), parent);
}
