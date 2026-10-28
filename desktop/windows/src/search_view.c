#define COBJMACROS
#include "search_view.h"

#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include <windowsx.h>

#include "canvas.h"
#include "cover.h"
#include "scroller.h"
#include "theme.h"
#include "wstr.h"

#define WM_APP_COVER (WM_APP + 21) /* decode the next cover */

enum {
  LIMIT_PER_KIND = 20,
  KIND_COUNT = MC_SEARCH_ALBUM_GROUP + 1,
  MAX_QUERY = 256,
  ID_EDIT = 1,
  TIMER_SEARCH = 1,      /* the pause in typing (the scroller's timers are 101 and 102) */
  SEARCH_DELAY_MS = 100,
};

/* Metrics at 96 DPI. */
enum {
  BOX_TOP = 36,
  BOX_H = 44,
  BOX_MAX_W = 560,
  BOX_RADIUS = 6,
  BOX_ICON_W = 44, /* the magnifier, at the box's start */
  RESULTS_TOP = 12, /* between the box and the results' view, and atop the results */
  RESULTS_MAX_W = 1100,
  SIDE_PAD = 24,
  HEADING_H = 40,
  SECTION_GAP = 16,
  TILE_W = 150,
  TILE_GAP = 20,
  TEXT_GAP = 8,
  HOVER_PAD = 8,
  COVER_RADIUS = 6,
  ROW_H = 52,
  ROW_ICON_W = 44,
  CARD_RADIUS = 8,
  RADIUS = 6,
};

typedef struct hit {
  mc_search_kind kind;
  char uid[MC_UID_STRLEN];
  char album_uid[MC_UID_STRLEN]; /* a track's album */
  wchar_t *name;
  wchar_t *sub; /* beneath the name, or NULL */
  BOOL opens;   /* there's a page here to open it on */
  RECT rc;      /* the tile or the row, in the content */
  /* Albums: the cover. */
  wchar_t *cover_dir; /* NULL when the album has none */
  HBITMAP thumb;      /* premultiplied, with rounded corners */
  const uint32_t *pixels;
  int thumb_size;
  BOOL failed; /* the cover couldn't be decoded; draw the placeholder */
} hit;

typedef struct section {
  mc_search_kind kind;
  int first, count; /* its hits, in v.hits */
  int top;          /* the heading's, in the content */
  RECT card;        /* behind a list section's rows */
} section;

/* An album the grid shows, for its cover and whether its hits open. */
typedef struct album_ref {
  char uid[MC_UID_STRLEN];
  wchar_t *cover_dir; /* NULL if it has no cover */
} album_ref;

static struct {
  HWND wnd, edit;
  UINT dpi;
  HFONT font; /* the main window's */
  HFONT box_font, heading_font, icon_font, small_icon_font, big_icon_font, glyph_font;
  int text_h;
  canvas buf;
  scroller scroll; /* the results, beneath the box */
  RECT box;        /* the search box, in the client area */
  IWICImagingFactory *wic;

  mc_library *lib;
  char *query; /* last searched for, or NULL */
  BOOL results_stale, lists_stale;
  album_ref *albums; /* sorted by uid */
  int nalbums;
  char (*playlists)[MC_UID_STRLEN]; /* the playable ones' uids, sorted */
  int nplaylists;

  hit *hits; /* grouped by section, in the sections' order */
  int nhits;
  section *sections;
  int nsections;
  int tile_w, tile_h, content_w;
  BOOL cover_posted; /* WM_APP_COVER is on its way */

  int hot;     /* the opening hit under the mouse, or -1 */
  int pressed; /* the hit the mouse went down on, or -1 */
  int focus;   /* the hit with the keyboard focus, or -1 */
  BOOL tracking;
  search_view_hit opened;
} v;

static int scale(int px) {
  return MulDiv(px, (int)v.dpi, USER_DEFAULT_SCREEN_DPI);
}

