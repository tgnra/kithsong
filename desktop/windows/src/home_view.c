#define COBJMACROS
#define _CRT_RAND_S /* rand_s(), for Random's seed */
#include "home_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include <windowsx.h>
#include <commctrl.h>
#include <uiautomation.h>

#include "canvas.h"
#include "cover.h"
#include "musiccore/home_picks.h"
#include "playback.h"
#include "scroller.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

#define WM_APP_THUMB (WM_APP + 20) /* lParam: thumb_result *, from the loader thread */
#define WM_APP_PICKS (WM_APP + 21) /* lParam: mc_home_picks *, from the loader thread */

/* Metrics at 96 DPI; the tiles are the album grid's and an artist page's. */
enum {
  PAD = 20,
  TILE_W = 150,
  TILE_GAP = 20,
  TEXT_GAP = 8,
  HOVER_PAD = 8,
  COVER_RADIUS = 6,
  TITLE_GAP = 14, /* under a row's title */
  ROW_GAP = 30,   /* between one row's text and the next row's title */
  ARROW = 36,     /* a row's arrows, round */
  ARROW_INSET = 16,
  BADGE = 30, /* a station's mark, on its cover's corner */
  BADGE_INSET = 8,
  RING_GAP = 2, /* the keyboard focus's ring */
  RING_W = 2,
  ACTION_H = 32,
};

enum { FLING_MS = 220 }; /* how far a let-go drag carries on: the speed it had, for this long */

enum { TIMER_ROWS = 1 }; /* the rows' easing; the page's scroller has 101 and 102 */

/* What's under the mouse. */
typedef enum { PART_NONE, PART_TILE, PART_LEFT, PART_RIGHT, PART_ACTION } part_id;

typedef struct hit {
  part_id part;
  int row, index; /* row: any but PART_ACTION and PART_NONE outside a row, -1 there */
} hit;

/* An mc_home_pick in wide strings, for drawing. */
typedef struct home_pick {
  mc_recent_kind kind;
  BOOL station, smart;
  char uid[MC_UID_STRLEN];
  char seed_uid[MC_UID_STRLEN]; /* a station's */
  wchar_t *title, *sub, *cover_dir;
  char *seed_title; /* UTF-8, as mc_wformat() takes it */
} home_pick;

typedef struct tile {
  home_pick pick;
  HBITMAP thumb; /* premultiplied, with rounded corners, or round for an artist */
  const uint32_t *pixels;
  int thumb_size;
  BOOL stale;  /* thumb is from before the last reload: drawn until it's decoded again */
  BOOL failed; /* there's no cover, or it couldn't be decoded; draw the placeholder */
} tile;

typedef struct home_row {
  tile *tiles;
  int count;
  int top;         /* of its title, down the page */
  int pos, target; /* pixels scrolled sideways, and where the easing is heading */
} home_row;

typedef struct load_job {
  int row, index;
  int size, radius;
  wchar_t *key;  /* the cover's folder, or a playlist's or group's uid */
  BOOL by_uid, group;
} load_job;

typedef struct thumb_result {
  int row, index;
  int size;
  UINT gen;
  HBITMAP bmp; /* NULL if decoding failed */
  const uint32_t *pixels;
} thumb_result;

static uia_host ui;

static struct {
  HWND wnd, tooltip; /* one tool, moved to the station under the mouse */
  mc_library *lib;
  BOOL dirty;  /* reload when next painted */
  BOOL loaded; /* the rows have been read at least once */
  unsigned seed;
  home_row rows[MC_HOME_ROW_COUNT];
  BOOL has_music, has_plays;
  UINT gen; /* bumped on every reload, so late thumbnails for old tiles are dropped */
  HFONT font, title_font, welcome_font, glyph_font, icon_font, badge_font;

  /* Layout, in device pixels. */
  int tile_w, gap, pad, text_gap, text_h, title_h, title_gap, row_gap;
  int client_w, client_h;
  int hint_top; /* of the note at the foot of a library not yet played; 0 if none */
  scroller scroll;

  hit hot;
  int hot_row;  /* the row under the mouse, whose arrows show, or -1 */
  BOOL tracking; /* asked for WM_MOUSELEAVE */
  RECT action_rc; /* the welcome's button, as last painted; empty if none */

  /* Pressed on `pressed`, which becomes dragging its row once it has moved far enough. */
  hit pressed;
  int press_x, press_pos;
  BOOL panning;
  int last_x;
  DWORD last_time;
  double speed; /* pixels a millisecond, smoothed */

  int focus_row, focus_index; /* or -1, and the ring only shows once the keys have moved it */
  BOOL focus_shown;

  char clicked_uid[MC_UID_STRLEN];
  mc_recent_kind clicked_kind;

  canvas buf;

  /* Loader thread, as the album grid's: the UI thread replaces the queue whenever the view
   * moves, and the loader takes jobs from the front. Tiles are only touched on the UI thread. */
  HANDLE thread;
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE wake;
  load_job *queue;
  int queue_len, queue_cap;
  int busy_row, busy_index; /* job the loader is working on, or -1 */
  UINT busy_gen;
  UINT loader_gen; /* gen the queued jobs belong to */
  BOOL picks_wanted; /* the loader reads the rows before it decodes anything else */
  char *lib_dir;   /* the library's folder, for the loader's own connection */
  char *art_dir;   /* where playlists' and groups' pictures are cached */
  BOOL quit;
} g;

static int scale(int px) {
  return MulDiv(px, (int)GetDpiForWindow(g.wnd), USER_DEFAULT_SCREEN_DPI);
}

/* --- Cover loading (loader thread) --- */

