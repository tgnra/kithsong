#include "edit_dialog.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cover.h"
#include "playback.h"

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "edit_dialog.c needs libadwaita 1.5 or later (AdwDialog)"
#endif

enum {
  FORM_WIDTH = 520,
  SEARCH_WIDTH = 640,
  SEARCH_HEIGHT = 620,
  BANNER_ROOM = 104, /* what the dialog grows by for the banner, its button under its text */
  ART_SIZE = 128,
  YEAR_DIGITS = 4,
  NUMBER_DIGITS = 3,
};

/* The form's fields, by what they hold. F_MB is shown in a row of its own, not typed. */
enum {
  F_TITLE,
  F_ARTIST,
  F_NAME,
  F_SORT_NAME,
  F_YEAR,
  F_ORIGINAL_YEAR,
  F_TRACK_NO,
  F_DISC_NO,
  F_GENRE,
  F_MB,
  NUM_FIELDS,
};

/* The form. Owned by its dialog: freed with it. */
typedef struct edit_form {
  app *a;
  edit_kind kind;
  mc_library *lib;
  char uid[MC_UID_STRLEN];
  GtkWidget *dialog, *toolbar, *cancel, *save, *spinner, *groups;
  AdwBanner *banner;
  gboolean banner_room;  /* the dialog has grown for it */
  GtkWidget *save_focus; /* what had the focus as Save started, which greying out takes away */
  GtkWidget *fields[NUM_FIELDS]; /* AdwEntryRows; NULL for those not on the form */
  GtkWidget *mb_row, *mb_search, *no_scrobble, *mix;
  GtkWidget *art, *art_picture, *art_none, *art_drop, *art_change, *art_auto, *art_note;
  char *mb;                /* the release the form has now, "" for none */
  char *saved[NUM_FIELDS]; /* what's in the library, as text; NULL for a field not on the form */
  mc_album_flags saved_flags;
  gboolean track_mix; /* a track alone on its album, or on none: the form has Continuous mix */
  gboolean art_ok;    /* the album has a folder here to put a cover in */
  GBytes *new_art;    /* the image that Save would use */
  gboolean reset_art; /* Automatic cover was pressed, for Save to do */
  GCancellable *art_closing; /* what's going on for the cover that closing stops */
  GtkWidget *search;  /* the search dialog while it's open */
  gboolean loaded;    /* the tags were read, so there's something to save */
  gboolean saving;
  gboolean reading;   /* an image is being read, for the cover */
  gboolean in_use;    /* Save found a file open while something plays: the banner offers to stop */
  gboolean changed;   /* something was saved */
  gboolean art_saved; /* a new cover was, of what changed */
  gboolean closed;
} edit_form;

/* The search dialog. Owned by its dialog: freed with it. */
typedef struct edit_search {
  edit_form *form; /* its dialog referenced while this lives */
  GtkWidget *dialog, *cancel, *artist, *title, *go, *use, *spinner, *message, *list, *scroller;
  mc_release_list *results;
  struct job *job; /* the search or the release being fetched; NULL if neither */
  gboolean used;   /* a release went into the form */
  gboolean closed;
} edit_search;

static edit_form *current; /* the form that's open */

/* --- Jobs: core calls off the UI thread --- */

/* One call, with copies of all it needs, so nothing it reads goes away while it runs. The
 * GTask owns it, and holds a reference to the dialog it's for until done has run. */
typedef struct job {
  void (*run)(struct job *j);
  void (*done)(gpointer owner, struct job *j); /* on the UI thread */
  mc_library *lib;
  char uid[MC_UID_STRLEN];
  mc_status st;
  /* Save. */
  edit_kind kind;
  char *text[NUM_FIELDS];
  mc_album_flags flags;
  gboolean set_tags, set_flags, tags_saved, flags_saved;
  /* A new cover: the image, to save or, read from art_file, to check and show at art_size. */
  GFile *art_file;
  GBytes *art;
  GdkTexture *art_texture;
  int art_size;
  gboolean set_art, reset_art, art_tried, art_saved;
  char merge_into[MC_UID_STRLEN]; /* the artist to merge with, rather than save the name; "" for none */
  /* Search. */
  char *artist, *title;
  mc_release_list *list;
  /* Using a release. */
  char mb_albumid[MC_UID_STRLEN];
  mc_album_tags tags;
  gboolean has_tags;
} job;

static GMutex jobs_lock;
static GCond jobs_idle;
static int jobs; /* running */

static void job_free(gpointer data) {
  job *j = data;
  for (int i = 0; i < NUM_FIELDS; i++) g_free(j->text[i]);
  g_free(j->artist);
  g_free(j->title);
  mc_release_list_free(j->list);
  if (j->has_tags) mc_album_tags_clear(&j->tags);
  g_clear_object(&j->art_file);
  g_clear_pointer(&j->art, g_bytes_unref);
  g_clear_object(&j->art_texture);
  g_free(j);
}

static void job_thread(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
  (void)source;
  (void)cancellable;
  job *j = data;
  j->run(j);
  g_mutex_lock(&jobs_lock);
  if (--jobs == 0) g_cond_broadcast(&jobs_idle);
  g_mutex_unlock(&jobs_lock);
  g_task_return_boolean(task, TRUE);
}

static void on_job_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  job *j = g_task_get_task_data(G_TASK(result));
  j->done(g_object_get_data(source, "edit-data"), j);
}

/* Runs j, then its done with what owner (a dialog) has as "edit-data". */
static void job_start(GtkWidget *owner, job *j) {
  g_mutex_lock(&jobs_lock);
  jobs++;
  g_mutex_unlock(&jobs_lock);
  GTask *task = g_task_new(owner, NULL, on_job_done, NULL);
  g_task_set_task_data(task, j, job_free);
  g_task_run_in_thread(task, job_thread);
  g_object_unref(task);
}

void edit_dialog_wait(void) {
  g_mutex_lock(&jobs_lock);
  while (jobs) g_cond_wait(&jobs_idle, &jobs_lock);
  g_mutex_unlock(&jobs_lock);
}

/* --- The search dialog --- */

static void form_use_release(edit_form *f, const mc_album_tags *t);

static const char *row_text(GtkWidget *row) {
  return gtk_editable_get_text(GTK_EDITABLE(row));
}

static gboolean grab_focus_later(gpointer data) {
  if (gtk_widget_get_root(data)) gtk_widget_grab_focus(data);
  g_object_unref(data);
  return G_SOURCE_REMOVE;
}

/* Into widget, once the dialog is up: the menu it was opened from gives the focus back as it
 * closes, to the window underneath. */
static void focus_later(GtkWidget *widget) {
  g_idle_add_full(G_PRIORITY_LOW, grab_focus_later, g_object_ref(widget), NULL);
}

