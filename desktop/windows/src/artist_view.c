#define COBJMACROS
#include "artist_view.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <wincodec.h>
#include <windowsx.h>

#include "add_to_playlist.h"
#include "canvas.h"
#include "cover.h"
#include "drag.h"
#include "remove_here.h"
#include "scroller.h"
#include "theme.h"
#include "wstr.h"

#define WM_APP_COVER (WM_APP + 22) /* decode the next cover */

/* An artist's page's strips of tiles, in order down the page. */
enum { STRIP_ALBUMS, STRIP_SINGLES, NUM_STRIPS };

enum { TIMER_STRIP = 1 }; /* plus the strip: its easing (the list's scroller has 101 and 102) */

/* Metrics at 96 DPI. */
enum {
  PAD_X = 24,
  LIST_TOP = 12, /* above the first artist, and below the last */
  ROW_H = 52,
  ROW_ICON_W = 44,
  RADIUS = 6,
  HEAD_TOP = 20,
  BACK = 32, /* the back button's square, and the arrows' */
  BACK_GAP = 12,
  NAME_GAP = 2, /* between the name and the summary */
  HEAD_BOTTOM = 12,
  HEADING_H = 40,
  ARROW_GAP = 4,
  TILE_W = 150,
  TILE_GAP = 20,
  TEXT_GAP = 8,
  HOVER_PAD = 8,
  COVER_RADIUS = 6,
};

/* Hit codes besides rows and tiles (which are >= 0; the tiles are numbered through the
 * albums, then the singles). Strip sk's arrows are HIT_ARROWS - 2sk (left) and one below
 * that (right): see arrow_hit(). */
enum { HIT_NONE = -1, HIT_BACK = -2, HIT_ARROWS = -3 };

typedef struct artist_row {
  char uid[MC_UID_STRLEN];
  wchar_t *name;
  wchar_t sub[64]; /* "12 albums", or "" for one only credited on tracks */
} artist_row;

/* An album, or a single: a track alone on its album here, or on none. */
typedef struct tile {
  char uid[MC_UID_STRLEN]; /* the album's, or the single's track's */
  wchar_t *title;
  wchar_t *sub;       /* its year, or an album's artist when that's someone else; NULL for none */
  wchar_t *cover_dir; /* NULL when the album has no cover */
  HBITMAP thumb;      /* premultiplied, with rounded corners */
  const uint32_t *pixels;
  int thumb_size;
  BOOL failed; /* the cover couldn't be decoded; draw the placeholder */
  /* A single's, for the play queue. */
  wchar_t *artist, *album; /* NULL if unknown, or for no album */
  char artist_uid[MC_UID_STRLEN]; /* "" if unknown */
  char *path;              /* UTF-8; NULL if it has no file on this device */
  int duration_ms;
} tile;

typedef struct strip {
  tile *tiles;
  int n;
  int pos, target;           /* scrolled sideways, and where the easing is heading */
  RECT heading, left, right; /* in the client area; empty while it has no tiles */
  int top;                   /* the tiles' */
} strip;

static struct {
  HWND wnd, tooltip;
  UINT dpi;
  HFONT font; /* the main window's */
  HFONT title_font, heading_font, icon_font, glyph_font;
  int text_h, title_h;
  canvas buf;
  scroller scroll; /* the list */
  IWICImagingFactory *wic;

  mc_library *lib;
  BOOL stale;       /* reload when next painted */
  mc_status status; /* of listing the artists */
  artist_row *artists;
  int nartists;

  /* The open artist, if any. */
  char open_uid[MC_UID_STRLEN];
  wchar_t *open_name;
  wchar_t summary[128]; /* "12 albums · 140 tracks" */
  strip strips[NUM_STRIPS];
  BOOL cover_posted; /* WM_APP_COVER is on its way */
  char opened_album[MC_UID_STRLEN];
  int play_index; /* the single ARN_PLAY_SINGLE was last sent for */
  track_list_request req;

  /* Layout of the open artist's header, in the client area. */
  RECT back, tracks_heading;
  int tile_h, header_h;

  int hot;     /* the hit under the mouse, or HIT_NONE */
  int pressed; /* the hit the mouse went down on: it acts on release */
  int focus;   /* the row (or, on an artist's page, the tile) with the keyboard focus, or -1 */
  int list_focus; /* the list's, kept while a page is open */
  BOOL reveal_focus; /* back from a page: show its row once the list has its size again */
  BOOL tracking;

  /* Dragging the single pressed (v.pressed) onto a playlist, once it has moved far enough
   * from drag_from; drop_target is the playlist it would be added to (drag.h), or "". */
  POINT drag_from;
  BOOL dragging;
  char drop_target[MC_UID_STRLEN];
} v;

static int scale(int px) {
  return MulDiv(px, (int)v.dpi, USER_DEFAULT_SCREEN_DPI);
}

static BOOL is_open(void) {
  return v.open_uid[0] != '\0';
}

static void notify(int code, BOOL post) {
  HWND parent = GetParent(v.wnd);
  WPARAM wp = MAKEWPARAM(GetDlgCtrlID(v.wnd), code);
  if (post) PostMessageW(parent, WM_COMMAND, wp, (LPARAM)v.wnd);
  else SendMessageW(parent, WM_COMMAND, wp, (LPARAM)v.wnd);
}

/* --- Tiles --- */

static int num_tiles(void) {
  return v.strips[STRIP_ALBUMS].n + v.strips[STRIP_SINGLES].n;
}

/* Tile i's strip, and with k its place there. */
static int tile_strip(int i, int *k) {
  int albums = v.strips[STRIP_ALBUMS].n, sk = i < albums ? STRIP_ALBUMS : STRIP_SINGLES;
  if (k) *k = sk == STRIP_ALBUMS ? i : i - albums;
  return sk;
}

/* The first tile of strip sk. */
static int strip_first(int sk) {
  return sk == STRIP_ALBUMS ? 0 : v.strips[STRIP_ALBUMS].n;
}

static tile *tile_at(int i) {
  int k, sk = tile_strip(i, &k);
  return &v.strips[sk].tiles[k];
}

static BOOL is_single(int i) {
  return i >= v.strips[STRIP_ALBUMS].n && i < num_tiles();
}

static int arrow_hit(int sk, int dir) {
  return HIT_ARROWS - 2 * sk - (dir > 0);
}

/* Whether hit is an arrow, and if so whose and which way. */
static BOOL is_arrow(int hit, int *sk, int *dir) {
  if (hit > HIT_ARROWS || hit <= HIT_ARROWS - 2 * NUM_STRIPS) return FALSE;
  *sk = (HIT_ARROWS - hit) / 2;
  *dir = (HIT_ARROWS - hit) % 2 ? +1 : -1;
  return TRUE;
}

/* --- Layout --- */

static int font_height(HFONT font) {
  HDC dc = GetDC(v.wnd);
  HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(v.wnd, dc);
  return tm.tmHeight;
}

