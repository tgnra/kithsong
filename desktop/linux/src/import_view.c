#include "import_view.h"

#include <stdio.h>
#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "import_view.c needs libadwaita 1.5 or later (AdwDialog, AdwAlertDialog)"
#endif

/* Groups of the item list, in display order. */
enum { GROUP_REVIEW, GROUP_ACTIVE, GROUP_DONE, GROUP_SKIPPED, GROUP_FAILED, NUM_GROUPS };

static const enum mc_str_id group_titles[NUM_GROUPS] = {MC_STR_IMPORT_NEEDS_DECISION, MC_STR_IMPORT_GROUP_ACTIVE,
                                                        MC_STR_IMPORT_STATE_DONE, MC_STR_IMPORT_STATE_SKIPPED,
                                                        MC_STR_IMPORT_STATE_FAILED};

typedef struct import_view import_view;

/* The import callback's user pointer: which view, and which of its sessions. Freed once the
 * session's thread has stopped. */
typedef struct session_tag {
  import_view *v;
  unsigned gen;
} session_tag;

/* One item's row in the list. */
typedef struct item_row {
  GtkWidget *row, *state, *spinner, *confidence;
  int group;
} item_row;

typedef struct review {
  import_view *v; /* holds a reference */
  int item;
  mc_import_item *snap;
  gboolean terms_set;
  int selected; /* match whose tracks are shown; -1 if none */

  AdwDialog *dialog;
  GtkWidget *heading, *folder, *note;
  GtkWidget *artist, *title, *search;
  GtkWidget *matches_group, *matches, *tracks_group, *tracks;
  GtkWidget *use, *as_is, *skip;
} review;

/* Reference counted (atomically: the import thread takes references), so events still queued
 * on the main loop when the widget goes away find root == NULL instead of freed memory. */
struct import_view {
  GtkWidget *root; /* the GtkStack; NULL once destroyed */
  import_view_callbacks cb;
  mc_library *lib;

  mc_import *imp;
  session_tag *tag;
  unsigned generation;
  gboolean idle;
  int scanned_files;
  int item_count;
  mc_import_state *states;
  item_row *rows;
  int group_count[NUM_GROUPS];

  GtkWidget *status, *progress;
  GtkWidget *banner; /* "You're offline", with Import Without Tagging */
  gboolean offline, skipped_tags;
  GtkWidget *groups[NUM_GROUPS], *lists[NUM_GROUPS];

  review *rev; /* open review dialog, if any */
};

static import_view *get_view(GtkWidget *widget) {
  return widget ? g_object_get_data(G_OBJECT(widget), "import-view") : NULL;
}

/* --- Helpers --- */

static const char *file_name(const char *path) {
  const char *sep = path ? strrchr(path, '/') : NULL;
  return sep ? sep + 1 : path ? path : "";
}

static void format_length(int ms, char *out, size_t n) {
  if (ms <= 0) {
    out[0] = '\0';
    return;
  }
  int s = (ms + 500) / 1000;
  snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static const char *state_label(mc_import_state s) {
  switch (s) {
    case MC_IMPORT_PENDING: return mc_str(MC_STR_IMPORT_STATE_WAITING);
    case MC_IMPORT_MATCHING: return mc_str(MC_STR_IMPORT_STATE_MATCHING);
    case MC_IMPORT_REVIEW: return mc_str(MC_STR_IMPORT_NEEDS_DECISION);
    case MC_IMPORT_WRITING: return mc_str(MC_STR_IMPORT_STATE_WRITING);
    case MC_IMPORT_DONE: return mc_str(MC_STR_IMPORT_STATE_DONE);
    case MC_IMPORT_SKIPPED: return mc_str(MC_STR_IMPORT_STATE_SKIPPED);
    case MC_IMPORT_FAILED: return mc_str(MC_STR_IMPORT_STATE_FAILED);
  }
  return "";
}

static int state_group(mc_import_state s) {
  switch (s) {
    case MC_IMPORT_REVIEW: return GROUP_REVIEW;
    case MC_IMPORT_DONE: return GROUP_DONE;
    case MC_IMPORT_SKIPPED: return GROUP_SKIPPED;
    case MC_IMPORT_FAILED: return GROUP_FAILED;
    default: return GROUP_ACTIVE;
  }
}

static gboolean state_busy(mc_import_state s) {
  return s == MC_IMPORT_PENDING || s == MC_IMPORT_MATCHING || s == MC_IMPORT_WRITING;
}

/* "Radiohead – OK Computer (1997)", or for a single "OutKast – Hey Ya! (2003)". */
static void match_summary(const mc_import_match *m, char *out, size_t n) {
  const char *artist = m->album_artist ? m->album_artist : mc_str(MC_STR_UNKNOWN_VALUE);
  const char *title = m->is_single && m->track_count ? m->tracks[0].title : m->album;
  if (!title) title = mc_str(MC_STR_UNKNOWN_VALUE);
  if (m->year) mc_format(out, n, mc_str(MC_STR_IMPORT_MATCH_YEAR), MC_S(artist), MC_S(title), MC_D(m->year));
  else mc_format(out, n, mc_str(MC_STR_TRACK_BYLINE), MC_S(artist), MC_S(title));
}

/* Appends part to a " · "-separated list in out. */
static void join(char *out, size_t n, const char *part) {
  if (!part || !*part) return;
  size_t len = strlen(out);
  if (len + 1 >= n) return;
  snprintf(out + len, n - len, "%s%s", len ? mc_str(MC_STR_SEPARATOR_DOT) : "", part);
}

/* A count's plural string with the count in it, e.g. "3 imported". */
static void count_text(enum mc_str_id id, int count, char *out, size_t n) {
  mc_format(out, n, mc_str_plural(id, count), MC_D(count));
}

static void alert(GtkWidget *parent, const char *heading, const char *body) {
  AdwDialog *dlg = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dlg), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(dlg, parent);
}

