#define COBJMACROS
#include "album_grid.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include <windowsx.h>

#include "canvas.h"
#include "theme.h"

#define WM_APP_THUMB (WM_APP + 20) /* lParam: thumb_result *, from the loader thread */
#define SCROLL_TIMER 1
#define SCROLLBAR_TIMER 2 /* hides the scrollbar a while after the mouse stops */

/* Metrics at 96 DPI. */
enum {
  COVER_RADIUS = 6,
  HOVER_PAD = 8, /* hover fill around the cover and title */
  RING_GAP = 2,  /* selection ring: gap from the cover, and width */
  RING_W = 2,
  BAR_THIN = 3, /* scrollbar width at rest, and under the mouse */
  BAR_WIDE = 6,
  BAR_MARGIN = 3,
  BAR_HIT = 14, /* how close to the right edge the mouse takes the scrollbar */
  BAR_MIN = 32, /* shortest thumb */
};

typedef struct tile {
  char uid[MC_UID_STRLEN];
  wchar_t *title;
  wchar_t *cover_dir; /* NULL when the album has no cover */
  HBITMAP thumb;      /* premultiplied, with rounded corners */
  const uint32_t *pixels;
  int thumb_size;
  BOOL failed; /* the cover couldn't be decoded; draw the placeholder */
} tile;

typedef struct load_job {
  int index;
  int size;
  int radius;
} load_job;

typedef struct thumb_result {
  int index;
  int size;
  UINT gen;
  HBITMAP bmp; /* NULL if decoding failed */
  const uint32_t *pixels;
} thumb_result;

static struct {
  HWND wnd;
  mc_library *lib;
  BOOL dirty; /* reload when next shown */
  tile *tiles;
  int count;
  UINT gen; /* bumped on every reload, so late thumbnails for old tiles are dropped */
  HFONT font, glyph_font;

  /* Layout, in device pixels. */
  int cols, tile_w, row_h, pad, gap, text_gap, text_h;
  int client_w, client_h, content_h;
  int scroll_y, target_y;
  BOOL animating;

  int hot;                          /* tile under the mouse, or -1 */
  int selected;                     /* or -1 */
  char selected_uid[MC_UID_STRLEN]; /* keeps the selection across reloads */
  BOOL tracking;                    /* asked for WM_MOUSELEAVE */
  BOOL bar_shown, bar_hot, dragging;
  int drag_dy; /* where on the thumb it was grabbed */

  canvas buf;
  int loaded; /* thumbnails in memory */

  /* Loader thread. The UI thread replaces the queue whenever the view moves; the loader
   * takes jobs from the front. Tiles are only touched on the UI thread. */
  HANDLE thread;
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE wake;
  load_job *queue;
  int queue_len, queue_cap;
  int busy_index; /* job the loader is working on, or -1 */
  UINT busy_gen;
  UINT loader_gen; /* gen the queued jobs belong to */
  wchar_t **dirs;  /* snapshot of cover folders for the loader, per gen */
  int dir_count;
  BOOL quit;
} g;

static int scale(int px) {
  return MulDiv(px, (int)GetDpiForWindow(g.wnd), USER_DEFAULT_SCREEN_DPI);
}

