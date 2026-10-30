#include "volume_button.h"

#include <math.h>
#include <string.h>

#include "app.h"
#include "musiccore/musiccore.h"
#include "playback.h"

enum {
  SLIDER_W = 200,
  KEY_STEP = 5,   /* the arrow keys, as the volume shortcuts */
  PAGE_STEP = 20, /* Page Up and Down */
};

static struct {
  void (*set_volume)(void *user, int volume);
  void (*set_muted)(void *user, gboolean muted);
  void *user;

  GtkWidget *button, *mute, *slider;
  gboolean updating; /* we're moving the slider or the toggle, not the user */
} s;

/* Muted (or at 0), then low, medium or high by thirds. */
static const char *icon_name(void) {
  int volume = playback_volume();
  if (playback_muted() || volume == 0) return "audio-volume-muted-symbolic";
  return volume < 34 ? "audio-volume-low-symbolic"
         : volume < 67 ? "audio-volume-medium-symbolic"
                       : "audio-volume-high-symbolic";
}

/* Moving it unmutes, even to the same level. It moves again while it does, which is no change. */
static void on_value_changed(GtkRange *range, gpointer user) {
  (void)user;
  int volume = (int)lround(gtk_range_get_value(range));
  if (!s.updating && s.set_volume && (volume != playback_volume() || playback_muted())) s.set_volume(s.user, volume);
}

static void on_mute_toggled(GtkToggleButton *button, gpointer user) {
  (void)user;
  if (!s.updating && s.set_muted) s.set_muted(s.user, gtk_toggle_button_get_active(button));
}

/* After the popover's own focus handling, which would pick the speaker. */
static void on_popover_map(GtkWidget *popover, gpointer user) {
  (void)popover;
  (void)user;
  gtk_widget_grab_focus(s.slider);
}

GtkWidget *volume_button_new(void (*set_volume)(void *user, int volume), void (*set_muted)(void *user, gboolean muted),
                             void *user) {
  memset(&s, 0, sizeof s);
  s.set_volume = set_volume;
  s.set_muted = set_muted;
  s.user = user;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  /* Doesn't take the focus when clicked, so the arrow keys still move the slider. */
  s.mute = gtk_toggle_button_new();
  gtk_widget_add_css_class(s.mute, "flat");
  gtk_widget_set_valign(s.mute, GTK_ALIGN_CENTER);
  gtk_widget_set_focus_on_click(s.mute, FALSE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.mute), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_MUTE), -1);
  g_signal_connect(s.mute, "toggled", G_CALLBACK(on_mute_toggled), NULL);
  gtk_box_append(GTK_BOX(box), s.mute);
  s.slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1);
  gtk_range_set_increments(GTK_RANGE(s.slider), KEY_STEP, PAGE_STEP);
  gtk_scale_set_draw_value(GTK_SCALE(s.slider), FALSE);
  gtk_widget_set_size_request(s.slider, SLIDER_W, -1);
  gtk_widget_set_valign(s.slider, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.slider), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_VOLUME), -1);
  g_signal_connect(s.slider, "value-changed", G_CALLBACK(on_value_changed), NULL);
  gtk_box_append(GTK_BOX(box), s.slider);

  GtkWidget *popover = gtk_popover_new();
  gtk_popover_set_child(GTK_POPOVER(popover), box);
  g_signal_connect_after(popover, "map", G_CALLBACK(on_popover_map), NULL);

  s.button = gtk_menu_button_new();
  gtk_menu_button_set_popover(GTK_MENU_BUTTON(s.button), popover);
  app_set_tooltip(s.button, mc_str(MC_STR_VOLUME), NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(s.button), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_VOLUME), -1);
  volume_button_update();
  return s.button;
}

void volume_button_update(void) {
  if (!s.button) return;
  const char *icon = icon_name();
  gboolean muted = playback_muted();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(s.button), icon);
  gtk_button_set_icon_name(GTK_BUTTON(s.mute), icon);
  app_set_tooltip(s.mute, mc_str(muted ? MC_STR_UNMUTE : MC_STR_MUTE), "<Control>m");
  s.updating = TRUE;
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s.mute), muted);
  gtk_range_set_value(GTK_RANGE(s.slider), playback_volume());
  s.updating = FALSE;
}
