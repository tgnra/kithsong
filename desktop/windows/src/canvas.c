#include "canvas.h"

#include <math.h>
#include <uxtheme.h>

static HTHEME text_theme; /* any theme handle will do for DrawThemeTextEx */

BOOL canvas_reserve(canvas *c, int w, int h) {
  if (c->dc && c->w >= w && c->h >= h) return TRUE;
  if (w < c->w) w = c->w;
  if (h < c->h) h = c->h;
  if (w < 1) w = 1;
  if (h < 1) h = 1;
  canvas_free(c);

  BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = w,
                                 .biHeight = -h, /* top-down */
                                 .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
  void *bits = NULL;
  HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  HDC dc = bmp ? CreateCompatibleDC(NULL) : NULL;
  if (!dc) {
    if (bmp) DeleteObject(bmp);
    return FALSE;
  }
  c->dc = dc;
  c->bmp = bmp;
  c->old = SelectObject(dc, bmp);
  c->px = bits;
  c->w = w;
  c->h = h;
  SetBkMode(dc, TRANSPARENT);
  return TRUE;
}

void canvas_free(canvas *c) {
  if (c->dc) {
    SelectObject(c->dc, c->old);
    DeleteDC(c->dc);
    DeleteObject(c->bmp);
  }
  *c = (canvas){0};
}

/* r clipped to the canvas, after letting GDI finish drawing into it. */
static BOOL clip(const canvas *c, const RECT *r, RECT *out) {
  RECT all = {0, 0, c->w, c->h};
  if (!c->px || !IntersectRect(out, r, &all)) return FALSE;
  GdiFlush();
  return TRUE;
}

static uint32_t premultiply(COLORREF color, int alpha) {
  uint32_t a = (uint32_t)alpha;
  return a << 24 | (GetRValue(color) * a + 127) / 255 << 16 | (GetGValue(color) * a + 127) / 255 << 8 |
         (GetBValue(color) * a + 127) / 255;
}

/* Each channel of p times k / 255. */
static uint32_t scale_px(uint32_t p, uint32_t k) {
  uint32_t rb = (p & 0x00FF00FF) * k + 0x00800080, ag = (p >> 8 & 0x00FF00FF) * k + 0x00800080;
  rb = (rb + (rb >> 8 & 0x00FF00FF)) >> 8 & 0x00FF00FF;
  ag = (ag + (ag >> 8 & 0x00FF00FF)) & 0xFF00FF00;
  return rb | ag;
}

/* src over dst, src scaled by coverage k (0-255). */
static uint32_t over(uint32_t dst, uint32_t src, uint32_t k) {
  if (k < 255) src = scale_px(src, k);
  return src + scale_px(dst, 255 - (src >> 24));
}

/* dst moved towards src by k (0-255). */
static uint32_t lerp(uint32_t dst, uint32_t src, uint32_t k) {
  return scale_px(src, k) + scale_px(dst, 255 - k);
}

/* Coverage (0-255) of pixel (x, y) by r with corners of the given radius. */
static uint32_t coverage(int x, int y, const RECT *r, int radius) {
  if (x < r->left || x >= r->right || y < r->top || y >= r->bottom) return 0;
  float px = (float)x + 0.5f, py = (float)y + 0.5f, cx, cy;
  if (px < (float)(r->left + radius)) cx = (float)(r->left + radius);
  else if (px > (float)(r->right - radius)) cx = (float)(r->right - radius);
  else return 255;
  if (py < (float)(r->top + radius)) cy = (float)(r->top + radius);
  else if (py > (float)(r->bottom - radius)) cy = (float)(r->bottom - radius);
  else return 255;
  float d = (float)radius + 0.5f - hypotf(px - cx, py - cy);
  return d >= 1.0f ? 255 : d <= 0.0f ? 0 : (uint32_t)(d * 255.0f);
}

static int clamp_radius(const RECT *r, int radius) {
  int w = r->right - r->left, h = r->bottom - r->top, max = (w < h ? w : h) / 2;
  return radius < 0 ? 0 : radius > max ? max : radius;
}

void canvas_clear(canvas *c, const RECT *r, COLORREF color, int alpha) {
  RECT rc;
  if (!clip(c, r, &rc)) return;
  uint32_t v = premultiply(color, alpha);
  for (int y = rc.top; y < rc.bottom; y++) {
    uint32_t *row = c->px + (size_t)y * (size_t)c->w;
    for (int x = rc.left; x < rc.right; x++) row[x] = v;
  }
}

void canvas_fill(canvas *c, const RECT *r, int radius, COLORREF color, int alpha) {
  RECT rc;
  if (alpha <= 0 || !clip(c, r, &rc)) return;
  radius = clamp_radius(r, radius);
  uint32_t v = premultiply(color, alpha);
  for (int y = rc.top; y < rc.bottom; y++) {
    uint32_t *row = c->px + (size_t)y * (size_t)c->w;
    for (int x = rc.left; x < rc.right; x++) {
      uint32_t k = radius ? coverage(x, y, r, radius) : 255;
      if (k) row[x] = over(row[x], v, k);
    }
  }
}

