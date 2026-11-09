#include "cast_button.h"

#include <math.h>
#include <string.h>

#include "app.h"
#include "kithcore/kithcore.h"
#include "playback.h"

enum {
  WIDTH = 300,
  ROW_H = 52,   /* a device's row, with its model under its name */
  MAX_ROWS = 6, /* shown before the list scrolls */
  KEY_STEP = 5, /* the volume's arrow keys, as the header's volume control has them */
  PAGE_STEP = 20,
};

static struct {
  GtkWidget *button, *popover, *heading, *spinner, *scroller, *list, *empty, *session, *volume, *slider, *stop;
  mc_cast_device_list *devices; /* as last read, or NULL */
  gboolean updating;            /* we're moving the slider, not the user */
} s;

/* What to call a device: its name, else its model, else "" (for MC_STR_CAST_DEVICE_UNNAMED). */
static const char *device_name(const mc_cast_device *d) {
  return d->name[0] ? d->name : d->model;
}

static GtkWidget *decoration(const char *icon) {
  GtkWidget *image = gtk_image_new_from_icon_name(icon);
  gtk_accessible_update_state(GTK_ACCESSIBLE(image), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
  return image;
}

static GtkWidget *device_row(const mc_cast_device *d) {
  const char *name = device_name(d);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_box_append(GTK_BOX(box), decoration("kithsong-cast-symbolic"));
  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  GtkWidget *title = gtk_label_new(name[0] ? name : mc_str(MC_STR_CAST_DEVICE_UNNAMED));
  gtk_label_set_xalign(GTK_LABEL(title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(text), title);
  if (d->name[0] && d->model[0]) { /* what it is, under what it's called */
    GtkWidget *model = gtk_label_new(d->model);
    gtk_label_set_xalign(GTK_LABEL(model), 0);
    gtk_label_set_ellipsize(GTK_LABEL(model), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(model, "caption");
    gtk_widget_add_css_class(model, "dim-label");
    gtk_box_append(GTK_BOX(text), model);
  }
  gtk_box_append(GTK_BOX(box), text);

  GtkWidget *row = gtk_list_box_row_new();
  gtk_widget_set_size_request(row, -1, ROW_H);
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "id", g_strdup(d->id), g_free);
  g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name), g_free);
  return row;
}

static GtkWidget *focused(void) {
  GtkRoot *root = s.button ? gtk_widget_get_root(s.button) : NULL;
  return root ? gtk_root_get_focus(root) : NULL;
}

static gboolean popover_open(void) {
  return s.popover && gtk_widget_get_mapped(s.popover);
}

/* The list again, the focus staying on the device it was on, if that's still there. */
static void reload_list(void) {
  char *had = NULL;
  for (GtkWidget *w = focused(); w; w = gtk_widget_get_parent(w))
    if (GTK_IS_LIST_BOX_ROW(w) && gtk_widget_get_parent(w) == s.list) {
      had = g_strdup(g_object_get_data(G_OBJECT(w), "id"));
      break;
    }
  gtk_list_box_remove_all(GTK_LIST_BOX(s.list));
  GtkWidget *refocus = NULL;
  for (int i = 0; s.devices && i < s.devices->count; i++) {
    GtkWidget *row = device_row(&s.devices->items[i]);
    gtk_list_box_append(GTK_LIST_BOX(s.list), row);
    if (had && !strcmp(had, s.devices->items[i].id)) refocus = row;
  }
  if (had && !refocus) refocus = GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(s.list), 0));
  if (refocus) gtk_widget_grab_focus(refocus);
  g_free(had);
}

/* Where the focus goes in the popover for what it shows: the first device, Stop casting while
 * connecting, the volume while casting. */
static void focus_default(mc_cast_state state) {
  GtkWidget *w = state == MC_CAST_CASTING      ? s.slider
                 : state == MC_CAST_CONNECTING ? s.stop
                                               : GTK_WIDGET(gtk_list_box_get_row_at_index(GTK_LIST_BOX(s.list), 0));
  if (w) gtk_widget_grab_focus(w);
}