static wchar_t *widen(const char *s) {
  if (!s) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

/* --- Cover decoding (loader thread) --- */

/* Decodes dir\cover.jpg (or .png) to a size x size premultiplied DIB: scaled so the short
 * side fits, centre-cropped, and with rounded corners. WIC's JPEG decoder does most of the
 * downscaling while decoding, which is what makes this fast. */
static HBITMAP decode_cover(IWICImagingFactory *wic, const wchar_t *dir, int size, int radius,
                            const uint32_t **pixels) {
  wchar_t path[MAX_PATH * 2];
  static const wchar_t *const names[] = {L"cover.jpg", L"cover.png"};
  BOOL found = FALSE;
  for (int i = 0; i < 2 && !found; i++) {
    found = swprintf(path, MAX_PATH * 2, L"%ls\\%ls", dir, names[i]) > 0 &&
            GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
  }
  if (!found || size <= 0) return NULL;

  IWICBitmapDecoder *decoder = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICBitmapScaler *scaler = NULL;
  IWICBitmapClipper *clipper = NULL;
  IWICFormatConverter *converter = NULL;
  HBITMAP bmp = NULL;
  UINT w = 0, h = 0;

  if (FAILED(IWICImagingFactory_CreateDecoderFromFilename(wic, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &decoder)) ||
      FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) ||
      FAILED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) || !w || !h)
    goto done;

  double s = (double)size / (double)(w < h ? w : h);
  UINT sw = (UINT)fmax(size, floor(w * s + 0.5)), sh = (UINT)fmax(size, floor(h * s + 0.5));
  WICRect crop = {(INT)(sw - (UINT)size) / 2, (INT)(sh - (UINT)size) / 2, size, size};
  if (FAILED(IWICImagingFactory_CreateBitmapScaler(wic, &scaler)) ||
      FAILED(IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)frame, sw, sh,
                                         WICBitmapInterpolationModeFant)) ||
      FAILED(IWICImagingFactory_CreateBitmapClipper(wic, &clipper)) ||
      FAILED(IWICBitmapClipper_Initialize(clipper, (IWICBitmapSource *)scaler, &crop)) ||
      FAILED(IWICImagingFactory_CreateFormatConverter(wic, &converter)) ||
      FAILED(IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)clipper,
                                            &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone,
                                            NULL, 0.0, WICBitmapPaletteTypeCustom)))
    goto done;

  BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = size,
                                 .biHeight = -size, /* top-down */
                                 .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
  void *bits = NULL;
  bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  UINT stride = (UINT)size * 4;
  if (!bmp || FAILED(IWICFormatConverter_CopyPixels(converter, NULL, stride, stride * (UINT)size,
                                                    (BYTE *)bits))) {
    if (bmp) DeleteObject(bmp);
    bmp = NULL;
    goto done;
  }
  canvas corners = {.px = bits, .w = size, .h = size};
  RECT all = {0, 0, size, size};
  canvas_round_corners(&corners, &all, radius, CORNER_ALL, 0, 0);
  *pixels = bits;

done:
  if (converter) IWICFormatConverter_Release(converter);
  if (clipper) IWICBitmapClipper_Release(clipper);
  if (scaler) IWICBitmapScaler_Release(scaler);
  if (frame) IWICBitmapFrameDecode_Release(frame);
  if (decoder) IWICBitmapDecoder_Release(decoder);
  return bmp;
}

static DWORD WINAPI loader_main(void *arg) {
  (void)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  IWICImagingFactory *wic = NULL;
  CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                   (void **)&wic);

  for (;;) {
    EnterCriticalSection(&g.lock);
    g.busy_index = -1;
    while (!g.quit && g.queue_len == 0) SleepConditionVariableCS(&g.wake, &g.lock, INFINITE);
    if (g.quit) {
      LeaveCriticalSection(&g.lock);
      break;
    }
    load_job job = g.queue[0];
    memmove(g.queue, g.queue + 1, (size_t)(--g.queue_len) * sizeof *g.queue);
    UINT gen = g.loader_gen;
    /* Copy the folder: the UI thread may swap the snapshot as soon as we let go. */
    wchar_t *dir = job.index < g.dir_count && g.dirs[job.index] ? _wcsdup(g.dirs[job.index]) : NULL;
    g.busy_index = job.index;
    g.busy_gen = gen;
    LeaveCriticalSection(&g.lock);

    thumb_result *r = malloc(sizeof *r);
    if (!r) {
      free(dir);
      continue;
    }
    r->index = job.index;
    r->size = job.size;
    r->gen = gen;
    r->pixels = NULL;
    r->bmp = wic && dir ? decode_cover(wic, dir, job.size, job.radius, &r->pixels) : NULL;
    free(dir);
    if (!PostMessageW(g.wnd, WM_APP_THUMB, 0, (LPARAM)r)) {
      if (r->bmp) DeleteObject(r->bmp);
      free(r);
    }
  }
  if (wic) IWICImagingFactory_Release(wic);
  CoUninitialize();
  return 0;
}

/* --- Layout --- */

static int max_scroll(void) {
  return g.content_h > g.client_h ? g.content_h - g.client_h : 0;
}

