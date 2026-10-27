#include "now_playing.h"

#include <string.h>

#include "cover.h"
#include "musiccore/musiccore.h"

enum {
  ART = 36, /* the cover's square, as tall as the playback buttons */
  KEY_STEP_MS = 5000,
};

static struct {
  void (*seek)(void *user, int ms);
  void *user;

  GtkWidget *picture, *title, *cur, *left, *bar;
  GCancellable *cover_loading;
  gboolean playing; /* there's a track */
  int duration_ms;
  gboolean pressed;      /* a pointer is down on the bar */
  gboolean seek_pending; /* the bar moved during that press */
  gboolean updating;     /* we're moving the bar, not the user */
} s;

static void format_time(int ms, char *out, size_t n) {
  int sec = ms > 0 ? ms / 1000 : 0;
  if (sec >= 3600) g_snprintf(out, n, "%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else g_snprintf(out, n, "%d:%02d", sec / 60, sec % 60);
}

/* "1:02" and "-5:21". Whole seconds, so the two always add up to the length. With no
 * length, both are "0:00". */
static void show_times(int pos_ms) {
  char cur[24], left[24] = "-";
  int total = (s.duration_ms + 500) / 1000 * 1000, pos = MIN(pos_ms, s.duration_ms) / 1000 * 1000;
  format_time(pos, cur, sizeof cur);
  format_time(total - pos, left + (total > 0), sizeof left - 1);
  gtk_label_set_text(GTK_LABEL(s.cur), cur);
  gtk_label_set_text(GTK_LABEL(s.left), left);
}

static gboolean seekable(void) {
  return s.playing && s.duration_ms > 0;
}

static void set_bar(int ms) {
  s.updating = TRUE;
  gtk_range_set_value(GTK_RANGE(s.bar), ms);
  s.updating = FALSE;
  show_times(ms);
}

/* The user moved the bar, by pointer or keyboard. A drag only seeks once it's let go. */
static gboolean on_change_value(GtkRange *range, GtkScrollType scroll, double value, gpointer user) {
  (void)range;
  (void)scroll;
  (void)user;
  if (s.updating || !seekable()) return TRUE;
  int ms = (int)CLAMP(value, 0, s.duration_ms);
  show_times(ms);
  if (s.pressed) s.seek_pending = TRUE;
  else if (s.seek) s.seek(s.user, ms);
  return FALSE; /* the bar moves as usual */
}

/* Sees presses and releases on the bar before its own gesture does. */
static gboolean on_bar_event(GtkEventControllerLegacy *controller, GdkEvent *event, gpointer user) {
  (void)controller;
  (void)user;
  switch (gdk_event_get_event_type(event)) {
    case GDK_BUTTON_PRESS:
    case GDK_TOUCH_BEGIN:
      s.pressed = TRUE;
      s.seek_pending = FALSE;
      break;
    case GDK_BUTTON_RELEASE:
    case GDK_TOUCH_END:
    case GDK_TOUCH_CANCEL:
      if (s.pressed && s.seek_pending && s.seek && seekable())
        s.seek(s.user, (int)gtk_range_get_value(GTK_RANGE(s.bar)));
      s.pressed = s.seek_pending = FALSE;
      break;
    default:
      break;
  }
  return FALSE;
}

static GtkWidget *time_label(void) {
  GtkWidget *l = gtk_label_new("0:00");
  gtk_widget_add_css_class(l, "numeric");
  gtk_widget_add_css_class(l, "caption");
  gtk_widget_add_css_class(l, "dim-label");
  return l;
}

GtkWidget *now_playing_new(void (*seek)(void *user, int ms), void *user) {
  memset(&s, 0, sizeof s);
  s.seek = seek;
  s.user = user;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_add_css_class(box, "now-playing");
  gtk_box_append(GTK_BOX(box), cover_frame_new(ART, 16, &s.picture));

  GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(right, TRUE);
  gtk_widget_set_valign(right, GTK_ALIGN_CENTER);
  s.title = gtk_label_new(NULL);
  gtk_label_set_ellipsize(GTK_LABEL(s.title), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(s.title, "caption");
  gtk_box_append(GTK_BOX(right), s.title);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  s.cur = time_label();
  gtk_label_set_xalign(GTK_LABEL(s.cur), 1);
  gtk_box_append(GTK_BOX(row), s.cur);
  s.bar = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, KEY_STEP_MS);
  gtk_range_set_increments(GTK_RANGE(s.bar), KEY_STEP_MS, 2 * KEY_STEP_MS);
  gtk_scale_set_draw_value(GTK_SCALE(s.bar), FALSE);
  gtk_widget_set_hexpand(s.bar, TRUE);
  gtk_widget_set_sensitive(s.bar, FALSE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.bar), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_SEEK), -1);
  g_signal_connect(s.bar, "change-value", G_CALLBACK(on_change_value), NULL);
  GtkEventController *legacy = gtk_event_controller_legacy_new();
  gtk_event_controller_set_propagation_phase(legacy, GTK_PHASE_CAPTURE);
  g_signal_connect(legacy, "event", G_CALLBACK(on_bar_event), NULL);
  gtk_widget_add_controller(s.bar, legacy);
  gtk_box_append(GTK_BOX(row), s.bar);
  s.left = time_label();
  gtk_label_set_xalign(GTK_LABEL(s.left), 0);
  gtk_box_append(GTK_BOX(row), s.left);
  gtk_box_append(GTK_BOX(right), row);
  gtk_box_append(GTK_BOX(box), right);

  now_playing_show(NULL, NULL, 0, NULL);
  return box;
}