static wchar_t *widen(const char *s) {
  if (!s) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

static const wchar_t *kind_heading(mc_search_kind kind) {
  switch (kind) {
    case MC_SEARCH_ARTIST: return mc_wstr(MC_STR_SECTION_ARTISTS);
    case MC_SEARCH_ALBUM: return mc_wstr(MC_STR_SECTION_ALBUMS);
    case MC_SEARCH_TRACK: return mc_wstr(MC_STR_TRACKS_TITLE);
    case MC_SEARCH_PLAYLIST: return mc_wstr(MC_STR_SECTION_PLAYLISTS);
    default: return mc_wstr(MC_STR_SECTION_ALBUM_GROUPS);
  }
}

/* As the sidebar's pages and recents show them. */
static wchar_t kind_icon(mc_search_kind kind) {
  switch (kind) {
    case MC_SEARCH_ARTIST: return 0xE77B;
    case MC_SEARCH_ALBUM: return 0xE93C;
    case MC_SEARCH_TRACK: return 0xE8D6; /* a note */
    case MC_SEARCH_PLAYLIST: return 0xE90B;
    default: return 0xE8F1; /* Library */
  }
}

/* --- The library's albums and playlists --- */

static void free_lists(void) {
  for (int i = 0; i < v.nalbums; i++) free(v.albums[i].cover_dir);
  free(v.albums);
  free(v.playlists);
  v.albums = NULL;
  v.playlists = NULL;
  v.nalbums = v.nplaylists = 0;
}

static int album_cmp(const void *a, const void *b) {
  return strcmp(((const album_ref *)a)->uid, ((const album_ref *)b)->uid);
}

static int uid_cmp(const void *a, const void *b) {
  return strcmp(a, b);
}

/* What the album grid shows, so results open only where it can show them. */
static void load_lists(void) {
  free_lists();
  v.lists_stale = FALSE;
  mc_album_list *albums = NULL;
  if (v.lib && mc_library_list_albums(v.lib, &albums) == MC_OK && albums->count) {
    v.albums = calloc((size_t)albums->count, sizeof *v.albums);
    v.nalbums = v.albums ? albums->count : 0;
    for (int i = 0; i < v.nalbums; i++) {
      const mc_album_info *a = &albums->items[i];
      memcpy(v.albums[i].uid, a->uid, MC_UID_STRLEN);
      v.albums[i].cover_dir = a->has_art ? widen(a->folder) : NULL;
    }
    qsort(v.albums, (size_t)v.nalbums, sizeof *v.albums, album_cmp);
  }
  mc_album_list_free(albums);

  mc_playlist_list *playlists = NULL;
  if (v.lib && mc_library_list_playlists(v.lib, MC_LIST_PLAYABLE, &playlists) == MC_OK && playlists->count) {
    v.playlists = calloc((size_t)playlists->count, sizeof *v.playlists);
    v.nplaylists = v.playlists ? playlists->count : 0;
    for (int i = 0; i < v.nplaylists; i++) memcpy(v.playlists[i], playlists->items[i].uid, MC_UID_STRLEN);
    qsort(v.playlists, (size_t)v.nplaylists, sizeof *v.playlists, uid_cmp);
  }
  mc_playlist_list_free(playlists);
}

static const album_ref *find_album(const char *uid) {
  if (!uid[0] || !v.nalbums) return NULL;
  album_ref key;
  memcpy(key.uid, uid, MC_UID_STRLEN);
  return bsearch(&key, v.albums, (size_t)v.nalbums, sizeof *v.albums, album_cmp);
}

static BOOL playable_playlist(const char *uid) {
  return v.nplaylists && bsearch(uid, v.playlists, (size_t)v.nplaylists, sizeof *v.playlists, uid_cmp) != NULL;
}

/* --- Layout --- */

/* A rect in the content, in the client area. */
static RECT to_client(RECT r) {
  OffsetRect(&r, 0, v.scroll.view.top - v.scroll.pos);
  return r;
}

/* A tile with room for the hover fill around it. */
static RECT tile_outer(const hit *h) {
  RECT r = h->rc;
  InflateRect(&r, scale(HOVER_PAD), scale(HOVER_PAD));
  return r;
}

/* The search box, the edit inside it, and the results beneath. */
static void layout(void) {
  RECT rc;
  GetClientRect(v.wnd, &rc);
  int w = rc.right, pad = scale(SIDE_PAD);

  int box_w = min(scale(BOX_MAX_W), w - 2 * pad);
  SetRect(&v.box, (w - box_w) / 2, scale(BOX_TOP), (w + box_w) / 2, scale(BOX_TOP + BOX_H));
  /* Inside the border, clear of the accent line at the bottom. */
  MoveWindow(v.edit, v.box.left + scale(BOX_ICON_W), v.box.top + scale(2),
             max(0, box_w - scale(BOX_ICON_W) - scale(12)), scale(BOX_H) - scale(4), TRUE);

  HDC dc = GetDC(v.wnd);
  HGDIOBJ old = SelectObject(dc, v.font ? (HGDIOBJ)v.font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(v.wnd, dc);
  v.text_h = tm.tmHeight;

  /* The results: a column up to RESULTS_MAX_W wide, centred; tiles as the album grid's
   * smallest, and as many in a row as fit. */
  v.content_w = min(scale(RESULTS_MAX_W), w - 2 * pad);
  int left = (w - v.content_w) / 2, gap = scale(TILE_GAP);
  v.tile_w = scale(TILE_W);
  v.tile_h = v.tile_w + scale(TEXT_GAP) + 2 * v.text_h;
  int cols = max(1, (v.content_w - 2 * scale(HOVER_PAD) + gap) / (v.tile_w + gap));
  int y = scale(RESULTS_TOP);
  for (int s = 0; s < v.nsections; s++) {
    section *sec = &v.sections[s];
    sec->top = y;
    y += scale(HEADING_H);
    if (sec->kind == MC_SEARCH_ALBUM) {
      y += scale(HOVER_PAD) / 2;
      for (int k = 0; k < sec->count; k++) {
        int x = left + scale(HOVER_PAD) + (k % cols) * (v.tile_w + gap), top = y + (k / cols) * (v.tile_h + gap);
        SetRect(&v.hits[sec->first + k].rc, x, top, x + v.tile_w, top + v.tile_h);
      }
      int rows = (sec->count + cols - 1) / cols;
      y += rows * (v.tile_h + gap) - gap + scale(HOVER_PAD);
      SetRectEmpty(&sec->card);
    } else {
      SetRect(&sec->card, left, y, left + v.content_w, y + sec->count * scale(ROW_H));
      for (int k = 0; k < sec->count; k++)
        SetRect(&v.hits[sec->first + k].rc, left, y + k * scale(ROW_H), left + v.content_w, y + (k + 1) * scale(ROW_H));
      y = sec->card.bottom;
    }
    y += scale(SECTION_GAP);
  }

  if (v.glyph_font) DeleteObject(v.glyph_font);
  v.glyph_font = CreateFontW(-v.tile_w / 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             theme_icon_face());

  RECT view = {0, v.box.bottom + scale(RESULTS_TOP), w, rc.bottom};
  if (view.top > view.bottom) view.top = view.bottom;
  scroller_resize(&v.scroll, &view, v.nsections ? y : 0);
}

/* --- Covers --- */

static void free_thumb(hit *h) {
  if (h->thumb) DeleteObject(h->thumb);
  h->thumb = NULL;
  h->pixels = NULL;
  h->thumb_size = 0;
}

static BOOL wants_cover(const hit *h) {
  return h->kind == MC_SEARCH_ALBUM && h->cover_dir && !h->failed && h->thumb_size != v.tile_w;
}

static void request_covers(void) {
  if (v.cover_posted) return;
  for (int i = 0; i < v.nhits; i++)
    if (wants_cover(&v.hits[i])) {
      v.cover_posted = PostMessageW(v.wnd, WM_APP_COVER, 0, 0);
      return;
    }
}

/* Decodes one cover, then asks for the next: typing between them goes first. */
static void decode_next_cover(void) {
  v.cover_posted = FALSE;
  hit *h = NULL;
  for (int i = 0; i < v.nhits && !h; i++)
    if (wants_cover(&v.hits[i])) h = &v.hits[i];
  if (!h) return;
  if (!v.wic)
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&v.wic);
  free_thumb(h);
  const uint32_t *px = NULL;
  HBITMAP bmp = v.wic ? cover_decode(v.wic, h->cover_dir, v.tile_w, scale(COVER_RADIUS), &px) : NULL;
  if (bmp) {
    h->thumb = bmp;
    h->pixels = px;
    h->thumb_size = v.tile_w;
  } else {
    h->failed = TRUE;
  }
  RECT r = to_client(h->rc);
  InvalidateRect(v.wnd, &r, FALSE);
  request_covers();
}

/* --- Results --- */

static void free_hits(hit *hits, int n) {
  for (int i = 0; i < n; i++) {
    free(hits[i].name);
    free(hits[i].sub);
    free(hits[i].cover_dir);
    free_thumb(&hits[i]);
  }
  free(hits);
}

/* A track's artist and album, or whichever is known; an album's artist; a playlist's
 * "Playlist". NULL for none. */
static wchar_t *hit_sub(const mc_search_hit *h) {
  wchar_t buf[512];
  switch (h->kind) {
    case MC_SEARCH_ALBUM: return widen(h->artist ? h->artist : mc_str(MC_STR_UNKNOWN_ARTIST));
    case MC_SEARCH_PLAYLIST: return h->is_smart ? NULL : _wcsdup(mc_wstr(MC_STR_PLAYLIST));
    case MC_SEARCH_TRACK:
      if (h->artist && h->album) {
        mc_wformat(buf, 512, mc_str(MC_STR_TRACK_BYLINE), MC_S(h->artist), MC_S(h->album));
        return _wcsdup(buf);
      }
      return widen(h->artist ? h->artist : h->album);
    default: return NULL;
  }
}

/* The hit's thumbnail from the last results, if it's the same album with the same cover:
 * the covers don't flash as the text changes. */
static void carry_thumb(hit *h) {
  if (h->kind != MC_SEARCH_ALBUM || !h->cover_dir) return;
  for (int i = 0; i < v.nhits; i++) {
    hit *old = &v.hits[i];
    if (old->kind != MC_SEARCH_ALBUM || !old->thumb || strcmp(old->uid, h->uid) || !old->cover_dir ||
        wcscmp(old->cover_dir, h->cover_dir))
      continue;
    h->thumb = old->thumb;
    h->pixels = old->pixels;
    h->thumb_size = old->thumb_size;
    old->thumb = NULL;
    return;
  }
}

/* Searches for v.query again. keep: a library change rather than new text, so the scroll
 * position and the focused result stay. */
static void show_results(BOOL keep) {
  v.results_stale = FALSE;
  BOOL query = v.query && *v.query;
  if (query && v.lists_stale) load_lists();
  mc_search_results *res = NULL;
  if (!query || !v.lib || mc_library_search(v.lib, v.query, LIMIT_PER_KIND, &res) != MC_OK) res = NULL;
  int n = res ? res->count : 0;

  /* Hits come best first, so the order kinds first turn up in is their sections' order. */
  mc_search_kind order[KIND_COUNT];
  BOOL seen[KIND_COUNT] = {FALSE};
  int kinds = 0;
  for (int i = 0; i < n; i++) {
    mc_search_kind k = res->items[i].kind;
    if ((unsigned)k < KIND_COUNT && !seen[k]) seen[k] = TRUE, order[kinds++] = k;
  }

  hit *hits = n ? calloc((size_t)n, sizeof *hits) : NULL;
  section *sections = kinds ? calloc((size_t)kinds, sizeof *sections) : NULL;
  if (!hits || !sections) kinds = 0;
  int count = 0, nsections = 0;
  for (int s = 0; s < kinds; s++) {
    section *sec = &sections[nsections];
    sec->kind = order[s];
    sec->first = count;
    for (int i = 0; i < n; i++) {
      const mc_search_hit *r = &res->items[i];
      if (r->kind != order[s]) continue;
      const album_ref *album = NULL;
      if (r->kind == MC_SEARCH_ALBUM && !(album = find_album(r->uid))) continue; /* gone since the index was built */
      hit *h = &hits[count++];
      h->kind = r->kind;
      memcpy(h->uid, r->uid, MC_UID_STRLEN);
      memcpy(h->album_uid, r->album_uid, MC_UID_STRLEN);
      h->name = widen(r->name && *r->name ? r->name : mc_str(MC_STR_UNTITLED));
      h->sub = hit_sub(r);
      switch (r->kind) {
        case MC_SEARCH_ARTIST:
        case MC_SEARCH_ALBUM: h->opens = TRUE; break;
        case MC_SEARCH_TRACK: h->opens = find_album(r->album_uid) != NULL; break;
        case MC_SEARCH_PLAYLIST: h->opens = !r->is_smart && playable_playlist(r->uid); break;
        default: h->opens = FALSE; break; /* no page for it here yet */
      }
      if (album && album->cover_dir) h->cover_dir = _wcsdup(album->cover_dir);
      carry_thumb(h);
    }
    sec->count = count - sec->first;
    if (sec->count) nsections++;
  }
  mc_search_results_free(res);

  /* The focused result stays focused across a library change, if it's still there. */
  int focus = -1;
  for (int i = 0; keep && v.focus >= 0 && v.focus < v.nhits && i < count; i++)
    if (hits[i].kind == v.hits[v.focus].kind && !strcmp(hits[i].uid, v.hits[v.focus].uid) && hits[i].opens) focus = i;

  if (v.pressed >= 0 && GetCapture() == v.wnd) ReleaseCapture();
  free_hits(v.hits, v.nhits);
  free(v.sections);
  v.hits = hits;
  v.nhits = count;
  v.sections = sections;
  v.nsections = nsections;
  v.hot = v.pressed = -1;
  v.focus = focus;
  if (!keep) v.scroll.pos = v.scroll.target = 0;
  layout();
  request_covers();
  InvalidateRect(v.wnd, NULL, FALSE);
}

/* Searches for the box's text now. */
static void search(void) {
  KillTimer(v.wnd, TIMER_SEARCH);
  wchar_t text[MAX_QUERY];
  GetWindowTextW(v.edit, text, MAX_QUERY);
  free(v.query);
  v.query = wstr_to_utf8(text);
  show_results(FALSE);
}

/* --- Focus and opening --- */

static void invalidate_hit(int i) {
  if (i < 0 || i >= v.nhits) return;
  RECT r = to_client(v.hits[i].kind == MC_SEARCH_ALBUM ? tile_outer(&v.hits[i]) : v.hits[i].rc);
  InvalidateRect(v.wnd, &r, FALSE);
}

static void set_hot(int i) {
  if (i == v.hot) return;
  invalidate_hit(v.hot);
  invalidate_hit(i);
  v.hot = i;
}

static void reveal(int i) {
  if (i < 0 || i >= v.nhits) return;
  const hit *h = &v.hits[i];
  RECT r = h->kind == MC_SEARCH_ALBUM ? tile_outer(h) : h->rc;
  /* The first of a section brings its heading along. */
  for (int s = 0; s < v.nsections; s++)
    if (v.sections[s].first == i) r.top = v.sections[s].top;
  scroller_reveal(&v.scroll, r.top, r.bottom + scale(SECTION_GAP) / 2);
}

static void set_focus(int i) {
  if (i == v.focus) return;
  invalidate_hit(v.focus);
  invalidate_hit(i);
  v.focus = i;
  reveal(i);
}

/* The next hit after from in direction dir (+1 or -1) that opens, or -1. */
static int next_hit(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < v.nhits; i += dir)
    if (v.hits[i].opens) return i;
  return -1;
}

