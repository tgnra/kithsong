#include "queue_button.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "kithcore/kithcore.h"
#include "playback.h"

enum {
  WIDTH = 360,
  ROW_H = 48,    /* as main.c's css has it */
  MIN_ROWS = 4,  /* the list's height, however few there are */
  MAX_ROWS = 8,  /* ...and however many; the rest scroll */
  SCROLL_MS = 30,
};

/* An entry of the queue as it shows, on its row. */
typedef struct queue_row {
  uint32_t id;
  GtkWidget *row, *title, *byline, *duration;
} queue_row;

static struct {
  GtkWidget *button, *popover, *clear, *scroller, *list, *empty;
  GtkAdjustment *scroll;
  GHashTable *rows; /* id -> queue_row, owned by their rows */

  GtkGesture *drag;
  uint32_t drag_id;   /* the entry pressed, or 0 */
  double drag_from_y; /* where, in the list */
  gboolean dragging;  /* it has moved far enough */
  double pointer_y;   /* as last seen, in the scroller */
  int drop_at;        /* where it would go (0 to count), or -1 */
  GtkWidget *drop_row;
  guint drag_scroll;  /* the timer scrolling near the ends */
} s;

static void format_duration(int64_t ms, char *out, size_t n) {
  int sec = (int)((ms + 500) / 1000);
  if (ms <= 0) out[0] = '\0';
  else if (sec >= 3600) g_snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else g_snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

static queue_row *row_data(GtkListBoxRow *row) {
  return row ? g_object_get_data(G_OBJECT(row), "queue-row") : NULL;
}

static int row_count(void) {
  return (int)g_hash_table_size(s.rows);
}

/* The row with the keyboard focus, or NULL. */
static GtkListBoxRow *focused_row(void) {
  GtkRoot *root = s.list ? gtk_widget_get_root(s.list) : NULL;
  for (GtkWidget *w = root ? gtk_root_get_focus(root) : NULL; w; w = gtk_widget_get_parent(w)) {
    if (GTK_IS_LIST_BOX_ROW(w)) return gtk_widget_get_parent(w) == s.list ? GTK_LIST_BOX_ROW(w) : NULL;
  }
  return NULL;
}

static void focus_row_at(int i) {
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s.list), CLAMP(i, 0, row_count() - 1));
  if (!row) return;
  gtk_list_box_select_row(GTK_LIST_BOX(s.list), row);
  gtk_widget_grab_focus(GTK_WIDGET(row));
}

static void on_remove_clicked(GtkButton *button, gpointer data) {
  (void)button;
  playback_up_next_remove(((queue_row *)data)->id);
}

static GtkWidget *row_label(const char *css) {
  GtkWidget *l = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(l), 0);
  gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
  if (css) {
    gtk_widget_add_css_class(l, css);
    gtk_widget_add_css_class(l, "dim-label");
  }
  return l;
}

static queue_row *new_row(uint32_t id) {
  queue_row *r = g_new0(queue_row, 1);
  r->id = id;
  r->row = gtk_list_box_row_new();
  g_object_set_data_full(G_OBJECT(r->row), "queue-row", r, g_free);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *grip = gtk_image_new_from_icon_name("list-drag-handle-symbolic");
  gtk_widget_add_css_class(grip, "queue-grip");
  gtk_box_append(GTK_BOX(box), grip);

  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  r->title = row_label(NULL);
  gtk_box_append(GTK_BOX(text), r->title);
  r->byline = row_label("caption");
  gtk_box_append(GTK_BOX(text), r->byline);
  gtk_box_append(GTK_BOX(box), text);

  r->duration = gtk_label_new(NULL);
  gtk_widget_add_css_class(r->duration, "caption");
  gtk_widget_add_css_class(r->duration, "numeric");
  gtk_widget_add_css_class(r->duration, "dim-label");
  gtk_box_append(GTK_BOX(box), r->duration);

  /* The mouse's way in: Delete is the keyboard's, so Tab doesn't stop on every row. */
  GtkWidget *remove = gtk_button_new_from_icon_name("window-close-symbolic");
  gtk_widget_add_css_class(remove, "flat");
  gtk_widget_add_css_class(remove, "circular");
  gtk_widget_set_valign(remove, GTK_ALIGN_CENTER);
  gtk_widget_set_focusable(remove, FALSE);
  gtk_widget_set_tooltip_text(remove, mc_str(MC_STR_QUEUE_REMOVE));
  gtk_accessible_update_property(GTK_ACCESSIBLE(remove), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_QUEUE_REMOVE), -1);
  g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_clicked), r);
  gtk_box_append(GTK_BOX(box), remove);

  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r->row), box);
  return r;
}