void canvas_frame(canvas *c, const RECT *r, int radius, int width, COLORREF color, int alpha) {
  RECT rc;
  if (alpha <= 0 || width <= 0 || !clip(c, r, &rc)) return;
  radius = clamp_radius(r, radius);
  RECT inner = *r;
  InflateRect(&inner, -width, -width);
  int inner_radius = radius > width ? radius - width : 0;
  uint32_t v = premultiply(color, alpha);
  for (int y = rc.top; y < rc.bottom; y++) {
    uint32_t *row = c->px + (size_t)y * (size_t)c->w;
    /* Rows through the straight sides only need the two edges. */
    BOOL sides = y >= inner.top + inner_radius && y < inner.bottom - inner_radius;
    int skip_from = sides ? inner.left + inner_radius : rc.right, skip_to = sides ? inner.right - inner_radius : rc.right;
    for (int x = rc.left; x < rc.right; x++) {
      if (x >= skip_from && x < skip_to) x = skip_to;
      if (x >= rc.right) break;
      uint32_t outer = coverage(x, y, r, radius), in = coverage(x, y, &inner, inner_radius);
      if (outer > in) row[x] = over(row[x], v, outer - in);
    }
  }
}

void canvas_draw_image(canvas *c, const RECT *r, const uint32_t *image, int w, int h) {
  RECT rc;
  int rw = r->right - r->left, rh = r->bottom - r->top;
  if (!image || w <= 0 || h <= 0 || rw <= 0 || rh <= 0 || !clip(c, r, &rc)) return;
  for (int y = rc.top; y < rc.bottom; y++) {
    uint32_t *row = c->px + (size_t)y * (size_t)c->w;
    const uint32_t *src = image + (size_t)((y - r->top) * h / rh) * (size_t)w;
    for (int x = rc.left; x < rc.right; x++) {
      uint32_t s = src[rw == w ? x - r->left : (x - r->left) * w / rw];
      row[x] = s >> 24 == 255 ? s : over(row[x], s, 255);
    }
  }
}

void canvas_round_corners(canvas *c, const RECT *r, int radius, int corners, COLORREF color, int alpha) {
  radius = clamp_radius(r, radius);
  if (!radius) return;
  uint32_t v = premultiply(color, alpha);
  for (int i = 0; i < 4; i++) {
    if (!(corners & 1 << i)) continue;
    int x0 = i & 1 ? r->right - radius : r->left, y0 = i & 2 ? r->bottom - radius : r->top;
    RECT corner = {x0, y0, x0 + radius, y0 + radius}, rc;
    if (!clip(c, &corner, &rc)) continue;
    for (int y = rc.top; y < rc.bottom; y++) {
      uint32_t *row = c->px + (size_t)y * (size_t)c->w;
      for (int x = rc.left; x < rc.right; x++) {
        uint32_t k = 255 - coverage(x, y, r, radius);
        if (k) row[x] = lerp(row[x], v, k);
      }
    }
  }
}

void canvas_text(canvas *c, const wchar_t *s, int n, const RECT *r, UINT format, HFONT font,
                 COLORREF color) {
  if (!c->dc) return;
  if (!text_theme) text_theme = OpenThemeData(NULL, L"WINDOW");
  HGDIOBJ old = SelectObject(c->dc, font);
  RECT rc = *r;
  if (text_theme) {
    DTTOPTS opts = {.dwSize = sizeof opts, .dwFlags = DTT_COMPOSITED | DTT_TEXTCOLOR, .crText = color};
    DrawThemeTextEx(text_theme, c->dc, 0, 0, s, n, format, &rc, &opts);
  } else { /* no visual styles: plain GDI, which is only right on opaque canvases */
    SetTextColor(c->dc, color);
    DrawTextW(c->dc, s, n, &rc, format);
  }
  SelectObject(c->dc, old);
}

void canvas_opaque(canvas *c, const RECT *r) {
  RECT rc;
  if (!clip(c, r, &rc)) return;
  for (int y = rc.top; y < rc.bottom; y++) {
    uint32_t *row = c->px + (size_t)y * (size_t)c->w;
    for (int x = rc.left; x < rc.right; x++) row[x] |= 0xFF000000u;
  }
}

void canvas_blit(canvas *c, HDC dc, int x, int y, const RECT *r) {
  if (c->dc) BitBlt(dc, x, y, r->right - r->left, r->bottom - r->top, c->dc, r->left, r->top, SRCCOPY);
}

void canvas_theme_changed(void) {
  if (text_theme) CloseThemeData(text_theme);
  text_theme = NULL;
}
