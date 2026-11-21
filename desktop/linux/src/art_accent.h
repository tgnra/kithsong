/* An accent colour from art, as Android's art theme takes one: a widget, and everything in
 * it, gets the art's first colour that isn't grey (mc_art_accent(), artcolors.h) in place of
 * the desktop's accent, toned so white reads on it and it reads on the theme's background,
 * at 4.5:1 or more, in light and dark alike. The suggested button, the .accent labels, a
 * scale's fill and a checked .shuffle toggle take it; with libadwaita 1.6 and GTK 4.16 or
 * later, so does anything else drawn in the accent (focus rings, drop indicators), through
 * its --accent-* variables. Without art, for art with no colour but grey, and in
 * high-contrast mode, the widget keeps the desktop's accent.
 *
 * The colours come from art_colors_find() (art_backdrop.h), so art the backdrop has seen
 * colours the accent at once; other art changes it when its colours are found. A change
 * between light and dark, or into or out of high contrast, follows at once. Call everything
 * on the UI thread. */
#ifndef KITHSONG_ART_ACCENT_H
#define KITHSONG_ART_ACCENT_H

#include <gtk/gtk.h>

/* The art to take widget's accent from: an image file (a cover, or a playlist's picture),
 * or NULL for the desktop's accent again. */
void art_accent_set_file(GtkWidget *widget, const char *path);

#endif