/* Into row's text, not just onto the row, where keys would press the dialog's mnemonics. */
static void focus_row(GtkWidget *row) {
  focus_later(GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(row))));
}

/* What the dialog says under the Search button, or for NULL nothing. spin: something is going on. */
static void search_say(edit_search *s, const char *text, gboolean error, gboolean spin) {
  gtk_label_set_text(GTK_LABEL(s->message), text ? text : "");
  gtk_widget_set_visible(s->message, text != NULL);
  if (error) gtk_widget_add_css_class(s->message, "error");
  else gtk_widget_remove_css_class(s->message, "error");
  gtk_widget_set_visible(s->spinner, spin);
  gtk_spinner_set_spinning(GTK_SPINNER(s->spinner), spin);
}

static void search_update(edit_search *s) {
  /* The list says its selected row has gone as the dialog takes it down. */
  if (s->closed || gtk_widget_in_destruction(s->dialog)) return;
  gboolean busy = s->job != NULL;
  /* The core needs a title to search for. */
  gtk_widget_set_sensitive(s->go, !busy && *row_text(s->title));
  gtk_widget_set_sensitive(s->use, !busy && gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list)));
  gtk_widget_set_sensitive(s->artist, !busy);
  gtk_widget_set_sensitive(s->title, !busy);
  gtk_widget_set_sensitive(s->list, !busy);
  gtk_widget_set_visible(s->scroller, s->results && s->results->count);
}

/* Title over "artist · year · country · label · format · 12 tracks", and how well it fits. */
static GtkWidget *release_row(const mc_release_info *r) {
  GString *line = g_string_new(r->artist ? r->artist : "");
  char *tracks = r->track_count ? mc_format_alloc(mc_str_plural(MC_STR_TRACKS, r->track_count), MC_D(r->track_count)) : NULL;
  char year[16] = "";
  if (r->year) g_snprintf(year, sizeof year, "%d", r->year);
  const char *parts[] = {year, r->country, r->label, r->format, tracks};
  for (size_t i = 0; i < G_N_ELEMENTS(parts); i++) {
    if (!parts[i] || !*parts[i]) continue;
    if (line->len) g_string_append(line, mc_str(MC_STR_SEPARATOR_DOT));
    g_string_append(line, parts[i]);
  }
  mc_free(tracks);
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), r->title ? r->title : "");
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), line->str);
  g_string_free(line, TRUE);
  char *fit = mc_format_alloc(mc_str(MC_STR_EDIT_FIT_PERCENT), MC_D((int)lround(r->score * 100)));
  GtkWidget *label = gtk_label_new(fit);
  mc_free(fit);
  gtk_widget_add_css_class(label, "dim-label");
  gtk_widget_add_css_class(label, "numeric");
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), label);
  return row;
}

/* A job is under way, which greys out what may have the focus: it stays in the dialog, on
 * Cancel, so Escape still closes it. */
static void search_busy(edit_search *s, job *j, const char *text) {
  s->job = j;
  search_say(s, text, FALSE, TRUE);
  search_update(s);
  focus_later(s->cancel);
  job_start(s->dialog, j);
}

static void run_search(job *j) {
  j->st = mc_release_search(j->lib, j->uid, j->artist, j->title, &j->list);
}

static void search_done(gpointer owner, job *j) {
  edit_search *s = owner;
  if (s->closed) return;
  s->job = NULL;
  if (j->st == MC_OK && j->list) {
    s->results = j->list;
    j->list = NULL;
    for (int i = 0; i < s->results->count; i++)
      gtk_list_box_append(GTK_LIST_BOX(s->list), release_row(&s->results->items[i]));
  }
  gboolean found = s->results && s->results->count;
  char *failed = NULL;
  if (j->st != MC_OK && j->st != MC_ERR_NET)
    failed = mc_format_alloc(mc_str(MC_STR_EDIT_SEARCH_FAILED), MC_S(mc_status_str(j->st)));
  search_say(s,
             j->st == MC_ERR_NET ? mc_str(MC_STR_IMPORT_ERROR_UNREACHABLE)
             : failed            ? failed
             : found             ? NULL
                                 : mc_str(MC_STR_EDIT_SEARCH_NOTHING),
             j->st != MC_OK, FALSE);
  mc_free(failed);
  search_update(s);
  /* From the search, on to what it found, or back to try other words. */
  if (found) {
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s->list), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(s->list), first);
    gtk_widget_grab_focus(GTK_WIDGET(first));
  } else {
    gtk_widget_grab_focus(GTK_WIDGET(gtk_editable_get_delegate(GTK_EDITABLE(s->title))));
  }
}

static void search_start(edit_search *s) {
  if (s->job || !*row_text(s->title)) return;
  job *j = g_new0(job, 1);
  j->run = run_search;
  j->done = search_done;
  j->lib = s->form->lib;
  memcpy(j->uid, s->form->uid, sizeof j->uid);
  j->artist = g_strdup(row_text(s->artist));
  j->title = g_strdup(row_text(s->title));
  mc_release_list_free(s->results);
  s->results = NULL;
  gtk_list_box_remove_all(GTK_LIST_BOX(s->list));
  search_busy(s, j, mc_str(MC_STR_IMPORT_SEARCHING));
}

static void run_release_tags(job *j) {
  j->st = mc_release_tags(j->lib, j->uid, j->mb_albumid, &j->tags);
  j->has_tags = j->st == MC_OK;
}

static void use_done(gpointer owner, job *j) {
  edit_search *s = owner;
  if (s->closed) return;
  s->job = NULL;
  if (j->st == MC_OK) {
    form_use_release(s->form, &j->tags);
    s->used = TRUE;
    adw_dialog_force_close(ADW_DIALOG(s->dialog));
    return;
  }
  char *failed = j->st == MC_ERR_NET ? NULL
                                     : mc_format_alloc(mc_str(MC_STR_EDIT_SEARCH_LOAD_FAILED), MC_S(mc_status_str(j->st)));
  search_say(s, failed ? failed : mc_str(MC_STR_IMPORT_ERROR_UNREACHABLE), TRUE, FALSE);
  mc_free(failed);
  search_update(s);
  GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list));
  if (row) gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void use_release(edit_search *s) {
  GtkListBoxRow *row = gtk_list_box_get_selected_row(GTK_LIST_BOX(s->list));
  int i = row ? gtk_list_box_row_get_index(row) : -1;
  if (s->job || i < 0 || !s->results || i >= s->results->count) return;
  job *j = g_new0(job, 1);
  j->run = run_release_tags;
  j->done = use_done;
  j->lib = s->form->lib;
  memcpy(j->uid, s->form->uid, sizeof j->uid);
  memcpy(j->mb_albumid, s->results->items[i].mb_albumid, sizeof j->mb_albumid);
  search_busy(s, j, mc_str(MC_STR_EDIT_SEARCH_LOADING));
}