/* An AdwActionRow whose title and subtitle are plain text, not markup. */
static GtkWidget *plain_row(const char *title, const char *subtitle) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title ? title : "");
  if (subtitle) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  return row;
}

static GtkWidget *dim_label(const char *text) {
  GtkWidget *label = gtk_label_new(text);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_add_css_class(label, "numeric");
  return label;
}

/* --- Session --- */

static void update_status(import_view *v);
static void on_event(import_view *v, mc_import_event ev, int arg);
static void review_load(review *r);

typedef struct event {
  import_view *v;
  unsigned gen;
  mc_import_event ev;
  int arg;
} event;

static gboolean deliver_event(gpointer data) {
  event *e = data;
  import_view *v = e->v;
  if (v->root && v->imp && e->gen == v->generation) on_event(v, e->ev, e->arg);
  g_atomic_rc_box_release(v);
  g_free(e);
  return G_SOURCE_REMOVE;
}

static void on_import_event(void *user, mc_import_event ev, int arg) {
  /* Import thread: hand the event to the main loop. The view outlives the session's thread,
   * so taking a reference here is safe. */
  const session_tag *tag = user;
  event *e = g_new(event, 1);
  e->v = g_atomic_rc_box_acquire(tag->v);
  e->gen = tag->gen;
  e->ev = ev;
  e->arg = arg;
  g_idle_add(deliver_event, e);
}

/* The offline banner shows while MusicBrainz can't be reached, until its button is used. */
static void update_banner(import_view *v) {
  if (v->root) adw_banner_set_revealed(ADW_BANNER(v->banner), v->imp && v->offline && !v->skipped_tags);
}

static void on_skip_tagging(AdwBanner *banner, gpointer data) {
  (void)banner;
  import_view *v = data;
  if (!v->imp || mc_import_skip_tagging(v->imp) != MC_OK) return;
  v->skipped_tags = TRUE;
  update_banner(v);
}

/* Ends the session. While the widget is being torn down (root is NULL) only the core's side is
 * freed; the widgets are going anyway. */
static void close_session(import_view *v) {
  if (!v->imp) return;
  if (v->rev && v->root) adw_dialog_force_close(v->rev->dialog);
  mc_import_free(v->imp); /* waits for the import thread */
  v->imp = NULL;
  g_free(v->tag);
  v->tag = NULL;
  v->generation++; /* events still queued are ignored */

  for (int g = 0; g < NUM_GROUPS; g++) {
    if (v->root) {
      gtk_list_box_remove_all(GTK_LIST_BOX(v->lists[g]));
      gtk_widget_set_visible(v->groups[g], FALSE);
    }
    v->group_count[g] = 0;
  }
  g_free(v->rows);
  v->rows = NULL;
  g_free(v->states);
  v->states = NULL;
  v->item_count = 0;
  v->scanned_files = 0;
  v->offline = FALSE;
  v->skipped_tags = FALSE;
  update_banner(v);

  if (v->root) gtk_stack_set_visible_child_name(GTK_STACK(v->root), "start");
  if (v->cb.session_changed) v->cb.session_changed(v->cb.user, FALSE);
}

static void update_status(import_view *v) {
  char text[512], progress[128], imported[128], other[128];
  int counts[MC_IMPORT_FAILED + 1] = {0};
  for (int i = 0; v->states && i < v->item_count; i++) counts[v->states[i]]++;
  int settled = counts[MC_IMPORT_REVIEW] + counts[MC_IMPORT_DONE] + counts[MC_IMPORT_SKIPPED] +
                counts[MC_IMPORT_FAILED];

  count_text(MC_STR_IMPORT_IMPORTED, counts[MC_IMPORT_DONE], imported, sizeof imported);
  if (!v->states) {
    count_text(MC_STR_IMPORT_SCANNING_FILES, v->scanned_files, text, sizeof text);
  } else if (v->item_count == 0) {
    g_strlcpy(text, mc_str(MC_STR_IMPORT_NO_MUSIC), sizeof text);
  } else if (!v->idle) {
    /* "Importing 3 of 40   2 imported, 1 needs a decision" */
    mc_format(progress, sizeof progress, mc_str(MC_STR_IMPORT_MATCHING),
              MC_D(settled < v->item_count ? settled + 1 : settled), MC_D(v->item_count));
    count_text(MC_STR_IMPORT_NEED_DECISION, counts[MC_IMPORT_REVIEW], other, sizeof other);
    snprintf(text, sizeof text, "%s   %s%s%s", progress, imported, mc_str(MC_STR_IMPORT_LIST_SEPARATOR), other);
  } else if (counts[MC_IMPORT_REVIEW]) {
    count_text(MC_STR_IMPORT_NEED_DECISION, counts[MC_IMPORT_REVIEW], other, sizeof other);
    mc_format(text, sizeof text, mc_str(MC_STR_IMPORT_STATUS_REVIEW), MC_S(imported), MC_S(other));
  } else {
    count_text(MC_STR_IMPORT_SKIPPED, counts[MC_IMPORT_SKIPPED], progress, sizeof progress);
    count_text(MC_STR_IMPORT_FAILED, counts[MC_IMPORT_FAILED], other, sizeof other);
    mc_format(text, sizeof text, mc_str(MC_STR_IMPORT_STATUS_FINISHED), MC_S(imported), MC_S(progress),
              MC_S(other));
  }
  gtk_label_set_text(GTK_LABEL(v->status), text);

  if (!v->states) gtk_progress_bar_pulse(GTK_PROGRESS_BAR(v->progress));
  else
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->progress),
                                  v->item_count ? (double)settled / v->item_count : 1.0);
}

