#define COBJMACROS
#include "album_grid.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include <windowsx.h>

#include "canvas.h"
#include "cover.h"
#include "scroller.h"
#include "theme.h"

#define WM_APP_THUMB (WM_APP + 20) /* lParam: thumb_result *, from the loader thread */

/* Metrics at 96 DPI. */
enum {
  COVER_RADIUS = 6,
  HOVER_PAD = 8, /* hover fill around the cover and title */
  RING_GAP = 2,  /* selection ring: gap from the cover, and width */
  RING_W = 2,
};

typedef struct tile {
  char uid[MC_UID_STRLEN];
  wchar_t *title;
  wchar_t *artist;    /* NULL if unknown */
  int year;           /* 0 if unknown */
  wchar_t *cover_dir; /* NULL when the album has no cover */
  wchar_t sub[32];    /* beneath a playlist's name: its track count (albums show the artist) */
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
  BOOL dirty;     /* reload when next shown */
  BOOL playlists; /* showing playlists rather than albums */
  tile *tiles;
  int count;
  UINT gen; /* bumped on every reload, so late thumbnails for old tiles are dropped */
  HFONT font, glyph_font;

  /* Layout, in device pixels. */
  int cols, tile_w, row_h, pad, gap, text_gap, text_h;
  int client_w, client_h;
  int full_w; /* the width the tiles are sized for; see album_grid_set_full_width */
  scroller scroll;

  int hot;                          /* tile under the mouse, or -1 */
  int selected;                     /* or -1 */
  char selected_uid[MC_UID_STRLEN]; /* keeps the selection across reloads */
  BOOL tracking;                    /* asked for WM_MOUSELEAVE */

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

/* --- Cover loading (loader thread) --- */

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
    r->bmp = wic && dir ? cover_decode(wic, dir, job.size, job.radius, &r->pixels) : NULL;
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

/* Tiles for a grid w wide: as many of at least 150 px (at 96 dpi) as fit, stretched to fill
 * the row. */
static void fit(int w, int *cols, int *tile_w) {
  int pad = scale(20), gap = scale(20), min_tile = scale(150);
  int avail = w - 2 * pad;
  *cols = avail + gap >= min_tile + gap ? (avail + gap) / (min_tile + gap) : 1;
  *tile_w = (avail - gap * (*cols - 1)) / *cols;
  if (*tile_w < scale(40)) *tile_w = scale(40);
}

/* Tiles are sized for the full width, so while a pane narrows the grid they keep their size
 * and only fewer fit. Keeps the first visible album in view across resizes. */
static void relayout(void) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  int first = g.cols && g.row_h ? (g.scroll.pos / g.row_h) * g.cols : 0;

  g.client_w = rc.right;
  g.client_h = rc.bottom;
  g.pad = scale(20);
  g.gap = scale(20);
  g.text_gap = scale(8);
  fit(g.full_w > g.client_w ? g.full_w : g.client_w, &g.cols, &g.tile_w);
  g.cols = (g.client_w - 2 * g.pad + g.gap) / (g.tile_w + g.gap);
  if (g.cols < 1) fit(g.client_w, &g.cols, &g.tile_w); /* too narrow for full-width tiles */

  HDC dc = GetDC(g.wnd);
  HGDIOBJ old = SelectObject(dc, g.font ? (HGDIOBJ)g.font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(g.wnd, dc);
  g.text_h = tm.tmHeight;
  g.row_h = g.tile_w + g.text_gap + 2 * g.text_h + g.gap; /* the title, and the artist or track count */

  int rows = (g.count + g.cols - 1) / g.cols;

  if (g.glyph_font) DeleteObject(g.glyph_font);
  g.glyph_font = CreateFontW(-g.tile_w / 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, theme_icon_face());

  g.scroll.pos = g.scroll.target = (first / g.cols) * g.row_h;
  scroller_resize(&g.scroll, &rc, rows ? 2 * g.pad + rows * g.row_h - g.gap : 0);
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
  int top_row = g.row_h ? (g.scroll.pos - g.pad) / g.row_h : 0;
  int bottom_row = g.row_h ? (g.scroll.pos + g.client_h - g.pad) / g.row_h : 0;
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
  RECT tr = {g.pad + col * (g.tile_w + g.gap), g.pad + row * g.row_h - g.scroll.pos, 0, 0};
  tr.right = tr.left + g.tile_w;
  tr.bottom = tr.top + g.tile_w;
  InvalidateRect(g.wnd, &tr, FALSE);
  if (t->thumb_size != g.tile_w) request_thumbs(); /* finished at an old size */
}

/* --- Data --- */

/* Posted rather than sent: reloads happen while painting. */
static void notify_selection(void) {
  PostMessageW(GetParent(g.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(g.wnd), AGN_SELCHANGE), (LPARAM)g.wnd);
}

const wchar_t *album_grid_selection(char uid[MC_UID_STRLEN]) {
  if (g.selected < 0 || g.selected >= g.count) return NULL;
  memcpy(uid, g.tiles[g.selected].uid, MC_UID_STRLEN);
  return g.tiles[g.selected].title;
}

const wchar_t *album_grid_selection_artist(void) {
  return g.selected >= 0 && g.selected < g.count ? g.tiles[g.selected].artist : NULL;
}

const wchar_t *album_grid_selection_cover(void) {
  return g.selected >= 0 && g.selected < g.count ? g.tiles[g.selected].cover_dir : NULL;
}

int album_grid_selection_year(void) {
  return g.selected >= 0 && g.selected < g.count ? g.tiles[g.selected].year : 0;
}

static void select_tile(int i);

void album_grid_unselect(void) {
  select_tile(-1);
}

static void clear_tiles(void) {
  for (int i = 0; i < g.count; i++) {
    free_thumb(&g.tiles[i]);
    free(g.tiles[i].title);
    free(g.tiles[i].artist);
  }
  free(g.tiles);
  g.tiles = NULL;
  g.count = 0;
}

/* The albums' tiles, into tiles and dirs (n of each). */
static void read_albums(tile **tiles, wchar_t ***dirs, int *n) {
  mc_album_list *list = NULL;
  if (g.lib) mc_library_list_albums(g.lib, &list);
  *n = list ? list->count : 0;
  *tiles = *n ? calloc((size_t)*n, sizeof **tiles) : NULL;
  *dirs = *n ? calloc((size_t)*n, sizeof **dirs) : NULL;
  if (*n && (!*tiles || !*dirs)) *n = 0;
  for (int i = 0; i < *n; i++) {
    const mc_album_info *a = &list->items[i];
    tile *t = &(*tiles)[i];
    memcpy(t->uid, a->uid, sizeof t->uid);
    t->title = widen(a->title && *a->title ? a->title : "Untitled");
    t->artist = a->artist && *a->artist ? widen(a->artist) : NULL;
    t->year = a->year;
    t->cover_dir = (*dirs)[i] = a->has_art ? widen(a->folder) : NULL;
  }
  mc_album_list_free(list);
}

/* The playlists' tiles: no cover, and the track count beneath the name. */
static void read_playlists(tile **tiles, wchar_t ***dirs, int *n) {
  mc_playlist_list *list = NULL;
  if (g.lib) mc_library_list_playlists(g.lib, &list);
  *n = list ? list->count : 0;
  *tiles = *n ? calloc((size_t)*n, sizeof **tiles) : NULL;
  *dirs = *n ? calloc((size_t)*n, sizeof **dirs) : NULL; /* all NULL: nothing to load */
  if (*n && (!*tiles || !*dirs)) *n = 0;
  for (int i = 0; i < *n; i++) {
    const mc_playlist_info *p = &list->items[i];
    tile *t = &(*tiles)[i];
    memcpy(t->uid, p->uid, sizeof t->uid);
    t->title = widen(p->name && *p->name ? p->name : "Untitled");
    swprintf(t->sub, sizeof t->sub / sizeof t->sub[0], L"%d %ls", p->track_count,
             p->track_count == 1 ? L"track" : L"tracks");
  }
  mc_playlist_list_free(list);
}

static void reload(void) {
  g.dirty = FALSE;
  tile *tiles;
  wchar_t **dirs;
  int n;
  if (g.playlists) read_playlists(&tiles, &dirs, &n);
  else read_albums(&tiles, &dirs, &n);
  int selected = -1;
  for (int i = 0; i < n; i++)
    if (g.selected_uid[0] && !strcmp(tiles[i].uid, g.selected_uid)) selected = i;
  /* A renamed selection is news too: the track list shows its name. */
  BOOL renamed = selected >= 0 && g.selected >= 0 && g.selected < g.count && g.tiles[g.selected].title &&
                 tiles[selected].title && wcscmp(tiles[selected].title, g.tiles[g.selected].title) != 0;

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
  if (selected < 0 && g.selected_uid[0]) { /* the selected album went */
    g.selected_uid[0] = '\0';
    notify_selection();
  } else if (renamed) {
    notify_selection();
  }

  relayout();
  request_thumbs();
  InvalidateRect(g.wnd, NULL, FALSE);
}

void album_grid_show_playlists(BOOL playlists) {
  if (!playlists == !g.playlists) return;
  BOOL had = g.selected_uid[0] != '\0';
  g.playlists = playlists;
  g.selected = g.hot = -1;
  g.selected_uid[0] = '\0';
  g.scroll.pos = g.scroll.target = 0;
  g.dirty = TRUE;
  if (g.wnd) InvalidateRect(g.wnd, NULL, FALSE);
  if (had) notify_selection();
}

BOOL album_grid_showing_playlists(void) {
  return g.playlists;
}

void album_grid_set_library(mc_library *lib) {
  g.lib = lib;
  g.dirty = TRUE;
  if (g.wnd) InvalidateRect(g.wnd, NULL, FALSE); /* reloads on the next paint, if visible */
}

/* --- Tiles under the mouse --- */

/* Where tile i is in the view: its cover, its title, and both with room for the hover fill. */
static void tile_rects(int i, RECT *art, RECT *text, RECT *outer) {
  int x = g.pad + (i % g.cols) * (g.tile_w + g.gap), y = g.pad + (i / g.cols) * g.row_h - g.scroll.pos;
  SetRect(art, x, y, x + g.tile_w, y + g.tile_w);
  SetRect(text, x, art->bottom + g.text_gap, x + g.tile_w,
          art->bottom + g.text_gap + 2 * g.text_h);
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
  int cx = x - g.pad, cy = y + g.scroll.pos - g.pad;
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
  set_hot(g.tracking && !g.scroll.hot && !g.scroll.dragging ? tile_at(pt.x, pt.y) : -1);
}

static void select_tile(int i) {
  if (i == g.selected) return;
  invalidate_tile(g.selected);
  invalidate_tile(i);
  g.selected = i;
  if (i >= 0) memcpy(g.selected_uid, g.tiles[i].uid, sizeof g.selected_uid);
  else g.selected_uid[0] = '\0';
  notify_selection();
}

/* --- Scrolling and the mouse --- */

/* The scroller already repainted; the tiles in view changed. */
static void on_scrolled(void) {
  request_thumbs();
  update_hot();
}

static void on_mouse_move(int x, int y) {
  if (!g.tracking) {
    TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, g.wnd, 0};
    TrackMouseEvent(&tme);
    g.tracking = TRUE;
  }
  BOOL on_bar = scroller_mouse_move(&g.scroll, x, y);
  set_hot(on_bar ? -1 : tile_at(x, y));
}