/* The scrollbar thumb, drawn over the grid. FALSE if everything fits. */
static BOOL thumb_rect(RECT *out) {
  int max = max_scroll();
  if (max <= 0) return FALSE;
  int margin = scale(BAR_MARGIN), track_h = g.client_h - 2 * margin;
  int thumb_h = MulDiv(track_h, g.client_h, g.content_h);
  if (thumb_h < scale(BAR_MIN)) thumb_h = scale(BAR_MIN);
  int top = margin + MulDiv(track_h - thumb_h, g.scroll_y, max);
  int width = scale(g.bar_hot || g.dragging ? BAR_WIDE : BAR_THIN), right = g.client_w - margin;
  SetRect(out, right - width, top, right, top + thumb_h);
  return TRUE;
}

static void invalidate_scrollbar(void) {
  RECT strip = {g.client_w - scale(BAR_HIT), 0, g.client_w, g.client_h};
  InvalidateRect(g.wnd, &strip, FALSE);
}

/* Shows the scrollbar for a while: it appears when the mouse moves or the view scrolls. */
static void show_scrollbar(void) {
  if (!g.bar_shown) invalidate_scrollbar();
  g.bar_shown = TRUE;
  SetTimer(g.wnd, SCROLLBAR_TIMER, 1500, NULL);
}

/* Columns follow the width: as many tiles of at least 150 px (at 96 dpi) as fit, then
 * stretched to fill the row. Keeps the first visible album in view across resizes. */
static void relayout(void) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  int first = g.cols && g.row_h ? (g.scroll_y / g.row_h) * g.cols : 0;

  g.client_w = rc.right;
  g.client_h = rc.bottom;
  g.pad = scale(20);
  g.gap = scale(20);
  g.text_gap = scale(8);
  int min_tile = scale(150);
  int avail = g.client_w - 2 * g.pad;
  g.cols = avail + g.gap >= min_tile + g.gap ? (avail + g.gap) / (min_tile + g.gap) : 1;
  g.tile_w = (avail - g.gap * (g.cols - 1)) / g.cols;
  if (g.tile_w < scale(40)) g.tile_w = scale(40);

  HDC dc = GetDC(g.wnd);
  HGDIOBJ old = SelectObject(dc, g.font ? (HGDIOBJ)g.font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(g.wnd, dc);
  g.text_h = tm.tmHeight;
  g.row_h = g.tile_w + g.text_gap + g.text_h + g.gap;

  int rows = (g.count + g.cols - 1) / g.cols;
  g.content_h = rows ? 2 * g.pad + rows * g.row_h - g.gap : 0;

  if (g.glyph_font) DeleteObject(g.glyph_font);
  g.glyph_font = CreateFontW(-g.tile_w / 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, theme_icon_face());

  g.scroll_y = (first / g.cols) * g.row_h;
  if (g.scroll_y > max_scroll()) g.scroll_y = max_scroll();
  g.target_y = g.scroll_y;
}

/* --- Thumbnails (UI thread) --- */

static void free_thumb(tile *t) {
  if (!t->thumb) return;
  DeleteObject(t->thumb);
  t->thumb = NULL;
  t->pixels = NULL;
  t->thumb_size = 0;
  g.loaded--;
}

/* Index range of tiles at least partly in view. */
static void visible_range(int *first, int *last) {
  int top_row = g.row_h ? (g.scroll_y - g.pad) / g.row_h : 0;
  int bottom_row = g.row_h ? (g.scroll_y + g.client_h - g.pad) / g.row_h : 0;
  if (top_row < 0) top_row = 0;
  *first = top_row * g.cols;
  *last = (bottom_row + 1) * g.cols - 1;
  if (*last >= g.count) *last = g.count - 1;
}

static BOOL wants_thumb(int i) {
  const tile *t = &g.tiles[i];
  return t->cover_dir && !t->failed && (!t->thumb || t->thumb_size != g.tile_w);
}

/* Tells the loader what to decode next: what's on screen first, then one screen below,
 * then one above. Called whenever the view moves, so fast scrolling never builds a
 * backlog. Also frees thumbnails well outside that window. */
