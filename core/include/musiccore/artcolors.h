/* The colours of a cover or picture, and the soft background a page draws from them: four
 * glows, one towards each corner, as Android's art backdrop has. The colours are found as
 * Android finds a wallpaper's (the Celebi quantizer, then Material's Score), so the first is
 * the art's seed colour, and each comes with its share of the art. Pure computation on pixels
 * the caller has decoded: no library, no files, safe from any thread. */
#ifndef MUSICCORE_ARTCOLORS_H
#define MUSICCORE_ARTCOLORS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* At most four colours of the art, then one neutral one. */
#define MC_ART_COLORS_MAX 5
/* A backdrop's glows: top left, top right, bottom left, bottom right. */
#define MC_ART_GLOWS 4

typedef struct mc_art_color {
  uint32_t argb; /* opaque, 0xAARRGGBB */
  /* How much of the art, from 0 to 1, is near the colour's hue or, for a neutral one, has
   * hardly any colour at all. */
  float share;
  int neutral; /* grey, black or white rather than a hue */
} mc_art_color;

/* The colours of width x height pixels, 4 bytes each in R, G, B, A order, stride bytes from
 * one row to the next; pixels that aren't opaque are left out. Scale the art to about 128
 * pixels a side first, as Android does: more costs time and finds the same colours. Up to
 * four colours, most prominent first, then, when at least a fifth of the art is grey, black
 * or white, one neutral colour for that. Returns how many are in out: 0 for art with no
 * colour but grey (or no opaque pixel), or for bad arguments. */
int mc_art_colors(const uint8_t *rgba, int width, int height, size_t stride,
                  mc_art_color out[MC_ART_COLORS_MAX]);

/* The backdrop's glow colours for count colours from mc_art_colors() (or several pieces of
 * art's, one after another, for a list: each art's then count by their shares), in the order
 * of MC_ART_GLOWS. Each colour gets corners by its share of the whole, but at most two while
 * there are others, and none if it's under a twentieth; near hues count as one, and grey gets
 * one corner at most, only when it's most of the art. A colour's corners are across from
 * each other where they can be, the colours taking theirs in the order they come, so a
 * list's top colours go to the top; a colour used again is quieter.
 *
 * Each colour is toned for the theme (dark or light) so that secondary text still reads on
 * it, its chroma tamed so it stays a tint. Its alpha is the glow's strength at its centre:
 * draw each as a radial gradient from that colour to fully clear, centred on
 * mc_art_glow_center()'s point, with a radius of 0.75 of the longer side. Returns
 * MC_ART_GLOWS, or 0 (out untouched) without colours. */
int mc_art_glows(const mc_art_color *colors, int count, int dark, uint32_t out[MC_ART_GLOWS]);

/* Where glow corner (0 to MC_ART_GLOWS - 1) centres, as fractions of the width (*x) and
 * height (*y): pulled in from the corner a little, so its brightest part is on screen. */
void mc_art_glow_center(int corner, float *x, float *y);

/* An accent colour from count colours from mc_art_colors(): the first that isn't grey, with
 * its hue and chroma kept, as Material's Content scheme keeps a seed's. *bg is for behind the
 * accent's white text (a suggested button, a slider's fill), with at least 4.5:1 contrast
 * against white; *text is for the accent as text or an icon on the theme's background (dark
 * or light), with at least 4.5:1 against it, from white to a light sidebar's grey or from
 * black to a dark sidebar's. Both opaque; either may be NULL. Returns 1, or 0 (outs
 * untouched) if none of the colours has colour enough to be an accent. */
int mc_art_accent(const mc_art_color *colors, int count, int dark, uint32_t *bg, uint32_t *text);

#ifdef __cplusplus
}
#endif

#endif