/* What e shows, on its row and to screen readers. */
static void fill_row(queue_row *r, const mc_up_next_entry *e) {
  char *tracks = NULL;
  if (!e->artist && e->block) tracks = mc_format_alloc(mc_str_plural(MC_STR_TRACKS, e->tracks), MC_D(e->tracks));
  const char *byline = e->artist ? e->artist : tracks ? tracks : "";
  char duration[16];
  format_duration(e->duration_ms, duration, sizeof duration);
  const char *title = e->title ? e->title : "";
  gtk_label_set_text(GTK_LABEL(r->title), title);
  gtk_label_set_text(GTK_LABEL(r->byline), byline);
  gtk_widget_set_visible(r->byline, byline[0] != '\0');
  gtk_label_set_text(GTK_LABEL(r->duration), duration);
  char *name = g_strjoin(", ", title, byline, duration, NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(r->row), GTK_ACCESSIBLE_PROPERTY_LABEL, name,
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, mc_str(MC_STR_QUEUE_ROW_HELP), -1);
  g_free(name);
  mc_free(tracks);
}

static void end_drag(void);
static void show_drop(int at);

/* Reads the queue again, moving the rows there are rather than making them afresh, so the scrolling
 * stays. The focus goes with its entry, wherever that has moved to: if it has gone, the focus stays
 * where it was. */
static void reload(void) {
  int drop = s.drop_at;
  show_drop(-1); /* its row may be about to go */
  GtkListBoxRow *had_focus = focused_row();
  uint32_t focus = had_focus ? row_data(had_focus)->id : 0;
  int focus_at = had_focus ? gtk_list_box_row_get_index(had_focus) : -1;
  mc_up_next_list *list = playback_up_next();
  int n = list ? list->count : 0;

  /* What's left the queue goes first. */
  GHashTable *keep = g_hash_table_new(NULL, NULL);
  for (int i = 0; i < n; i++) g_hash_table_add(keep, GUINT_TO_POINTER(list->items[i].id));
  GPtrArray *gone = g_ptr_array_new();
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, s.rows);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    if (g_hash_table_contains(keep, key)) continue;
    g_ptr_array_add(gone, value);
    g_hash_table_iter_remove(&iter);
  }
  g_hash_table_unref(keep);
  if (s.drag_id && !g_hash_table_contains(s.rows, GUINT_TO_POINTER(s.drag_id))) end_drag(); /* it played, or went */
  for (guint i = 0; i < gone->len; i++) gtk_list_box_remove(GTK_LIST_BOX(s.list), ((queue_row *)gone->pdata[i])->row);
  g_ptr_array_unref(gone);

  for (int i = 0; i < n; i++) {
    const mc_up_next_entry *e = &list->items[i];
    queue_row *r = g_hash_table_lookup(s.rows, GUINT_TO_POINTER(e->id));
    if (!r) {
      r = new_row(e->id);
      g_hash_table_insert(s.rows, GUINT_TO_POINTER(e->id), r);
      gtk_list_box_insert(GTK_LIST_BOX(s.list), r->row, i);
    } else if (gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(r->row)) != i) {
      g_object_ref(r->row);
      gtk_list_box_remove(GTK_LIST_BOX(s.list), r->row);
      gtk_list_box_insert(GTK_LIST_BOX(s.list), r->row, i);
      g_object_unref(r->row);
    }
    fill_row(r, e);
  }
  mc_up_next_list_free(list);

  if (s.dragging && drop >= 0) show_drop(MIN(drop, n));
  gtk_widget_set_sensitive(s.clear, n > 0);
  gtk_widget_set_visible(s.scroller, n > 0);
  gtk_widget_set_visible(s.empty, n == 0);
  if (focus_at < 0) return;
  queue_row *r = g_hash_table_lookup(s.rows, GUINT_TO_POINTER(focus));
  if (r) {
    gtk_list_box_select_row(GTK_LIST_BOX(s.list), GTK_LIST_BOX_ROW(r->row));
    gtk_widget_grab_focus(r->row);
  } else {
    focus_row_at(focus_at);
  }
}

