/* Cover pictures for the ArtworkDB: JPEG/PNG in, RGB565 out. */
#include <stdlib.h>
#include <string.h>

#include <stb_image.h>

#include "kithipod/artwork.h"
#include "util.h"

int ipod_image_to_rgb565(const uint8_t *image, size_t len, int w, int h, uint8_t *out) {
  if (!image || len == 0 || len > 0x7fffffff || w <= 0 || h <= 0) return IPOD_ERR_ARG;
  int sw, sh, comp;
  unsigned char *px = stbi_load_from_memory(image, (int)len, &sw, &sh, &comp, 3);
  if (!px) return IPOD_ERR_FORMAT;
  /* Centre-crop the source to the target's aspect ratio. */
  double sx0 = 0, sy0 = 0, cw = sw, ch = sh;
  double want = (double)w / h;
  if ((double)sw / sh > want) {
    cw = sh * want;
    sx0 = (sw - cw) / 2;
  } else {
    ch = sw / want;
    sy0 = (sh - ch) / 2;
  }
  double fx = cw / w, fy = ch / h;
  for (int y = 0; y < h; y++) {
    double y0 = sy0 + y * fy, y1 = y0 + fy;
    int iy0 = (int)y0, iy1 = (int)(y1 + 0.999999);
    if (iy1 <= iy0) iy1 = iy0 + 1;
    if (iy1 > sh) iy1 = sh;
    if (iy0 >= sh) iy0 = sh - 1;
    for (int x = 0; x < w; x++) {
      double x0 = sx0 + x * fx, x1 = x0 + fx;
      int ix0 = (int)x0, ix1 = (int)(x1 + 0.999999);
      if (ix1 <= ix0) ix1 = ix0 + 1;
      if (ix1 > sw) ix1 = sw;
      if (ix0 >= sw) ix0 = sw - 1;
      /* Box filter over the covered source pixels, each weighted by its overlap. */
      double r = 0, g = 0, b = 0, wsum = 0;
      for (int yy = iy0; yy < iy1; yy++) {
        double wy = 1.0;
        if (fy >= 1.0) {
          double top = yy < y0 ? y0 : yy, bot = yy + 1 > y1 ? y1 : yy + 1;
          wy = bot - top;
          if (wy <= 0) continue;
        }
        for (int xx = ix0; xx < ix1; xx++) {
          double wx = 1.0;
          if (fx >= 1.0) {
            double l = xx < x0 ? x0 : xx, rr = xx + 1 > x1 ? x1 : xx + 1;
            wx = rr - l;
            if (wx <= 0) continue;
          }
          const unsigned char *p = px + ((size_t)yy * sw + xx) * 3;
          double wt = wx * wy;
          r += p[0] * wt;
          g += p[1] * wt;
          b += p[2] * wt;
          wsum += wt;
        }
      }
      if (wsum <= 0) wsum = 1;
      unsigned ri = (unsigned)(r / wsum + 0.5), gi = (unsigned)(g / wsum + 0.5), bi = (unsigned)(b / wsum + 0.5);
      if (ri > 255) ri = 255;
      if (gi > 255) gi = 255;
      if (bi > 255) bi = 255;
      uint16_t v = (uint16_t)(((ri >> 3) << 11) | ((gi >> 2) << 5) | (bi >> 3));
      ipod_wr16(out + ((size_t)y * w + x) * 2, v);
    }
  }
  stbi_image_free(px);
  return 0;
}