static void request_thumbs(void) {
  if (!g.count || !g.cols) return;
  int first, last;
  visible_range(&first, &last);
  int screen = last - first + 1;
  int ahead_last = last + screen < g.count ? last + screen : g.count - 1;
  int behind_first = first - screen > 0 ? first - screen : 0;

  EnterCriticalSection(&g.lock);
  g.queue_len = 0;
  int need = (ahead_last - behind_first + 1);
  if (need > g.queue_cap) {
    load_job *grown = realloc(g.queue, (size_t)need * sizeof *grown);
    if (grown) {
      g.queue = grown;
      g.queue_cap = need;
    }
  }
  int ranges[3][2] = {{first, last}, {last + 1, ahead_last}, {behind_first, first - 1}};
  for (int r = 0; r < 3; r++) {
    for (int i = ranges[r][0]; i <= ranges[r][1] && g.queue_len < g.queue_cap; i++) {
      if (!wants_thumb(i) || (i == g.busy_index && g.busy_gen == g.gen)) continue;
      g.queue[g.queue_len++] = (load_job){i, g.tile_w, scale(COVER_RADIUS)};
    }
  }
  if (g.queue_len) WakeConditionVariable(&g.wake);
  LeaveCriticalSection(&g.lock);

  /* Keep about three screens of thumbnails; drop the rest. */
  if (g.loaded > 3 * screen + 2 * g.cols) {
    int keep_first = behind_first - g.cols, keep_last = ahead_last + g.cols;
    for (int i = 0; i < g.count; i++)
      if ((i < keep_first || i > keep_last) && g.tiles[i].thumb) free_thumb(&g.tiles[i]);
  }
}

static void on_thumb(thumb_result *r) {
  if (r->gen != g.gen || r->index < 0 || r->index >= g.count) {
    if (r->bmp) DeleteObject(r->bmp);
    free(r);
    return;
  }
  int index = r->index;
  tile *t = &g.tiles[index];
  free_thumb(t);
  if (r->bmp) {
    t->thumb = r->bmp;
    t->pixels = r->pixels;
    t->thumb_size = r->size;
    g.loaded++;
  } else {
    t->failed = TRUE;
  }
  free(r);

  /* Repaint just that tile, if it's in view. */
  int row = index / g.cols, col = index % g.cols;
  RECT tr = {g.pad + col * (g.tile_w + g.gap), g.pad + row * g.row_h - g.scroll_y, 0, 0};
  tr.right = tr.left + g.tile_w;
  tr.bottom = tr.top + g.tile_w;
  InvalidateRect(g.wnd, &tr, FALSE);
  if (t->thumb_size != g.tile_w) request_thumbs(); /* finished at an old size */
}

/* --- Data --- */

static void clear_tiles(void) {
  for (int i = 0; i < g.count; i++) {
    free_thumb(&g.tiles[i]);
    free(g.tiles[i].title);
  }
  free(g.tiles);
  g.tiles = NULL;
  g.count = 0;
}

static void reload(void) {
  g.dirty = FALSE;
  mc_album_list *list = NULL;
  if (g.lib) mc_library_list_albums(g.lib, &list);
  int n = list ? list->count : 0;
  tile *tiles = n ? calloc((size_t)n, sizeof *tiles) : NULL;
  wchar_t **dirs = n ? calloc((size_t)n, sizeof *dirs) : NULL;
  if (n && (!tiles || !dirs)) n = 0;
  int selected = -1;
  for (int i = 0; i < n; i++) {
    const mc_album_info *a = &list->items[i];
    memcpy(tiles[i].uid, a->uid, sizeof tiles[i].uid);
    tiles[i].title = widen(a->title && *a->title ? a->title : "Untitled");
    tiles[i].cover_dir = dirs[i] = a->has_art ? widen(a->folder) : NULL;
    if (g.selected_uid[0] && !strcmp(a->uid, g.selected_uid)) selected = i;
  }
  mc_album_list_free(list);

  /* Swap in the new data; the loader's snapshot of folders changes under the lock. */
  clear_tiles();
  EnterCriticalSection(&g.lock);
  g.gen++;
  g.loader_gen = g.gen;
  g.queue_len = 0;
  for (int i = 0; i < g.dir_count; i++) free(g.dirs[i]);
  free(g.dirs);
  g.dirs = dirs;
  g.dir_count = n;
  LeaveCriticalSection(&g.lock);
  g.tiles = tiles;
  g.count = n;
  g.selected = selected;
  g.hot = -1;

  relayout();
  request_thumbs();
  InvalidateRect(g.wnd, NULL, FALSE);
}

