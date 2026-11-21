/* See stats_dialog.h. */
#include "stats_dialog.h"

#include <string.h>

enum {
  COLUMN_W = 316,     /* at least: two of them fill 680 */
  MESSAGE_W = 420,
  MESSAGE_H = 280,
  MAX_ROWS = 18,
  OPEN_WAIT_MS = 100, /* too short to notice, and the numbers are in by then in all but a huge
                       * library on a cold disk, so the dialog opens as it'll stay */
};

/* A number and what it counts. */
typedef struct stat_row {
  GtkWidget *row, *value;
} stat_row;

static struct {
  app *a;
  AdwDialog *dialog; /* while it's open */
  GtkWidget *toolbar;
  guint open_wait;   /* a timer, until the dialog opens */

  /* What's shown: the numbers on these rows, or with nrows 0 a message. */
  stat_row rows[MAX_ROWS];
  int nrows;
  gboolean whole;     /* with Whole library among them */
  GtkWidget *group;   /* the card rows are going into, or NULL to set the rows there are */
  int next;           /* the row to set */

  /* The thread is one read: it fills in status and stats, which are left alone until it has
   * ended. It has its own copy of the folder, so it outlives the dialog. */
  GThread *thread;
  gboolean again; /* the library changed during this read */
  mc_status status;
  mc_library_stats stats;
} g;

/* --- What's shown --- */

/* A number and its label are read out as one. */
static void set_row(const stat_row *r, const char *label, const char *value) {
  gtk_label_set_text(GTK_LABEL(r->value), value);
  char *name = g_strjoin(", ", label, value, NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(r->row), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_free(name);
}

static void add_row(const char *label, const char *value) {
  if (g.group) {
    if (g.nrows == MAX_ROWS) return;
    stat_row *r = &g.rows[g.nrows++];
    r->row = adw_preferences_row_new();
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(r->row), FALSE);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 24);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);
    GtkWidget *name = gtk_label_new(label);
    gtk_label_set_xalign(GTK_LABEL(name), 0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(name, TRUE);
    gtk_widget_add_css_class(name, "dim-label");
    gtk_box_append(GTK_BOX(box), name);
    r->value = gtk_label_new(NULL);
    gtk_widget_add_css_class(r->value, "numeric");
    gtk_box_append(GTK_BOX(box), r->value);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r->row), box);
    adw_preferences_group_add(ADW_PREFERENCES_GROUP(g.group), r->row);
    set_row(r, label, value);
  } else if (g.next < g.nrows) {
    set_row(&g.rows[g.next++], label, value);
  }
}

static void add_count(enum mc_str_id label, int64_t n) {
  char text[32];
  app_group_digits(n, text, sizeof text);
  add_row(mc_str(label), text);
}

static void add_time(enum mc_str_id label, int64_t ms) {
  char text[64];
  mc_format_total_time(text, sizeof text, ms, app_group_digits);
  add_row(mc_str(label), text);
}

static void add_tally(const mc_library_tally *t) {
  add_count(MC_STR_TRACKS_TITLE, t->tracks);
  add_count(MC_STR_SECTION_ALBUMS, t->albums);
  add_count(MC_STR_SECTION_SINGLES, t->singles);
  add_count(MC_STR_SECTION_ARTISTS, t->artists);
  add_count(MC_STR_STATS_GENRES, t->genres);
  add_time(MC_STR_STATS_PLAYING_TIME, t->duration_ms);
}

static void start_card(GtkWidget *parent, enum mc_str_id heading) {
  if (!parent) return; /* setting the rows there are */
  g.group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g.group), mc_str(heading));
  gtk_widget_set_size_request(g.group, COLUMN_W, -1);
  gtk_widget_set_valign(g.group, GTK_ALIGN_START);
  if (GTK_IS_FLOW_BOX(parent)) {
    gtk_flow_box_append(GTK_FLOW_BOX(parent), g.group);
    GtkWidget *child = gtk_widget_get_parent(g.group);
    gtk_widget_set_focusable(child, FALSE); /* the rows are what's in it */
  } else {
    gtk_box_append(GTK_BOX(parent), g.group);
  }
}

/* The cards, in reading order, going into flow, or with flow NULL the numbers set on the rows
 * there are. The flow box puts two to a line, or one if that's all that fits. Without Whole
 * library, Lists and Listening are one item, to stay beside On this computer. */
static void fill_cards(GtkWidget *flow, gboolean whole) {
  const mc_library_stats *s = &g.stats;
  char size[64];
  mc_format_size(size, sizeof size, s->size_bytes);
  start_card(flow, MC_STR_LINUX_STATS_THIS_COMPUTER);
  add_tally(&s->here);
  add_row(mc_str(MC_STR_STATS_SIZE), size);
  if (whole) {
    start_card(flow, MC_STR_STATS_ALL);
    add_tally(&s->all);
  }
  GtkWidget *parent = flow;
  if (flow && !whole) {
    parent = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_flow_box_append(GTK_FLOW_BOX(flow), parent);
    gtk_widget_set_focusable(gtk_widget_get_parent(parent), FALSE);
  }
  start_card(parent, MC_STR_STATS_LISTS);
  add_count(MC_STR_SECTION_PLAYLISTS, s->playlists + s->smart_playlists);
  add_count(MC_STR_SECTION_ALBUM_GROUPS, s->album_groups + s->smart_album_groups);
  start_card(parent, MC_STR_STATS_LISTENING);
  add_count(MC_STR_STATS_PLAYS, s->plays);
  add_count(MC_STR_STATS_TRACKS_PLAYED, s->tracks_played);
  add_time(MC_STR_STATS_TIME_LISTENED, s->listened_ms);
  g.group = NULL;
}