static void on_mouse_down(int x, int y) {
  SetFocus(g.wnd); /* for keyboard scrolling */
  if (!scroller_mouse_down(&g.scroll, x, y)) select_tile(tile_at(x, y));
}

/* The arrow keys, Home, End, Page Up and Page Down move the selection, as clicks would, and
 * scroll it into view. Returns FALSE for other keys. */
static BOOL on_key(WPARAM key) {
  if (!g.count || !g.cols) return FALSE;
  int cur = g.selected, last = g.count - 1, page = max(1, g.client_h / g.row_h) * g.cols, to;
  switch (key) {
    case VK_LEFT: to = cur < 0 ? 0 : cur - 1; break;
    case VK_RIGHT: to = cur < 0 ? 0 : cur + 1; break;
    case VK_UP: to = cur < 0 ? 0 : cur - g.cols; break;
    case VK_DOWN: to = cur < 0 ? 0 : cur + g.cols; break;
    case VK_PRIOR: to = cur < 0 ? 0 : cur - page; break;
    case VK_NEXT: to = cur < 0 ? 0 : cur + page; break;
    case VK_HOME: to = 0; break;
    case VK_END: to = last; break;
    default: return FALSE;
  }
  if (key == VK_UP || key == VK_PRIOR) to = max(to, cur % g.cols); /* stay in the column at the top */
  to = max(0, min(to, last));
  select_tile(to);
  int top = g.pad + (to / g.cols) * g.row_h;
  scroller_reveal(&g.scroll, top - g.pad, top + g.row_h - g.gap + g.pad);
  return TRUE;
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
      const wchar_t *glyph = g.playlists ? L"\xE90B" : L"\xE8D6"; /* the sidebar's Playlists icon, or a note */
      DrawTextW(c->dc, glyph, -1, &art, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(c->dc, f);
    }
  }
  RECT line = {text.left, text.top, text.right, text.top + g.text_h};
  UINT one_line = DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, t->title, -1, &line, one_line);
  OffsetRect(&line, 0, g.text_h);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, g.playlists ? t->sub : t->artist ? t->artist : L"Unknown Artist", -1, &line, one_line);
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
    const wchar_t *empty = !g.lib         ? L"Could not open the library."
                           : g.playlists ? L"No playlists yet. Right-click a track and choose Add to Playlist to start one."
                                         : L"No albums yet. Use Import to add music.";
    /* Wrapped if need be, and centred as a block. */
    InflateRect(&rc, -g.pad, 0);
    RECT fit_rc = rc;
    DrawTextW(c->dc, empty, -1, &fit_rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
    OffsetRect(&rc, 0, (g.client_h - (fit_rc.bottom - fit_rc.top)) / 2);
    DrawTextW(c->dc, empty, -1, &rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
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

  scroller_paint(&g.scroll, c);

  /* The content area's border and rounded corner, against the sidebar and toolbar. */
  int radius = scale(CONTENT_RADIUS);
  RECT all = {0, 0, g.client_w, g.client_h}, border = {0, 0, g.client_w + radius, g.client_h + radius};
  canvas_frame(c, &border, radius, 1, theme.line, 255);
  canvas_round_corners(c, &all, radius, CORNER_TL, theme.frame, theme.mica ? 0 : 255);

  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

int album_grid_set_full_width(int full_w, int min_w) {
  if (!g.wnd) return 0;
  if (full_w != g.full_w) {
    g.full_w = full_w;
    relayout();
    request_thumbs();
    InvalidateRect(g.wnd, NULL, FALSE);
  }
  int cols, tile_w, pitch;
  fit(full_w, &cols, &tile_w);
  pitch = tile_w + scale(20);
  int n = (min_w + pitch - 1) / pitch;
  return n < cols ? n * pitch : 0;
}

void album_grid_theme_changed(void) {
  relayout(); /* the icon font */
  InvalidateRect(g.wnd, NULL, FALSE);
}

/* --- Window --- */

static LRESULT CALLBACK grid_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!g.wnd) g.wnd = g.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
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
      scroller_wheel(&g.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_TIMER:
      scroller_timer(&g.scroll, wp);
      return 0;
    case WM_MOUSEMOVE:
      on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      g.tracking = FALSE;
      scroller_mouse_leave(&g.scroll);
      set_hot(-1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
      scroller_mouse_up(&g.scroll);
      return 0;
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&g.scroll);
      return 0;
    case WM_KEYDOWN:
      if (on_key(wp)) return 0;
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
  g.scroll.moved = on_scrolled;
  g.dirty = TRUE;
  InitializeCriticalSection(&g.lock);
  InitializeConditionVariable(&g.wake);
  g.wnd = CreateWindowExW(0, L"musicapp_albumgrid", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (g.wnd) g.thread = CreateThread(NULL, 0, loader_main, NULL, 0, NULL);
  return g.wnd;
}