static void set_group(import_view *v, int idx, int group) {
  item_row *r = &v->rows[idx];
  if (r->group == group) return;
  g_object_ref(r->row); /* the old list holds the only reference */
  if (r->group >= 0) {
    gtk_list_box_remove(GTK_LIST_BOX(v->lists[r->group]), r->row);
    if (--v->group_count[r->group] == 0) gtk_widget_set_visible(v->groups[r->group], FALSE);
  }
  gtk_list_box_append(GTK_LIST_BOX(v->lists[group]), r->row);
  g_object_unref(r->row);
  if (v->group_count[group]++ == 0) gtk_widget_set_visible(v->groups[group], TRUE);
  r->group = group;
}

static void refresh_row(import_view *v, int idx) {
  mc_import_item *it;
  if (mc_import_item_get(v->imp, idx, &it) != MC_OK) return;
  v->states[idx] = it->state;
  item_row *r = &v->rows[idx];

  const char *title = it->album        ? it->album
                      : it->file_count ? file_name(it->files[0].path)
                                       : mc_str(MC_STR_UNKNOWN_VALUE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r->row), title);

  const mc_import_match *m = NULL;
  if (it->chosen >= 0 && it->chosen < it->match_count) m = &it->matches[it->chosen];
  else if (it->state != MC_IMPORT_DONE && it->match_count) m = &it->matches[0];
  gboolean as_is = it->state == MC_IMPORT_DONE && it->chosen < 0;

  char files[64], match[512] = "", subtitle[1024] = "";
  if (it->kind == MC_IMPORT_SINGLE) g_strlcpy(files, mc_str(MC_STR_IMPORT_SINGLE), sizeof files);
  else count_text(MC_STR_LINUX_IMPORT_FILE_COUNT, it->file_count, files, sizeof files);
  if (it->error) g_strlcpy(match, it->error, sizeof match);
  else if (as_is) g_strlcpy(match, mc_str(MC_STR_IMPORT_EXISTING_TAGS), sizeof match);
  else if (m) match_summary(m, match, sizeof match);
  else if (it->state == MC_IMPORT_REVIEW) g_strlcpy(match, mc_str(MC_STR_IMPORT_ERROR_NO_MATCHES), sizeof match);
  join(subtitle, sizeof subtitle, it->artist);
  join(subtitle, sizeof subtitle, files);
  if (*match) {
    size_t len = strlen(subtitle);
    snprintf(subtitle + len, sizeof subtitle - len, "%s%s", len ? "\n" : "", match);
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(r->row), subtitle);

  char conf[16] = "";
  if (m && !as_is) snprintf(conf, sizeof conf, "%.0f%%", m->confidence * 100);
  gtk_label_set_text(GTK_LABEL(r->confidence), conf);

  /* The group headings say the rest; only work in progress gets a status of its own. */
  gboolean busy = state_busy(it->state);
  gtk_label_set_text(GTK_LABEL(r->state), state_label(it->state));
  gtk_widget_set_visible(r->state, busy);
  gtk_widget_set_visible(r->spinner, it->state == MC_IMPORT_MATCHING || it->state == MC_IMPORT_WRITING);
  gtk_spinner_set_spinning(GTK_SPINNER(r->spinner), gtk_widget_get_visible(r->spinner));
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->row), !busy);

  set_group(v, idx, state_group(it->state));
  mc_import_item_free(it);
}

static GtkWidget *new_item_row(import_view *v, int idx) {
  item_row *r = &v->rows[idx];
  r->group = -1;
  r->row = plain_row(NULL, NULL);
  adw_action_row_set_subtitle_lines(ADW_ACTION_ROW(r->row), 2);
  g_object_set_data(G_OBJECT(r->row), "item", GINT_TO_POINTER(idx));
  r->spinner = gtk_spinner_new();
  r->state = dim_label(NULL);
  r->confidence = dim_label(NULL);
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->row), r->spinner);
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->row), r->state);
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->row), r->confidence);
  adw_action_row_add_suffix(ADW_ACTION_ROW(r->row), gtk_image_new_from_icon_name("go-next-symbolic"));
  return r->row;
}