static void on_search_clicked(GtkButton *button, gpointer data) {
  (void)button;
  search_start(data);
}

static void on_search_entry(AdwEntryRow *row, gpointer data) {
  (void)row;
  search_start(data);
}

static void on_search_changed(GtkEditable *editable, gpointer data) {
  (void)editable;
  search_update(data);
}

static void on_use_clicked(GtkButton *button, gpointer data) {
  (void)button;
  use_release(data);
}

static void on_row_selected(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list;
  (void)row;
  search_update(data);
}

/* Enter or a double click on a release uses it. */
static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list;
  (void)row;
  use_release(data);
}

static void on_search_cancel(GtkButton *button, gpointer data) {
  (void)button;
  edit_search *s = data;
  adw_dialog_close(ADW_DIALOG(s->dialog));
}

/* Closing doesn't wait for what's running: it finishes on its own, and done finds s closed. */
static void on_search_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  edit_search *s = data;
  s->closed = TRUE;
  s->form->search = NULL;
  if (!s->form->closed) gtk_widget_grab_focus(s->used ? s->form->save : s->form->mb_search);
}

static void search_free(gpointer data) {
  edit_search *s = data;
  mc_release_list_free(s->results);
  g_object_unref(s->form->dialog);
  g_free(s);
}

static GtkWidget *search_field(edit_search *s, enum mc_str_id label, const char *text) {
  GtkWidget *row = adw_entry_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mc_str(label));
  gtk_editable_set_text(GTK_EDITABLE(row), text);
  gtk_editable_set_enable_undo(GTK_EDITABLE(row), TRUE);
  g_signal_connect(row, "changed", G_CALLBACK(on_search_changed), s);
  g_signal_connect(row, "entry-activated", G_CALLBACK(on_search_entry), s);
  return row;
}

/* Searches for f's album from what its form has now. */
static void search_present(edit_form *f) {
  if (f->search) return;
  edit_search *s = g_new0(edit_search, 1);
  s->form = f;
  g_object_ref(f->dialog);

  s->artist = search_field(s, MC_STR_ARTIST, row_text(f->fields[F_ARTIST]));
  s->title = search_field(s, MC_STR_TITLE, row_text(f->fields[F_TITLE]));
  GtkWidget *fields = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(fields), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(fields, "boxed-list");
  gtk_list_box_append(GTK_LIST_BOX(fields), s->artist);
  gtk_list_box_append(GTK_LIST_BOX(fields), s->title);

  s->go = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_SEARCH));
  gtk_widget_set_halign(s->go, GTK_ALIGN_END);
  g_signal_connect(s->go, "clicked", G_CALLBACK(on_search_clicked), s);

  s->spinner = gtk_spinner_new();
  s->message = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(s->message), TRUE);
  gtk_label_set_xalign(GTK_LABEL(s->message), 0);
  GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_box_append(GTK_BOX(status), s->spinner);
  gtk_box_append(GTK_BOX(status), s->message);

  s->list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(s->list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(s->list), FALSE);
  gtk_widget_add_css_class(s->list, "boxed-list");
  gtk_widget_set_valign(s->list, GTK_ALIGN_START);
  gtk_widget_set_margin_start(s->list, 1); /* room for the list's outline in the scroller */
  gtk_widget_set_margin_end(s->list, 1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s->list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_EDIT_SEARCH_RESULTS), -1);
  g_signal_connect(s->list, "row-selected", G_CALLBACK(on_row_selected), s);
  g_signal_connect(s->list, "row-activated", G_CALLBACK(on_row_activated), s);
  s->scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s->scroller), s->list);
  gtk_widget_set_vexpand(s->scroller, TRUE);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  gtk_widget_set_margin_top(box, 12);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_box_append(GTK_BOX(box), fields);
  gtk_box_append(GTK_BOX(box), s->go);
  gtk_box_append(GTK_BOX(box), status);
  gtk_box_append(GTK_BOX(box), s->scroller);

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  s->cancel = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_CANCEL));
  g_signal_connect(s->cancel, "clicked", G_CALLBACK(on_search_cancel), s);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), s->cancel);
  s->use = gtk_button_new_with_label(mc_str(MC_STR_EDIT_SEARCH_USE));
  gtk_widget_add_css_class(s->use, "suggested-action");
  g_signal_connect(s->use, "clicked", G_CALLBACK(on_use_clicked), s);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), s->use);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), box);

  s->dialog = GTK_WIDGET(adw_dialog_new());
  g_object_set_data_full(G_OBJECT(s->dialog), "edit-data", s, search_free);
  adw_dialog_set_title(ADW_DIALOG(s->dialog), mc_str(MC_STR_EDIT_SEARCH_TITLE));
  adw_dialog_set_content_width(ADW_DIALOG(s->dialog), SEARCH_WIDTH);
  adw_dialog_set_content_height(ADW_DIALOG(s->dialog), SEARCH_HEIGHT);
  adw_dialog_set_child(ADW_DIALOG(s->dialog), toolbar);
  g_signal_connect(s->dialog, "closed", G_CALLBACK(on_search_closed), s);
  f->search = s->dialog;
  search_say(s, NULL, FALSE, FALSE);
  search_update(s);
  adw_dialog_present(ADW_DIALOG(s->dialog), f->dialog);
  focus_row(s->title);
  /* What the button says is what the user came for, so it's under way as the dialog opens,
   * if there's a title to search for. */
  search_start(s);
}

/* --- The form --- */

/* A number as a field shows it: nothing for 0, which is unknown. */
static char *number_text(int n) {
  return n ? g_strdup_printf("%d", n) : g_strdup("");
}

static gboolean is_number(int field) {
  return field == F_YEAR || field == F_ORIGINAL_YEAR || field == F_TRACK_NO || field == F_DISC_NO;
}

static char *dup_text(const char *s) {
  return g_strdup(s ? s : "");
}