static DWORD WINAPI loader_main(void *arg) {
  (void)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  IWICImagingFactory *wic = NULL;
  CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                   (void **)&wic);
  cover_art_source art = {0};

  for (;;) {
    EnterCriticalSection(&g.lock);
    g.busy_row = g.busy_index = -1;
    while (!g.quit && g.queue_len == 0 && !g.picks_wanted) {
      if (art.lib) { /* idle: nothing of ours holds the library open, say for a move */
        LeaveCriticalSection(&g.lock);
        cover_art_close(&art);
        EnterCriticalSection(&g.lock);
        continue;
      }
      SleepConditionVariableCS(&g.wake, &g.lock, INFINITE);
    }
    if (g.quit) {
      LeaveCriticalSection(&g.lock);
      break;
    }
    if (g.picks_wanted) {
      g.picks_wanted = FALSE;
      char *dir = g.lib_dir ? _strdup(g.lib_dir) : NULL;
      unsigned seed = g.seed;
      LeaveCriticalSection(&g.lock);
      mc_home_picks *picks = malloc(sizeof *picks);
      if (picks) {
        mc_home_picks_load(cover_art_library(&art, dir), seed, picks);
        if (!PostMessageW(g.wnd, WM_APP_PICKS, 0, (LPARAM)picks)) {
          mc_home_picks_free(picks);
          free(picks);
        }
      }
      free(dir);
      continue;
    }
    load_job job = g.queue[0];
    memmove(g.queue, g.queue + 1, (size_t)(--g.queue_len) * sizeof *g.queue);
    UINT gen = g.loader_gen;
    char *lib_dir = job.by_uid && g.lib_dir ? _strdup(g.lib_dir) : NULL;
    char *art_dir = job.by_uid && g.art_dir ? _strdup(g.art_dir) : NULL;
    g.busy_row = job.row;
    g.busy_index = job.index;
    g.busy_gen = gen;
    LeaveCriticalSection(&g.lock);

    thumb_result *r = calloc(1, sizeof *r);
    if (r) {
      r->row = job.row;
      r->index = job.index;
      r->size = job.size;
      r->gen = gen;
      if (job.by_uid) {
        wchar_t *file = cover_art_file(&art, lib_dir, art_dir, job.key, job.group);
        if (wic && file) r->bmp = cover_decode_file(wic, file, job.size, job.radius, &r->pixels);
        free(file);
      } else if (wic) {
        r->bmp = cover_decode(wic, job.key, job.size, job.radius, &r->pixels);
      }
      if (!PostMessageW(g.wnd, WM_APP_THUMB, 0, (LPARAM)r)) {
        if (r->bmp) DeleteObject(r->bmp);
        free(r);
      }
    }
    free(job.key);
    free(lib_dir);
    free(art_dir);
  }
  cover_art_close(&art);
  if (wic) IWICImagingFactory_Release(wic);
  CoUninitialize();
  return 0;
}

/* --- Layout --- */

static int pitch(void) {
  return g.tile_w + g.gap;
}

/* How far row r scrolls sideways. */
static int row_max(const home_row *r) {
  int content = 2 * g.pad + r->count * pitch() - g.gap;
  return content > g.client_w ? content - g.client_w : 0;
}

static int clamp_pos(const home_row *r, int pos) {
  int max = row_max(r);
  return pos < 0 ? 0 : pos > max ? max : pos;
}

static int font_height(HFONT font) {
  HDC dc = GetDC(g.wnd);
  HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(g.wnd, dc);
  return tm.tmHeight;
}

/* Everything sized from the DPI and the window. */
static void relayout(void) {
  RECT rc;
  GetClientRect(g.wnd, &rc);
  g.client_w = rc.right;
  g.client_h = rc.bottom;
  g.pad = scale(PAD);
  g.tile_w = scale(TILE_W);
  g.gap = scale(TILE_GAP);
  g.text_gap = scale(TEXT_GAP);
  g.title_gap = scale(TITLE_GAP);
  g.row_gap = scale(ROW_GAP);
  g.text_h = font_height(g.font);
  g.title_h = font_height(g.title_font);

  int y = g.pad;
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    home_row *r = &g.rows[i];
    r->pos = clamp_pos(r, r->pos);
    r->target = clamp_pos(r, r->target);
    if (!r->count) continue;
    r->top = y;
    y += g.title_h + g.title_gap + g.tile_w + g.text_gap + 2 * g.text_h + g.row_gap;
  }
  g.hint_top = g.has_music && !g.has_plays ? y : 0;
  if (g.hint_top) y += g.text_h + g.row_gap;
  scroller_resize(&g.scroll, &rc, g.has_music ? y - g.row_gap + g.pad : 0);
}

/* The cover of tile index of row r, in the client area. */
static RECT cover_rect(int row, int index) {
  const home_row *r = &g.rows[row];
  int x = g.pad + index * pitch() - r->pos;
  int y = r->top + g.title_h + g.title_gap - g.scroll.pos;
  RECT rc = {x, y, x + g.tile_w, y + g.tile_w};
  return rc;
}

/* Around tile index of row: cover, text and hover fill. */
static RECT tile_outer(int row, int index) {
  RECT rc = cover_rect(row, index);
  rc.bottom += g.text_gap + 2 * g.text_h;
  InflateRect(&rc, scale(HOVER_PAD), scale(HOVER_PAD));
  return rc;
}

/* All of a row, across the page, title and text included. */
static RECT row_rect(int row) {
  const home_row *r = &g.rows[row];
  int top = r->top - g.scroll.pos;
  RECT rc = {0, top, g.client_w, top + g.title_h + g.title_gap + g.tile_w + g.text_gap + 2 * g.text_h + scale(HOVER_PAD)};
  return rc;
}

/* A row's arrows, on its ends, level with the middle of the covers. */
static RECT arrow_rect(int row, BOOL right) {
  RECT cover = cover_rect(row, 0);
  int d = scale(ARROW), y = (cover.top + cover.bottom - d) / 2;
  int x = right ? g.client_w - scale(ARROW_INSET) - d : scale(ARROW_INSET);
  RECT rc = {x, y, x + d, y + d};
  return rc;
}

static BOOL arrow_shown(int row, BOOL right) {
  const home_row *r = &g.rows[row];
  return (row == g.hot_row || row == g.pressed.row) && !g.panning && (right ? r->target < row_max(r) : r->target > 0);
}

static hit hit_test(int x, int y) {
  hit h = {PART_NONE, -1, -1};
  POINT pt = {x, y};
  if (PtInRect(&g.action_rc, pt)) {
    h.part = PART_ACTION;
    return h;
  }
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    const home_row *r = &g.rows[i];
    RECT band = row_rect(i);
    if (!r->count || !PtInRect(&band, pt)) continue;
    h.row = i;
    for (int side = 0; side < 2; side++) {
      RECT a = arrow_rect(i, side);
      if (arrow_shown(i, side) && PtInRect(&a, pt)) {
        h.part = side ? PART_RIGHT : PART_LEFT;
        return h;
      }
    }
    RECT first = cover_rect(i, 0);
    int cx = x + r->pos - g.pad, index = cx >= 0 ? cx / pitch() : -1;
    if (index >= 0 && index < r->count && cx % pitch() < g.tile_w && y >= first.top) {
      h.part = PART_TILE;
      h.index = index;
    }
    return h;
  }
  return h;
}

/* --- Thumbnails (UI thread) --- */

static BOOL by_uid(const tile *t) {
  return t->pick.kind == MC_RECENT_PLAYLIST || t->pick.kind == MC_RECENT_ALBUM_GROUP;
}