static void on_event(import_view *v, mc_import_event ev, int arg) {
  switch (ev) {
    case MC_IMPORT_EV_SCANNING:
      v->scanned_files = arg;
      break;
    case MC_IMPORT_EV_SCANNED:
      v->item_count = mc_import_item_count(v->imp);
      v->states = g_new0(mc_import_state, (size_t)v->item_count + 1);
      v->rows = g_new0(item_row, (size_t)v->item_count + 1);
      for (int i = 0; i < v->item_count; i++) {
        new_item_row(v, i);
        refresh_row(v, i);
      }
      break;
    case MC_IMPORT_EV_ITEM_CHANGED: {
      if (arg < 0 || arg >= v->item_count) break;
      mc_import_state before = v->states[arg];
      refresh_row(v, arg);
      if (v->rev && v->rev->item == arg) review_load(v->rev);
      if (v->states[arg] == MC_IMPORT_DONE && before != MC_IMPORT_DONE && v->cb.library_changed)
        v->cb.library_changed(v->cb.user);
      break;
    }
    case MC_IMPORT_EV_IDLE:
      v->idle = TRUE;
      break;
    case MC_IMPORT_EV_OFFLINE:
      v->offline = arg != 0;
      update_banner(v);
      break;
  }
  if (v->imp) update_status(v); /* a callback may have ended the session */
}

gboolean import_view_start(GtkWidget *view, const char *const *paths, int count) {
  import_view *v = get_view(view);
  if (!v || count <= 0) return FALSE;
  if (!v->lib) {
    alert(view, mc_str(MC_STR_LINUX_IMPORT_CANT_TITLE), mc_str(MC_STR_IMPORT_NO_LIBRARY_TEXT));
    return FALSE;
  }
  if (v->imp && !v->idle) {
    alert(view, mc_str(MC_STR_IMPORT_RUNNING_TITLE), mc_str(MC_STR_IMPORT_RUNNING_TEXT));
    return FALSE;
  }
  close_session(v);
  v->idle = FALSE;

  session_tag *tag = g_new(session_tag, 1);
  tag->v = v;
  tag->gen = v->generation;
  mc_import_options opts = {.threshold = 0, .fetch_art = 1};
  mc_status st = mc_import_start(v->lib, paths, count, &opts, on_import_event, tag, &v->imp);
  if (st != MC_OK) {
    g_free(tag);
    v->imp = NULL;
    char *body = mc_format_alloc(mc_str(MC_STR_IMPORT_START_FAILED), MC_S(mc_status_str(st)));
    alert(view, mc_str(MC_STR_LINUX_IMPORT_CANT_TITLE), body);
    mc_free(body);
    return FALSE;
  }
  v->tag = tag;
  update_status(v);
  gtk_stack_set_visible_child_name(GTK_STACK(v->root), "session");
  if (v->cb.session_changed) v->cb.session_changed(v->cb.user, TRUE);
  return TRUE;
}

/* --- Picking and dropping folders --- */

/* Imports the native paths of a list of GFiles. */
static gboolean start_files(import_view *v, GListModel *files) {
  guint n = files ? g_list_model_get_n_items(files) : 0;
  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < n; i++) {
    GFile *f = g_list_model_get_item(files, i);
    char *path = g_file_get_path(f);
    if (path) g_ptr_array_add(paths, path);
    g_object_unref(f);
  }
  gboolean ok = paths->len > 0 &&
                import_view_start(v->root, (const char *const *)paths->pdata, (int)paths->len);
  g_ptr_array_free(paths, TRUE);
  return ok;
}

static void on_folders_chosen(GObject *source, GAsyncResult *result, gpointer data) {
  import_view *v = data;
  GListModel *files = gtk_file_dialog_select_multiple_folders_finish(GTK_FILE_DIALOG(source), result, NULL);
  if (files && v->root) start_files(v, files);
  if (files) g_object_unref(files);
  g_atomic_rc_box_release(v);
}

void import_view_choose_folders(GtkWidget *view) {
  import_view *v = get_view(view);
  if (!v) return;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, mc_str(MC_STR_IMPORT_CHOOSE_FOLDERS_TITLE));
  gtk_file_dialog_set_accept_label(dlg, mc_str_mnemonic(MC_STR_IMPORT_BUTTON));
  GtkRoot *root = gtk_widget_get_root(view);
  gtk_file_dialog_select_multiple_folders(dlg, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, NULL,
                                          on_folders_chosen, g_atomic_rc_box_acquire(v));
  g_object_unref(dlg);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
  (void)target;
  (void)x;
  (void)y;
  GtkWidget *view = data;
  import_view *v = get_view(view);
  if (!v || !G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
  GSList *list = gdk_file_list_get_files(g_value_get_boxed(value));
  GListStore *files = g_list_store_new(G_TYPE_FILE);
  for (GSList *l = list; l; l = l->next) g_list_store_append(files, l->data);
  g_slist_free(list);
  if (v->cb.present) v->cb.present(v->cb.user);
  gboolean ok = start_files(v, G_LIST_MODEL(files));
  g_object_unref(files);
  return ok;
}

void import_view_add_drop_target(GtkWidget *view, GtkWidget *widget) {
  if (!get_view(view) || !widget) return;
  GtkDropTarget *target = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
  /* The handler only needs the view while widget lives; widget is normally its ancestor. */
  g_signal_connect_object(target, "drop", G_CALLBACK(on_drop), view, 0);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(target));
}

/* --- Closing --- */

typedef struct confirm {
  import_view *v;
  void (*then)(void *user, gboolean closed);
  void *user;
} confirm;

static void finish_confirm(confirm *c, gboolean closed) {
  if (closed && c->v->root) close_session(c->v);
  if (c->then) c->then(c->user, closed);
  g_atomic_rc_box_release(c->v);
  g_free(c);
}

static void on_confirm_response(AdwAlertDialog *dlg, const char *response, gpointer data) {
  (void)dlg;
  finish_confirm(data, g_strcmp0(response, "stop") == 0);
}