static void cover_loaded(GdkTexture *texture, gpointer user) {
  (void)user;
  g_clear_object(&s.cover_loading);
  gtk_picture_set_paintable(GTK_PICTURE(s.picture), GDK_PAINTABLE(texture));
}

void now_playing_show(const char *title, const char *artist, int duration_ms, const char *cover_dir) {
  s.playing = title != NULL;
  s.duration_ms = title && duration_ms > 0 ? duration_ms : 0;
  s.pressed = s.seek_pending = FALSE;

  char *byline = artist ? mc_format_alloc(mc_str(MC_STR_NOW_PLAYING_ARTIST), MC_S(artist)) : NULL;
  char *markup = !title ? g_strdup("")
                 : byline ? g_markup_printf_escaped("<b>%s</b> <span alpha=\"60%%\">%s</span>", title, byline)
                          : g_markup_printf_escaped("<b>%s</b>", title);
  gtk_label_set_markup(GTK_LABEL(s.title), markup);
  g_free(markup);
  mc_free(byline);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.title), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 title ? title : mc_str(MC_STR_NOTHING_PLAYING), -1);

  /* Both times get room for the longest the track can show, so the bar doesn't move. */
  char widest[24] = "-";
  format_time(s.duration_ms, widest + 1, sizeof widest - 1);
  int chars = MAX((int)strlen(widest), 5);
  gtk_label_set_width_chars(GTK_LABEL(s.cur), chars);
  gtk_label_set_width_chars(GTK_LABEL(s.left), chars);

  s.updating = TRUE;
  gtk_range_set_range(GTK_RANGE(s.bar), 0, s.duration_ms > 0 ? s.duration_ms : 1);
  s.updating = FALSE;
  gtk_widget_set_sensitive(s.bar, seekable());
  set_bar(0);

  if (s.cover_loading) g_cancellable_cancel(s.cover_loading);
  g_clear_object(&s.cover_loading);
  GdkTexture *texture = NULL;
  if (title && cover_dir) {
    s.cover_loading = g_cancellable_new();
    texture = cover_get(cover_dir, ART * gtk_widget_get_scale_factor(s.picture), s.cover_loading, cover_loaded, NULL);
  }
  gtk_picture_set_paintable(GTK_PICTURE(s.picture), GDK_PAINTABLE(texture));
  g_clear_object(&texture);
}

void now_playing_set_position(int ms) {
  if (!s.pressed) set_bar(MAX(0, MIN(ms, s.duration_ms)));
}