static BOOL round_tile(const tile *t) {
  return t->pick.kind == MC_RECENT_ARTIST && !t->pick.station;
}

static BOOL wants_thumb(const tile *t) {
  return (t->pick.cover_dir || by_uid(t)) && !t->failed && (!t->thumb || t->stale || t->thumb_size != g.tile_w);
}

/* Adds tiles first to last of row to the queue, those it wants. The lock is held. */
static void queue_tiles(int row, int first, int last) {
  home_row *r = &g.rows[row];
  if (first < 0) first = 0;
  for (int i = first; i <= last && i < r->count; i++) {
    tile *t = &r->tiles[i];
    if (!wants_thumb(t) || (row == g.busy_row && i == g.busy_index && g.busy_gen == g.gen)) continue;
    if (g.queue_len == g.queue_cap) {
      int cap = g.queue_cap ? g.queue_cap * 2 : 64;
      load_job *grown = realloc(g.queue, (size_t)cap * sizeof *grown);
      if (!grown) return;
      g.queue = grown;
      g.queue_cap = cap;
    }
    wchar_t uid[MC_UID_STRLEN];
    if (by_uid(t)) wstr_from_utf8(uid, MC_UID_STRLEN, t->pick.uid);
    g.queue[g.queue_len++] = (load_job){row, i, g.tile_w, round_tile(t) ? g.tile_w / 2 : scale(COVER_RADIUS),
                                        _wcsdup(by_uid(t) ? uid : t->pick.cover_dir), by_uid(t),
                                        t->pick.kind == MC_RECENT_ALBUM_GROUP};
  }
}

/* Tells the loader what to decode next: the tiles in view, then those within a screen of them,
 * along their rows or in the rows above and below. Called whenever anything moves, so fast
 * scrolling never builds a backlog. */
static void request_thumbs(void) {
  if (!pitch()) return;
  int per_screen = g.client_w / pitch() + 2;
  EnterCriticalSection(&g.lock);
  for (int i = 0; i < g.queue_len; i++) free(g.queue[i].key);
  g.queue_len = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
      const home_row *r = &g.rows[i];
      if (!r->count) continue;
      RECT band = row_rect(i);
      BOOL in_view = band.bottom > 0 && band.top < g.client_h;
      BOOL near_view = band.bottom > -g.client_h && band.top < 2 * g.client_h;
      int first = (r->pos - g.pad) / pitch();
      if (pass == 0 && in_view) queue_tiles(i, first, first + per_screen - 1);
      if (pass == 1 && in_view) queue_tiles(i, first - per_screen, first - 1);
      if (pass == 1 && in_view) queue_tiles(i, first + per_screen, first + 2 * per_screen - 1);
      if (pass == 1 && near_view && !in_view) queue_tiles(i, first, first + per_screen - 1);
    }
  }
  if (g.queue_len) WakeConditionVariable(&g.wake);
  LeaveCriticalSection(&g.lock);
}

static void free_thumb(tile *t) {
  if (t->thumb) DeleteObject(t->thumb);
  t->thumb = NULL;
  t->pixels = NULL;
  t->thumb_size = 0;
  t->stale = FALSE;
}

static void on_thumb(thumb_result *r) {
  if (r->gen != g.gen || r->row < 0 || r->row >= MC_HOME_ROW_COUNT || r->index < 0 || r->index >= g.rows[r->row].count) {
    if (r->bmp) DeleteObject(r->bmp);
    free(r);
    return;
  }
  tile *t = &g.rows[r->row].tiles[r->index];
  if (r->bmp) {
    free_thumb(t);
    t->thumb = r->bmp;
    t->pixels = r->pixels;
    t->thumb_size = r->size;
  } else if (!t->thumb) {
    t->failed = TRUE;
  } else {
    t->stale = FALSE; /* it had a cover a moment ago: keep showing that */
  }
  RECT rc = cover_rect(r->row, r->index);
  InvalidateRect(g.wnd, &rc, FALSE);
  if (t->thumb && t->thumb_size != g.tile_w) request_thumbs(); /* finished at an old size */
  free(r);
}

/* --- Data --- */

static void update_hot(void);
static void ui_changed(void);

static void free_tile(tile *t) {
  free_thumb(t);
  free(t->pick.title);
  free(t->pick.sub);
  free(t->pick.cover_dir);
  free(t->pick.seed_title);
}

/* The thumbnail of the old tile in any row that shows the same thing, so a reload doesn't blank
 * the covers. It's marked stale and decoded again, since the cover may have changed: a
 * playlist's picture does whenever a track that changes it comes or goes. */
static void carry_thumb(tile *t) {
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    for (int k = 0; k < g.rows[i].count; k++) {
      tile *old = &g.rows[i].tiles[k];
      if (!old->thumb || old->pick.kind != t->pick.kind || old->pick.station != t->pick.station ||
          strcmp(old->pick.uid, t->pick.uid) || strcmp(old->pick.seed_uid, t->pick.seed_uid))
        continue;
      if (!old->pick.cover_dir != !t->pick.cover_dir ||
          (t->pick.cover_dir && wcscmp(old->pick.cover_dir, t->pick.cover_dir)))
        continue;
      t->thumb = old->thumb;
      t->pixels = old->pixels;
      t->thumb_size = old->thumb_size;
      t->stale = TRUE;
      old->thumb = NULL;
      old->pixels = NULL;
      return;
    }
  }
}

/* Asks the loader to read the rows: that lists every album and evaluates the smart lists, too
 * slow for the UI thread in a big library. What's showing stays until they arrive. */
static void reload(void) {
  g.dirty = FALSE;
  EnterCriticalSection(&g.lock);
  free(g.lib_dir);
  g.lib_dir = g.lib ? _strdup(mc_library_dir(g.lib)) : NULL;
  g.picks_wanted = TRUE;
  WakeConditionVariable(&g.wake);
  LeaveCriticalSection(&g.lock);
}

static home_pick wide_pick(const mc_home_pick *p) {
  home_pick w = {.kind = p->kind, .station = p->station != 0, .smart = p->smart != 0};
  memcpy(w.uid, p->uid, sizeof w.uid);
  memcpy(w.seed_uid, p->seed_uid, sizeof w.seed_uid);
  w.seed_title = p->seed_title ? _strdup(p->seed_title) : NULL;
  w.title = wstr_dup_from_utf8(p->title);
  w.sub = wstr_dup_from_utf8(p->sub);
  w.cover_dir = wstr_dup_from_utf8(p->cover_dir);
  return w;
}