void import_view_confirm_close(GtkWidget *view, void (*then)(void *user, gboolean closed), void *user) {
  import_view *v = get_view(view);
  if (!v) {
    if (then) then(user, TRUE);
    return;
  }
  confirm *c = g_new(confirm, 1);
  c->v = g_atomic_rc_box_acquire(v);
  c->then = then;
  c->user = user;
  if (!v->imp || v->idle) {
    finish_confirm(c, TRUE);
    return;
  }
  AdwDialog *dlg = adw_alert_dialog_new(mc_str(MC_STR_IMPORT_STOP_TITLE), mc_str(MC_STR_IMPORT_STOP_TEXT));
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dlg), "keep", mc_str_mnemonic(MC_STR_LINUX_IMPORT_KEEP_GOING),
                                 "stop", mc_str_mnemonic(MC_STR_LINUX_IMPORT_STOP), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dlg), "stop", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dlg), "keep");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dlg), "keep");
  g_signal_connect(dlg, "response", G_CALLBACK(on_confirm_response), c);
  adw_dialog_present(dlg, view);
}

static void after_done(void *user, gboolean closed) {
  import_view *v = user;
  if (!closed || !v->root) return;
  if (v->cb.library_changed) v->cb.library_changed(v->cb.user);
  if (v->cb.done) v->cb.done(v->cb.user);
}

static void on_done_clicked(GtkButton *button, gpointer data) {
  (void)button;
  import_view *v = data;
  import_view_confirm_close(v->root, after_done, v);
}

gboolean import_view_has_session(GtkWidget *view) {
  import_view *v = get_view(view);
  return v && v->imp;
}

gboolean import_view_is_running(GtkWidget *view) {
  import_view *v = get_view(view);
  return v && v->imp && !v->idle;
}

void import_view_set_library(GtkWidget *view, mc_library *lib) {
  import_view *v = get_view(view);
  if (!v) return;
  close_session(v);
  v->lib = lib;
}

/* --- Review dialog --- */

static void review_fill_tracks(review *r, int match) {
  r->selected = match;
  gtk_list_box_remove_all(GTK_LIST_BOX(r->tracks));
  const mc_import_item *it = r->snap;
  if (!it) return;

  char num[32], len[32], sub[1024];
  gboolean *used = g_new0(gboolean, (size_t)it->file_count + 1);
  if (match >= 0 && match < it->match_count) {
    const mc_import_match *m = &it->matches[match];
    gboolean multi_disc = m->track_count && m->tracks[m->track_count - 1].disc > 1;
    for (int j = 0; j < m->track_count; j++) {
      const mc_import_match_track *t = &m->tracks[j];
      const char *name = t->title ? t->title : mc_str(MC_STR_UNKNOWN_VALUE);
      char *title;
      if (it->kind == MC_IMPORT_SINGLE) title = g_strdup(name);
      else {
        if (multi_disc) snprintf(num, sizeof num, "%d-%d", t->disc, t->number);
        else snprintf(num, sizeof num, "%d", t->number);
        title = g_strdup_printf("%s. %s", num, name);
      }
      sub[0] = '\0';
      if (t->file >= 0 && t->file < it->file_count) {
        const mc_import_file *f = &it->files[t->file];
        used[t->file] = TRUE;
        join(sub, sizeof sub, file_name(f->path));
        join(sub, sizeof sub, f->title);
        format_length(f->duration_ms, len, sizeof len);
        join(sub, sizeof sub, len);
      } else {
        join(sub, sizeof sub, mc_str(MC_STR_IMPORT_TRACK_MISSING));
      }
      GtkWidget *row = plain_row(title, sub);
      g_free(title);
      format_length(t->length_ms, len, sizeof len);
      adw_action_row_add_suffix(ADW_ACTION_ROW(row), dim_label(len));
      gtk_list_box_append(GTK_LIST_BOX(r->tracks), row);
    }
  }
  /* Files that weren't matched to any track: left untouched if this match is applied. */
  for (int i = 0; i < it->file_count; i++) {
    if (used[i]) continue;
    const mc_import_file *f = &it->files[i];
    sub[0] = '\0';
    if (match >= 0) join(sub, sizeof sub, mc_str(MC_STR_IMPORT_NOT_MATCHED));
    join(sub, sizeof sub, f->title);
    GtkWidget *row = plain_row(file_name(f->path), sub);
    format_length(f->duration_ms, len, sizeof len);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), dim_label(len));
    gtk_list_box_append(GTK_LIST_BOX(r->tracks), row);
  }
  g_free(used);
}

static void on_match_toggled(GtkCheckButton *check, gpointer data) {
  review *r = data;
  if (!gtk_check_button_get_active(check)) return;
  int match = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(check), "match"));
  if (match != r->selected) review_fill_tracks(r, match);
}

