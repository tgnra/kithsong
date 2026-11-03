#include "art_accent.h"

#include <adwaita.h>
#include <string.h>

#include "art_backdrop.h"

/* A widget's accent: its own class, and a stylesheet for the display that colours only
 * what's under that class, rewritten as the art or the theme changes. */
typedef struct accent {
  GtkWidget *widget; /* not a reference: the accent is the widget's data */
  GtkCssProvider *provider;
  char css_class[32];
  char *path;
  GCancellable *finding;
  mc_art_color colors[MC_ART_COLORS_MAX];
  int count;
} accent;

static const char KEY[] = "kithsong-art-accent";

/* libadwaita 1.6 and GTK 4.16 draw the accent from CSS variables, which reach everything
 * under the class; before that only the rules for the widgets named below do. */
static gboolean has_variables(void) {
  return (adw_get_major_version() > 1 || adw_get_minor_version() >= 6) &&
         (gtk_get_major_version() > 4 || gtk_get_minor_version() >= 16);
}

static void hex(char out[8], uint32_t argb) {
  g_snprintf(out, 8, "#%06X", argb & 0xFFFFFFu);
}

static void restyle(accent *a) {
  AdwStyleManager *style = adw_style_manager_get_default();
  uint32_t bg = 0, text = 0;
  if (adw_style_manager_get_high_contrast(style) ||
      !mc_art_accent(a->colors, a->count, adw_style_manager_get_dark(style), &bg, &text)) {
    gtk_css_provider_load_from_string(a->provider, "");
    return;
  }
  char b[8], t[8];
  hex(b, bg);
  hex(t, text);
  const char *c = a->css_class;
  GString *css = g_string_new(NULL);
  if (has_variables())
    g_string_append_printf(css, ".%s { --accent-bg-color: %s; --accent-fg-color: #FFFFFF; --accent-color: %s; }\n", c, b, t);
  /* Named as well, for libadwaita 1.5, and so the app's own @accent_color rules follow. */
  g_string_append_printf(css,
                         ".%1$s button.suggested-action { background-color: %2$s; color: #FFFFFF; }\n"
                         ".%1$s scale > trough > highlight { background-color: %2$s; }\n"
                         ".%1$s .accent, .%1$s .shuffle:checked { color: %3$s; }\n",
                         c, b, t);
  gtk_css_provider_load_from_string(a->provider, css->str);
  g_string_free(css, TRUE);
}

static void on_found(const mc_art_color *colors, int count, gpointer user) {
  accent *a = user;
  a->count = MIN(count, MC_ART_COLORS_MAX);
  if (a->count > 0) memcpy(a->colors, colors, sizeof *colors * (size_t)a->count);
  restyle(a);
}

static void on_style_changed(accent *a) {
  restyle(a);
}

static void accent_free(gpointer data) {
  accent *a = data;
  if (a->finding) g_cancellable_cancel(a->finding);
  g_clear_object(&a->finding);
  g_signal_handlers_disconnect_by_data(adw_style_manager_get_default(), a);
  gtk_style_context_remove_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(a->provider));
  g_object_unref(a->provider);
  g_free(a->path);
  g_free(a);
}

static accent *accent_of(GtkWidget *widget) {
  accent *a = g_object_get_data(G_OBJECT(widget), KEY);
  if (a) return a;
  static unsigned next;
  a = g_new0(accent, 1);
  a->widget = widget;
  g_snprintf(a->css_class, sizeof a->css_class, "art-accent-%u", ++next);
  gtk_widget_add_css_class(widget, a->css_class);
  a->provider = gtk_css_provider_new();
  /* Over the app's own stylesheet, which uses the desktop's accent. */
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(a->provider),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
  AdwStyleManager *style = adw_style_manager_get_default();
  g_signal_connect_swapped(style, "notify::dark", G_CALLBACK(on_style_changed), a);
  g_signal_connect_swapped(style, "notify::high-contrast", G_CALLBACK(on_style_changed), a);
  g_object_set_data_full(G_OBJECT(widget), KEY, a, accent_free);
  return a;
}

void art_accent_set_file(GtkWidget *widget, const char *path) {
  accent *a = accent_of(widget);
  if (!g_strcmp0(path, a->path)) return;
  g_free(a->path);
  a->path = g_strdup(path);
  if (a->finding) g_cancellable_cancel(a->finding);
  g_clear_object(&a->finding);
  if (path && art_colors_cached(path, a->colors, &a->count)) {
    restyle(a);
    return;
  }
  /* The last art's accent stays until the new art's colours are found, as on Android. */
  a->count = 0;
  if (!path) {
    restyle(a);
    return;
  }
  a->finding = g_cancellable_new();
  art_colors_find(path, a->finding, on_found, a);
}