/* The loader has read the rows: they become tiles. */
static void on_picks(mc_home_picks *picks) {
  tile *tiles[MC_HOME_ROW_COUNT] = {0};
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    int n = picks->counts[i];
    tiles[i] = n ? calloc((size_t)n, sizeof **tiles) : NULL;
    for (int k = 0; tiles[i] && k < n; k++) {
      tiles[i][k].pick = wide_pick(&picks->rows[i][k]);
      carry_thumb(&tiles[i][k]);
    }
  }

  EnterCriticalSection(&g.lock);
  g.gen++;
  g.loader_gen = g.gen;
  for (int i = 0; i < g.queue_len; i++) free(g.queue[i].key);
  g.queue_len = 0;
  LeaveCriticalSection(&g.lock);

  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    home_row *r = &g.rows[i];
    for (int k = 0; k < r->count; k++) free_tile(&r->tiles[k]);
    free(r->tiles);
    r->tiles = tiles[i];
    r->count = tiles[i] ? picks->counts[i] : 0;
  }
  g.has_music = picks->has_music != 0;
  g.has_plays = picks->has_plays != 0;
  g.loaded = TRUE;
  mc_home_picks_free(picks);
  free(picks);

  g.hot.part = PART_NONE;
  if (g.focus_row >= 0 && g.focus_index >= g.rows[g.focus_row].count) g.focus_row = g.focus_index = -1;
  relayout();
  request_thumbs();
  InvalidateRect(g.wnd, NULL, FALSE);
  update_hot();
  ui_changed();
}

void home_view_set_library(mc_library *lib) {
  g.lib = lib;
  home_view_refresh();
}

void home_view_refresh(void) {
  g.dirty = TRUE;
  if (g.wnd) InvalidateRect(g.wnd, NULL, FALSE); /* reloads on the next paint, if visible */
}

void home_view_set_playlist_art_dir(const char *dir) {
  EnterCriticalSection(&g.lock);
  free(g.art_dir);
  g.art_dir = dir ? _strdup(dir) : NULL;
  LeaveCriticalSection(&g.lock);
}

const char *home_view_clicked(mc_recent_kind *kind) {
  *kind = g.clicked_kind;
  return g.clicked_uid[0] ? g.clicked_uid : NULL;
}

/* --- Scrolling sideways --- */

static void invalidate_row(int row) {
  RECT rc = row_rect(row);
  InvalidateRect(g.wnd, &rc, FALSE);
}

static void row_jump(int row, int pos) {
  home_row *r = &g.rows[row];
  pos = clamp_pos(r, pos);
  r->target = pos;
  if (pos == r->pos) return;
  r->pos = pos;
  invalidate_row(row);
  request_thumbs();
}

static void row_animate(int row, int pos) {
  home_row *r = &g.rows[row];
  int was = r->target;
  r->target = clamp_pos(r, pos);
  if (r->target != r->pos) SetTimer(g.wnd, TIMER_ROWS, USER_TIMER_MINIMUM, NULL);
  if (r->target != was) invalidate_row(row); /* its arrows may come or go */
}

/* One frame of every row that's on its way, eased as the scroller eases the page. */
static void rows_tick(void) {
  BOOL moving = FALSE;
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    home_row *r = &g.rows[i];
    if (r->pos == r->target) continue;
    int diff = r->target - r->pos, step = diff / 4;
    r->pos += step ? step : diff;
    invalidate_row(i);
    moving |= r->pos != r->target;
  }
  if (!moving) KillTimer(g.wnd, TIMER_ROWS);
  request_thumbs();
  update_hot();
}

/* Scrolls by dir pages of whole tiles, stopping with a tile at the left edge. */
static void row_page(int row, int dir) {
  home_row *r = &g.rows[row];
  int whole = (g.client_w - 2 * g.pad + g.gap) / pitch();
  if (whole < 1) whole = 1;
  int first = (r->target + pitch() / 2) / pitch();
  row_animate(row, (first + dir * whole) * pitch());
}

/* Scrolls tile index of row into view, along its row and down the page. */
static void reveal(int row, int index) {
  const home_row *r = &g.rows[row];
  int left = index * pitch(), right = left + g.tile_w + 2 * g.pad;
  if (left < r->target) row_animate(row, left);
  else if (right > r->target + g.client_w) row_animate(row, right - g.client_w);
  RECT band = row_rect(row);
  scroller_reveal(&g.scroll, band.top + g.scroll.pos - g.pad, band.bottom + g.scroll.pos + g.pad);
}

/* What a station plays, for its tooltip and a screen reader. */
static void station_tip(const tile *t, wchar_t *buf, int n) {
  mc_wformat(buf, n, mc_str(MC_STR_WINDOWS_HOME_STATION_TIP), MC_S(t->pick.seed_title ? t->pick.seed_title : ""));
}

/* --- The mouse --- */

/* The row the wheel means: the one under the message's screen position. */
static int wheel_row(LPARAM lp) {
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(g.wnd, &pt);
  return hit_test(pt.x, pt.y).row;
}

static void on_wheel(UINT msg, WPARAM wp, LPARAM lp) {
  int delta = GET_WHEEL_DELTA_WPARAM(wp);
  BOOL shift = (GET_KEYSTATE_WPARAM(wp) & MK_SHIFT) != 0;
  if (msg == WM_MOUSEWHEEL && !shift) {
    scroller_wheel(&g.scroll, delta);
    return;
  }
  if (msg == WM_MOUSEHWHEEL) delta = -delta; /* it's positive to the right, the wheel's upwards */
  int row = wheel_row(lp);
  if (row >= 0) row_animate(row, g.rows[row].target - MulDiv(delta, pitch(), WHEEL_DELTA));
}

static void invalidate_arrows(int row) {
  if (row < 0) return;
  for (int side = 0; side < 2; side++) {
    RECT a = arrow_rect(row, side);
    InvalidateRect(g.wnd, &a, FALSE);
  }
}

static void invalidate_hit(hit h) {
  if (h.part == PART_TILE) {
    RECT rc = tile_outer(h.row, h.index);
    InvalidateRect(g.wnd, &rc, FALSE);
  } else if (h.part == PART_LEFT || h.part == PART_RIGHT) {
    invalidate_arrows(h.row);
  } else if (h.part == PART_ACTION) {
    InvalidateRect(g.wnd, &g.action_rc, FALSE);
  }
}

static BOOL same_hit(hit a, hit b) {
  return a.part == b.part && (a.part == PART_NONE || a.part == PART_ACTION || (a.row == b.row && a.index == b.index));
}

/* The tooltip says what a station is, which a tile can't in two short lines; it covers the
 * station under the mouse, or nothing. */