static void review_load(review *r) {
  import_view *v = r->v;
  mc_import_item_free(r->snap);
  r->snap = NULL;
  if (!v->imp || mc_import_item_get(v->imp, r->item, &r->snap) != MC_OK) {
    adw_dialog_force_close(r->dialog);
    return;
  }
  const mc_import_item *it = r->snap;
  gboolean busy = state_busy(it->state);

  const char *unknown = mc_str(MC_STR_UNKNOWN_VALUE);
  const char *album = it->album ? it->album : it->file_count ? file_name(it->files[0].path) : unknown;
  const char *artist = it->artist ? it->artist : unknown;
  char *heading = it->kind == MC_IMPORT_ALBUM
                    ? mc_format_alloc(mc_str_plural(MC_STR_IMPORT_REVIEW_ALBUM, it->file_count), MC_S(artist),
                                      MC_S(album), MC_D(it->file_count))
                    : mc_format_alloc(mc_str(MC_STR_IMPORT_REVIEW_SINGLE), MC_S(artist), MC_S(album));
  gtk_label_set_text(GTK_LABEL(r->heading), heading ? heading : album);
  mc_free(heading);
  gtk_label_set_text(GTK_LABEL(r->folder), it->folder ? it->folder : "");
  const char *note = busy ? mc_str(MC_STR_IMPORT_SEARCHING) : it->error ? it->error : NULL;
  gtk_label_set_text(GTK_LABEL(r->note), note ? note : "");
  gtk_widget_set_visible(r->note, note != NULL);

  if (!r->terms_set) {
    gtk_editable_set_text(GTK_EDITABLE(r->artist), it->artist ? it->artist : "");
    gtk_editable_set_text(GTK_EDITABLE(r->title), it->album ? it->album : "");
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r->title),
                                  mc_str(it->kind == MC_IMPORT_SINGLE ? MC_STR_TITLE : MC_STR_ALBUM));
    r->terms_set = TRUE;
  }

  gtk_list_box_remove_all(GTK_LIST_BOX(r->matches));
  int sel = it->chosen >= 0 && it->chosen < it->match_count ? it->chosen : it->match_count ? 0 : -1;
  GtkWidget *first = NULL;
  for (int i = 0; i < it->match_count; i++) {
    const mc_import_match *m = &it->matches[i];
    /* "Artist – Album", or for a single "Artist – Release → Track". */
    char *into = m->is_single ? mc_format_alloc(mc_str(MC_STR_IMPORT_MATCH_INTO),
                                                MC_S(m->release ? m->release : unknown),
                                                MC_S(m->album ? m->album : unknown))
                              : NULL;
    char *title = mc_format_alloc(mc_str(MC_STR_TRACK_BYLINE), MC_S(m->album_artist ? m->album_artist : unknown),
                                  MC_S(m->is_single ? into : m->album ? m->album : unknown));
    mc_free(into);
    char sub[1024] = "", buf[64];
    if (m->year) {
      snprintf(buf, sizeof buf, "%d", m->year);
      join(sub, sizeof sub, buf);
    }
    count_text(MC_STR_TRACKS, m->track_count, buf, sizeof buf);
    join(sub, sizeof sub, buf);
    join(sub, sizeof sub, m->release_type);
    join(sub, sizeof sub, m->format);
    join(sub, sizeof sub, m->country);
    join(sub, sizeof sub, m->label);
    join(sub, sizeof sub, m->catalog);
    GtkWidget *row = plain_row(title, sub);
    mc_free(title);

    GtkWidget *check = gtk_check_button_new();
    gtk_widget_set_valign(check, GTK_ALIGN_CENTER);
    g_object_set_data(G_OBJECT(check), "match", GINT_TO_POINTER(i));
    if (first) gtk_check_button_set_group(GTK_CHECK_BUTTON(check), GTK_CHECK_BUTTON(first));
    else first = check;
    gtk_check_button_set_active(GTK_CHECK_BUTTON(check), i == sel);
    g_signal_connect(check, "toggled", G_CALLBACK(on_match_toggled), r);
    adw_action_row_add_prefix(ADW_ACTION_ROW(row), check);
    adw_action_row_set_activatable_widget(ADW_ACTION_ROW(row), check);

    snprintf(buf, sizeof buf, "%.0f%%", m->confidence * 100);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), dim_label(buf));
    gtk_list_box_append(GTK_LIST_BOX(r->matches), row);
  }
  gtk_widget_set_visible(r->matches, it->match_count > 0);
  adw_preferences_group_set_description(
    ADW_PREFERENCES_GROUP(r->matches_group),
    busy || it->match_count ? NULL
    : it->error             ? mc_str(MC_STR_LINUX_IMPORT_MATCHES_NONE_YET)
                            : mc_str(MC_STR_LINUX_IMPORT_MATCHES_NOTHING));
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(r->tracks_group),
                                        sel >= 0 ? mc_str(MC_STR_LINUX_IMPORT_TRACKS_TEXT) : NULL);
  review_fill_tracks(r, sel);

  gtk_widget_set_sensitive(r->use, !busy && it->match_count > 0);
  gtk_widget_set_sensitive(r->as_is, !busy);
  gtk_widget_set_sensitive(r->skip, !busy && it->state != MC_IMPORT_DONE);
  gtk_widget_set_sensitive(r->search, !busy);
}

static void review_done(review *r, mc_status st) {
  if (st == MC_OK) {
    r->v->idle = FALSE; /* the import thread has work again */
    update_status(r->v);
    adw_dialog_close(r->dialog);
  } else {
    alert(GTK_WIDGET(r->dialog), mc_str(MC_STR_LINUX_IMPORT_ITEM_BUSY_TITLE), mc_str(MC_STR_IMPORT_ITEM_BUSY));
  }
}

static void on_use(GtkButton *button, gpointer data) {
  (void)button;
  review *r = data;
  if (r->v->imp && r->selected >= 0) review_done(r, mc_import_accept(r->v->imp, r->item, r->selected));
}