void cast_button_update(gboolean devices) {
  if (!s.button) return;
  mc_cast *cast = playback_cast();
  if (devices || (!s.devices && cast)) {
    mc_cast_device_list_free(s.devices);
    s.devices = NULL;
    if (cast && mc_cast_devices(cast, &s.devices) != MC_OK) s.devices = NULL;
  }
  mc_cast_status st;
  memset(&st, 0, sizeof st);
  if (cast) mc_cast_status_get(cast, &st);
  int count = s.devices ? s.devices->count : 0;
  const char *name = st.device_name[0] ? st.device_name : mc_str(MC_STR_CAST_DEVICE_UNNAMED);

  /* The button. */
  char *label = st.state == MC_CAST_CASTING      ? mc_format_alloc(mc_str(MC_STR_CAST_CASTING_TO), MC_S(name))
                : st.state == MC_CAST_CONNECTING ? mc_format_alloc(mc_str(MC_STR_CAST_CONNECTING_TO), MC_S(name))
                                                 : NULL;
  const char *text = label ? label : mc_str(MC_STR_CAST);
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(s.button), st.state == MC_CAST_IDLE ? "kithsong-cast-symbolic"
                                                                                     : "kithsong-cast-connected-symbolic");
  if (st.state == MC_CAST_CASTING) gtk_widget_add_css_class(s.button, "cast-active");
  else gtk_widget_remove_css_class(s.button, "cast-active");
  if (st.state == MC_CAST_CONNECTING) gtk_widget_add_css_class(s.button, "cast-connecting");
  else gtk_widget_remove_css_class(s.button, "cast-connecting");
  app_set_tooltip(s.button, text, NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.button), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
  gboolean open = popover_open();
  gtk_widget_set_visible(s.button, cast && (count > 0 || st.state != MC_CAST_IDLE || open));

  /* The popover. */
  gtk_label_set_text(GTK_LABEL(s.heading), label ? label : mc_str(MC_STR_CAST_TO));
  mc_free(label);
  gboolean busy = st.state != MC_CAST_CASTING; /* looking for devices, or connecting */
  gtk_widget_set_visible(s.spinner, busy);
  gtk_spinner_set_spinning(GTK_SPINNER(s.spinner), busy && open);
  if (devices) reload_list();
  gtk_widget_set_visible(s.scroller, st.state == MC_CAST_IDLE && count > 0);
  gtk_widget_set_visible(s.empty, st.state == MC_CAST_IDLE && count == 0);
  gtk_widget_set_visible(s.session, st.state != MC_CAST_IDLE);
  gtk_widget_set_visible(s.volume, st.state == MC_CAST_CASTING);
  if (st.state == MC_CAST_CASTING) {
    s.updating = TRUE;
    gtk_range_set_value(GTK_RANGE(s.slider), playback_volume());
    s.updating = FALSE;
  }
  /* What had the focus may have gone with what the popover showed before. */
  GtkWidget *f = focused();
  if (open && (!f || !gtk_widget_is_ancestor(f, s.popover) || !gtk_widget_is_visible(f))) focus_default(st.state);
}

static void on_row_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  (void)user;
  playback_cast_connect(g_object_get_data(G_OBJECT(row), "id"), g_object_get_data(G_OBJECT(row), "name"));
}

static void on_volume_changed(GtkRange *range, gpointer user) {
  (void)user;
  if (!s.updating) playback_cast_set_volume((int)lround(gtk_range_get_value(range)));
}

static void on_stop(GtkButton *button, gpointer user) {
  (void)button;
  (void)user;
  playback_cast_disconnect();
  gtk_popover_popdown(GTK_POPOVER(s.popover));
}

/* After the popover's own focus handling. */
static void on_popover_map(GtkWidget *popover, gpointer user) {
  (void)popover;
  (void)user;
  cast_button_update(TRUE);
  mc_cast_status st;
  memset(&st, 0, sizeof st);
  if (playback_cast()) mc_cast_status_get(playback_cast(), &st);
  focus_default(st.state);
}

/* The button may have stayed only for the popover. */
static void on_popover_closed(GtkPopover *popover, gpointer user) {
  (void)popover;
  (void)user;
  cast_button_update(FALSE);
}

static void on_destroy(GtkWidget *widget, gpointer user) {
  (void)widget;
  (void)user;
  mc_cast_device_list_free(s.devices);
  memset(&s, 0, sizeof s);
}