static void open_hit(int i) {
  if (i < 0 || i >= v.nhits || !v.hits[i].opens) return;
  v.opened.kind = v.hits[i].kind;
  memcpy(v.opened.uid, v.hits[i].uid, MC_UID_STRLEN);
  memcpy(v.opened.album_uid, v.hits[i].album_uid, MC_UID_STRLEN);
  SendMessageW(GetParent(v.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(v.wnd), SVN_OPEN), (LPARAM)v.wnd);
}

/* The opening hit at a point in the client area, or -1. */
static int hit_at(int x, int y) {
  POINT pt = {x, y};
  if (!PtInRect(&v.scroll.view, pt)) return -1;
  for (int i = 0; i < v.nhits; i++) {
    RECT r = to_client(v.hits[i].rc);
    if (v.hits[i].opens && PtInRect(&r, pt)) return i;
  }
  return -1;
}

static void update_hot(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(v.wnd, &pt);
  BOOL over = v.tracking && !v.scroll.hot && !v.scroll.dragging;
  if (v.pressed < 0) set_hot(over ? hit_at(pt.x, pt.y) : -1);
}

static void on_scrolled(void) {
  update_hot();
}

/* The arrow keys, Home and End move the focus through the results; Up from the first goes
 * back to the box. Enter opens the focused one. */
