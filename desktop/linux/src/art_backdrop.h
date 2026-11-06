/* A background of soft colour from art, as Android's art backdrop and the site have: four
 * glows, one towards each corner, in the art's colours (mc_art_glows(), artcolors.h), drawn
 * behind the backdrop's child. Each is toned so the theme's text reads on it, in light and
 * dark alike; without art, or with art that has no colour but grey, it's clear, for the
 * surface beneath. In high-contrast mode it's always clear.
 *
 *   +--------------------------------+
 *   | (glow)                 (glow)  |
 *   |        child, drawn on top     |
 *   | (glow)                 (glow)  |
 *   +-- thinning out at the bottom --+
 *
 * The colours are found off the UI thread, from the art scaled down to 128 pixels, and
 * remembered (the last 512 pieces of art), so art seen before colours the backdrop at once.
 * Shown, or within a moment of being shown, it takes its colours at once; colours that come
 * later than that arrive in a quick fade, and a change of colours fades over most of a
 * second. Those fades follow the desktop's animation setting. A change between light and dark
 * re-tones the colours at once. Call everything on the UI thread. */
#ifndef KITHSONG_ART_BACKDROP_H
#define KITHSONG_ART_BACKDROP_H

#include <gtk/gtk.h>

#include "kithcore/kithcore.h"

G_BEGIN_DECLS

#define ART_TYPE_BACKDROP (art_backdrop_get_type())
G_DECLARE_FINAL_TYPE(ArtBackdrop, art_backdrop, ART, BACKDROP, GtkWidget)

GtkWidget *art_backdrop_new(void);

/* The widget drawn over the glows, which sizes the backdrop; NULL for none. */
void art_backdrop_set_child(ArtBackdrop *self, GtkWidget *child);

/* How much of the height, from 0 to 1, the glows thin out to clear over at the bottom, so
 * the backdrop can end above a list without an edge. 0 (the default) for none. */
void art_backdrop_set_fade_bottom(ArtBackdrop *self, double share);

/* The art to take colours from: an image file (a cover, or a playlist's picture), an album
 * folder's cover (cover_path()), or several image files at once, each counting the same and
 * their colours in order, for a page of several albums. NULL, or none, clears it. */
void art_backdrop_set_file(ArtBackdrop *self, const char *path);
void art_backdrop_set_dir(ArtBackdrop *self, const char *cover_dir);
void art_backdrop_set_files(ArtBackdrop *self, const char *const *paths, int count);

/* Art that isn't a file, such as the logo: its colours are found afresh each time, not
 * remembered. NULL clears it. */
void art_backdrop_set_texture(ArtBackdrop *self, GdkTexture *texture);

/* Colours ready-made, for a caller that has found them itself (art_colors_find()). */
void art_backdrop_set_colors(ArtBackdrop *self, const mc_art_color *colors, int count);

/* --- The colours themselves, for other uses of them (an accent colour, say) --- */

/* Called on the UI thread with path's colours (count may be 0). */
typedef void (*art_colors_done)(const mc_art_color *colors, int count, gpointer user);

/* path's colours if they've been found since the file last changed: TRUE, with them in out
 * and *count. */
gboolean art_colors_cached(const char *path, mc_art_color out[MC_ART_COLORS_MAX], int *count);

/* Finds path's colours (at once if cached) and calls done with them, unless cancellable is
 * cancelled first. They're remembered either way. */
void art_colors_find(const char *path, GCancellable *cancellable, art_colors_done done, gpointer user);

/* A texture's colours, found off the UI thread and not remembered. */
void art_colors_find_texture(GdkTexture *texture, GCancellable *cancellable, art_colors_done done, gpointer user);

void art_colors_cache_free(void);

G_END_DECLS

#endif
