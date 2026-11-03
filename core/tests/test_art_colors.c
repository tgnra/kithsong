/* Art colours and the backdrop's glows (artcolors.h): grey art has no colours, a colour's
 * share follows how much of the art it is, much grey adds a neutral colour, and one loud
 * colour leaves a corner to a smaller one. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "musiccore/artcolors.h"
#include "test.h"

#define SIDE 128

/* SIDE x SIDE pixels: the first `first` rows in colour a, the rest in b (0xRRGGBB). */
static uint8_t *two_bands(uint32_t a, uint32_t b, int first) {
  uint8_t *p = malloc(SIDE * SIDE * 4);
  for (int y = 0; y < SIDE; y++)
    for (int x = 0; x < SIDE; x++) {
      uint32_t c = y < first ? a : b;
      uint8_t *px = p + (y * SIDE + x) * 4;
      /* A little noise, as art has, so the quantizer has more than one pixel to go on. */
      int jitter = (x * 7 + y * 13) % 5 - 2;
      for (int i = 0; i < 3; i++) {
        int v = (int)(c >> (16 - 8 * i) & 255) + jitter;
        px[i] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
      }
      px[3] = 255;
    }
  return p;
}

static int red(uint32_t c) { return c >> 16 & 255; }
static int green(uint32_t c) { return c >> 8 & 255; }
static int blue(uint32_t c) { return c & 255; }
static int is_blueish(uint32_t c) { return blue(c) > red(c) + 10 && blue(c) > green(c); }
static int is_orangeish(uint32_t c) { return red(c) > blue(c) + 10 && red(c) >= green(c); }
static int brightness(uint32_t c) { return red(c) + green(c) + blue(c); }

int main(void) {
  mc_art_color colors[MC_ART_COLORS_MAX];
  uint32_t glows[MC_ART_GLOWS];

  /* Bad arguments, and art with nothing opaque: no colours, no glows. */
  CHECK(mc_art_colors(NULL, SIDE, SIDE, SIDE * 4, colors) == 0);
  uint8_t *art = two_bands(0x2050C0, 0x2050C0, SIDE);
  CHECK(mc_art_colors(art, SIDE, SIDE, SIDE * 2, colors) == 0); /* stride too short */
  CHECK(mc_art_colors(art, 0, SIDE, SIDE * 4, colors) == 0);
  for (int i = 0; i < SIDE * SIDE; i++) art[i * 4 + 3] = 0;
  CHECK(mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors) == 0);
  free(art);
  CHECK(mc_art_glows(colors, 0, 0, glows) == 0);

  /* Grey only (a ramp from black to white): no colours, so a page keeps its own surface. */
  art = malloc(SIDE * SIDE * 4);
  for (int y = 0; y < SIDE; y++)
    for (int x = 0; x < SIDE; x++) {
      uint8_t *px = art + (y * SIDE + x) * 4;
      px[0] = px[1] = px[2] = (uint8_t)(x * 2);
      px[3] = 255;
    }
  CHECK(mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors) == 0);
  free(art);

  /* One colour: it, with the whole of the art, and every corner its, quieter each time. */
  art = two_bands(0x2050C0, 0x2050C0, SIDE);
  int n = mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors);
  CHECK(n == 1);
  CHECK(n >= 1 && is_blueish(colors[0].argb) && !colors[0].neutral && colors[0].share > 0.95f);
  CHECK(mc_art_glows(colors, n, 1, glows) == MC_ART_GLOWS);
  for (int i = 0; i < MC_ART_GLOWS; i++) CHECK(is_blueish(glows[i]) && glows[i] >> 24 == 204);
  free(art);

  /* The same art gives the same colours every time. */
  art = two_bands(0xE07020, 0x2050C0, SIDE / 3);
  n = mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors);
  mc_art_color again[MC_ART_COLORS_MAX];
  CHECK(mc_art_colors(art, SIDE, SIDE, SIDE * 4, again) == n && !memcmp(colors, again, sizeof(mc_art_color) * n));
  free(art);

  /* Mostly blue, a little orange: blue first, with the bigger share, but the orange still
   * gets a corner. */
  art = two_bands(0xE07020, 0x2050C0, SIDE * 15 / 100);
  n = mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors);
  CHECK(n == 2);
  if (n == 2) {
    CHECK(is_blueish(colors[0].argb) && is_orangeish(colors[1].argb));
    CHECK(colors[0].share > 0.8f && colors[1].share > 0.1f && colors[1].share < 0.2f);
  }
  CHECK(mc_art_glows(colors, n, 0, glows) == MC_ART_GLOWS);
  int orange = 0, blues = 0;
  for (int i = 0; i < MC_ART_GLOWS; i++) {
    orange += is_orangeish(glows[i]);
    blues += is_blueish(glows[i]);
  }
  CHECK(orange >= 1 && blues >= 2 && orange + blues == MC_ART_GLOWS);
  /* Blue's first two corners are across from each other: top left and bottom right. */
  CHECK(is_blueish(glows[0]) && is_blueish(glows[3]));

  /* Dark glows are dark and light ones light, whatever the art's own tone. */
  uint32_t dark[MC_ART_GLOWS];
  CHECK(mc_art_glows(colors, n, 1, dark) == MC_ART_GLOWS);
  for (int i = 0; i < MC_ART_GLOWS; i++) CHECK(brightness(dark[i]) < 300 && brightness(glows[i]) > 500);
  free(art);

  /* Half grey, half green: the green, then a neutral colour for the grey; grey gets no
   * corner while there's less of it than of the green. Shares are of the pixels WSMeans
   * keeps: as in Material's original, a cluster that lands on a colour another already has is
   * dropped with its pixels, so here the grey comes out under half. */
  art = two_bands(0x808080, 0x30A040, SIDE / 2);
  n = mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors);
  CHECK(n == 2);
  if (n == 2) {
    CHECK(!colors[0].neutral && colors[1].neutral);
    CHECK(colors[1].share >= 0.2f && colors[1].share < 0.6f);
  }
  CHECK(mc_art_glows(colors, n, 0, glows) == MC_ART_GLOWS);
  for (int i = 0; i < MC_ART_GLOWS; i++) CHECK(green(glows[i]) > red(glows[i]) + 5);
  free(art);

  /* Mostly grey, a little green: grey has one corner, the green the rest. */
  art = two_bands(0x808080, 0x30A040, SIDE * 3 / 4);
  n = mc_art_colors(art, SIDE, SIDE, SIDE * 4, colors);
  CHECK(n == 2 && colors[1].neutral);
  CHECK(mc_art_glows(colors, n, 0, glows) == MC_ART_GLOWS);
  int greys = 0;
  for (int i = 0; i < MC_ART_GLOWS; i++) greys += abs(red(glows[i]) - green(glows[i])) < 6;
  CHECK(greys == 1);
  free(art);

  /* The glows' centres, pulled in from each corner. */
  float x, y;
  mc_art_glow_center(0, &x, &y);
  CHECK(x < 0.5f && y < 0.5f);
  mc_art_glow_center(3, &x, &y);
  CHECK(x > 0.5f && y > 0.5f);
  return TEST_RESULT();
}