/* The tags and flags from the core, as text. */
static mc_status load(edit_form *f) {
  char **t = f->saved;
  mc_status st = MC_ERR_INVALID_ARG;
  if (f->kind == EDIT_ALBUM) {
    mc_album_tags tags = {0};
    st = mc_album_tags_get(f->lib, f->uid, &tags);
    if (st != MC_OK) return st;
    t[F_TITLE] = dup_text(tags.title);
    t[F_ARTIST] = dup_text(tags.artist);
    t[F_YEAR] = number_text(tags.year);
    t[F_ORIGINAL_YEAR] = number_text(tags.original_year);
    t[F_MB] = dup_text(tags.mb_albumid);
    mc_album_tags_clear(&tags);
    st = mc_album_flags_get(f->lib, f->uid, &f->saved_flags);
  } else if (f->kind == EDIT_ARTIST) {
    mc_artist_tags tags = {0};
    st = mc_artist_tags_get(f->lib, f->uid, &tags);
    if (st != MC_OK) return st;
    t[F_NAME] = dup_text(tags.name);
    t[F_SORT_NAME] = dup_text(tags.sort_name);
    mc_artist_tags_clear(&tags);
  } else {
    mc_track_tags tags = {0};
    st = mc_track_tags_get(f->lib, f->uid, &tags);
    if (st != MC_OK) return st;
    t[F_TITLE] = dup_text(tags.title);
    t[F_ARTIST] = dup_text(tags.artist);
    t[F_TRACK_NO] = number_text(tags.track_no);
    t[F_DISC_NO] = number_text(tags.disc_no);
    t[F_YEAR] = number_text(tags.year);
    t[F_GENRE] = dup_text(tags.genre);
    mc_track_tags_clear(&tags);
    mc_track_mix m;
    st = mc_track_mix_get(f->lib, f->uid, &m);
    f->track_mix = st == MC_OK && m.can_set;
    f->saved_flags.mix = st == MC_OK && m.mix;
  }
  return st;
}

/* On the save's thread: the tags, then the flags, each if it changed, then a new cover, which
 * goes last as tags can move the album's folder. */
static void run_save(job *j) {
  char **f = j->text;
  mc_status st = MC_OK;
  if (j->set_tags) {
    if (j->kind == EDIT_ALBUM) {
      mc_album_tags t = {f[F_TITLE], f[F_ARTIST], atoi(f[F_YEAR]), atoi(f[F_ORIGINAL_YEAR]), f[F_MB]};
      st = mc_album_tags_set(j->lib, j->uid, &t);
    } else if (j->kind == EDIT_ARTIST) {
      mc_artist_tags t = {f[F_NAME], f[F_SORT_NAME]};
      st = j->merge_into[0] ? mc_artist_merge(j->lib, j->uid, j->merge_into) : mc_artist_tags_set(j->lib, j->uid, &t);
    } else {
      mc_track_tags t = {f[F_TITLE], f[F_ARTIST], atoi(f[F_TRACK_NO]), atoi(f[F_DISC_NO]), atoi(f[F_YEAR]), f[F_GENRE]};
      st = mc_track_tags_set(j->lib, j->uid, &t);
    }
    j->tags_saved = st == MC_OK;
  }
  if (st == MC_OK && j->set_flags) {
    st = j->kind == EDIT_TRACK ? mc_track_mix_set(j->lib, j->uid, j->flags.mix)
                               : mc_album_flags_set(j->lib, j->uid, &j->flags);
    j->flags_saved = st == MC_OK;
  }
  if (st == MC_OK && j->set_art) {
    gsize len = 0;
    const void *image = j->art ? g_bytes_get_data(j->art, &len) : NULL;
    j->art_tried = TRUE;
    st = j->reset_art ? mc_album_art_reset(j->lib, j->uid) : mc_album_art_set(j->lib, j->uid, image, len);
    j->art_saved = st == MC_OK;
  }
  j->st = st;
}

/* What the banner says, or for NULL nothing; it offers to stop playing while f->in_use. */
static void form_say(edit_form *f, const char *text) {
  if (text && !f->banner_room) {
    f->banner_room = TRUE;
    adw_dialog_set_content_height(ADW_DIALOG(f->dialog), gtk_widget_get_height(f->toolbar) + BANNER_ROOM);
  }
  if (text) adw_banner_set_title(f->banner, text);
  adw_banner_set_button_label(f->banner, f->in_use ? mc_str(MC_STR_EDIT_STOP_RETRY) : NULL);
  adw_banner_set_revealed(f->banner, text != NULL);
}

static void form_update(edit_form *f) {
  gboolean off = f->saving || !f->loaded;
  gtk_widget_set_sensitive(f->save, !off && !f->reading);
  gtk_widget_set_sensitive(f->cancel, !f->saving);
  gtk_widget_set_sensitive(f->groups, !off);
  gtk_widget_set_visible(f->spinner, f->saving || f->reading);
  gtk_spinner_set_spinning(GTK_SPINNER(f->spinner), f->saving || f->reading);
  adw_dialog_set_can_close(ADW_DIALOG(f->dialog), !f->saving); /* a save can't be taken back halfway */
}

/* What the library and what shows it need once the form is done: the views key what they've
 * decoded on the file, which is the same one, so the playing track's cover is shown again. */
static void form_finished(edit_form *f) {
  if (f->changed) app_reload_library(f->a);
  if (f->changed && f->a->lib == f->lib) playback_tracks_changed(f->lib);
  if (f->art_saved && f->a->lib == f->lib) playback_art_changed(f->lib, f->uid);
}

static void save_done(gpointer owner, job *j) {
  edit_form *f = owner;
  f->saving = FALSE;
  /* What did get saved is saved, whatever came next: a second try leaves it out. */
  if (j->tags_saved)
    for (int i = 0; i < NUM_FIELDS; i++) {
      g_free(f->saved[i]);
      f->saved[i] = j->text[i];
      j->text[i] = NULL;
    }
  if (j->flags_saved) f->saved_flags = j->flags;
  if (j->art_saved) {
    g_clear_pointer(&f->new_art, g_bytes_unref);
    f->reset_art = FALSE;
    f->art_saved = TRUE;
  }
  f->changed |= j->tags_saved || j->flags_saved || j->art_saved;
  if (f->closed) { /* closed from outside, with the library going: edit_dialog_close() */
    form_finished(f);
    return;
  }
  form_update(f);
  if (j->st == MC_OK) {
    adw_dialog_force_close(ADW_DIALOG(f->dialog));
    return;
  }
  /* Nothing changed, and it's most likely the track playing; a cover isn't played. */
  f->in_use = j->st == MC_ERR_IN_USE && !j->art_tried && playback_queue_count() > 0;
  char *failed = j->st == MC_ERR_NOT_FOUND ? NULL
                                           : mc_format_alloc(mc_str(MC_STR_EDIT_SAVE_FAILED), MC_S(mc_status_str(j->st)));
  form_say(f, failed ? failed : mc_str(MC_STR_EDIT_GONE));
  mc_free(failed);
  if (f->save_focus) gtk_widget_grab_focus(f->save_focus);
}

static void start_save(edit_form *f, job *j);

typedef struct merge_question {
  edit_form *f;
  job *j;
} merge_question;

