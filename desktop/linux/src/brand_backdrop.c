#include "brand_backdrop.h"

#include "art_backdrop.h"
#include "kithcore/kithcore.h"

/* The logo, embedded by CMake (EmbedFile.cmake). */
extern const unsigned char kithsong_logo_png[];
extern const size_t kithsong_logo_png_len;

/* The logo's colours: found once, then kept. */
static mc_art_color logo_colors[MC_ART_COLORS_MAX];
static int logo_count = -1;

GdkTexture *brand_logo(void) {
  static GdkTexture *logo;
  static gboolean tried;
  if (tried) return logo;
  tried = TRUE;
  GBytes *png = g_bytes_new_static(kithsong_logo_png, kithsong_logo_png_len);
  GError *err = NULL;
  logo = gdk_texture_new_from_bytes(png, &err);
  g_bytes_unref(png);
  if (!logo) {
    g_warning("Couldn't decode the logo: %s", err ? err->message : "unknown error");
    g_clear_error(&err);
  }
  return logo;
}

/* At 128 pixels the logo is the size mc_art_colors() wants, so it's done here, on the UI
 * thread, rather than on a worker as art is: once, and quickly. */
static void find_logo_colors(void) {
  logo_count = 0;
  GdkTexture *texture = brand_logo();
  if (!texture) return;
  GdkTextureDownloader *down = gdk_texture_downloader_new(texture);
  gdk_texture_downloader_set_format(down, GDK_MEMORY_R8G8B8A8);
  gsize stride = 0;
  GBytes *pixels = gdk_texture_downloader_download_bytes(down, &stride);
  gdk_texture_downloader_free(down);
  logo_count = mc_art_colors(g_bytes_get_data(pixels, NULL), gdk_texture_get_width(texture),
                             gdk_texture_get_height(texture), stride, logo_colors);
  g_bytes_unref(pixels);
}

GtkWidget *brand_backdrop_new(GtkWidget *child) {
  if (logo_count < 0) find_logo_colors();
  GtkWidget *backdrop = art_backdrop_new();
  art_backdrop_set_child(ART_BACKDROP(backdrop), child);
  art_backdrop_set_colors(ART_BACKDROP(backdrop), logo_colors, logo_count);
  return backdrop;
}

void brand_backdrop_set_lit(GtkWidget *backdrop, gboolean lit) {
  /* Unlit is kept as data, so a page reloading while it stays empty doesn't fade again. */
  gboolean unlit = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(backdrop), "brand-unlit"));
  if ((!lit) == unlit) return;
  g_object_set_data(G_OBJECT(backdrop), "brand-unlit", GINT_TO_POINTER(!lit));
  art_backdrop_set_colors(ART_BACKDROP(backdrop), logo_colors, lit ? logo_count : 0);
}