/* Replaces what the dialog holds with the cards. */
static void show_new_cards(gboolean whole) {
  GtkWidget *flow = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 2);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 12);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 24);
  gtk_widget_set_valign(flow, GTK_ALIGN_START);
  gtk_widget_set_margin_start(flow, 12);
  gtk_widget_set_margin_end(flow, 12);
  gtk_widget_set_margin_top(flow, 12);
  gtk_widget_set_margin_bottom(flow, 18);
  g.nrows = 0;
  fill_cards(flow, whole);
  g.whole = whole;

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroller), TRUE);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), flow);
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(g.toolbar), scroller);
}

/* In place of the cards: the empty library's title and icon over its text, or just a text. */
static void show_message(const char *title, const char *text) {
  GtkWidget *page = adw_status_page_new();
  gtk_widget_add_css_class(page, "compact");
  gtk_widget_set_size_request(page, MESSAGE_W, MESSAGE_H);
  if (title) {
    adw_status_page_set_icon_name(ADW_STATUS_PAGE(page), "folder-music-symbolic");
    adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
  }
  adw_status_page_set_description(ADW_STATUS_PAGE(page), text);
  g.nrows = 0;
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(g.toolbar), page);
}

/* What the read found. Setting the numbers on the rows there are keeps the scrolling and the focus
 * as a sync changes them. A read that fails with numbers on show leaves them there. */
static void show_read(void) {
  const mc_library_stats *s = &g.stats;
  if (g.status != MC_OK) {
    if (g.nrows) return;
    char *text = mc_format_alloc(mc_str(MC_STR_STATS_FAILED), MC_S(mc_status_str(g.status)));
    show_message(NULL, text);
    mc_free(text);
    return;
  }
  if (!s->all.tracks) {
    show_message(mc_str(MC_STR_EMPTY_TITLE), mc_str(MC_STR_STATS_EMPTY_TEXT));
    return;
  }
  /* The same numbers twice say nothing when all of it is here. */
  gboolean whole = memcmp(&s->all, &s->here, sizeof s->all) != 0;
  if (g.nrows && whole == g.whole) {
    g.next = 0;
    fill_cards(NULL, whole);
  } else {
    show_new_cards(whole);
  }
}

/* --- The dialog --- */

static void on_destroy(GtkWidget *dialog, gpointer user) {
  (void)dialog;
  (void)user;
  g.dialog = NULL;
  g.toolbar = NULL;
  g.nrows = 0;
}

static void open_dialog(gboolean read) {
  g.toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(g.toolbar), adw_header_bar_new());
  g.dialog = adw_dialog_new();
  adw_dialog_set_title(g.dialog, mc_str(MC_STR_STATS_TITLE));
  adw_dialog_set_follows_content_size(g.dialog, TRUE); /* the cards come, or go from two columns */
  adw_dialog_set_child(g.dialog, g.toolbar);
  g_signal_connect(g.dialog, "destroy", G_CALLBACK(on_destroy), NULL);
  if (read) show_read();
  else show_message(NULL, mc_str(MC_STR_STATS_READING));
  adw_dialog_present(g.dialog, GTK_WIDGET(g.a->win));
}

static gboolean on_open_wait(gpointer user) {
  (void)user;
  g.open_wait = 0;
  open_dialog(FALSE);
  return G_SOURCE_REMOVE;
}

/* --- Reading --- */

static gboolean on_read_done(gpointer user);

static gpointer read_main(gpointer data) {
  char *dir = data;
  mc_library *lib = NULL;
  mc_status st = mc_library_open(dir, &lib);
  if (st == MC_OK) {
    st = mc_library_read_stats(lib, &g.stats);
    mc_library_close(lib);
  }
  g_free(dir);
  g.status = st;
  g_idle_add(on_read_done, NULL);
  return NULL;
}

static void read_start(void) {
  if (g.thread) {
    g.again = TRUE;
    return;
  }
  g.thread = g_thread_new("stats", read_main, g_strdup(mc_library_dir(g.a->lib)));
}

/* The thread has ended, or stats_dialog_stop() saw to it already. The dialog may have closed since
 * it began, or not be open yet. */
static gboolean on_read_done(gpointer user) {
  (void)user;
  if (!g.thread) return G_SOURCE_REMOVE;
  g_thread_join(g.thread);
  g.thread = NULL;
  if (g.open_wait) {
    g_clear_handle_id(&g.open_wait, g_source_remove);
    open_dialog(TRUE);
  } else if (g.dialog) {
    show_read();
  }
  gboolean again = g.again && g.dialog && g.a->lib;
  g.again = FALSE;
  if (again) read_start();
  return G_SOURCE_REMOVE;
}

void stats_dialog_present(app *a) {
  if (g.dialog || g.open_wait || !a->lib) return;
  g.a = a;
  read_start();
  g.open_wait = g_timeout_add(OPEN_WAIT_MS, on_open_wait, NULL);
}

void stats_dialog_refresh(void) {
  if ((g.dialog || g.open_wait) && g.a->lib) read_start();
}

void stats_dialog_stop(void) {
  g_clear_handle_id(&g.open_wait, g_source_remove);
  g.again = FALSE;
  if (g.thread) g_thread_join(g.thread);
  g.thread = NULL;
}