void queue_button_update(void) {
  if (s.popover && gtk_widget_get_mapped(s.popover)) reload();
}

/* to is a place between rows: 0 before the first, count after the last. */
static void move_row(uint32_t id, int from, int to) {
  if (to > from) to--;
  if (to != from) playback_up_next_move(id, to);
}

/* --- Dragging a row into another order --- */

static void show_drop(int at) {
  if (s.drop_row) {
    gtk_widget_remove_css_class(s.drop_row, "drop-above");
    gtk_widget_remove_css_class(s.drop_row, "drop-below");
    s.drop_row = NULL;
  }
  s.drop_at = at;
  if (at < 0) return;
  gboolean after_last = at >= row_count();
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(s.list), after_last ? row_count() - 1 : at);
  if (!row) return;
  s.drop_row = GTK_WIDGET(row);
  gtk_widget_add_css_class(s.drop_row, after_last ? "drop-below" : "drop-above");
}

/* The place between rows nearest y, in the list. */
static int drop_index(double y) {
  GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(s.list), (int)y);
  if (!row) return y < 0 ? 0 : row_count();
  graphene_rect_t r;
  int i = gtk_list_box_row_get_index(row);
  if (!gtk_widget_compute_bounds(GTK_WIDGET(row), s.list, &r)) return i;
  return y < r.origin.y + r.size.height / 2 ? i : i + 1;
}

/* Near the top or bottom of the list, it scrolls while dragging: how far per tick. */
static double scroll_step(void) {
  double page = gtk_adjustment_get_page_size(s.scroll);
  if (s.pointer_y < ROW_H / 2.0) return -ROW_H / 3.0;
  if (s.pointer_y > page - ROW_H / 2.0) return ROW_H / 3.0;
  return 0;
}

static gboolean on_drag_scroll(gpointer data) {
  (void)data;
  double step = s.dragging ? scroll_step() : 0;
  if (step == 0) {
    s.drag_scroll = 0;
    return G_SOURCE_REMOVE;
  }
  gtk_adjustment_set_value(s.scroll, gtk_adjustment_get_value(s.scroll) + step);
  show_drop(drop_index(s.pointer_y + gtk_adjustment_get_value(s.scroll)));
  return G_SOURCE_CONTINUE;
}

/* Ends a press or drag, without doing anything about it. */
static void end_drag(void) {
  queue_row *r = g_hash_table_lookup(s.rows, GUINT_TO_POINTER(s.drag_id));
  if (r) gtk_widget_remove_css_class(r->row, "queue-dragged");
  show_drop(-1);
  s.drag_id = 0;
  s.dragging = FALSE;
  g_clear_handle_id(&s.drag_scroll, g_source_remove);
}

static void on_drag_begin(GtkGestureDrag *gesture, double x, double y, gpointer user) {
  (void)gesture;
  (void)x;
  (void)user;
  queue_row *r = row_data(gtk_list_box_get_row_at_y(GTK_LIST_BOX(s.list), (int)y));
  s.drag_id = r ? r->id : 0;
  s.drag_from_y = y;
}