/* What was answered: the dialog holds on to the form until it's gone. */
static void on_merge_answer(GObject *source, GAsyncResult *result, gpointer data) {
  merge_question *q = data;
  const char *answer = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  if (!strcmp(answer, "cancel")) {
    job_free(q->j);
  } else {
    if (strcmp(answer, "merge")) q->j->merge_into[0] = '\0';
    start_save(q->f, q->j);
  }
  g_object_unref(q->f->dialog);
  g_free(q);
}

/* Saving an artist with a name another has: whether to make the two one. */
static void ask_merge(edit_form *f, job *j) {
  char *text = mc_format_alloc(mc_str(MC_STR_EDIT_MERGE_TEXT), MC_S(j->text[F_NAME]));
  AdwDialog *alert = adw_alert_dialog_new(mc_str(MC_STR_EDIT_MERGE_TITLE), text);
  mc_free(text);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(alert), "cancel", mc_str(MC_STR_CANCEL), "keep",
                                 mc_str(MC_STR_EDIT_MERGE_KEEP), "merge", mc_str(MC_STR_EDIT_MERGE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(alert), "merge", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(alert), "merge");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(alert), "cancel");
  merge_question *q = g_new(merge_question, 1);
  q->f = f;
  q->j = j;
  g_object_ref(f->dialog);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(alert), f->dialog, NULL, on_merge_answer, q);
}

/* Save: nothing to do closes the dialog, as there's nothing to lose. */
static void save(edit_form *f) {
  if (f->saving || f->reading || !f->loaded) return;
  job *j = g_new0(job, 1);
  j->run = run_save;
  j->done = save_done;
  j->lib = f->lib;
  j->kind = f->kind;
  memcpy(j->uid, f->uid, sizeof j->uid);
  for (int i = 0; i < NUM_FIELDS; i++) {
    if (!f->saved[i]) continue;
    if (i == F_MB) j->text[i] = g_strdup(f->mb);
    else if (is_number(i)) j->text[i] = number_text(atoi(row_text(f->fields[i]))); /* 007 is 7, and 0 unknown */
    else j->text[i] = g_strdup(row_text(f->fields[i]));
    if (strcmp(j->text[i], f->saved[i])) j->set_tags = TRUE;
  }
  if (f->kind == EDIT_ALBUM) {
    j->flags.no_scrobble = adw_switch_row_get_active(ADW_SWITCH_ROW(f->no_scrobble));
    j->flags.mix = adw_switch_row_get_active(ADW_SWITCH_ROW(f->mix));
    j->set_flags = !j->flags.no_scrobble != !f->saved_flags.no_scrobble || !j->flags.mix != !f->saved_flags.mix;
  } else if (f->track_mix) {
    j->flags.mix = adw_switch_row_get_active(ADW_SWITCH_ROW(f->mix));
    j->set_flags = !j->flags.mix != !f->saved_flags.mix;
  }
  if (f->reset_art) {
    j->reset_art = TRUE;
    j->set_art = TRUE;
  }
  if (f->new_art) {
    j->art = g_bytes_ref(f->new_art);
    j->set_art = TRUE;
  }
  if (!j->set_tags && !j->set_flags && !j->set_art) {
    job_free(j);
    adw_dialog_force_close(ADW_DIALOG(f->dialog));
    return;
  }
  if (f->kind == EDIT_ARTIST && strcmp(j->text[F_NAME], f->saved[F_NAME]) &&
      mc_artist_same_name(f->lib, f->uid, j->text[F_NAME], j->merge_into) == MC_OK) {
    ask_merge(f, j);
    return;
  }
  start_save(f, j);
}

static void start_save(edit_form *f, job *j) {
  f->save_focus = adw_dialog_get_focus(ADW_DIALOG(f->dialog));
  f->saving = TRUE;
  f->in_use = FALSE;
  form_say(f, mc_str(MC_STR_EDIT_SAVING));
  form_update(f);
  job_start(f->dialog, j);
}

/* --- The cover --- */

/* What the form says beside the cover. */
static void art_say(edit_form *f, const char *text, gboolean error) {
  gtk_label_set_text(GTK_LABEL(f->art_note), text);
  if (error) {
    gtk_widget_remove_css_class(f->art_note, "dim-label");
    gtk_widget_add_css_class(f->art_note, "error");
  } else {
    gtk_widget_remove_css_class(f->art_note, "error");
    gtk_widget_add_css_class(f->art_note, "dim-label");
  }
}

/* The cover shown, or for NULL the place for one, and what a screen reader calls it. */
static void art_show(edit_form *f, GdkTexture *texture, enum mc_str_id name) {
  gtk_picture_set_paintable(GTK_PICTURE(f->art_picture), GDK_PAINTABLE(texture));
  gtk_widget_set_visible(f->art_none, !texture);
  gtk_accessible_update_property(GTK_ACCESSIBLE(f->art), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(name), -1);
}

/* On a thread of its own: the file's bytes, if they make a cover, and what they look like. */
static void run_read_art(job *j) {
  j->st = MC_ERR_IO;
  GFileInfo *info = g_file_query_info(j->art_file, G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NONE, NULL, NULL);
  if (!info) return;
  goffset size = g_file_info_get_size(info);
  g_object_unref(info);
  char *data = NULL;
  gsize len = 0;
  if (size > MC_ALBUM_ART_MAX_BYTES) {
    j->st = MC_ERR_FORMAT;
  } else if (g_file_load_contents(j->art_file, NULL, &data, &len, NULL, NULL)) {
    j->art = g_bytes_new_take(data, len);
    j->st = mc_album_art_check(data, len);
    if (j->st == MC_OK) j->art_texture = cover_decode_bytes(j->art, j->art_size);
  }
}

static void read_art_done(gpointer owner, job *j) {
  edit_form *f = owner;
  if (f->closed) return;
  f->reading = FALSE;
  if (j->st == MC_OK && j->art_texture) {
    g_clear_pointer(&f->new_art, g_bytes_unref);
    f->new_art = g_steal_pointer(&j->art);
    f->reset_art = FALSE;
    gtk_widget_set_sensitive(f->art_auto, TRUE);
    art_show(f, j->art_texture, MC_STR_EDIT_ART_NEW_NAME);
    art_say(f, mc_str(MC_STR_EDIT_ART_NEW), FALSE);
  } else {
    art_say(f, mc_str(MC_STR_EDIT_ART_BAD), TRUE);
  }
  form_update(f);
}

/* Whether a file can be taken as the cover now: not with Save or a read under way, nor while
 * the search dialog is over this one. */
static gboolean can_take_art(edit_form *f) {
  return f->kind == EDIT_ALBUM && f->loaded && !f->closed && !f->saving && !f->reading && !f->search;
}