void album_grid_set_library(mc_library *lib) {
  g.lib = lib;
  g.dirty = TRUE;
  if (g.wnd) InvalidateRect(g.wnd, NULL, FALSE); /* reloads on the next paint, if visible */
}

/* --- Tiles under the mouse --- */

/* Where tile i is in the view: its cover, its title, and both with room for the hover fill. */
static void tile_rects(int i, RECT *art, RECT *text, RECT *outer) {
  int x = g.pad + (i % g.cols) * (g.tile_w + g.gap), y = g.pad + (i / g.cols) * g.row_h - g.scroll_y;
  SetRect(art, x, y, x + g.tile_w, y + g.tile_w);
  SetRect(text, x, art->bottom + g.text_gap, x + g.tile_w, art->bottom + g.text_gap + g.text_h);
  SetRect(outer, x, y, x + g.tile_w, text->bottom);
  InflateRect(outer, scale(HOVER_PAD), scale(HOVER_PAD));
}

static void invalidate_tile(int i) {
  if (i < 0 || i >= g.count) return;
  RECT art, text, outer;
  tile_rects(i, &art, &text, &outer);
  InvalidateRect(g.wnd, &outer, FALSE);
}

/* The tile (cover or title) at a point in the view, or -1. */
static int tile_at(int x, int y) {
  int cx = x - g.pad, cy = y + g.scroll_y - g.pad;
  if (!g.count || !g.cols || cx < 0 || cy < 0 || y >= g.client_h) return -1;
  int col = cx / (g.tile_w + g.gap), row = cy / g.row_h, i = row * g.cols + col;
  if (col >= g.cols || cx % (g.tile_w + g.gap) >= g.tile_w || cy % g.row_h >= g.row_h - g.gap) return -1;
  return i < g.count ? i : -1;
}

static void set_hot(int i) {
  if (i == g.hot) return;
  invalidate_tile(g.hot);
  invalidate_tile(i);
  g.hot = i;
}

/* Re-checks the tile under the mouse, after the tiles moved under it. */
static void update_hot(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(g.wnd, &pt);
  set_hot(g.tracking && !g.bar_hot && !g.dragging ? tile_at(pt.x, pt.y) : -1);
}

static void select_tile(int i) {
  if (i == g.selected) return;
  invalidate_tile(g.selected);
  invalidate_tile(i);
  g.selected = i;
  if (i >= 0) memcpy(g.selected_uid, g.tiles[i].uid, sizeof g.selected_uid);
  else g.selected_uid[0] = '\0';
}

/* --- Scrolling --- */

static void scroll_to(int y) {
  if (y < 0) y = 0;
  if (y > max_scroll()) y = max_scroll();
  if (y == g.scroll_y) return;
  g.scroll_y = y;
  InvalidateRect(g.wnd, NULL, FALSE);
  request_thumbs();
  show_scrollbar();
  update_hot();
}

/* Eases towards target_y over a few frames. */
static void animate_to(int y) {
  if (y < 0) y = 0;
  if (y > max_scroll()) y = max_scroll();
  g.target_y = y;
  if (!g.animating && g.target_y != g.scroll_y) {
    g.animating = TRUE;
    SetTimer(g.wnd, SCROLL_TIMER, USER_TIMER_MINIMUM, NULL);
  }
}

static void jump_to(int y) {
  if (g.animating) KillTimer(g.wnd, SCROLL_TIMER);
  g.animating = FALSE;
  scroll_to(y);
  g.target_y = g.scroll_y;
}

static void on_scroll_timer(void) {
  int diff = g.target_y - g.scroll_y;
  int step = diff / 4;
  if (step == 0) step = diff;
  scroll_to(g.scroll_y + step);
  if (g.scroll_y == g.target_y || step == 0) {
    KillTimer(g.wnd, SCROLL_TIMER);
    g.animating = FALSE;
  }
}