static void update_tip(void) {
  if (!g.tooltip) return;
  const tile *t = g.hot.part == PART_TILE ? &g.rows[g.hot.row].tiles[g.hot.index] : NULL;
  wchar_t tip[256] = L"";
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = g.wnd, .uId = 1, .lpszText = tip};
  if (t && t->pick.station) {
    ti.rect = tile_outer(g.hot.row, g.hot.index);
    station_tip(t, tip, 256);
  }
  SendMessageW(g.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  SendMessageW(g.tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

static void set_hot(hit h) {
  if (h.row != g.hot_row) {
    int was = g.hot_row;
    g.hot_row = h.row;
    invalidate_arrows(was);
    invalidate_arrows(h.row);
  }
  if (same_hit(h, g.hot)) return;
  invalidate_hit(g.hot);
  invalidate_hit(h);
  g.hot = h;
  update_tip();
}

/* Re-checks what's under the mouse, after things moved under it. */
static void update_hot(void) {
  if (g.panning) return;
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(g.wnd, &pt);
  hit none = {PART_NONE, -1, -1};
  set_hot(g.tracking && !g.scroll.hot && !g.scroll.dragging ? hit_test(pt.x, pt.y) : none);
}

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
  if (g.pressed.row >= 0 && g.pressed.part != PART_LEFT && g.pressed.part != PART_RIGHT && GetCapture() == g.wnd) {
    if (!g.panning && abs(x - g.press_x) >= GetSystemMetrics(SM_CXDRAG)) {
      g.panning = TRUE;
      g.press_x = x; /* so it doesn't jump by the distance it took to start */
      g.last_x = x;
      g.last_time = GetTickCount();
      g.speed = 0;
      invalidate_arrows(g.pressed.row);
      hit none = {PART_NONE, -1, -1};
      invalidate_hit(g.hot);
      g.hot = none;
    }
    if (g.panning) {
      DWORD now = GetTickCount();
      if (now != g.last_time) {
        g.speed = g.speed / 2 + (double)(g.last_x - x) / (double)(now - g.last_time) / 2;
        g.last_x = x;
        g.last_time = now;
      }
      row_jump(g.pressed.row, g.press_pos - (x - g.press_x));
    }
    return;
  }
  if (scroller_mouse_move(&g.scroll, x, y) || g.scroll.dragging) {
    hit none = {PART_NONE, -1, -1};
    set_hot(none);
    return;
  }
  set_hot(hit_test(x, y));
}

static void on_mouse_down(int x, int y) {
  SetFocus(g.wnd);
  g.focus_shown = FALSE;
  if (scroller_mouse_down(&g.scroll, x, y)) return;
  g.pressed = hit_test(x, y);
  g.press_x = x;
  g.panning = FALSE;
  if (g.pressed.row >= 0) {
    home_row *r = &g.rows[g.pressed.row];
    r->target = r->pos; /* catches a row that's still moving */
    g.press_pos = r->pos;
    invalidate_hit(g.pressed);
  }
  if (g.pressed.part != PART_NONE || g.pressed.row >= 0) SetCapture(g.wnd);
}

static void open_tile(int row, int index) {
  const tile *t = &g.rows[row].tiles[index];
  if (t->pick.station) {
    char seed[MC_UID_STRLEN]; /* playing can bring a reload, which frees the tile */
    memcpy(seed, t->pick.seed_uid, sizeof seed);
    playback_play_radio(seed);
    return;
  }
  memcpy(g.clicked_uid, t->pick.uid, sizeof g.clicked_uid);
  g.clicked_kind = t->pick.kind;
  PostMessageW(GetParent(g.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(g.wnd), HVN_OPEN), (LPARAM)g.wnd);
}

static void press_action(void) {
  PostMessageW(GetParent(g.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(g.wnd), HVN_IMPORT), (LPARAM)g.wnd);
}

static void on_mouse_up(int x, int y) {
  scroller_mouse_up(&g.scroll);
  hit was = g.pressed, now = hit_test(x, y);
  BOOL panned = g.panning;
  hit none = {PART_NONE, -1, -1};
  g.pressed = none;
  g.panning = FALSE;
  if (GetCapture() == g.wnd) ReleaseCapture();
  if (panned) {
    /* Stopped still before letting go, it stays put. */
    if (GetTickCount() - g.last_time < 80) row_animate(was.row, g.rows[was.row].pos + (int)(g.speed * FLING_MS));
    update_hot();
    return;
  }
  invalidate_hit(was);
  if (!same_hit(now, was)) return;
  if (was.part == PART_TILE) open_tile(was.row, was.index);
  else if (was.part == PART_LEFT) row_page(was.row, -1);
  else if (was.part == PART_RIGHT) row_page(was.row, 1);
  else if (was.part == PART_ACTION) press_action();
}

/* --- The keyboard --- */

static int first_row(void) {
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++)
    if (g.rows[i].count) return i;
  return -1;
}

static void invalidate_focus(void) {
  if (g.focus_row < 0) return;
  RECT rc = tile_outer(g.focus_row, g.focus_index);
  InvalidateRect(g.wnd, &rc, FALSE);
}

static void move_focus(int row, int index) {
  invalidate_focus();
  g.focus_row = row;
  g.focus_index = index;
  g.focus_shown = TRUE;
  reveal(row, index);
  invalidate_focus();
  uia_focus(&ui);
}

/* The first tile showing in row, for coming to it from another. */
static int first_shown(int row) {
  const home_row *r = &g.rows[row];
  int index = (r->target + pitch() - 1) / pitch();
  return index < r->count ? index : r->count - 1;
}

static BOOL on_key(WPARAM key) {
  if (key == VK_NEXT || key == VK_PRIOR || key == VK_HOME || key == VK_END) {
    int code = key == VK_NEXT ? SB_PAGEDOWN : key == VK_PRIOR ? SB_PAGEUP : key == VK_HOME ? SB_TOP : SB_BOTTOM;
    scroller_command(&g.scroll, code, 0);
    return TRUE;
  }
  if (!g.has_music) {
    if (key == VK_RETURN && !IsRectEmpty(&g.action_rc)) press_action();
    return key == VK_RETURN;
  }
  if (key != VK_LEFT && key != VK_RIGHT && key != VK_UP && key != VK_DOWN && key != VK_RETURN) return FALSE;
  int row = g.focus_row, index = g.focus_index;
  if (row < 0) { /* the first key shows where the focus is */
    row = first_row();
    if (row >= 0) move_focus(row, first_shown(row));
    return row >= 0;
  }
  if (key == VK_RETURN) {
    open_tile(row, index);
  } else if (key == VK_LEFT || key == VK_RIGHT) {
    index += key == VK_LEFT ? -1 : 1;
    if (index >= 0 && index < g.rows[row].count) move_focus(row, index);
  } else {
    int step = key == VK_UP ? -1 : 1;
    for (int i = row + step; i >= 0 && i < MC_HOME_ROW_COUNT; i += step) {
      if (!g.rows[i].count) continue;
      move_focus(i, first_shown(i));
      break;
    }
  }
  return TRUE;
}