static int tile_pitch(void) {
  return scale(TILE_W) + scale(TILE_GAP);
}

/* How far strip sk scrolls: its tiles, with room for the hover fill, past the padding. */
static int strip_max(int sk) {
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int n = v.strips[sk].n;
  int content = n ? n * tile_pitch() - scale(TILE_GAP) + 2 * scale(HOVER_PAD) : 0;
  int view = rc.right - 2 * scale(PAD_X);
  return content > view ? content - view : 0;
}

static int clamp_strip(int sk, int pos) {
  int max = strip_max(sk);
  return pos < 0 ? 0 : pos > max ? max : pos;
}

static void layout(void) {
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int w = rc.right, pad = scale(PAD_X);
  v.text_h = font_height(v.font);
  v.title_h = font_height(v.title_font);

  /* An artist's header: the back button beside the name, then the strips that have anything
   * in them, each under its heading, then a heading for the track list beneath. */
  int name_block = v.title_h + scale(NAME_GAP) + v.text_h;
  int top = scale(HEAD_TOP);
  SetRect(&v.back, pad, top + (v.title_h - scale(BACK)) / 2, pad + scale(BACK), top + (v.title_h + scale(BACK)) / 2);
  int y = top + max(scale(BACK), name_block) + scale(HEAD_BOTTOM);
  v.tile_h = scale(TILE_W) + scale(TEXT_GAP) + 2 * v.text_h;
  for (int sk = 0; sk < NUM_STRIPS; sk++) {
    strip *st = &v.strips[sk];
    st->top = y;
    if (!st->n) {
      SetRectEmpty(&st->heading);
      SetRectEmpty(&st->left);
      SetRectEmpty(&st->right);
      continue;
    }
    SetRect(&st->heading, pad, y, w - pad, y + scale(HEADING_H));
    int mid = y + scale(HEADING_H) / 2;
    SetRect(&st->right, w - pad - scale(BACK), mid - scale(BACK) / 2, w - pad, mid + scale(BACK) / 2);
    st->left = st->right;
    OffsetRect(&st->left, -(scale(BACK) + scale(ARROW_GAP)), 0);
    st->top = st->heading.bottom + scale(HOVER_PAD);
    y = st->top + v.tile_h + scale(HOVER_PAD);
    st->pos = clamp_strip(sk, st->pos);
    st->target = clamp_strip(sk, st->target);
  }
  SetRect(&v.tracks_heading, pad, y, w - pad, y + scale(HEADING_H));
  int was = v.header_h;
  v.header_h = v.tracks_heading.bottom;
  if (is_open() && v.header_h != was) notify(ARN_RESIZED, TRUE);

  RECT view = rc;
  if (is_open()) SetRectEmpty(&view);
  scroller_resize(&v.scroll, &view, v.nartists ? v.nartists * scale(ROW_H) + 2 * scale(LIST_TOP) : 0);

  if (v.tooltip) { /* Back's is 1, then each strip's arrows, left and right */
    RECT none = {0};
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = v.wnd, .uId = 1, .rect = is_open() ? v.back : none};
    SendMessageW(v.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    for (int sk = 0; sk < NUM_STRIPS; sk++) {
      BOOL arrows = is_open() && strip_max(sk) > 0;
      ti.uId = 2 + 2 * (UINT_PTR)sk;
      ti.rect = arrows ? v.strips[sk].left : none;
      SendMessageW(v.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
      ti.uId++;
      ti.rect = arrows ? v.strips[sk].right : none;
      SendMessageW(v.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    }
  }
}

/* Row i's fill, in the client area. */
static RECT row_rect(int i) {
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int top = scale(LIST_TOP) + i * scale(ROW_H) - v.scroll.pos;
  RECT r = {scale(PAD_X), top, rc.right - scale(PAD_X), top + scale(ROW_H)};
  return r;
}

/* Tile i's cover and text, in the client area. */
static RECT tile_rect(int i) {
  int k, sk = tile_strip(i, &k);
  const strip *st = &v.strips[sk];
  int x = scale(PAD_X) + scale(HOVER_PAD) + k * tile_pitch() - st->pos;
  RECT r = {x, st->top, x + scale(TILE_W), st->top + v.tile_h};
  return r;
}

/* With room for the hover fill around it. */
static RECT tile_outer(int i) {
  RECT r = tile_rect(i);
  InflateRect(&r, scale(HOVER_PAD), scale(HOVER_PAD));
  return r;
}

/* Where strip sk draws: the full width, so tiles slide out under the edges. Empty while it
 * has no tiles. */
static RECT strip_rect(int sk) {
  RECT rc, r = {0};
  GetClientRect(v.wnd, &rc);
  const strip *st = &v.strips[sk];
  if (st->n) SetRect(&r, 0, st->top - scale(HOVER_PAD), rc.right, st->top + v.tile_h + scale(HOVER_PAD));
  return r;
}

static void invalidate_hit(int i) {
  RECT r;
  int sk, dir;
  if (i == HIT_BACK) r = v.back;
  else if (is_arrow(i, &sk, &dir)) r = dir < 0 ? v.strips[sk].left : v.strips[sk].right;
  else if (i >= 0 && is_open() && i < num_tiles()) r = tile_outer(i);
  else if (i >= 0 && !is_open() && i < v.nartists) r = row_rect(i);
  else return;
  InvalidateRect(v.wnd, &r, FALSE);
}

static int hit_at(int x, int y) {
  POINT pt = {x, y};
  if (!is_open()) {
    int rel = y - scale(LIST_TOP) + v.scroll.pos; /* from the first row's top */
    int i = rel >= 0 ? rel / scale(ROW_H) : -1;
    if (i < 0 || i >= v.nartists) return HIT_NONE;
    RECT r = row_rect(i);
    return PtInRect(&r, pt) ? i : HIT_NONE;
  }
  if (PtInRect(&v.back, pt)) return HIT_BACK;
  for (int sk = 0; sk < NUM_STRIPS; sk++) {
    const strip *st = &v.strips[sk];
    if (strip_max(sk) > 0 && PtInRect(&st->left, pt)) return arrow_hit(sk, -1);
    if (strip_max(sk) > 0 && PtInRect(&st->right, pt)) return arrow_hit(sk, +1);
    RECT band = strip_rect(sk);
    if (!PtInRect(&band, pt)) continue;
    for (int k = 0, first = strip_first(sk); k < st->n; k++) {
      RECT r = tile_outer(first + k);
      if (PtInRect(&r, pt)) return first + k;
    }
  }
  return HIT_NONE;
}

static void set_hot(int i) {
  if (i == v.hot) return;
  invalidate_hit(v.hot);
  invalidate_hit(i);
  v.hot = i;
}

static void update_hot(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(v.wnd, &pt);
  set_hot(v.tracking && !v.scroll.hot && !v.scroll.dragging && v.pressed == HIT_NONE ? hit_at(pt.x, pt.y) : HIT_NONE);
}

static void on_scrolled(void) {
  update_hot();
}

/* --- The strips' sideways scrolling --- */

static void strip_jump(int sk, int pos) {
  strip *st = &v.strips[sk];
  pos = clamp_strip(sk, pos);
  if (pos == st->pos) return;
  st->pos = pos;
  RECT r = strip_rect(sk);
  InvalidateRect(v.wnd, &r, FALSE);
  InvalidateRect(v.wnd, &st->left, FALSE); /* an arrow may grey out */
  InvalidateRect(v.wnd, &st->right, FALSE);
  update_hot();
}

static void strip_animate(int sk, int pos) {
  strip *st = &v.strips[sk];
  st->target = clamp_strip(sk, pos);
  if (st->target != st->pos) SetTimer(v.wnd, TIMER_STRIP + sk, USER_TIMER_MINIMUM, NULL);
}

/* Eases towards the target, a quarter of the way each tick, as scroller.c does. */
static void strip_tick(int sk) {
  strip *st = &v.strips[sk];
  int diff = st->target - st->pos, step = diff / 4;
  if (step == 0) step = diff;
  strip_jump(sk, st->pos + step);
  if (st->pos == st->target || step == 0) KillTimer(v.wnd, TIMER_STRIP + sk);
}

static void strips_stop(void) {
  for (int sk = 0; sk < NUM_STRIPS; sk++) KillTimer(v.wnd, TIMER_STRIP + sk);
}

/* A page of tiles back or forward. */
static void strip_page(int sk, int dir) {
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int view = rc.right - 2 * scale(PAD_X), whole = max(1, view / tile_pitch());
  strip_animate(sk, v.strips[sk].target + dir * whole * tile_pitch());
}

static void reveal_tile(int i) {
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int k, sk = tile_strip(i, &k), target = v.strips[sk].target;
  int view = rc.right - 2 * scale(PAD_X);
  int left = k * tile_pitch(), right = left + scale(TILE_W) + 2 * scale(HOVER_PAD);
  if (left < target) strip_animate(sk, left);
  else if (right > target + view) strip_animate(sk, right - view);
}

/* --- Covers --- */

static void free_thumb(tile *t) {
  if (t->thumb) DeleteObject(t->thumb);
  t->thumb = NULL;
  t->pixels = NULL;
  t->thumb_size = 0;
}

static BOOL wants_cover(const tile *t) {
  return t->cover_dir && !t->failed && t->thumb_size != scale(TILE_W);
}

static void request_covers(void) {
  if (v.cover_posted) return;
  for (int i = 0, n = num_tiles(); i < n; i++)
    if (wants_cover(tile_at(i))) {
      v.cover_posted = PostMessageW(v.wnd, WM_APP_COVER, 0, 0);
      return;
    }
}

/* Decodes one cover, then asks for the next, so input between them goes first. */
static void decode_next_cover(void) {
  v.cover_posted = FALSE;
  int i = 0, n = num_tiles();
  while (i < n && !wants_cover(tile_at(i))) i++;
  if (i == n) return;
  tile *t = tile_at(i);
  if (!v.wic)
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&v.wic);
  free_thumb(t);
  const uint32_t *px = NULL;
  int size = scale(TILE_W);
  HBITMAP bmp = v.wic ? cover_decode(v.wic, t->cover_dir, size, scale(COVER_RADIUS), &px) : NULL;
  if (bmp) {
    t->thumb = bmp;
    t->pixels = px;
    t->thumb_size = size;
  } else {
    t->failed = TRUE;
  }
  invalidate_hit(i);
  request_covers();
}

/* --- Data --- */

static void free_artists(void) {
  for (int i = 0; i < v.nartists; i++) free(v.artists[i].name);
  free(v.artists);
  v.artists = NULL;
  v.nartists = 0;
}

static void free_tiles(tile *tiles, int n) {
  for (int i = 0; i < n; i++) {
    free(tiles[i].title);
    free(tiles[i].sub);
    free(tiles[i].cover_dir);
    free(tiles[i].artist);
    free(tiles[i].album);
    free(tiles[i].path);
    free_thumb(&tiles[i]);
  }
  free(tiles);
}

static void free_strips(void) {
  for (int sk = 0; sk < NUM_STRIPS; sk++) {
    free_tiles(v.strips[sk].tiles, v.strips[sk].n);
    v.strips[sk].tiles = NULL;
    v.strips[sk].n = 0;
    v.strips[sk].pos = v.strips[sk].target = 0;
  }
}

static int find_artist(const char *uid) {
  for (int i = 0; uid && i < v.nartists; i++)
    if (!strcmp(v.artists[i].uid, uid)) return i;
  return -1;
}

/* Takes the cover t had in strip sk before a reload, if it's the same, so it doesn't flash. */
static void keep_thumb(tile *t, int sk) {
  strip *st = &v.strips[sk];
  for (int k = 0; k < st->n; k++) {
    tile *old = &st->tiles[k];
    if (!old->thumb || strcmp(old->uid, t->uid) || !old->cover_dir || !t->cover_dir ||
        wcscmp(old->cover_dir, t->cover_dir))
      continue;
    t->thumb = old->thumb;
    t->pixels = old->pixels;
    t->thumb_size = old->thumb_size;
    old->thumb = NULL;
    return;
  }
}

/* The open artist's singles: their tracks alone on an album here, or on none. */
static int load_singles(tile **out) {
  mc_track_list *list = NULL;
  if (!v.lib || mc_library_list_artist_singles(v.lib, v.open_uid, &list) != MC_OK) list = NULL;
  int n = list ? list->count : 0;
  tile *tiles = n ? calloc((size_t)n, sizeof *tiles) : NULL;
  if (!tiles) n = 0;
  for (int i = 0; i < n; i++) {
    const mc_track_info *tr = &list->items[i];
    tile *t = &tiles[i];
    memcpy(t->uid, tr->uid, sizeof t->uid);
    memcpy(t->artist_uid, tr->artist_uid, sizeof t->artist_uid);
    t->title = wstr_dup_from_utf8(tr->title && *tr->title ? tr->title : mc_str(MC_STR_UNTITLED));
    t->cover_dir = wstr_dup_from_utf8(tr->cover_dir); /* NULL for none */
    if (tr->year > 0 && (t->sub = malloc(16 * sizeof *t->sub))) swprintf(t->sub, 16, L"%d", tr->year);
    t->artist = tr->artist && *tr->artist ? wstr_dup_from_utf8(tr->artist) : NULL;
    t->album = tr->album && *tr->album ? wstr_dup_from_utf8(tr->album) : NULL;
    t->path = tr->path ? _strdup(tr->path) : NULL;
    t->duration_ms = tr->duration_ms;
    keep_thumb(t, STRIP_SINGLES);
  }
  mc_track_list_free(list);
  *out = tiles;
  return n;
}

/* The open artist's albums and singles, keeping the covers already decoded so a reload
 * doesn't flash. */
static void load_tiles(void) {
  mc_album_list *list = NULL;
  if (!v.lib || mc_library_list_artist_albums(v.lib, v.open_uid, &list) != MC_OK) list = NULL;
  int n = list ? list->count : 0, tracks = 0;
  tile *tiles = n ? calloc((size_t)n, sizeof *tiles) : NULL;
  if (!tiles) n = 0;
  for (int i = 0; i < n; i++) {
    const mc_album_info *a = &list->items[i];
    tile *t = &tiles[i];
    memcpy(t->uid, a->uid, sizeof t->uid);
    t->title = wstr_dup_from_utf8(a->title && *a->title ? a->title : mc_str(MC_STR_UNTITLED));
    t->cover_dir = a->has_art ? wstr_dup_from_utf8(a->folder) : NULL;
    wchar_t *artist = a->artist ? wstr_dup_from_utf8(a->artist) : NULL;
    if (artist && v.open_name && wcscmp(artist, v.open_name)) { /* someone else's */
      t->sub = artist;
      artist = NULL;
    } else if (a->year > 0 && (t->sub = malloc(16 * sizeof *t->sub))) {
      swprintf(t->sub, 16, L"%d", a->year);
    }
    free(artist);
    tracks += a->credited_count;
    keep_thumb(t, STRIP_ALBUMS);
  }
  mc_album_list_free(list);
  tile *singles = NULL;
  int nsingles = load_singles(&singles);
  tracks += nsingles; /* their albums aren't among the albums */
  free_tiles(v.strips[STRIP_ALBUMS].tiles, v.strips[STRIP_ALBUMS].n);
  free_tiles(v.strips[STRIP_SINGLES].tiles, v.strips[STRIP_SINGLES].n);
  v.strips[STRIP_ALBUMS].tiles = tiles;
  v.strips[STRIP_ALBUMS].n = n;
  v.strips[STRIP_SINGLES].tiles = singles;
  v.strips[STRIP_SINGLES].n = nsingles;
  if (v.focus >= num_tiles()) v.focus = num_tiles() - 1;

  /* "12 albums · 140 tracks", or with no albums "3 tracks". */
  char a[64] = "", t[64], text[160];
  if (n) mc_format(a, sizeof a, mc_str_plural(MC_STR_COUNT_ALBUMS, n), MC_D(n));
  mc_format(t, sizeof t, mc_str_plural(MC_STR_TRACKS, tracks), MC_D(tracks));
  snprintf(text, sizeof text, "%s%s%s", a, n ? mc_str(MC_STR_SEPARATOR_DOT) : "", t);
  wstr_from_utf8(v.summary, sizeof v.summary / sizeof v.summary[0], text);
  layout();
  request_covers();
}

static void close_artist(void) {
  strips_stop();
  v.open_uid[0] = '\0';
  free(v.open_name);
  v.open_name = NULL;
  free_strips();
  v.hot = v.pressed = HIT_NONE;
  v.focus = v.list_focus;
  v.reveal_focus = TRUE;
  layout();
  InvalidateRect(v.wnd, NULL, FALSE);
}

/* Lists the artists again, and the open one's albums; if it has left the library, back to
 * the list. */
static void reload(void) {
  v.stale = FALSE;
  free_artists();
  mc_artist_list *list = NULL;
  v.status = v.lib ? mc_library_list_artists(v.lib, &list) : MC_OK;
  int n = list ? list->count : 0;
  v.artists = n ? calloc((size_t)n, sizeof *v.artists) : NULL;
  if (!v.artists) n = 0;
  for (int i = 0; i < n; i++) {
    const mc_artist_info *a = &list->items[i];
    artist_row *r = &v.artists[i];
    memcpy(r->uid, a->uid, sizeof r->uid);
    r->name = wstr_dup_from_utf8(a->name && *a->name ? a->name : mc_str(MC_STR_UNKNOWN_ARTIST));
    if (a->album_count > 0) mc_wformat(r->sub, 64, mc_str_plural(MC_STR_COUNT_ALBUMS, a->album_count), MC_D(a->album_count));
  }
  v.nartists = n;
  mc_artist_list_free(list);
  if (v.list_focus >= n) v.list_focus = n - 1;
  if (!is_open() && v.focus >= n) v.focus = n - 1;

  if (is_open()) {
    int i = find_artist(v.open_uid);
    if (i < 0) {
      close_artist();
      notify(ARN_BACK, TRUE);
      return;
    }
    free(v.open_name);
    v.open_name = _wcsdup(v.artists[i].name);
    load_tiles();
  }
  layout();
  InvalidateRect(v.wnd, NULL, FALSE);
}

/* --- Navigation --- */

static void open_row(int i) {
  if (i < 0 || i >= v.nartists) return;
  v.list_focus = i;
  artist_view_open(v.artists[i].uid);
  notify(ARN_OPEN, FALSE);
}

static void go_back(void) {
  if (!is_open()) return;
  close_artist();
  notify(ARN_BACK, FALSE);
}

/* An album opens on the Albums page; a single plays, and the singles after it. */
static void open_tile(int i) {
  if (i < 0 || i >= num_tiles()) return;
  if (is_single(i)) {
    v.play_index = i - strip_first(STRIP_SINGLES);
    notify(ARN_PLAY_SINGLE, FALSE);
    return;
  }
  memcpy(v.opened_album, tile_at(i)->uid, MC_UID_STRLEN);
  notify(ARN_OPEN_ALBUM, FALSE);
}

static void activate(int i) {
  int sk, dir;
  if (i == HIT_BACK) go_back();
  else if (is_arrow(i, &sk, &dir)) strip_page(sk, dir);
  else if (is_open()) open_tile(i);
  else open_row(i);
}

static void set_focus(int i) {
  int n = is_open() ? num_tiles() : v.nartists;
  if (n == 0) return;
  i = i < 0 ? 0 : i >= n ? n - 1 : i;
  invalidate_hit(v.focus);
  v.focus = i;
  invalidate_hit(i);
  if (is_open()) {
    reveal_tile(i);
  } else {
    int top = scale(LIST_TOP) + i * scale(ROW_H);
    scroller_reveal(&v.scroll, top - scale(LIST_TOP), top + scale(ROW_H) + scale(LIST_TOP));
  }
}

/* Typing a letter in the list: the next artist whose name starts with it. */
static void type_ahead(wchar_t ch) {
  SendMessageW(GetAncestor(v.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  ch = towlower(ch);
  for (int k = 1; k <= v.nartists; k++) {
    int i = (v.focus + k + v.nartists) % v.nartists;
    if (v.artists[i].name && towlower(v.artists[i].name[0]) == ch) {
      set_focus(i);
      return;
    }
  }
}

static void on_key(WPARAM key) {
  SendMessageW(GetAncestor(v.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  if (is_open()) {
    /* Left, Right, Home and End stay in the focused tile's strip (with none focused, the
     * first); Up and Down go to the other strip, to its tile in the same place or its last. */
    int k = 0, sk = v.focus >= 0 ? tile_strip(v.focus, &k) : STRIP_ALBUMS;
    if (!v.strips[sk].n) sk = STRIP_SINGLES;
    int first = strip_first(sk), last = first + v.strips[sk].n - 1;
    int other = sk == STRIP_ALBUMS ? STRIP_SINGLES : STRIP_ALBUMS;
    switch (key) {
      case VK_LEFT: set_focus(max(first, v.focus - 1)); break;
      case VK_RIGHT: set_focus(v.focus < 0 ? first : min(last, v.focus + 1)); break;
      case VK_HOME: set_focus(first); break;
      case VK_END: set_focus(last); break;
      case VK_UP:
      case VK_DOWN:
        if (v.focus < 0) set_focus(first);
        else if ((key == VK_DOWN) == (other > sk) && v.strips[other].n)
          set_focus(strip_first(other) + min(k, v.strips[other].n - 1));
        break;
      case VK_RETURN: open_tile(v.focus); break;
      case VK_ESCAPE:
      case VK_BACK: go_back(); break;
    }
    return;
  }
  int page = max(1, (v.scroll.view.bottom - v.scroll.view.top) / scale(ROW_H) - 1);
  switch (key) {
    case VK_UP: set_focus(v.focus - 1); break;
    case VK_DOWN: set_focus(v.focus < 0 ? 0 : v.focus + 1); break;
    case VK_PRIOR: set_focus(v.focus - page); break;
    case VK_NEXT: set_focus(v.focus + page); break;
    case VK_HOME: set_focus(0); break;
    case VK_END: set_focus(v.nartists - 1); break;
    case VK_RETURN: open_row(v.focus); break;
  }
}

/* --- Painting --- */

static void paint_placeholder_cover(canvas *c, const RECT *art, BOOL glyph) {
  canvas_fill(c, art, scale(COVER_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
  if (!glyph) return;
  HGDIOBJ f = SelectObject(c->dc, v.glyph_font);
  SetTextColor(c->dc, theme.text3);
  DrawTextW(c->dc, L"\xE8D6", 1, (RECT *)art, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* a note */
  SelectObject(c->dc, f);
}

static void paint_row(canvas *c, int i, BOOL last) {
  const artist_row *a = &v.artists[i];
  RECT rc = row_rect(i), fill = rc;
  InflateRect(&fill, 0, -scale(2));
  if (i == v.hot) theme_draw_state(c, &fill, scale(RADIUS), v.pressed == i ? STATE_PRESSED : STATE_HOT);
  if (!last) {
    RECT line = {rc.left + scale(ROW_ICON_W), rc.bottom - 1, rc.right - scale(12), rc.bottom};
    canvas_clear(c, &line, theme.line, 255);
  }
  RECT icon = {rc.left, rc.top, rc.left + scale(ROW_ICON_W), rc.bottom};
  HGDIOBJ old = SelectObject(c->dc, v.icon_font);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, L"\xE77B", 1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* as Artists */
  SelectObject(c->dc, old);

  UINT one_line = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  int lines = a->sub[0] ? 2 : 1, top = rc.top + (rc.bottom - rc.top - lines * v.text_h) / 2;
  RECT text = {icon.right, top, rc.right - scale(12), top + v.text_h};
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, a->name, -1, &text, one_line);
  if (a->sub[0]) {
    OffsetRect(&text, 0, v.text_h);
    SetTextColor(c->dc, theme.text2);
    DrawTextW(c->dc, a->sub, -1, &text, one_line);
  }
}

static void paint_tile(canvas *c, int i) {
  const tile *t = tile_at(i);
  RECT art = tile_rect(i), outer = tile_outer(i);
  art.bottom = art.top + scale(TILE_W);
  if (v.dragging && i == v.pressed) theme_draw_state(c, &outer, scale(8), STATE_PRESSED);
  else if (i == v.hot) theme_draw_state(c, &outer, scale(8), v.pressed == i ? STATE_PRESSED : STATE_HOT);
  if (t->pixels) canvas_draw_image(c, &art, t->pixels, t->thumb_size, t->thumb_size);
  else paint_placeholder_cover(c, &art, !t->cover_dir || t->failed);
  RECT line = {art.left, art.bottom + scale(TEXT_GAP), art.right, art.bottom + scale(TEXT_GAP) + v.text_h};
  UINT one_line = DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  /* Greyed out: a single with no file on this device, as the track list shows one. */
  SetTextColor(c->dc, is_single(i) && !t->path ? theme.text3 : theme.text);
  DrawTextW(c->dc, t->title, -1, &line, one_line);
  OffsetRect(&line, 0, v.text_h);
  SetTextColor(c->dc, theme.text2);
  if (t->sub) DrawTextW(c->dc, t->sub, -1, &line, one_line);
}

/* A round icon button: the back button and the strip's arrows. */
static void paint_button(canvas *c, const RECT *r, int hit, wchar_t glyph, BOOL enabled) {
  control_state state = !enabled ? STATE_NORMAL
                        : v.pressed == hit && v.hot == hit ? STATE_PRESSED
                        : v.hot == hit                     ? STATE_HOT
                                                           : STATE_NORMAL;
  COLORREF color = theme_draw_state(c, r, scale(BACK) / 2, state);
  if (!enabled) color = theme.text3;
  HGDIOBJ old = SelectObject(c->dc, v.icon_font);
  SetTextColor(c->dc, color);
  DrawTextW(c->dc, &glyph, 1, (RECT *)r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(c->dc, old);
}

static void paint_heading(canvas *c, const RECT *r, enum mc_str_id text) {
  HGDIOBJ old = SelectObject(c->dc, v.heading_font);
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, mc_wstr(text), -1, (RECT *)r, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(c->dc, old);
}

static void paint_header(canvas *c, const RECT *dirty) {
  RECT isect;
  paint_button(c, &v.back, HIT_BACK, 0xE72B, TRUE); /* Back */
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int left = v.back.right + scale(BACK_GAP), right = rc.right - scale(PAD_X);
  RECT name = {left, scale(HEAD_TOP), right, scale(HEAD_TOP) + v.title_h};
  UINT one_line = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  HGDIOBJ old = SelectObject(c->dc, v.title_font);
  SetTextColor(c->dc, theme.text);
  if (v.open_name) DrawTextW(c->dc, v.open_name, -1, &name, one_line);
  SelectObject(c->dc, old);
  RECT sub = {left, name.bottom + scale(NAME_GAP), right, name.bottom + scale(NAME_GAP) + v.text_h};
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, v.summary, -1, &sub, one_line);

  for (int sk = 0; sk < NUM_STRIPS; sk++) {
    const strip *st = &v.strips[sk];
    if (!st->n) continue;
    paint_heading(c, &st->heading, sk == STRIP_ALBUMS ? MC_STR_SECTION_ALBUMS : MC_STR_SECTION_SINGLES);
    if (strip_max(sk) > 0) {
      paint_button(c, &st->left, arrow_hit(sk, -1), 0xE76B, st->pos > 0); /* ChevronLeft */
      paint_button(c, &st->right, arrow_hit(sk, +1), 0xE76C, st->pos < strip_max(sk)); /* ChevronRight */
    }
    RECT band = strip_rect(sk);
    HRGN clip = CreateRectRgnIndirect(&band);
    SelectClipRgn(c->dc, clip);
    for (int k = 0, first = strip_first(sk); k < st->n; k++) {
      RECT r = tile_outer(first + k);
      if (IntersectRect(&isect, &r, dirty) && IntersectRect(&isect, &r, &band)) paint_tile(c, first + k);
    }
    SelectClipRgn(c->dc, NULL);
    DeleteObject(clip);
  }
  paint_heading(c, &v.tracks_heading, MC_STR_TRACKS_TITLE);
}

static void paint_empty(canvas *c, const RECT *client) {
  wchar_t text[512];
  if (!v.lib) swprintf(text, 512, L"%ls", mc_wstr(MC_STR_LIBRARY_OPEN_FAILED));
  else if (v.status != MC_OK) mc_wformat(text, 512, mc_str(MC_STR_LIST_FAILED), MC_S(mc_status_str(v.status)));
  else swprintf(text, 512, L"%ls", mc_wstr(MC_STR_NO_ARTISTS));
  RECT rc = *client, fit_rc;
  InflateRect(&rc, -scale(PAD_X), 0);
  fit_rc = rc;
  DrawTextW(c->dc, text, -1, &fit_rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  OffsetRect(&rc, 0, (client->bottom - (fit_rc.bottom - fit_rc.top)) / 2);
  SetTextColor(c->dc, theme.text3);
  DrawTextW(c->dc, text, -1, &rc, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

/* Into a canvas, then made opaque, then the focus ring, scrollbar and the content area's
 * border and rounded corner, which shows the sidebar's Mica, as the album grid draws them. */
static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(v.wnd, &client);
  if (!canvas_reserve(&v.buf, client.right, client.bottom)) return;
  canvas *c = &v.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, v.font ? (HGDIOBJ)v.font : GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(c->dc, TRANSPARENT);

  if (is_open()) {
    paint_header(c, dirty);
  } else if (!v.nartists) {
    paint_empty(c, &client);
  } else {
    /* The rows dirty touches, by arithmetic: a long list isn't walked each paint. */
    int row_h = scale(ROW_H), base = scale(LIST_TOP) - v.scroll.pos;
    int first = max(0, (dirty->top - base) / row_h), last = min(v.nartists - 1, (dirty->bottom - 1 - base) / row_h);
    for (int i = first; i <= last; i++) paint_row(c, i, i == v.nartists - 1);
  }
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  BOOL focused = GetFocus() == v.wnd && !(SendMessageW(v.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
  if (focused && v.focus >= 0) {
    if (is_open() && v.focus < num_tiles()) {
      RECT ring = tile_outer(v.focus), band = strip_rect(tile_strip(v.focus, NULL)), isect;
      if (IntersectRect(&isect, &ring, &band)) canvas_frame(c, &ring, scale(8), scale(2), theme.text, 255);
    } else if (!is_open() && v.focus < v.nartists) {
      RECT ring = row_rect(v.focus);
      InflateRect(&ring, 0, -scale(2));
      canvas_frame(c, &ring, scale(RADIUS), scale(2), theme.text, 255);
    }
  }
  if (!is_open()) scroller_paint(&v.scroll, c);

  int radius = scale(CONTENT_RADIUS);
  RECT border = {0, 0, client.right + radius, client.bottom + radius};
  if (is_open()) border.bottom = client.bottom + 2 * radius; /* the left edge carries on past the bottom */
  canvas_frame(c, &border, radius, 1, theme.line, 255);
  canvas_round_corners(c, &client, radius, CORNER_TL, theme.frame, theme.mica ? 0 : 255);
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Menu --- */

/* A single's menu is the track list's track menu, Play Next and Play Last stubs included. */
enum {
  CMD_REMOVE_HERE = 1,
  CMD_SYNC_HERE,
  CMD_NEW_PLAYLIST,
  CMD_PLAY_NEXT,
  CMD_PLAY_LAST,
  CMD_DELETE_TRACK,
  CMD_GO_TO_ARTIST,
  CMD_PLAYLIST_FIRST = 100, /* Add to Playlist's playlists */
};

static UINT menu_flags(void) {
  return TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
}

/* Asks the parent to add a single to playlist_uid, or to a new playlist for NULL. */
static void request_add(const char *track_uid, BOOL here, const char *playlist_uid) {
  memset(&v.req, 0, sizeof v.req);
  snprintf(v.req.track_uid, sizeof v.req.track_uid, "%s", track_uid);
  if (playlist_uid) snprintf(v.req.playlist_uid, sizeof v.req.playlist_uid, "%s", playlist_uid);
  v.req.here = here;
  notify(ARN_ADD_TO_PLAYLIST, FALSE);
}

/* Single i's menu, at pt (screen coordinates): Add to Playlist and Delete from Library, as a
 * track's in the track list, and Go to Artist when it's credited to someone else. A single has
 * no album page to go to. */
static void single_menu(int i, POINT pt) {
  /* Copies: the menu's message loop repaints the page, which may reload it. */
  char uid[MC_UID_STRLEN], artist[MC_UID_STRLEN];
  memcpy(uid, tile_at(i)->uid, sizeof uid);
  memcpy(artist, tile_at(i)->artist_uid, sizeof artist);
  BOOL here = tile_at(i)->path != NULL;
  HMENU menu = CreatePopupMenu();
  mc_playlist_list *lists = NULL;
  HMENU add = add_to_playlist_menu(v.lib, CMD_PLAYLIST_FIRST, CMD_NEW_PLAYLIST, &lists);
  if (add) AppendMenuW(menu, MF_POPUP, (UINT_PTR)add, mc_wstr_mnemonic(MC_STR_ADD_TO_PLAYLIST));
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  /* Greyed out until there's a play queue to add to (see TODO.md, Playback). */
  AppendMenuW(menu, MF_STRING | MF_GRAYED, CMD_PLAY_NEXT, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_NEXT));
  AppendMenuW(menu, MF_STRING | MF_GRAYED, CMD_PLAY_LAST, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_LAST));
  if (artist[0] && strcmp(artist, v.open_uid)) {
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_GO_TO_ARTIST, mc_wstr_mnemonic(MC_STR_GO_TO_ARTIST));
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_DELETE_TRACK, mc_wstr_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU));
  int cmd = TrackPopupMenu(menu, menu_flags(), pt.x, pt.y, 0, v.wnd, NULL);
  DestroyMenu(menu); /* and the submenu */

  if (cmd >= CMD_PLAYLIST_FIRST && lists && cmd - CMD_PLAYLIST_FIRST < lists->count)
    request_add(uid, here, lists->items[cmd - CMD_PLAYLIST_FIRST].uid);
  mc_playlist_list_free(lists);
  if (cmd == CMD_NEW_PLAYLIST) {
    request_add(uid, here, NULL);
  } else if (cmd == CMD_DELETE_TRACK) {
    memset(&v.req, 0, sizeof v.req);
    memcpy(v.req.track_uid, uid, sizeof uid);
    notify(ARN_DELETE_TRACK, FALSE);
  } else if (cmd == CMD_GO_TO_ARTIST) {
    memset(&v.req, 0, sizeof v.req);
    memcpy(v.req.artist_uid, artist, sizeof artist);
    notify(ARN_GO_TO_ARTIST, FALSE);
  }
}

/* WM_CONTEXTMENU: Remove from This PC... or Sync to This PC where they apply (none if neither
 * does; an artist can't be deleted from the library), for the row under the pointer (from the
 * keyboard, lp -1, the focused row), or anywhere on an artist's page for that artist; but on a
 * single, the single's menu. */
static BOOL on_context_menu(LPARAM lp) {
  if (!v.lib) return FALSE;
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  int i = -1;
  if (lp == -1) {
    i = v.focus;
  } else {
    POINT client = pt;
    ScreenToClient(v.wnd, &client);
    i = hit_at(client.x, client.y);
  }
  if (is_open() && is_single(i)) {
    if (lp == -1) {
      RECT r = tile_rect(i);
      pt.x = r.left;
      pt.y = r.top + scale(TILE_W);
      ClientToScreen(v.wnd, &pt);
    }
    single_menu(i, pt);
    return TRUE;
  }
  if (!is_open() && (i < 0 || i >= v.nartists)) return FALSE;
  if (lp == -1) {
    RECT r = is_open() ? v.back : row_rect(i);
    pt.x = r.left;
    pt.y = r.bottom;
    ClientToScreen(v.wnd, &pt);
  }
  /* Copies: the dialog's message loop repaints the list, which may reload it. */
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", is_open() ? v.open_uid : v.artists[i].uid);
  wchar_t *name = _wcsdup(is_open() ? (v.open_name ? v.open_name : L"") : v.artists[i].name);
  HMENU menu = CreatePopupMenu();
  remove_here_append_menu(menu, MC_PROFILE_ARTIST, uid, CMD_REMOVE_HERE, CMD_SYNC_HERE);
  int cmd = GetMenuItemCount(menu) > 0 ? TrackPopupMenu(menu, menu_flags(), pt.x, pt.y, 0, v.wnd, NULL) : 0;
  DestroyMenu(menu);
  if ((cmd == CMD_REMOVE_HERE || cmd == CMD_SYNC_HERE) && name &&
      remove_here_command(GetAncestor(v.wnd, GA_ROOT), (UINT)cmd, CMD_REMOVE_HERE, CMD_SYNC_HERE, MC_PROFILE_ARTIST,
                          uid, name))
    notify(ARN_LIBRARY_CHANGED, TRUE);
  free(name);
  return TRUE;
}

/* --- Dragging a single onto a playlist --- */

/* The pressed single has moved to x, y: once far enough, it's dragged, and outside the page
 * the window under the pointer says whether there's a playlist to add it to. Nothing on the
 * page takes it. */
static void drag_to(int x, int y) {
  if (!v.dragging) {
    if (abs(x - v.drag_from.x) < GetSystemMetrics(SM_CXDRAG) && abs(y - v.drag_from.y) < GetSystemMetrics(SM_CYDRAG))
      return;
    v.dragging = TRUE;
    set_hot(HIT_NONE);
    invalidate_hit(v.pressed);
  }
  RECT rc;
  GetClientRect(v.wnd, &rc);
  POINT pt = {x, y};
  if (!PtInRect(&rc, pt)) {
    ClientToScreen(v.wnd, &pt);
    drag_over(DRAG_TRACK, pt, v.drop_target);
    return;
  }
  drag_leave();
  v.drop_target[0] = '\0';
  drag_set_cursor(FALSE);
}

/* Stops dragging, without adding anything. */
static void end_drag(void) {
  if (!v.dragging) return;
  v.dragging = FALSE;
  v.drop_target[0] = '\0';
  invalidate_hit(v.pressed);
  drag_done();
}

/* --- Window --- */

/* The strip the wheel scrolls, for the pointer at lp (screen coordinates): the one it's over,
 * else the first that can scroll, else -1. */
static int wheel_strip(LPARAM lp) {
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(v.wnd, &pt);
  for (int sk = 0; sk < NUM_STRIPS; sk++) {
    RECT r = strip_rect(sk);
    r.top = v.strips[sk].heading.top;
    if (v.strips[sk].n && PtInRect(&r, pt)) return sk;
  }
  for (int sk = 0; sk < NUM_STRIPS; sk++)
    if (strip_max(sk) > 0) return sk;
  return -1;
}

static LRESULT CALLBACK artist_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!v.wnd) v.wnd = v.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  switch (msg) {
    case WM_SIZE:
      layout();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      if (v.stale) reload(); /* deferred while hidden; painting means we're visible */
      if (v.reveal_focus && !is_open() && v.focus >= 0) { /* at once: it's where the user just was */
        int top = scale(LIST_TOP) + v.focus * scale(ROW_H), view = v.scroll.view.bottom - v.scroll.view.top;
        if (top < v.scroll.pos) scroller_jump(&v.scroll, top - scale(LIST_TOP));
        else if (top + scale(ROW_H) > v.scroll.pos + view) scroller_jump(&v.scroll, top + scale(ROW_H) + scale(LIST_TOP) - view);
      }
      v.reveal_focus = FALSE;
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wp >= TIMER_STRIP && wp < TIMER_STRIP + NUM_STRIPS) strip_tick((int)(wp - TIMER_STRIP));
      else scroller_timer(&v.scroll, wp);
      return 0;
    case WM_APP_COVER:
      decode_next_cover();
      return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
      int delta = GET_WHEEL_DELTA_WPARAM(wp);
      if (!is_open()) {
        if (msg == WM_MOUSEWHEEL) scroller_wheel(&v.scroll, delta);
      } else { /* nothing else in the header scrolls, so the wheel scrolls a strip */
        if (msg == WM_MOUSEHWHEEL) delta = -delta;
        int sk = wheel_strip(lp);
        if (sk >= 0) strip_animate(sk, v.strips[sk].target - MulDiv(delta, tile_pitch(), WHEEL_DELTA));
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (!v.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        v.tracking = TRUE;
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (v.pressed >= 0 && is_single(v.pressed) && (wp & MK_LBUTTON)) {
        drag_to(x, y);
        if (v.dragging) return 0;
      }
      BOOL on_bar = !is_open() && scroller_mouse_move(&v.scroll, x, y);
      int i = on_bar ? HIT_NONE : hit_at(x, y);
      set_hot(v.pressed != HIT_NONE ? (i == v.pressed ? i : HIT_NONE) : i);
      return 0;
    }
    case WM_MOUSELEAVE:
      v.tracking = FALSE;
      scroller_mouse_leave(&v.scroll);
      if (v.pressed == HIT_NONE) set_hot(HIT_NONE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      SetFocus(wnd);
      if (!is_open() && scroller_mouse_down(&v.scroll, x, y)) return 0;
      int i = hit_at(x, y);
      if (i == HIT_NONE) return 0;
      if (i >= 0) {
        invalidate_hit(v.focus);
        v.focus = i;
      }
      v.pressed = i;
      v.drag_from.x = x;
      v.drag_from.y = y;
      SetCapture(wnd);
      set_hot(i);
      invalidate_hit(i);
      return 0;
    }
    case WM_LBUTTONUP:
      if (v.dragging) { /* dropped: onto a playlist, or nowhere */
        char target[MC_UID_STRLEN], uid[MC_UID_STRLEN];
        memcpy(target, v.drop_target, sizeof target);
        memcpy(uid, tile_at(v.pressed)->uid, sizeof uid);
        BOOL here = tile_at(v.pressed)->path != NULL;
        end_drag();
        ReleaseCapture();
        if (target[0]) request_add(uid, here, target);
      } else if (v.pressed != HIT_NONE) {
        int i = v.pressed;
        BOOL clicked = hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == i;
        ReleaseCapture(); /* WM_CAPTURECHANGED clears pressed */
        if (clicked) activate(i);
      } else {
        scroller_mouse_up(&v.scroll);
      }
      return 0;
    case WM_CAPTURECHANGED:
      end_drag();
      if (v.pressed != HIT_NONE) {
        invalidate_hit(v.pressed);
        v.pressed = HIT_NONE;
        update_hot();
      }
      scroller_capture_changed(&v.scroll);
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == VK_LEFT && is_open()) { /* Alt+Left, as in a browser */
        go_back();
        return 0;
      }
      break;
    case WM_CHAR:
      if (!is_open() && iswalnum((wint_t)wp)) type_ahead((wchar_t)wp);
      return 0;
    case WM_APPCOMMAND: /* the mouse's back button */
      if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_BACKWARD && is_open()) {
        go_back();
        return TRUE;
      }
      break;
    case WM_CONTEXTMENU:
      if (on_context_menu(lp)) return 0;
      break;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      invalidate_hit(v.focus);
      break;
    case WM_CREATE: {
      v.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, wnd, NULL,
                                  GetModuleHandleW(NULL), NULL);
      /* Back, then each strip's arrows (see layout()). */
      const enum mc_str_id tips[] = {MC_STR_BACK, MC_STR_WINDOWS_SCROLL_LEFT, MC_STR_WINDOWS_SCROLL_RIGHT,
                                     MC_STR_WINDOWS_SCROLL_LEFT, MC_STR_WINDOWS_SCROLL_RIGHT};
      for (int i = 0; i < 1 + 2 * NUM_STRIPS; i++) {
        wchar_t back_tip[64]; /* Back's has its shortcut */
        TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = wnd, .uId = (UINT_PTR)i + 1,
                          .lpszText = i == 0 ? wstr_tooltip(back_tip, 64, tips[i], MC_STR_WINDOWS_SHORTCUT_BACK)
                                             : (wchar_t *)mc_wstr(tips[i])};
        SendMessageW(v.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      }
      return 0;
    }
    case WM_DESTROY:
      strips_stop();
      free_artists();
      free_strips();
      free(v.open_name);
      canvas_free(&v.buf);
      if (v.wic) IWICImagingFactory_Release(v.wic);
      if (v.tooltip) DestroyWindow(v.tooltip);
      HFONT fonts[] = {v.title_font, v.heading_font, v.icon_font, v.glyph_font};
      for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
      memset(&v, 0, sizeof v);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND artist_view_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DBLCLKS, .lpfnWndProc = artist_proc,
                    .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"kithsong_artists"};
  RegisterClassExW(&wc);

  memset(&v, 0, sizeof v);
  v.hot = v.pressed = HIT_NONE;
  v.focus = v.list_focus = -1;
  v.dpi = USER_DEFAULT_SCREEN_DPI;
  v.stale = TRUE;
  v.scroll.moved = on_scrolled;
  return CreateWindowExW(0, L"kithsong_artists", NULL, WS_CHILD, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

static HFONT icon_font(int px) {
  return CreateFontW(-scale(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

void artist_view_update(UINT dpi, HFONT font) {
  v.dpi = dpi;
  v.font = font;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  HFONT old[] = {v.title_font, v.heading_font, v.icon_font, v.glyph_font};
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 2, 1); /* 9pt -> 18pt */
  lf.lfWeight = FW_SEMIBOLD;
  v.title_font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 4, 3); /* 12pt, as the search page's headings */
  v.heading_font = CreateFontIndirectW(&lf);
  v.icon_font = icon_font(16);
  v.glyph_font = icon_font(TILE_W / 4);
  for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
    if (old[i]) DeleteObject(old[i]);
  if (v.tooltip) theme_apply_control(v.tooltip);
  if (!v.wnd) return;
  layout();
  request_covers(); /* at the new size */
  InvalidateRect(v.wnd, NULL, FALSE);
}

void artist_view_set_library(mc_library *lib) {
  v.lib = lib;
  v.stale = TRUE;
  if (v.wnd) InvalidateRect(v.wnd, NULL, FALSE);
}

BOOL artist_view_open(const char *uid) {
  if (v.stale) reload();
  int i = find_artist(uid);
  if (i < 0) return FALSE;
  if (is_open() && !strcmp(v.open_uid, uid)) return TRUE;
  if (!is_open()) v.list_focus = i;
  strips_stop();
  free_strips();
  memcpy(v.open_uid, v.artists[i].uid, MC_UID_STRLEN);
  free(v.open_name);
  v.open_name = _wcsdup(v.artists[i].name);
  v.hot = v.pressed = HIT_NONE;
  v.focus = -1;
  load_tiles();
  InvalidateRect(v.wnd, NULL, FALSE);
  return TRUE;
}

void artist_view_back(void) {
  if (is_open()) close_artist();
}

const char *artist_view_artist(void) {
  return is_open() ? v.open_uid : NULL;
}

int artist_view_header_height(void) {
  return v.header_h;
}

const char *artist_view_opened_album(void) {
  return v.opened_album;
}

int artist_view_single_count(void) {
  return is_open() ? v.strips[STRIP_SINGLES].n : 0;
}

track_list_item artist_view_single_at(int i) {
  track_list_item item = {0};
  if (i < 0 || i >= artist_view_single_count()) return item;
  const tile *t = &v.strips[STRIP_SINGLES].tiles[i];
  item.uid = t->uid;
  item.title = t->title;
  item.artist = t->artist;
  item.album = t->album;
  item.cover_dir = t->cover_dir;
  item.duration_ms = t->duration_ms;
  item.path = t->path;
  return item;
}

int artist_view_play_index(void) {
  return v.play_index;
}

const track_list_request *artist_view_last_request(void) {
  return &v.req;
}