static void on_vscroll(int code) {
  int line = g.row_h / 4, page = g.client_h * 9 / 10;
  switch (code) {
    case SB_LINEUP: animate_to(g.target_y - line); break;
    case SB_LINEDOWN: animate_to(g.target_y + line); break;
    case SB_PAGEUP: animate_to(g.target_y - page); break;
    case SB_PAGEDOWN: animate_to(g.target_y + page); break;
    case SB_TOP: jump_to(0); break;
    case SB_BOTTOM: jump_to(max_scroll()); break;
  }
}

static void on_wheel(int delta) {
  UINT lines = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  int px = lines == WHEEL_PAGESCROLL ? g.client_h * 9 / 10 : (int)lines * scale(40);
  animate_to(g.target_y - MulDiv(delta, px, WHEEL_DELTA));
}

/* --- Mouse --- */

static void on_mouse_move(int x, int y) {
  if (!g.tracking) {
    TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, g.wnd, 0};
    TrackMouseEvent(&tme);
    g.tracking = TRUE;
  }
  if (g.dragging) {
    RECT thumb;
    if (!thumb_rect(&thumb)) return;
    int margin = scale(BAR_MARGIN), travel = g.client_h - 2 * margin - (thumb.bottom - thumb.top);
    if (travel > 0) jump_to(MulDiv(y - g.drag_dy - margin, max_scroll(), travel));
    return;
  }
  BOOL bar_hot = max_scroll() > 0 && x >= g.client_w - scale(BAR_HIT);
  if (bar_hot != g.bar_hot) {
    g.bar_hot = bar_hot;
    invalidate_scrollbar();
  }
  show_scrollbar();
  set_hot(bar_hot ? -1 : tile_at(x, y));
}

static void on_mouse_down(int x, int y) {
  SetFocus(g.wnd); /* for keyboard scrolling */
  RECT thumb;
  if (g.bar_hot && thumb_rect(&thumb)) {
    if (y >= thumb.top && y < thumb.bottom) {
      g.dragging = TRUE;
      g.drag_dy = y - thumb.top;
      SetCapture(g.wnd);
    } else {
      on_vscroll(y < thumb.top ? SB_PAGEUP : SB_PAGEDOWN);
    }
    return;
  }
  select_tile(tile_at(x, y));
}

/* --- Painting --- */