/* A file picked or dropped: it's read, and if it makes a cover it's shown, for Save to keep. */
static void take_art(edit_form *f, GFile *file) {
  if (!can_take_art(f)) return;
  if (!f->art_ok) {
    art_say(f, mc_str(MC_STR_EDIT_ART_NO_FILES), TRUE);
    return;
  }
  job *j = g_new0(job, 1);
  j->run = run_read_art;
  j->done = read_art_done;
  j->art_file = g_object_ref(file);
  j->art_size = ART_SIZE * gtk_widget_get_scale_factor(f->art);
  f->reading = TRUE;
  art_say(f, mc_str(MC_STR_EDIT_ART_READING), FALSE);
  form_update(f);
  job_start(f->dialog, j);
}

static void on_art_loaded(GdkTexture *texture, gpointer data) {
  edit_form *f = data;
  if (texture && !f->new_art) art_show(f, texture, MC_STR_EDIT_ART_COVER);
}

/* The cover the album has now, and whether it could have another. */
static void load_art(edit_form *f) {
  char *path = NULL;
  f->art_ok = mc_album_art_get(f->lib, f->uid, &path) == MC_OK;
  GdkTexture *texture = NULL;
  if (path) texture = cover_get_file(path, ART_SIZE * gtk_widget_get_scale_factor(f->art), f->art_closing, on_art_loaded, f);
  mc_free(path);
  if (texture) art_show(f, texture, MC_STR_EDIT_ART_COVER);
  g_clear_object(&texture);
}

static void on_art_picked(GObject *source, GAsyncResult *result, gpointer data) {
  edit_form *f = data;
  GFile *file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, NULL);
  if (file) take_art(f, file);
  g_clear_object(&file);
  g_object_unref(f->dialog);
}

/* Automatic cover: Save goes back to it; a cover picked after this is what Save uses instead. */
static void on_art_auto(GtkButton *button, gpointer data) {
  (void)button;
  edit_form *f = data;
  g_clear_pointer(&f->new_art, g_bytes_unref);
  f->reset_art = TRUE;
  gtk_widget_set_sensitive(f->art_auto, FALSE);
  art_say(f, mc_str(MC_STR_EDIT_ART_AUTOMATIC_NOTE), FALSE);
}

static void on_art_change(GtkButton *button, gpointer data) {
  (void)button;
  edit_form *f = data;
  if (!can_take_art(f)) return;
  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, mc_str(MC_STR_EDIT_ART_FILE_TYPES));
  gtk_file_filter_add_suffix(filter, "jpg");
  gtk_file_filter_add_suffix(filter, "jpeg");
  gtk_file_filter_add_suffix(filter, "png");
  GListStore *filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
  g_list_store_append(filters, filter);
  GtkFileDialog *chooser = gtk_file_dialog_new();
  gtk_file_dialog_set_title(chooser, mc_str(MC_STR_EDIT_ART_PICK_TITLE));
  gtk_file_dialog_set_filters(chooser, G_LIST_MODEL(filters));
  gtk_file_dialog_set_default_filter(chooser, filter);
  GtkRoot *root = gtk_widget_get_root(f->dialog);
  g_object_ref(f->dialog);
  gtk_file_dialog_open(chooser, GTK_IS_WINDOW(root) ? GTK_WINDOW(root) : NULL, f->art_closing, on_art_picked, f);
  g_object_unref(chooser);
  g_object_unref(filters);
  g_object_unref(filter);
}

/* A file over the dialog: the whole of it takes the file, so a ring runs inside its edge, and
 * the cover's place says what a drop would do. */
static void art_dragging(edit_form *f, gboolean on) {
  gtk_widget_set_visible(f->art_drop, on);
  if (on) gtk_widget_add_css_class(f->toolbar, "edit-dropping");
  else gtk_widget_remove_css_class(f->toolbar, "edit-dropping");
}

static GdkDragAction on_drag_enter(GtkDropTarget *target, double x, double y, gpointer data) {
  (void)target;
  (void)x;
  (void)y;
  edit_form *f = data;
  if (!can_take_art(f)) return 0;
  art_dragging(f, TRUE);
  return GDK_ACTION_COPY;
}

static void on_drag_leave(GtkDropTarget *target, gpointer data) {
  (void)target;
  art_dragging(data, FALSE);
}

static gboolean on_art_drop(GtkDropTarget *target, const GValue *value, double x, double y, gpointer data) {
  (void)target;
  (void)x;
  (void)y;
  edit_form *f = data;
  art_dragging(f, FALSE);
  if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
  GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
  gboolean any = files != NULL;
  if (any) take_art(f, files->data); /* the first of several, as a cover is one image */
  g_slist_free(files);
  return any;
}

static void set_mb(edit_form *f, const char *mb) {
  g_free(f->mb);
  f->mb = dup_text(mb);
  if (f->mb_row) adw_action_row_set_subtitle(ADW_ACTION_ROW(f->mb_row), *f->mb ? f->mb : mc_str(MC_STR_EDIT_MB_NONE));
}

/* The release used in the search: the form takes it, for Save to keep. */
static void form_use_release(edit_form *f, const mc_album_tags *t) {
  char *year = number_text(t->year), *original = number_text(t->original_year);
  gtk_editable_set_text(GTK_EDITABLE(f->fields[F_TITLE]), t->title ? t->title : "");
  gtk_editable_set_text(GTK_EDITABLE(f->fields[F_ARTIST]), t->artist ? t->artist : "");
  gtk_editable_set_text(GTK_EDITABLE(f->fields[F_YEAR]), year);
  gtk_editable_set_text(GTK_EDITABLE(f->fields[F_ORIGINAL_YEAR]), original);
  g_free(year);
  g_free(original);
  set_mb(f, t->mb_albumid);
  f->in_use = FALSE;
  form_say(f, NULL);
}

static void on_save(GtkButton *button, gpointer data) {
  (void)button;
  save(data);
}

static void on_field_activated(AdwEntryRow *row, gpointer data) {
  (void)row;
  save(data);
}

/* The banner's Stop playing and retry: lets go of the file now, so the save can have it. */
static void on_stop_retry(AdwBanner *banner, gpointer data) {
  (void)banner;
  playback_clear();
  save(data);
}

static void on_cancel(GtkButton *button, gpointer data) {
  (void)button;
  edit_form *f = data;
  adw_dialog_close(ADW_DIALOG(f->dialog));
}

static void on_search_mb(GtkButton *button, gpointer data) {
  (void)button;
  search_present(data);
}

static void on_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  edit_form *f = data;
  f->closed = TRUE;
  if (f->art_closing) g_cancellable_cancel(f->art_closing);
  if (current == f) current = NULL;
  form_finished(f);
}

static void form_free(gpointer data) {
  edit_form *f = data;
  if (current == f) current = NULL;
  for (int i = 0; i < NUM_FIELDS; i++) g_free(f->saved[i]);
  g_free(f->mb);
  g_clear_pointer(&f->new_art, g_bytes_unref);
  g_clear_object(&f->art_closing);
  g_free(f);
}