static void on_as_is(GtkButton *button, gpointer data) {
  (void)button;
  review *r = data;
  if (r->v->imp) review_done(r, mc_import_accept_as_is(r->v->imp, r->item));
}

static void on_skip(GtkButton *button, gpointer data) {
  (void)button;
  review *r = data;
  if (r->v->imp) review_done(r, mc_import_skip(r->v->imp, r->item));
}

static void on_search(GtkWidget *widget, gpointer data) {
  (void)widget;
  review *r = data;
  if (!r->v->imp || !gtk_widget_get_sensitive(r->search)) return;
  const char *artist = gtk_editable_get_text(GTK_EDITABLE(r->artist));
  const char *title = gtk_editable_get_text(GTK_EDITABLE(r->title));
  if (title && *title && mc_import_search(r->v->imp, r->item, artist, title) == MC_OK) {
    r->v->idle = FALSE;
    update_status(r->v);
  }
  /* The dialog stays open and refreshes when results arrive. */
}

static void on_review_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  review *r = data;
  if (r->v->rev == r) r->v->rev = NULL;
  mc_import_item_free(r->snap);
  g_atomic_rc_box_release(r->v);
  g_free(r);
}

static GtkWidget *boxed_list(void) {
  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  return list;
}

static void open_review(import_view *v, int idx) {
  if (!v->imp || idx < 0 || idx >= v->item_count || state_busy(v->states[idx]) || v->rev) return;
  review *r = g_new0(review, 1);
  r->v = g_atomic_rc_box_acquire(v);
  r->item = idx;
  r->selected = -1;
  v->rev = r;

  /* The item: what it is and where it came from. */
  r->heading = gtk_label_new(NULL);
  gtk_widget_add_css_class(r->heading, "title-4");
  r->folder = gtk_label_new(NULL);
  gtk_widget_add_css_class(r->folder, "dim-label");
  r->note = gtk_label_new(NULL);
  GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *labels[] = {r->heading, r->folder, r->note};
  for (size_t i = 0; i < G_N_ELEMENTS(labels); i++) {
    gtk_label_set_xalign(GTK_LABEL(labels[i]), 0);
    gtk_label_set_wrap(GTK_LABEL(labels[i]), TRUE);
    gtk_label_set_wrap_mode(GTK_LABEL(labels[i]), PANGO_WRAP_WORD_CHAR);
    gtk_box_append(GTK_BOX(info), labels[i]);
  }
  GtkWidget *info_group = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(info_group), info);

  /* Search again with the user's own terms. */
  r->artist = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r->artist), mc_str(MC_STR_ARTIST));
  r->title = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r->title), mc_str(MC_STR_ALBUM));
  g_signal_connect(r->artist, "entry-activated", G_CALLBACK(on_search), r);
  g_signal_connect(r->title, "entry-activated", G_CALLBACK(on_search), r);
  r->search = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_SEARCH));
  gtk_widget_set_valign(r->search, GTK_ALIGN_CENTER);
  g_signal_connect(r->search, "clicked", G_CALLBACK(on_search), r);
  GtkWidget *search_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(search_group), mc_str(MC_STR_IMPORT_SEARCH_AGAIN));
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(search_group), r->search);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(search_group), r->artist);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(search_group), r->title);

  r->matches = boxed_list();
  r->matches_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(r->matches_group), mc_str(MC_STR_IMPORT_MATCHES));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(r->matches_group), r->matches);

  r->tracks = boxed_list();
  r->tracks_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(r->tracks_group), mc_str(MC_STR_TRACKS_TITLE));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(r->tracks_group), r->tracks);

  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(info_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(r->matches_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(r->tracks_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(search_group));

  r->skip = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_IMPORT_SKIP));
  r->as_is = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_IMPORT_AS_TAGGED));
  r->use = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_IMPORT_USE_MATCH));
  gtk_widget_add_css_class(r->use, "suggested-action");
  g_signal_connect(r->skip, "clicked", G_CALLBACK(on_skip), r);
  g_signal_connect(r->as_is, "clicked", G_CALLBACK(on_as_is), r);
  g_signal_connect(r->use, "clicked", G_CALLBACK(on_use), r);
  GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(spacer, TRUE);
  GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(actions, 12);
  gtk_widget_set_margin_end(actions, 12);
  gtk_widget_set_margin_top(actions, 12);
  gtk_widget_set_margin_bottom(actions, 12);
  gtk_box_append(GTK_BOX(actions), r->skip);
  gtk_box_append(GTK_BOX(actions), spacer);
  gtk_box_append(GTK_BOX(actions), r->as_is);
  gtk_box_append(GTK_BOX(actions), r->use);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
  adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), actions);

  r->dialog = adw_dialog_new();
  adw_dialog_set_title(r->dialog, mc_str(MC_STR_IMPORT_REVIEW_TITLE));
  adw_dialog_set_content_width(r->dialog, 760);
  adw_dialog_set_content_height(r->dialog, 720);
  adw_dialog_set_child(r->dialog, toolbar);
  g_signal_connect(r->dialog, "closed", G_CALLBACK(on_review_closed), r);

  review_load(r);
  if (v->rev == r) adw_dialog_present(r->dialog, v->root);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list;
  open_review(data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "item")));
}

/* --- Widget --- */

static int sort_by_item(GtkListBoxRow *a, GtkListBoxRow *b, gpointer data) {
  (void)data;
  return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(a), "item")) -
         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "item"));
}