GtkWidget *cast_button_new(void) {
  memset(&s, 0, sizeof s);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_size_request(box, WIDTH, -1);

  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(header, 6);
  gtk_widget_set_margin_end(header, 6);
  s.heading = gtk_label_new(mc_str(MC_STR_CAST_TO));
  gtk_widget_add_css_class(s.heading, "title-4");
  gtk_label_set_xalign(GTK_LABEL(s.heading), 0);
  gtk_label_set_wrap(GTK_LABEL(s.heading), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(s.heading), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_max_width_chars(GTK_LABEL(s.heading), 1); /* a long device name wraps, at WIDTH */
  gtk_widget_set_hexpand(s.heading, TRUE);
  gtk_box_append(GTK_BOX(header), s.heading);
  /* The heading and the list say what's going on; the spinner is only to see. */
  s.spinner = gtk_spinner_new();
  gtk_widget_set_valign(s.spinner, GTK_ALIGN_CENTER);
  gtk_accessible_update_state(GTK_ACCESSIBLE(s.spinner), GTK_ACCESSIBLE_STATE_HIDDEN, TRUE, -1);
  gtk_box_append(GTK_BOX(header), s.spinner);
  gtk_box_append(GTK_BOX(box), header);
  gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* The devices. */
  s.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(s.list), GTK_SELECTION_NONE);
  gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(s.list), TRUE);
  gtk_widget_add_css_class(s.list, "navigation-sidebar");
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.list), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_CAST_TO), -1);
  g_signal_connect(s.list, "row-activated", G_CALLBACK(on_row_activated), NULL);
  s.scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(s.scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(s.scroller), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(s.scroller), MAX_ROWS * ROW_H);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(s.scroller), s.list);
  gtk_box_append(GTK_BOX(box), s.scroller);

  /* None yet. */
  s.empty = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  gtk_widget_set_margin_start(s.empty, 6);
  gtk_widget_set_margin_end(s.empty, 6);
  gtk_widget_set_margin_top(s.empty, 6);
  gtk_widget_set_margin_bottom(s.empty, 6);
  GtkWidget *looking = gtk_label_new(mc_str(MC_STR_CAST_LOOKING));
  gtk_label_set_xalign(GTK_LABEL(looking), 0);
  gtk_label_set_wrap(GTK_LABEL(looking), TRUE);
  gtk_box_append(GTK_BOX(s.empty), looking);
  GtkWidget *why = gtk_label_new(mc_str(MC_STR_CAST_LOOKING_TEXT));
  gtk_label_set_xalign(GTK_LABEL(why), 0);
  gtk_label_set_wrap(GTK_LABEL(why), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(why), 1); /* the box's width, not the text's */
  gtk_widget_add_css_class(why, "dim-label");
  gtk_box_append(GTK_BOX(s.empty), why);
  gtk_box_append(GTK_BOX(box), s.empty);

  /* Connecting or casting: the volume (once casting), and Stop casting. */
  s.session = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(s.session, 6);
  gtk_widget_set_margin_end(s.session, 6);
  gtk_widget_set_margin_top(s.session, 6);
  gtk_widget_set_margin_bottom(s.session, 6);
  s.volume = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(s.volume), decoration("audio-volume-low-symbolic"));
  s.slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_increments(GTK_RANGE(s.slider), KEY_STEP, PAGE_STEP);
  gtk_scale_set_draw_value(GTK_SCALE(s.slider), FALSE);
  gtk_widget_set_hexpand(s.slider, TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.slider), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_VOLUME), -1);
  g_signal_connect(s.slider, "value-changed", G_CALLBACK(on_volume_changed), NULL);
  gtk_box_append(GTK_BOX(s.volume), s.slider);
  gtk_box_append(GTK_BOX(s.volume), decoration("audio-volume-high-symbolic"));
  gtk_box_append(GTK_BOX(s.session), s.volume);
  s.stop = gtk_button_new_with_label(mc_str(MC_STR_CAST_STOP));
  g_signal_connect(s.stop, "clicked", G_CALLBACK(on_stop), NULL);
  gtk_box_append(GTK_BOX(s.session), s.stop);
  gtk_box_append(GTK_BOX(box), s.session);

  s.popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(s.popover), box);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.popover), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_CAST), -1);
  g_signal_connect_after(s.popover, "map", G_CALLBACK(on_popover_map), NULL);
  g_signal_connect(s.popover, "closed", G_CALLBACK(on_popover_closed), NULL);

  /* Doesn't take the focus when clicked, like shuffle, so Space still plays. */
  s.button = gtk_menu_button_new();
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(s.button), s.popover);
  gtk_widget_add_css_class(s.button, "flat");
  gtk_widget_add_css_class(s.button, "cast");
  gtk_widget_set_valign(s.button, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click(s.button, FALSE);
  g_signal_connect(s.button, "destroy", G_CALLBACK(on_destroy), NULL);
  cast_button_update(TRUE); /* hidden, until playback_init() has a cast and it finds a device */
  return s.button;
}