/* Fields of numbers take digits only. */
static void on_digits_insert(GtkEditable *editable, const char *text, int length, int *position, gpointer data) {
  (void)position;
  (void)data;
  for (int i = 0; length < 0 ? text[i] != '\0' : i < length; i++)
    if (!g_ascii_isdigit(text[i])) {
      g_signal_stop_emission_by_name(editable, "insert-text");
      return;
    }
}

static GtkWidget *add_group(edit_form *f) {
  GtkWidget *group = adw_preferences_group_new();
  gtk_box_append(GTK_BOX(f->groups), group);
  return group;
}

static GtkWidget *add_field(edit_form *f, GtkWidget *group, int field, enum mc_str_id label) {
  GtkWidget *row = adw_entry_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mc_str(label));
  gtk_editable_set_text(GTK_EDITABLE(row), f->saved[field] ? f->saved[field] : "");
  gtk_editable_set_enable_undo(GTK_EDITABLE(row), TRUE);
  g_signal_connect(row, "entry-activated", G_CALLBACK(on_field_activated), f);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  f->fields[field] = row;
  return row;
}

static void add_number(edit_form *f, GtkWidget *group, int field, enum mc_str_id label, int digits) {
  GtkWidget *row = add_field(f, group, field, label);
  adw_entry_row_set_input_purpose(ADW_ENTRY_ROW(row), GTK_INPUT_PURPOSE_DIGITS);
  GtkEditable *text = gtk_editable_get_delegate(GTK_EDITABLE(row));
  gtk_text_set_max_length(GTK_TEXT(text), digits);
  g_signal_connect(text, "insert-text", G_CALLBACK(on_digits_insert), NULL);
}

static GtkWidget *add_switch(GtkWidget *group, enum mc_str_id label, enum mc_str_id note, int active) {
  GtkWidget *row = adw_switch_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), mc_str(label));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), mc_str(note));
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), active != 0);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  return row;
}

/* The cue a track's Artist shows while it's empty, which means the album's artist. An entry
 * row's own placeholder only shows while it has the focus. */
static void on_artist_changed(GtkEditable *editable, gpointer cue) {
  gtk_widget_set_visible(cue, !*gtk_editable_get_text(editable));
}

/* The cover, which is a button for the same as Change…, with the place for one under it, and
 * over it what a file dragged over the dialog shows. */
static GtkWidget *build_cover(edit_form *f) {
  GtkWidget *frame = cover_frame_new(ART_SIZE, ART_SIZE / 3, &f->art_picture);
  f->art_none = gtk_label_new(mc_str(MC_STR_EDIT_ART_NONE));
  gtk_widget_add_css_class(f->art_none, "dim-label");
  gtk_widget_set_valign(f->art_none, GTK_ALIGN_END);
  gtk_widget_set_margin_bottom(f->art_none, 12);
  gtk_overlay_add_overlay(GTK_OVERLAY(frame), f->art_none);
  f->art_drop = gtk_label_new(mc_str(MC_STR_EDIT_ART_DROP));
  gtk_label_set_wrap(GTK_LABEL(f->art_drop), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(f->art_drop), 12);
  gtk_label_set_justify(GTK_LABEL(f->art_drop), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class(f->art_drop, "edit-cover-drop");
  gtk_widget_set_visible(f->art_drop, FALSE);
  gtk_overlay_add_overlay(GTK_OVERLAY(frame), f->art_drop);
  f->art = gtk_button_new();
  gtk_button_set_child(GTK_BUTTON(f->art), frame);
  gtk_widget_add_css_class(f->art, "flat");
  gtk_widget_add_css_class(f->art, "edit-cover");
  gtk_widget_set_valign(f->art, GTK_ALIGN_START);
  g_signal_connect(f->art, "clicked", G_CALLBACK(on_art_change), f);
  return f->art;
}

/* The album's cover, in a group of its own above the tags, and the note beside it: how to change
 * it, how that's going, or why it can't be. */
static void build_art(edit_form *f) {
  GtkWidget *cover = build_cover(f);
  f->art_note = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(f->art_note), TRUE);
  gtk_label_set_xalign(GTK_LABEL(f->art_note), 0);
  f->art_change = gtk_button_new_with_label(mc_str(MC_STR_EDIT_ART_CHANGE));
  gtk_widget_set_halign(f->art_change, GTK_ALIGN_START);
  g_signal_connect(f->art_change, "clicked", G_CALLBACK(on_art_change), f);
  f->art_auto = gtk_button_new_with_label(mc_str(MC_STR_EDIT_ART_AUTOMATIC));
  gtk_widget_set_halign(f->art_auto, GTK_ALIGN_START);
  g_signal_connect(f->art_auto, "clicked", G_CALLBACK(on_art_auto), f);
  GtkWidget *side = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_valign(side, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(side, TRUE);
  gtk_box_append(GTK_BOX(side), f->art_note);
  gtk_box_append(GTK_BOX(side), f->art_change);
  gtk_box_append(GTK_BOX(side), f->art_auto);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
  gtk_box_append(GTK_BOX(box), cover);
  gtk_box_append(GTK_BOX(box), side);
  gtk_box_append(GTK_BOX(f->groups), box);

  f->art_closing = g_cancellable_new();
  art_show(f, NULL, MC_STR_EDIT_ART_NONE_NAME);
  if (f->loaded) load_art(f);
  gtk_widget_set_sensitive(f->art, f->art_ok);
  gtk_widget_set_sensitive(f->art_change, f->art_ok);
  gtk_widget_set_visible(f->art_auto, f->saved_flags.art_chosen);
  gtk_widget_set_sensitive(f->art_auto, f->art_ok);
  art_say(f, mc_str(f->art_ok || !f->loaded ? MC_STR_EDIT_ART_HELP : MC_STR_EDIT_ART_NO_FILES), FALSE);
}

static void build_album(edit_form *f) {
  build_art(f);
  GtkWidget *group = add_group(f);
  add_field(f, group, F_TITLE, MC_STR_TITLE);
  add_field(f, group, F_ARTIST, MC_STR_EDIT_ALBUM_ARTIST);
  add_number(f, group, F_YEAR, MC_STR_COL_YEAR, YEAR_DIGITS);
  add_number(f, group, F_ORIGINAL_YEAR, MC_STR_EDIT_ORIGINAL_YEAR, YEAR_DIGITS);

  group = add_group(f);
  f->mb_row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(f->mb_row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(f->mb_row), mc_str(MC_STR_EDIT_MB_RELEASE));
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(f->mb_row), TRUE);
  f->mb_search = gtk_button_new_with_label(mc_str(MC_STR_EDIT_MB_SEARCH));
  gtk_widget_set_valign(f->mb_search, GTK_ALIGN_CENTER);
  g_signal_connect(f->mb_search, "clicked", G_CALLBACK(on_search_mb), f);
  adw_action_row_add_suffix(ADW_ACTION_ROW(f->mb_row), f->mb_search);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), f->mb_row);
  set_mb(f, f->saved[F_MB]);

  group = add_group(f);
  f->no_scrobble = add_switch(group, MC_STR_EDIT_NO_SCROBBLE, MC_STR_EDIT_NO_SCROBBLE_TEXT, f->saved_flags.no_scrobble);
  f->mix = add_switch(group, MC_STR_EDIT_MIX, MC_STR_EDIT_MIX_TEXT, f->saved_flags.mix);
}