static void on_choose_clicked(GtkButton *button, gpointer data) {
  (void)button;
  import_view *v = data;
  import_view_choose_folders(v->root);
}

static void on_root_destroy(GtkWidget *root, gpointer data) {
  (void)root;
  import_view *v = data;
  /* The caller may be tearing down too, so its callbacks aren't called from here on. An open
   * review dialog is left alone: with the session gone its buttons do nothing. */
  memset(&v->cb, 0, sizeof v->cb);
  v->root = NULL;
  close_session(v);
}

static void release_view(gpointer data) {
  g_atomic_rc_box_release(data);
}

static void add_css(void) {
  static gboolean added;
  GdkDisplay *display = gdk_display_get_default();
  if (added || !display) return;
  GtkCssProvider *css = gtk_css_provider_new();
  gtk_css_provider_load_from_string(css,
                                    ".import-drop { border: 2px dashed alpha(currentColor, 0.2);"
                                    " border-radius: 18px; }"
                                    ".import-view:drop(active) .import-drop {"
                                    " border-color: @accent_color; }");
  gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css);
  added = TRUE;
}

GtkWidget *import_view_new(mc_library *lib, const import_view_callbacks *cb) {
  add_css();
  import_view *v = g_atomic_rc_box_new0(import_view);
  v->lib = lib;
  if (cb) v->cb = *cb;

  /* Before an import: a drop zone with a button. */
  GtkWidget *choose = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_IMPORT_CHOOSE_FOLDERS));
  gtk_widget_set_halign(choose, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(choose, "pill");
  gtk_widget_add_css_class(choose, "suggested-action");
  g_signal_connect(choose, "clicked", G_CALLBACK(on_choose_clicked), v);
  GtkWidget *start = adw_status_page_new();
  adw_status_page_set_icon_name(ADW_STATUS_PAGE(start), "folder-music-symbolic");
  adw_status_page_set_title(ADW_STATUS_PAGE(start), mc_str(MC_STR_IMPORT_SHEET_TITLE));
  adw_status_page_set_description(ADW_STATUS_PAGE(start), mc_str(MC_STR_LINUX_IMPORT_START_TEXT));
  adw_status_page_set_child(ADW_STATUS_PAGE(start), choose);
  gtk_widget_add_css_class(start, "import-drop");
  gtk_widget_set_margin_start(start, 24);
  gtk_widget_set_margin_end(start, 24);
  gtk_widget_set_margin_top(start, 24);
  gtk_widget_set_margin_bottom(start, 24);

  /* During and after: status, progress and the items, grouped by state. */
  v->status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(v->status), 0);
  gtk_label_set_ellipsize(GTK_LABEL(v->status), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(v->status, TRUE);
  GtkWidget *done = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_DONE));
  g_signal_connect(done, "clicked", G_CALLBACK(on_done_clicked), v);
  GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_box_append(GTK_BOX(top), v->status);
  gtk_box_append(GTK_BOX(top), done);
  v->progress = gtk_progress_bar_new();

  GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_margin_start(head, 12); /* lines up with the groups below */
  gtk_widget_set_margin_end(head, 12);
  gtk_widget_set_margin_top(head, 12);
  gtk_box_append(GTK_BOX(head), top);
  gtk_box_append(GTK_BOX(head), v->progress);
  GtkWidget *clamp = adw_clamp_new();
  adw_clamp_set_maximum_size(ADW_CLAMP(clamp), 600); /* AdwPreferencesPage's width */
  adw_clamp_set_child(ADW_CLAMP(clamp), head);

  GtkWidget *page = adw_preferences_page_new();
  gtk_widget_set_vexpand(page, TRUE);
  for (int g = 0; g < NUM_GROUPS; g++) {
    v->lists[g] = boxed_list();
    gtk_list_box_set_sort_func(GTK_LIST_BOX(v->lists[g]), sort_by_item, NULL, NULL);
    g_signal_connect(v->lists[g], "row-activated", G_CALLBACK(on_row_activated), v);
    v->groups[g] = adw_preferences_group_new();
    adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(v->groups[g]), mc_str(group_titles[g]));
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(v->groups[g]), v->lists[g]);
    gtk_widget_set_visible(v->groups[g], FALSE);
    adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(v->groups[g]));
  }

  v->banner = adw_banner_new(mc_str(MC_STR_IMPORT_OFFLINE));
  adw_banner_set_use_markup(ADW_BANNER(v->banner), FALSE);
  adw_banner_set_button_label(ADW_BANNER(v->banner), mc_str_mnemonic(MC_STR_IMPORT_SKIP_TAGGING));
  g_signal_connect(v->banner, "button-clicked", G_CALLBACK(on_skip_tagging), v);

  GtkWidget *session = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(session), v->banner);
  gtk_box_append(GTK_BOX(session), clamp);
  gtk_box_append(GTK_BOX(session), page);

  v->root = gtk_stack_new();
  gtk_widget_add_css_class(v->root, "import-view");
  gtk_stack_set_transition_type(GTK_STACK(v->root), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(v->root), start, "start");
  gtk_stack_add_named(GTK_STACK(v->root), session, "session");
  gtk_stack_set_visible_child_name(GTK_STACK(v->root), "start");

  g_signal_connect(v->root, "destroy", G_CALLBACK(on_root_destroy), v);
  g_object_set_data_full(G_OBJECT(v->root), "import-view", v, release_view);
  import_view_add_drop_target(v->root, v->root);
  return v->root;
}
