/* Drawing into a 32-bit premultiplied-alpha bitmap, for what GDI can't do: anti-aliased
 * rounded shapes, translucent fills, and anything over Mica, where every pixel's alpha
 * matters and GDI would leave it at 0 (transparent).
 *
 * GDI can still draw into the canvas's DC, e.g. ClearType text and cover art on surfaces
 * that are opaque anyway; canvas_opaque then sets the alpha it left at 0. Text over
 * transparent pixels goes through canvas_text instead.
 */
#ifndef MUSICAPP_CANVAS_H
#define MUSICAPP_CANVAS_H

#include <stdint.h>
#include <windows.h>

typedef struct canvas {
  HDC dc;       /* GDI drawing into the pixels */
  uint32_t *px; /* premultiplied BGRA, top-down rows of w pixels */
  int w, h;
  HBITMAP bmp;
  HGDIOBJ old;
} canvas;

enum { CORNER_TL = 1, CORNER_TR = 2, CORNER_BL = 4, CORNER_BR = 8, CORNER_ALL = 15 };

/* Makes the canvas at least w x h. It only grows; contents are undefined after it does. */
BOOL canvas_reserve(canvas *c, int w, int h);
void canvas_free(canvas *c);

/* Sets every pixel in r to color at alpha (0 is fully transparent). */
void canvas_clear(canvas *c, const RECT *r, COLORREF color, int alpha);
/* Draws color at alpha over r. radius > 0 rounds the corners. */
void canvas_fill(canvas *c, const RECT *r, int radius, COLORREF color, int alpha);
/* An outline width pixels wide, just inside r. */
void canvas_frame(canvas *c, const RECT *r, int radius, int width, COLORREF color, int alpha);
/* Rounds off the given corners of whatever is in r: the pixels outside the rounded shape
 * become color at alpha (alpha 0 cuts them out). */
void canvas_round_corners(canvas *c, const RECT *r, int radius, int corners, COLORREF color, int alpha);
/* Draws a w x h premultiplied, top-down image over r, scaled (roughly) if the sizes differ. */
void canvas_draw_image(canvas *c, const RECT *r, const uint32_t *image, int w, int h);
/* DrawText with correct alpha (grayscale anti-aliasing), so it works over transparency. */
void canvas_text(canvas *c, const wchar_t *s, int n, const RECT *r, UINT format, HFONT font,
                 COLORREF color);
/* Sets alpha to 255 in r, after GDI drew opaque things there. */
void canvas_opaque(canvas *c, const RECT *r);
/* Copies r of the canvas to dc at (x, y), alpha included. */
void canvas_blit(canvas *c, HDC dc, int x, int y, const RECT *r);

/* Call when the visual style changes. */
void canvas_theme_changed(void);

#endif