static void build_artist(edit_form *f) {
  GtkWidget *group = add_group(f);
  add_field(f, group, F_NAME, MC_STR_NAME);
  add_field(f, group, F_SORT_NAME, MC_STR_EDIT_SORT_NAME);
}

static void build_track(edit_form *f) {
  GtkWidget *group = add_group(f);
  add_field(f, group, F_TITLE, MC_STR_TITLE);
  GtkWidget *artist = add_field(f, group, F_ARTIST, MC_STR_ARTIST);
  GtkWidget *cue = gtk_label_new(mc_str(MC_STR_EDIT_TRACK_ARTIST_CUE));
  gtk_widget_add_css_class(cue, "dim-label");
  adw_entry_row_add_suffix(ADW_ENTRY_ROW(artist), cue);
  on_artist_changed(GTK_EDITABLE(artist), cue);
  g_signal_connect(artist, "changed", G_CALLBACK(on_artist_changed), cue);
  add_number(f, group, F_TRACK_NO, MC_STR_EDIT_TRACK_NUMBER, NUMBER_DIGITS);
  add_number(f, group, F_DISC_NO, MC_STR_EDIT_DISC_NUMBER, NUMBER_DIGITS);
  add_number(f, group, F_YEAR, MC_STR_COL_YEAR, YEAR_DIGITS);
  add_field(f, group, F_GENRE, MC_STR_EDIT_GENRE);
  if (f->track_mix) {
    group = add_group(f);
    f->mix = add_switch(group, MC_STR_EDIT_MIX, MC_STR_EDIT_MIX_TRACK_TEXT, f->saved_flags.mix);
  }
}

void edit_dialog_present(app *a, edit_kind kind, const char *uid) {
  static const enum mc_str_id titles[] = {
    [EDIT_ALBUM] = MC_STR_EDIT_ALBUM_TITLE,
    [EDIT_ARTIST] = MC_STR_EDIT_ARTIST_TITLE,
    [EDIT_TRACK] = MC_STR_EDIT_TRACK_TITLE,
  };
  if (!a->lib || !uid || !*uid || current) return;
  edit_form *f = g_new0(edit_form, 1);
  f->a = a;
  f->kind = kind;
  f->lib = a->lib;
  g_strlcpy(f->uid, uid, sizeof f->uid);
  mc_status st = load(f);
  f->loaded = st == MC_OK;

  f->groups = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start(f->groups, 18);
  gtk_widget_set_margin_end(f->groups, 18);
  gtk_widget_set_margin_top(f->groups, 18);
  gtk_widget_set_margin_bottom(f->groups, 18);
  if (kind == EDIT_ALBUM) build_album(f);
  else if (kind == EDIT_ARTIST) build_artist(f);
  else build_track(f);
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), f->groups);
  gtk_widget_set_vexpand(scroller, TRUE);

  GtkWidget *banner = adw_banner_new("");
  f->banner = ADW_BANNER(banner);
  adw_banner_set_use_markup(f->banner, FALSE);
  g_signal_connect(banner, "button-clicked", G_CALLBACK(on_stop_retry), f);
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(content), banner);
  gtk_box_append(GTK_BOX(content), scroller);

  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  f->cancel = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_CANCEL));
  g_signal_connect(f->cancel, "clicked", G_CALLBACK(on_cancel), f);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), f->cancel);
  f->save = gtk_button_new_with_mnemonic(mc_str_mnemonic(MC_STR_SAVE));
  gtk_widget_add_css_class(f->save, "suggested-action");
  g_signal_connect(f->save, "clicked", G_CALLBACK(on_save), f);
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), f->save);
  f->spinner = gtk_spinner_new();
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), f->spinner);
  f->toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(f->toolbar), header);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(f->toolbar), content);

  f->dialog = GTK_WIDGET(adw_dialog_new());
  g_object_set_data_full(G_OBJECT(f->dialog), "edit-data", f, form_free);
  adw_dialog_set_title(ADW_DIALOG(f->dialog), mc_str(titles[kind]));
  adw_dialog_set_content_width(ADW_DIALOG(f->dialog), FORM_WIDTH);
  adw_dialog_set_child(ADW_DIALOG(f->dialog), f->toolbar);
  g_signal_connect(f->dialog, "closed", G_CALLBACK(on_closed), f);
  if (kind == EDIT_ALBUM) {
    GtkDropTarget *drop = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY);
    /* Ahead of the fields, which would take a file as its path, in the text. */
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(drop), GTK_PHASE_CAPTURE);
    g_signal_connect(drop, "enter", G_CALLBACK(on_drag_enter), f);
    g_signal_connect(drop, "leave", G_CALLBACK(on_drag_leave), f);
    g_signal_connect(drop, "drop", G_CALLBACK(on_art_drop), f);
    gtk_widget_add_controller(f->toolbar, GTK_EVENT_CONTROLLER(drop));
  }
  form_update(f);
  current = f;
  adw_dialog_present(ADW_DIALOG(f->dialog), GTK_WIDGET(a->win));
  /* A failure leaves an empty form with only Cancel to press, and says why. */
  if (f->loaded) {
    focus_row(f->fields[kind == EDIT_ARTIST ? F_NAME : F_TITLE]);
  } else {
    char *failed = st == MC_ERR_NOT_FOUND ? NULL : mc_format_alloc(mc_str(MC_STR_EDIT_LOAD_FAILED), MC_S(mc_status_str(st)));
    form_say(f, failed ? failed : mc_str(MC_STR_EDIT_GONE));
    mc_free(failed);
    focus_later(f->cancel);
  }
}

gboolean edit_dialog_saving(void) {
  return current && current->saving;
}

void edit_dialog_close(void) {
  if (current) {
    edit_form *f = current;
    f->loaded = FALSE; /* no Save now, with the library going, as it animates away */
    if (f->search) adw_dialog_force_close(ADW_DIALOG(f->search));
    adw_dialog_force_close(ADW_DIALOG(f->dialog));
  }
  edit_dialog_wait();
}