static void on_drag_update(GtkGestureDrag *gesture, double dx, double dy, gpointer user) {
  (void)user;
  if (!s.drag_id) return;
  if (!s.dragging) {
    if (!gtk_drag_check_threshold(s.list, 0, 0, (int)dx, (int)dy)) return;
    s.dragging = TRUE;
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    queue_row *r = g_hash_table_lookup(s.rows, GUINT_TO_POINTER(s.drag_id));
    if (r) gtk_widget_add_css_class(r->row, "queue-dragged");
  }
  /* The list scrolls under the pointer, so this is in the list as it was at the press. The offset
   * is from that press, measured as the list is now. */
  double y = s.drag_from_y + dy;
  s.pointer_y = y - gtk_adjustment_get_value(s.scroll);
  show_drop(drop_index(y));
  if (scroll_step() != 0 && !s.drag_scroll) s.drag_scroll = g_timeout_add(SCROLL_MS, on_drag_scroll, NULL);
}

static void on_drag_end(GtkGestureDrag *gesture, double dx, double dy, gpointer user) {
  (void)gesture;
  (void)dx;
  (void)dy;
  (void)user;
  uint32_t id = s.drag_id;
  int to = s.drop_at;
  gboolean moved = s.dragging;
  queue_row *r = g_hash_table_lookup(s.rows, GUINT_TO_POINTER(id));
  int from = r ? gtk_list_box_row_get_index(GTK_LIST_BOX_ROW(r->row)) : -1;
  end_drag();
  if (moved && from >= 0 && to >= 0) move_row(id, from, to);
}

static void on_drag_cancel(GtkGesture *gesture, GdkEventSequence *sequence, gpointer user) {
  (void)gesture;
  (void)sequence;
  (void)user;
  end_drag();
}

/* --- Keys --- */

/* Tab goes between the list and Clear, and the arrow keys up past the first row to Clear and back
 * down; the list box does the rest of them. */
static gboolean on_key(GtkEventControllerKey *keys, guint key, guint code, GdkModifierType mods, gpointer user) {
  (void)keys;
  (void)code;
  (void)user;
  if (key == GDK_KEY_Escape && s.drag_id) {
    gtk_event_controller_reset(GTK_EVENT_CONTROLLER(s.drag));
    end_drag();
    return TRUE;
  }
  if (s.drag_id || !row_count()) return FALSE;
  GtkListBoxRow *row = focused_row();
  gboolean alt = (mods & GDK_ALT_MASK) != 0, on_clear = !row;
  int i = row ? gtk_list_box_row_get_index(row) : -1;
  switch (key) {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab:
      if (on_clear) focus_row_at(0);
      else gtk_widget_grab_focus(s.clear);
      return TRUE;
    case GDK_KEY_Up:
      if (on_clear) return TRUE;
      if (alt) {
        move_row(row_data(row)->id, i, MAX(i - 1, 0));
        return TRUE;
      }
      if (i > 0) return FALSE;
      gtk_widget_grab_focus(s.clear);
      return TRUE;
    case GDK_KEY_Down:
      if (on_clear) {
        focus_row_at(0);
        return TRUE;
      }
      if (!alt) return FALSE;
      move_row(row_data(row)->id, i, i + 2);
      return TRUE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
      if (on_clear) return FALSE;
      playback_up_next_remove(row_data(row)->id);
      return TRUE;
    default: return FALSE;
  }
}

/* --- Opening --- */

static void on_clear(GtkButton *button, gpointer user) {
  (void)button;
  (void)user;
  playback_up_next_clear();
}

/* After the popover's own focus handling, which would pick Clear. The first row shows as selected,
 * so what Delete would take out shows however it was opened. */