static void on_key(WPARAM key) {
  SendMessageW(GetAncestor(v.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  int to;
  switch (key) {
    case VK_UP:
    case VK_LEFT:
      to = next_hit(v.focus, -1);
      if (to < 0 && key == VK_UP) {
        SetFocus(v.edit);
        return;
      }
      break;
    case VK_DOWN:
    case VK_RIGHT: to = next_hit(v.focus, +1); break;
    case VK_HOME: to = next_hit(-1, +1); break;
    case VK_END: to = next_hit(v.nhits, -1); break;
    case VK_RETURN: open_hit(v.focus); return;
    case VK_ESCAPE: SetFocus(v.edit); return;
    default: return;
  }
  if (to >= 0) set_focus(to);
}

/* The box: Enter searches at once, Escape clears it, Down goes to the results. */
static LRESULT CALLBACK edit_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        search();
        return 0;
      }
      if (wp == VK_ESCAPE) {
        SetWindowTextW(wnd, L""); /* EN_CHANGE searches */
        return 0;
      }
      if (wp == VK_DOWN) {
        int first = next_hit(-1, +1);
        if (first >= 0) {
          SetFocus(v.wnd);
          if (v.focus < 0) set_focus(first);
          else reveal(v.focus);
          SendMessageW(GetAncestor(v.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        }
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r' || wp == 27) return 0; /* no beep */
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, edit_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* --- Painting --- */

static void paint_tile(canvas *c, int i) {
  const hit *h = &v.hits[i];
  RECT art = to_client(h->rc), outer = to_client(tile_outer(h));
  art.bottom = art.top + v.tile_w;
  if (i == v.hot) theme_draw_state(c, &outer, scale(8), v.pressed == i ? STATE_PRESSED : STATE_HOT);

  if (h->pixels) {
    canvas_draw_image(c, &art, h->pixels, h->thumb_size, h->thumb_size);
  } else {
    canvas_fill(c, &art, scale(COVER_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
    if (!h->cover_dir || h->failed) {
      HGDIOBJ f = SelectObject(c->dc, v.glyph_font);
      SetTextColor(c->dc, theme.text3);
      DrawTextW(c->dc, L"\xE8D6", -1, &art, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* a note */
      SelectObject(c->dc, f);
    }
  }
  RECT line = {art.left, art.bottom + scale(TEXT_GAP), art.right, art.bottom + scale(TEXT_GAP) + v.text_h};
  UINT one_line = DT_CENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, h->name, -1, &line, one_line);
  OffsetRect(&line, 0, v.text_h);
  SetTextColor(c->dc, theme.text2);
  if (h->sub) DrawTextW(c->dc, h->sub, -1, &line, one_line);
}

static void paint_row(canvas *c, int i, BOOL last) {
  const hit *h = &v.hits[i];
  RECT rc = to_client(h->rc), fill = rc;
  InflateRect(&fill, -scale(4), -scale(4));
  if (i == v.hot) theme_draw_state(c, &fill, scale(RADIUS), v.pressed == i ? STATE_PRESSED : STATE_HOT);
  if (!last) {
    RECT line = {rc.left + scale(ROW_ICON_W), rc.bottom - 1, rc.right - scale(12), rc.bottom};
    canvas_clear(c, &line, theme.line, 255);
  }

  wchar_t icon = kind_icon(h->kind);
  RECT icon_rc = {rc.left, rc.top, rc.left + scale(ROW_ICON_W), rc.bottom};
  HGDIOBJ old = SelectObject(c->dc, v.icon_font);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, &icon, 1, &icon_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  int right = rc.right - scale(12);
  if (h->opens) {
    RECT chevron = {rc.right - scale(40), rc.top, rc.right - scale(12), rc.bottom};
    SelectObject(c->dc, v.small_icon_font);
    DrawTextW(c->dc, L"\xE76C", 1, &chevron, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* ChevronRight */
    right = chevron.left;
  }
  SelectObject(c->dc, old);

  UINT one_line = DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
  int lines = h->sub ? 2 : 1, top = rc.top + (rc.bottom - rc.top - lines * v.text_h) / 2;
  RECT text = {icon_rc.right, top, right, top + v.text_h};
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, h->name, -1, &text, one_line);
  if (h->sub) {
    OffsetRect(&text, 0, v.text_h);
    SetTextColor(c->dc, theme.text2);
    DrawTextW(c->dc, h->sub, -1, &text, one_line);
  }
}

/* Nothing found: a magnifier, "No results", and what wasn't found, centred in the view. */
static void paint_no_results(canvas *c) {
  wchar_t text[MAX_QUERY + 128];
  mc_wformat(text, sizeof text / sizeof text[0], mc_str(MC_STR_SEARCH_NO_RESULTS_TEXT), MC_S(v.query));
  RECT view = v.scroll.view, measure = view;
  InflateRect(&view, -scale(SIDE_PAD), 0);
  measure = view;
  DrawTextW(c->dc, text, -1, &measure, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
  int icon_h = scale(64), title_h = scale(36), text_h = measure.bottom - measure.top;
  int top = view.top + max(scale(24), (view.bottom - view.top - icon_h - title_h - text_h) / 3);

  RECT icon = {view.left, top, view.right, top + icon_h};
  HGDIOBJ old = SelectObject(c->dc, v.big_icon_font);
  SetTextColor(c->dc, theme.text3);
  DrawTextW(c->dc, L"\xE721", 1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* Search */
  RECT title = {view.left, icon.bottom, view.right, icon.bottom + title_h};
  SelectObject(c->dc, v.heading_font);
  SetTextColor(c->dc, theme.text);
  DrawTextW(c->dc, mc_wstr(MC_STR_SEARCH_NO_RESULTS_TITLE), -1, &title,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  SelectObject(c->dc, old);
  RECT body = {view.left, title.bottom, view.right, title.bottom + text_h};
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, text, -1, &body, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);
}

/* The results into a canvas, then the box over whatever scrolled up under it. GDI text
 * leaves alpha at 0, so it's made opaque before the content area's rounded corner, which
 * shows the sidebar's Mica. */
static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(v.wnd, &client);
  if (!canvas_reserve(&v.buf, client.right, client.bottom)) return;
  canvas *c = &v.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, v.font ? (HGDIOBJ)v.font : GetStockObject(DEFAULT_GUI_FONT));
  SetBkMode(c->dc, TRANSPARENT);

  BOOL focused = GetFocus() == v.wnd && !(SendMessageW(v.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
  RECT isect;
  for (int s = 0; s < v.nsections; s++) {
    const section *sec = &v.sections[s];
    RECT heading = to_client((RECT){sec->card.left, sec->top, sec->card.right, sec->top + scale(HEADING_H)});
    if (sec->kind == MC_SEARCH_ALBUM) {
      heading.left = (client.right - v.content_w) / 2;
      heading.right = heading.left + v.content_w;
    }
    if (IntersectRect(&isect, &heading, dirty)) {
      HGDIOBJ f = SelectObject(c->dc, v.heading_font);
      SetTextColor(c->dc, theme.text);
      DrawTextW(c->dc, kind_heading(sec->kind), -1, &heading, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(c->dc, f);
    }
    if (sec->kind != MC_SEARCH_ALBUM) {
      RECT card = to_client(sec->card);
      if (IntersectRect(&isect, &card, dirty)) {
        canvas_fill(c, &card, scale(CARD_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), theme.dark ? 10 : 6);
        canvas_frame(c, &card, scale(CARD_RADIUS), 1, theme.line, 255);
      }
    }
    for (int k = 0; k < sec->count; k++) {
      int i = sec->first + k;
      const hit *h = &v.hits[i];
      RECT r = to_client(h->kind == MC_SEARCH_ALBUM ? tile_outer(h) : h->rc);
      if (!IntersectRect(&isect, &r, dirty)) continue;
      if (h->kind == MC_SEARCH_ALBUM) paint_tile(c, i);
      else paint_row(c, i, k == sec->count - 1);
    }
  }
  if (!v.nsections && v.query && *v.query && !v.results_stale) paint_no_results(c);
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  /* Focus rings, which canvas_frame draws with alpha. */
  if (focused && v.focus >= 0 && v.focus < v.nhits) {
    const hit *h = &v.hits[v.focus];
    RECT ring = to_client(h->kind == MC_SEARCH_ALBUM ? tile_outer(h) : h->rc);
    if (h->kind != MC_SEARCH_ALBUM) InflateRect(&ring, -scale(4), -scale(4));
    canvas_frame(c, &ring, scale(h->kind == MC_SEARCH_ALBUM ? 8 : RADIUS), scale(2), theme.text, 255);
  }

  /* Above the results: the box. The edit draws its own text over it. */
  RECT above = {0, 0, client.right, v.scroll.view.top};
  canvas_clear(c, &above, theme.bg, 255);
  canvas_fill(c, &v.box, scale(BOX_RADIUS), theme.field, 255);
  canvas_frame(c, &v.box, scale(BOX_RADIUS), 1, theme.line, 255);
  if (GetFocus() == v.edit) { /* an accent line along the bottom, as Windows 11's text boxes */
    RECT accent = {v.box.left + scale(BOX_RADIUS) / 2, v.box.bottom - scale(2), v.box.right - scale(BOX_RADIUS) / 2,
                   v.box.bottom};
    canvas_fill(c, &accent, scale(1), theme.accent, 255);
  }
  RECT icon = {v.box.left, v.box.top, v.box.left + scale(BOX_ICON_W), v.box.bottom};
  canvas_text(c, L"\xE721", 1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, v.icon_font, theme.text2);

  scroller_paint(&v.scroll, c);

  /* The content area's border and rounded corner, as the album grid draws them. */
  int radius = scale(CONTENT_RADIUS);
  RECT border = {0, 0, client.right + radius, client.bottom + radius};
  canvas_frame(c, &border, radius, 1, theme.line, 255);
  canvas_round_corners(c, &client, radius, CORNER_TL, theme.frame, theme.mica ? 0 : 255);

  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Window --- */

static LRESULT CALLBACK search_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!v.wnd) v.wnd = v.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  switch (msg) {
    case WM_SIZE:
      layout();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      if (v.results_stale) show_results(TRUE); /* deferred while hidden; painting means we're visible */
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wp) != ID_EDIT) break;
      if (HIWORD(wp) == EN_CHANGE) {
        SetTimer(wnd, TIMER_SEARCH, SEARCH_DELAY_MS, NULL);
      } else if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
        InvalidateRect(wnd, &v.box, FALSE); /* the accent line */
      }
      return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
      return (LRESULT)theme_ctlcolor(WM_CTLCOLOREDIT, (HDC)wp); /* the box's fill */
    case WM_TIMER:
      if (wp == TIMER_SEARCH) search();
      else scroller_timer(&v.scroll, wp);
      return 0;
    case WM_APP_COVER:
      decode_next_cover();
      return 0;
    case WM_MOUSEWHEEL:
      scroller_wheel(&v.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_MOUSEMOVE: {
      if (!v.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        v.tracking = TRUE;
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      BOOL on_bar = scroller_mouse_move(&v.scroll, x, y);
      int i = on_bar ? -1 : hit_at(x, y);
      set_hot(v.pressed >= 0 ? (i == v.pressed ? i : -1) : i);
      return 0;
    }
    case WM_MOUSELEAVE:
      v.tracking = FALSE;
      scroller_mouse_leave(&v.scroll);
      if (v.pressed < 0) set_hot(-1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (scroller_mouse_down(&v.scroll, x, y)) return 0;
      int i = hit_at(x, y);
      if (i < 0) return 0;
      SetFocus(wnd);
      set_focus(i);
      v.pressed = i; /* opens on release, like a button */
      SetCapture(wnd);
      set_hot(i);
      invalidate_hit(i);
      return 0;
    }
    case WM_LBUTTONUP:
      if (v.pressed >= 0) {
        int i = v.pressed;
        BOOL clicked = hit_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == i;
        ReleaseCapture(); /* WM_CAPTURECHANGED clears pressed */
        if (clicked) open_hit(i);
      } else {
        scroller_mouse_up(&v.scroll);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (v.pressed >= 0) {
        invalidate_hit(v.pressed);
        v.pressed = -1;
      }
      scroller_capture_changed(&v.scroll);
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      invalidate_hit(v.focus);
      break;
    case WM_DESTROY:
      KillTimer(wnd, TIMER_SEARCH);
      free_hits(v.hits, v.nhits);
      free(v.sections);
      free_lists();
      free(v.query);
      canvas_free(&v.buf);
      if (v.wic) IWICImagingFactory_Release(v.wic);
      HFONT fonts[] = {v.box_font, v.heading_font, v.icon_font, v.small_icon_font, v.big_icon_font, v.glyph_font};
      for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
      memset(&v, 0, sizeof v);
      v.hot = v.pressed = v.focus = -1;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND search_view_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DBLCLKS, .lpfnWndProc = search_proc,
                    .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"musicapp_search"};
  RegisterClassExW(&wc);

  memset(&v, 0, sizeof v);
  v.hot = v.pressed = v.focus = -1;
  v.dpi = USER_DEFAULT_SCREEN_DPI;
  v.scroll.moved = on_scrolled;
  if (!CreateWindowExW(0, L"musicapp_search", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent,
                       (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL))
    return NULL;
  v.edit = CreateWindowExW(0, WC_EDITW, NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 0, 0, v.wnd,
                           (HMENU)(INT_PTR)ID_EDIT, GetModuleHandleW(NULL), NULL);
  SendMessageW(v.edit, EM_LIMITTEXT, MAX_QUERY - 1, 0);
  SendMessageW(v.edit, EM_SETCUEBANNER, TRUE, (LPARAM)mc_wstr(MC_STR_SEARCH_PLACEHOLDER));
  SetWindowSubclass(v.edit, edit_proc, 0, 0);
  theme_center_edit(v.edit);
  return v.wnd;
}

static HFONT icon_font(int px) {
  return CreateFontW(-scale(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

void search_view_update(UINT dpi, HFONT font) {
  v.dpi = dpi;
  v.font = font;

  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 14, 9); /* 9pt -> 14pt */
  HFONT box_font = CreateFontIndirectW(&lf);
  SendMessageW(v.edit, WM_SETFONT, (WPARAM)box_font, TRUE);
  if (v.box_font) DeleteObject(v.box_font);
  v.box_font = box_font;
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 4, 3); /* 9pt -> 12pt */
  lf.lfWeight = FW_SEMIBOLD;
  if (v.heading_font) DeleteObject(v.heading_font);
  v.heading_font = CreateFontIndirectW(&lf);
  if (v.icon_font) DeleteObject(v.icon_font);
  v.icon_font = icon_font(16);
  if (v.small_icon_font) DeleteObject(v.small_icon_font);
  v.small_icon_font = icon_font(12);
  if (v.big_icon_font) DeleteObject(v.big_icon_font);
  v.big_icon_font = icon_font(48);

  layout();
  request_covers(); /* at the new size */
  InvalidateRect(v.wnd, NULL, FALSE);
}

void search_view_set_library(mc_library *lib) {
  v.lib = lib;
  v.lists_stale = TRUE;
  if (!v.query || !*v.query) return;
  v.results_stale = TRUE;
  if (v.wnd) InvalidateRect(v.wnd, NULL, FALSE); /* searches again on the next paint, if visible */
}

void search_view_focus(void) {
  if (!v.edit) return;
  SetFocus(v.edit);
  SendMessageW(v.edit, EM_SETSEL, 0, -1);
}

const search_view_hit *search_view_opened(void) {
  return &v.opened;
}