/* --- Painting --- */

static void paint_placeholder(canvas *c, const tile *t, const RECT *art) {
  BOOL round = round_tile(t);
  canvas_fill(c, art, round ? g.tile_w / 2 : scale(COVER_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
  if ((t->pick.cover_dir || by_uid(t)) && !t->failed) return; /* on its way */
  /* A smart list's bolt, the sidebar's icon for the kind, or a note. */
  const wchar_t *glyph = t->pick.smart                             ? L"\xE945"
                         : t->pick.kind == MC_RECENT_PLAYLIST      ? L"\xE90B"
                         : t->pick.kind == MC_RECENT_ALBUM_GROUP   ? L"\xE8F1"
                         : t->pick.kind == MC_RECENT_ARTIST        ? L"\xE77B"
                                                                   : L"\xE8D6";
  HGDIOBJ f = SelectObject(c->dc, g.glyph_font);
  SetTextColor(c->dc, theme.text3);
  DrawTextW(c->dc, glyph, -1, (RECT *)art, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(c->dc, f);
}

/* A station's mark, so it doesn't read as the album on it. */
static void paint_badge(canvas *c, const RECT *art) {
  int d = scale(BADGE), inset = scale(BADGE_INSET);
  RECT r = {art->right - inset - d, art->bottom - inset - d, art->right - inset, art->bottom - inset};
  canvas_fill(c, &r, d / 2, theme.accent, 255);
  HGDIOBJ f = SelectObject(c->dc, g.badge_font);
  SetTextColor(c->dc, theme.on_accent);
  DrawTextW(c->dc, L"\xEC05", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(c->dc, f);
}

static void paint_tile(canvas *c, int row, int index) {
  const tile *t = &g.rows[row].tiles[index];
  RECT art = cover_rect(row, index), outer = tile_outer(row, index);
  hit me = {PART_TILE, row, index};
  BOOL pressed = same_hit(g.pressed, me) && !g.panning;
  if (pressed) theme_draw_state(c, &outer, scale(8), STATE_PRESSED);
  else if (same_hit(g.hot, me)) theme_draw_state(c, &outer, scale(8), STATE_HOT);
  if (g.focus_shown && row == g.focus_row && index == g.focus_index && GetFocus() == g.wnd) {
    RECT ring = art;
    InflateRect(&ring, scale(RING_GAP + RING_W), scale(RING_GAP + RING_W));
    int radius = round_tile(t) ? (ring.right - ring.left) / 2 : scale(COVER_RADIUS + RING_GAP + RING_W);
    canvas_frame(c, &ring, radius, scale(RING_W), theme.accent, 255);
  }

  if (t->pixels) canvas_draw_image(c, &art, t->pixels, t->thumb_size, t->thumb_size); /* mid-resize: stretched */
  else paint_placeholder(c, t, &art);
  if (t->pick.station) paint_badge(c, &art);

  RECT line = {art.left, art.bottom + g.text_gap, art.right, art.bottom + g.text_gap + g.text_h};
  UINT one_line = DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, t->pick.title ? t->pick.title : L"", -1, &line, one_line);
  OffsetRect(&line, 0, g.text_h);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, t->pick.sub ? t->pick.sub : L"", -1, &line, one_line);
}

static void paint_row(canvas *c, int row, const RECT *dirty) {
  const home_row *r = &g.rows[row];
  RECT band = row_rect(row), isect;
  if (!r->count || !IntersectRect(&isect, &band, dirty)) return;

  RECT title = {g.pad, band.top, g.client_w - g.pad, band.top + g.title_h};
  HGDIOBJ f = SelectObject(c->dc, g.title_font ? g.title_font : g.font);
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, mc_wstr(mc_home_row_title((mc_home_row_id)row)), -1, &title, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(c->dc, f);

  int first = (r->pos - g.pad - scale(HOVER_PAD)) / pitch(), last = (r->pos + g.client_w) / pitch();
  if (first < 0) first = 0;
  for (int i = first; i <= last && i < r->count; i++) {
    RECT outer = tile_outer(row, i);
    if (IntersectRect(&isect, &outer, dirty)) paint_tile(c, row, i);
  }
}

/* A row's arrow: a disc over the covers, so it has to stand off them in either theme. */
static void paint_arrow(canvas *c, int row, BOOL right) {
  if (!arrow_shown(row, right)) return;
  RECT r = arrow_rect(row, right);
  int d = r.right - r.left;
  hit me = {right ? PART_RIGHT : PART_LEFT, row, -1};
  BOOL hot = same_hit(g.hot, me), pressed = same_hit(g.pressed, me);
  canvas_fill(c, &r, d / 2, theme.bg, pressed ? 200 : hot ? 255 : 235);
  canvas_frame(c, &r, d / 2, 1, theme.high_contrast ? theme.text : theme.line, 255);
  if (hot && !theme.high_contrast) canvas_fill(c, &r, d / 2, theme.text, 20);
  canvas_text(c, right ? L"\xE76C" : L"\xE76B", 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
              g.icon_font, theme.text);
}

/* A library with nothing in it: a welcome rather than a blank page. Keeps g.action_rc. */
static void paint_welcome(canvas *c) {
  SetRectEmpty(&g.action_rc);
  wchar_t title[128] = L"";
  const wchar_t *text = NULL;
  if (g.lib) {
    mc_wformat(title, 128, mc_str(MC_STR_HOME_WELCOME_TITLE), MC_S(mc_str(MC_STR_APP_NAME)));
    text = mc_wstr(MC_STR_HOME_WELCOME_TEXT);
  } else {
    wcscpy(title, mc_wstr(MC_STR_LIBRARY_OPEN_FAILED));
  }
  const wchar_t *label = g.lib ? mc_wstr(MC_STR_IMPORT_MUSIC) : NULL;

  int w = g.client_w - 2 * g.pad, max_w = scale(420);
  if (w > max_w) w = max_w;
  RECT block = {(g.client_w - w) / 2, 0, (g.client_w - w) / 2 + w, 0};
  RECT text_rc = block;
  if (text) DrawTextW(c->dc, text, -1, &text_rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  int disc = scale(96), welcome_h = font_height(g.welcome_font), gap = scale(16), action_h = scale(ACTION_H);
  int text_h = text ? text_rc.bottom - text_rc.top : 0;
  int total = disc + gap + welcome_h + (text ? scale(8) + text_h : 0) + (label ? scale(20) + action_h : 0);
  int y = (g.client_h - total) / 2;

  RECT d = {(g.client_w - disc) / 2, y, (g.client_w + disc) / 2, y + disc};
  canvas_fill(c, &d, disc / 2, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
  HGDIOBJ f = SelectObject(c->dc, g.glyph_font);
  SetTextColor(c->dc, theme.text3);
  DrawTextW(c->dc, L"\xE8D6", -1, &d, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(c->dc, f);
  y += disc + gap;

  RECT line = {block.left, y, block.right, y + welcome_h};
  f = SelectObject(c->dc, g.welcome_font ? g.welcome_font : g.font);
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, title, -1, &line, DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(c->dc, f);
  y += welcome_h;

  if (text) {
    y += scale(8);
    RECT body = {block.left, y, block.right, y + text_h};
    SetTextColor(c->dc, theme.text2);
    DrawTextW(c->dc, text, -1, &body, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
    y += text_h;
  }
  if (!label) return;
  y += scale(20);
  SIZE size;
  GetTextExtentPoint32W(c->dc, label, (int)wcslen(label), &size);
  int button_w = size.cx + 2 * scale(20);
  RECT b = {(g.client_w - button_w) / 2, y, (g.client_w + button_w) / 2, y + action_h};
  g.action_rc = b;
  hit me = {PART_ACTION, -1, -1};
  BOOL hot = same_hit(g.hot, me), pressed = same_hit(g.pressed, me);
  canvas_fill(c, &b, scale(4), theme.accent, theme.high_contrast || (!hot && !pressed) ? 255 : pressed ? 200 : 230);
  SetTextColor(c->dc, theme.on_accent);
  DrawTextW(c->dc, label, -1, &b, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

static void paint(HDC dc, const RECT *dirty) {
  if (!canvas_reserve(&g.buf, g.client_w, g.client_h)) return;
  canvas *c = &g.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, g.font ? (HGDIOBJ)g.font : GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(c->dc, TRANSPARENT);

  if (!g.loaded) {
    /* a moment's blank: the welcome would flash up over a full library */
  } else if (!g.has_music) {
    paint_welcome(c);
  } else {
    SetRectEmpty(&g.action_rc);
    for (int i = 0; i < MC_HOME_ROW_COUNT; i++) paint_row(c, i, dirty);
    if (g.hint_top) {
      RECT hint = {g.pad, g.hint_top - g.scroll.pos, g.client_w - g.pad, g.hint_top - g.scroll.pos + g.text_h};
      SetTextColor(c->dc, theme.text3);
      DrawTextW(c->dc, mc_wstr(MC_STR_HOME_NO_HISTORY), -1, &hint, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
  }
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  if (g.hot_row >= 0) {
    paint_arrow(c, g.hot_row, FALSE);
    paint_arrow(c, g.hot_row, TRUE);
  }
  scroller_paint(&g.scroll, c);

  /* The content area's border and rounded corner, against the sidebar and toolbar. */
  int radius = scale(CONTENT_RADIUS);
  RECT all = {0, 0, g.client_w, g.client_h}, border = {0, 0, g.client_w + radius, g.client_h + radius};
  canvas_frame(c, &border, radius, 1, theme.line, 255);
  canvas_round_corners(c, &all, radius, CORNER_TL, theme.frame, theme.mica ? 0 : 255);

  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- The window --- */

static void free_fonts(void) {
  HFONT *owned[] = {&g.title_font, &g.welcome_font, &g.glyph_font, &g.icon_font, &g.badge_font};
  for (size_t i = 0; i < sizeof owned / sizeof owned[0]; i++) {
    if (*owned[i]) DeleteObject(*owned[i]);
    *owned[i] = NULL;
  }
}

static void make_fonts(HFONT font) {
  free_fonts();
  g.font = font;
  LOGFONTW lf;
  if (font && GetObjectW(font, sizeof lf, &lf)) {
    LONG height = lf.lfHeight;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = MulDiv(height, 3, 2);
    g.title_font = CreateFontIndirectW(&lf);
    lf.lfHeight = MulDiv(height, 2, 1);
    g.welcome_font = CreateFontIndirectW(&lf);
  }
  HFONT *icons[] = {&g.glyph_font, &g.icon_font, &g.badge_font};
  int sizes[] = {scale(TILE_W) / 4, scale(14), scale(15)};
  for (int i = 0; i < 3; i++)
    *icons[i] = CreateFontW(-sizes[i], 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

void home_view_update(UINT dpi, HFONT font) {
  (void)dpi; /* scale() asks the window */
  make_fonts(font);
  if (g.tooltip) theme_apply_control(g.tooltip);
  relayout();
  request_thumbs();
  InvalidateRect(g.wnd, NULL, FALSE);
}

/* --- Screen readers ---
 *
 * The rows that have tiles are nodes 0 on, in order, and each one's tiles follow all the rows,
 * row after row. */

static int ui_rows(void) {
  int n = 0;
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) n += g.rows[i].count > 0;
  return n;
}

/* The row of g.rows that's the nth with tiles, or -1. */
static int ui_row_at(int n) {
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++)
    if (g.rows[i].count && n-- == 0) return i;
  return -1;
}

/* Tile index of row, as a node. */
static int ui_tile(int row, int index) {
  int node = ui_rows();
  for (int i = 0; i < row; i++) node += g.rows[i].count;
  return node + index;
}

/* The row and tile of a node, FALSE if it's no tile. */
static BOOL ui_find_tile(int node, int *row, int *index) {
  int t = node - ui_rows();
  if (node < 0 || t < 0) return FALSE;
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    if (t < g.rows[i].count) {
      *row = i;
      *index = t;
      return TRUE;
    }
    t -= g.rows[i].count;
  }
  return FALSE;
}

static BOOL ui_node(int i, uia_node *n) {
  n->parent = UIA_ROOT;
  if (i == UIA_ROOT) {
    n->type = UIA_PaneControlTypeId;
    lstrcpynW(n->name, mc_wstr(MC_STR_SECTION_HOME), 512);
    n->count = ui_rows();
    return TRUE;
  }
  int row = i >= 0 && i < ui_rows() ? ui_row_at(i) : -1;
  if (row >= 0) {
    n->type = UIA_GroupControlTypeId;
    n->rect = row_rect(row);
    n->first = ui_tile(row, 0);
    n->count = g.rows[row].count;
    lstrcpynW(n->name, mc_wstr(mc_home_row_title((mc_home_row_id)row)), 512);
    return TRUE;
  }
  int index;
  if (!ui_find_tile(i, &row, &index)) return FALSE;
  const tile *t = &g.rows[row].tiles[index];
  n->parent = 0;
  for (int k = 0; k < row; k++) n->parent += g.rows[k].count > 0;
  n->type = UIA_ButtonControlTypeId;
  n->flags = UIA_FOCUSABLE | UIA_INVOKABLE;
  RECT client = {0, 0, g.client_w, g.client_h}, outer = tile_outer(row, index);
  InflateRect(&outer, -scale(HOVER_PAD), -scale(HOVER_PAD));
  if (IntersectRect(&n->rect, &outer, &client)) n->rect = outer;
  if (t->pick.station) {
    uia_name(n, mc_wstr(MC_STR_WINDOWS_HOME_STATION_NAME), t->pick.title ? t->pick.title : L"",
             t->pick.sub ? t->pick.sub : L"", NULL);
    station_tip(t, n->help, 256);
  } else {
    uia_name(n, t->pick.title ? t->pick.title : L"", t->pick.sub ? t->pick.sub : L"", NULL);
  }
  return TRUE;
}

static int ui_focus(void) {
  return g.focus_row >= 0 && g.focus_index >= 0 && g.focus_index < g.rows[g.focus_row].count
           ? ui_tile(g.focus_row, g.focus_index)
           : UIA_ROOT;
}

static int ui_at(POINT pt) {
  hit h = hit_test(pt.x, pt.y);
  if (h.part == PART_TILE) return ui_tile(h.row, h.index);
  for (int i = 0, n = 0; h.row >= 0 && i < MC_HOME_ROW_COUNT; i++) {
    if (i == h.row) return n;
    n += g.rows[i].count > 0;
  }
  return UIA_ROOT;
}

static void ui_invoke(int i) {
  int row, index;
  if (ui_find_tile(i, &row, &index)) open_tile(row, index);
}

static void ui_reveal(int i) {
  int row, index;
  if (ui_find_tile(i, &row, &index)) reveal(row, index);
}

static void ui_set_focus(int i) {
  int row, index;
  if (ui_find_tile(i, &row, &index)) move_focus(row, index);
}

static const uia_ops ui_ops = {
  .node = ui_node,
  .focus = ui_focus,
  .at = ui_at,
  .invoke = ui_invoke,
  .reveal = ui_reveal,
  .set_focus = ui_set_focus,
};

/* After a reload: what the nodes are, so clients only start over when they changed. */
static void ui_changed(void) {
  unsigned long sig = 0;
  for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
    sig = uia_hash(sig, &g.rows[i].count, sizeof g.rows[i].count);
    for (int k = 0; k < g.rows[i].count; k++) {
      const home_pick *p = &g.rows[i].tiles[k].pick;
      sig = uia_hash(sig, p->uid, strlen(p->uid));
      sig = uia_hash(sig, p->seed_uid, strlen(p->seed_uid));
    }
  }
  uia_changed(&ui, sig);
}

static LRESULT CALLBACK home_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!g.wnd) g.wnd = g.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  LRESULT result;
  if (ui.wnd && uia_message(&ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_SIZE:
      relayout();
      request_thumbs();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1; /* painted entirely in WM_PAINT */
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      invalidate_focus();
      break;
    case WM_PAINT: {
      if (g.dirty) reload(); /* deferred while hidden; painting means we're visible */
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      on_wheel(msg, wp, lp);
      return 0;
    case WM_TIMER:
      if (wp == TIMER_ROWS) rows_tick();
      else scroller_timer(&g.scroll, wp);
      return 0;
    case WM_MOUSEMOVE:
      on_mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE: {
      g.tracking = FALSE;
      scroller_mouse_leave(&g.scroll);
      hit none = {PART_NONE, -1, -1};
      if (!g.panning) set_hot(none);
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
      on_mouse_up(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&g.scroll);
      g.panning = FALSE;
      g.pressed.part = PART_NONE;
      g.pressed.row = g.pressed.index = -1;
      return 0;
    case WM_KEYDOWN:
      if (on_key(wp)) return 0;
      break;
    case WM_APP_THUMB:
      on_thumb((thumb_result *)lp);
      return 0;
    case WM_APP_PICKS:
      on_picks((mc_home_picks *)lp);
      return 0;
    case WM_DESTROY:
      uia_destroy(&ui);
      if (g.tooltip) DestroyWindow(g.tooltip);
      EnterCriticalSection(&g.lock);
      g.quit = TRUE;
      WakeConditionVariable(&g.wake);
      LeaveCriticalSection(&g.lock);
      WaitForSingleObject(g.thread, INFINITE);
      CloseHandle(g.thread);
      for (int i = 0; i < g.queue_len; i++) free(g.queue[i].key);
      free(g.queue);
      for (int i = 0; i < MC_HOME_ROW_COUNT; i++) {
        for (int k = 0; k < g.rows[i].count; k++) free_tile(&g.rows[i].tiles[k]);
        free(g.rows[i].tiles);
      }
      free(g.lib_dir);
      free(g.art_dir);
      canvas_free(&g.buf);
      free_fonts();
      DeleteCriticalSection(&g.lock);
      memset(&g, 0, sizeof g);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND home_view_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = home_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_home"};
  RegisterClassExW(&wc);

  memset(&g, 0, sizeof g);
  g.busy_row = g.busy_index = -1;
  g.hot_row = g.focus_row = g.focus_index = -1;
  g.hot.row = g.hot.index = g.pressed.row = g.pressed.index = -1;
  g.scroll.moved = on_scrolled;
  g.dirty = TRUE;
  if (rand_s(&g.seed)) g.seed = GetTickCount();
  InitializeCriticalSection(&g.lock);
  InitializeConditionVariable(&g.wake);
  g.wnd = CreateWindowExW(0, L"kithsong_home", NULL, WS_CHILD, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                          GetModuleHandleW(NULL), NULL);
  if (!g.wnd) return NULL;
  uia_init(&ui, g.wnd, &ui_ops);
  g.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g.wnd, NULL,
                              GetModuleHandleW(NULL), NULL);
  if (g.tooltip) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = g.wnd, .uId = 1, .lpszText = L""};
    SendMessageW(g.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(g.tooltip, TTM_SETMAXTIPWIDTH, 0, scale(320));
    theme_apply_control(g.tooltip);
  }
  g.thread = CreateThread(NULL, 0, loader_main, NULL, 0, NULL);
  return g.wnd;
}