static void on_popover_map(GtkWidget *popover, gpointer user) {
  (void)popover;
  (void)user;
  reload();
  gtk_adjustment_set_value(s.scroll, 0);
  focus_row_at(0);
}

static void on_popover_closed(GtkPopover *popover, gpointer user) {
  (void)popover;
  (void)user;
  if (s.drag_id) gtk_event_controller_reset(GTK_EVENT_CONTROLLER(s.drag));
  end_drag();
}

static void on_destroy(GtkWidget *widget, gpointer user) {
  (void)widget;
  (void)user;
  g_clear_handle_id(&s.drag_scroll, g_source_remove);
  g_clear_pointer(&s.rows, g_hash_table_unref);
  memset(&s, 0, sizeof s);
}

GtkWidget *queue_button_new(void) {
  memset(&s, 0, sizeof s);
  s.rows = g_hash_table_new(NULL, NULL);
  s.drop_at = -1;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(box, WIDTH, -1);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(header, 6);
  GtkWidget *heading = gtk_label_new(mc_str(MC_STR_QUEUE));
  gtk_widget_add_css_class(heading, "title-4");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_set_hexpand(heading, TRUE);
  gtk_box_append(GTK_BOX(header), heading);
  s.clear = gtk_button_new_with_label(mc_str(MC_STR_QUEUE_CLEAR));
  g_signal_connect(s.clear, "clicked", G_CALLBACK(on_clear), NULL);
  gtk_box_append(GTK_BOX(header), s.clear);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  s.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(s.list), GTK_SELECTION_SINGLE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(s.list), FALSE);
  gtk_widget_add_css_class(s.list, "navigation-sidebar");
  gtk_widget_add_css_class(s.list, "queue-list");
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.list), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_QUEUE), -1);
  s.drag = gtk_gesture_drag_new();
  g_signal_connect(s.drag, "drag-begin", G_CALLBACK(on_drag_begin), NULL);
  g_signal_connect(s.drag, "drag-update", G_CALLBACK(on_drag_update), NULL);
  g_signal_connect(s.drag, "drag-end", G_CALLBACK(on_drag_end), NULL);
  g_signal_connect(s.drag, "cancel", G_CALLBACK(on_drag_cancel), NULL);
  gtk_widget_add_controller(s.list, GTK_EVENT_CONTROLLER(s.drag));

  s.scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s.scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(s.scroller), TRUE);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(s.scroller), MIN_ROWS * ROW_H);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(s.scroller), MAX_ROWS * ROW_H);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s.scroller), s.list);
  s.scroll = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(s.scroller));
  gtk_box_append(GTK_BOX(box), s.scroller);

  s.empty = gtk_label_new(mc_str(MC_STR_QUEUE_EMPTY));
  gtk_widget_add_css_class(s.empty, "dim-label");
  gtk_widget_set_size_request(s.empty, -1, MIN_ROWS * ROW_H);
  gtk_box_append(GTK_BOX(box), s.empty);

  s.popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(s.popover), box);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.popover), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_QUEUE), -1);
  g_signal_connect_after(s.popover, "map", G_CALLBACK(on_popover_map), NULL);
  g_signal_connect(s.popover, "closed", G_CALLBACK(on_popover_closed), NULL);
  GtkEventController *keys = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
  g_signal_connect(keys, "key-pressed", G_CALLBACK(on_key), NULL);
  gtk_widget_add_controller(s.popover, keys);

  s.button = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(s.button), "view-list-bullet-symbolic");
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(s.button), s.popover);
  gtk_widget_add_css_class(s.button, "flat");
  gtk_widget_set_valign(s.button, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click(s.button, FALSE);
  app_set_tooltip(s.button, mc_str(MC_STR_QUEUE), NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.button), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_QUEUE), -1);
  g_signal_connect(s.button, "destroy", G_CALLBACK(on_destroy), NULL);
  reload();
  return s.button;
}