static void paint_tile(canvas *c, int i) {
  tile *t = &g.tiles[i];
  RECT art, text, outer;
  tile_rects(i, &art, &text, &outer);

  if (i == g.hot) theme_draw_state(c, &outer, scale(8), STATE_HOT);
  if (i == g.selected) {
    RECT ring = art;
    InflateRect(&ring, scale(RING_GAP + RING_W), scale(RING_GAP + RING_W));
    canvas_frame(c, &ring, scale(COVER_RADIUS + RING_GAP + RING_W), scale(RING_W), theme.accent, 255);
  }

  if (t->pixels) {
    canvas_draw_image(c, &art, t->pixels, t->thumb_size, t->thumb_size); /* mid-resize: the old size, stretched */
  } else {
    canvas_fill(c, &art, scale(COVER_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
    if (!t->cover_dir || t->failed) {
      HGDIOBJ f = SelectObject(c->dc, g.glyph_font);
      SetTextColor(c->dc, theme.text3);
      DrawTextW(c->dc, L"\xE8D6", -1, &art, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* music note */
      SelectObject(c->dc, f);
    }
  }
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, t->title, -1, &text, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

/* Everything is drawn into a canvas, then copied in one go. GDI text leaves alpha at 0, so
 * it's made opaque at the end, except outside the rounded top-left corner, which shows
 * the sidebar's Mica. */
static void paint(HDC dc, const RECT *dirty) {
  if (!canvas_reserve(&g.buf, g.client_w, g.client_h)) return;
  canvas *c = &g.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, g.font ? (HGDIOBJ)g.font : GetStockObject(DEFAULT_GUI_FONT));

  if (g.count == 0) {
    RECT rc = {0, 0, g.client_w, g.client_h};
    SetTextColor(c->dc, theme.text3);
    DrawTextW(c->dc, g.lib ? L"No albums yet. Use Import to add music." : L"Could not open the library.",
              -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  } else {
    int first, last;
    visible_range(&first, &last);
    for (int i = first; i <= last; i++) {
      RECT art, text, outer, isect;
      tile_rects(i, &art, &text, &outer);
      if (IntersectRect(&isect, &outer, dirty)) paint_tile(c, i);
    }
  }
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  RECT thumb;
  if (g.bar_shown && thumb_rect(&thumb))
    canvas_fill(c, &thumb, thumb.right - thumb.left, theme.text, g.bar_hot || g.dragging ? 140 : 110);

  /* The content area's border and rounded corner, against the sidebar and toolbar. */
  int radius = scale(CONTENT_RADIUS);
  RECT all = {0, 0, g.client_w, g.client_h}, border = {0, 0, g.client_w + radius, g.client_h + radius};
  canvas_frame(c, &border, radius, 1, theme.line, 255);
  canvas_round_corners(c, &all, radius, CORNER_TL, theme.frame, theme.mica ? 0 : 255);

  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

void album_grid_theme_changed(void) {
  relayout(); /* the icon font */
  InvalidateRect(g.wnd, NULL, FALSE);
}

/* --- Window --- */

static LRESULT CALLBACK grid_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!g.wnd) g.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  switch (msg) {
    case WM_SIZE:
      relayout();
      request_thumbs();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_SETFONT:
      g.font = (HFONT)wp;
      relayout();
      if (LOWORD(lp)) InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1; /* painted entirely in WM_PAINT */
    case WM_PAINT: {
      if (g.dirty) reload(); /* deferred while hidden; painting means we're visible */
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_MOUSEWHEEL:
      on_wheel(GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_TIMER:
      if (wp == SCROLL_TIMER) {
        on_scroll_timer();
      } else if (wp == SCROLLBAR_TIMER && !g.bar_hot && !g.dragging) {
        KillTimer(wnd, SCROLLBAR_TIMER);
        g.bar_shown = FALSE;
        invalidate_scrollbar();
      }
      return 0;
    case WM_MOUSEMOVE:
      on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      g.tracking = FALSE;
      if (!g.dragging) {
        g.bar_hot = FALSE;
        invalidate_scrollbar();
      }
      set_hot(-1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
      if (g.dragging) ReleaseCapture();
      return 0;
    case WM_CAPTURECHANGED:
      g.dragging = FALSE;
      invalidate_scrollbar();
      return 0;
    case WM_KEYDOWN:
      switch (wp) {
        case VK_UP: on_vscroll(SB_LINEUP); return 0;
        case VK_DOWN: on_vscroll(SB_LINEDOWN); return 0;
        case VK_PRIOR: on_vscroll(SB_PAGEUP); return 0;
        case VK_NEXT: on_vscroll(SB_PAGEDOWN); return 0;
        case VK_HOME: on_vscroll(SB_TOP); return 0;
        case VK_END: on_vscroll(SB_BOTTOM); return 0;
      }
      break;
    case WM_APP_THUMB:
      on_thumb((thumb_result *)lp);
      return 0;
    case WM_DESTROY:
      EnterCriticalSection(&g.lock);
      g.quit = TRUE;
      WakeConditionVariable(&g.wake);
      LeaveCriticalSection(&g.lock);
      WaitForSingleObject(g.thread, INFINITE);
      CloseHandle(g.thread);
      clear_tiles();
      for (int i = 0; i < g.dir_count; i++) free(g.dirs[i]);
      free(g.dirs);
      free(g.queue);
      canvas_free(&g.buf);
      if (g.glyph_font) DeleteObject(g.glyph_font);
      DeleteCriticalSection(&g.lock);
      memset(&g, 0, sizeof g);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND album_grid_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DBLCLKS, .lpfnWndProc = grid_proc,
                    .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"musicapp_albumgrid"};
  RegisterClassExW(&wc);

  memset(&g, 0, sizeof g);
  g.busy_index = -1;
  g.hot = g.selected = -1;
  g.dirty = TRUE;
  InitializeCriticalSection(&g.lock);
  InitializeConditionVariable(&g.wake);
  g.wnd = CreateWindowExW(0, L"musicapp_albumgrid", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (g.wnd) g.thread = CreateThread(NULL, 0, loader_main, NULL, 0, NULL);
  return g.wnd;
}
