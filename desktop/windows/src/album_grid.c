#define COBJMACROS
#include "album_grid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wincodec.h>
#include <windowsx.h>

#include "add_to_playlist.h"
#include "canvas.h"
#include "cover.h"
#include "delete_dialog.h"
#include "playlist_dialogs.h"
#include "scroller.h"
#include "theme.h"
#include "wstr.h"

#define WM_APP_THUMB (WM_APP + 20) /* lParam: thumb_result *, from the loader thread */

/* Metrics at 96 DPI. */
enum {
  COVER_RADIUS = 6,
  HOVER_PAD = 8, /* hover fill around the cover and title */
  RING_GAP = 2,  /* selection ring: gap from the cover, and width */
  RING_W = 2,
  HEADER_BUTTON = 36, /* a group's Back and More buttons */
  HEADER_GAP = 16,    /* below them, above the tiles */
  HEADER_TEXT_GAP = 12,
};

/* The right-click menus' commands. */
enum {
  CMD_NEW_GROUP = 1,
  CMD_REMOVE_FROM_GROUP,
  CMD_RENAME_GROUP,
  CMD_DELETE_GROUP,
  CMD_DELETE_ALBUM,
  CMD_GROUP_FIRST = 100, /* Add to Album Group's groups, in order */
};

/* The parts of a group's header that act on a click. */
enum { PART_NONE, PART_BACK, PART_MORE };

typedef struct tile {
  char uid[MC_UID_STRLEN];
  wchar_t *title;
  wchar_t *artist;    /* NULL if unknown */
  int year;           /* 0 if unknown */
  wchar_t *cover_dir; /* NULL when the album has no cover */
  wchar_t *cover_file; /* a playlist's or group's picture, once the loader has looked; NULL if none */
  wchar_t sub[64];     /* beneath a playlist's or group's name: its track or album count */
  HBITMAP thumb;      /* premultiplied, with rounded corners */
  const uint32_t *pixels;
  int thumb_size;
  BOOL stale;  /* thumb is from before the last reload: drawn until it's decoded again */
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
  wchar_t *file; /* a playlist's or group's picture, or NULL */
} thumb_result;

/* Playlists and album groups show pictures made from their albums' covers, found by uid;
 * albums show their own covers, found by folder. */
static BOOL pictures(album_grid_view view) {
  return view == AGV_PLAYLISTS || view == AGV_GROUPS;
}

static struct {
  HWND wnd;
  mc_library *lib;
  BOOL dirty;                    /* reload when next shown */
  album_grid_view view;          /* what's shown */
  album_grid_view tiles_view;    /* what the tiles are (view switches before the reload) */
  char group_uid[MC_UID_STRLEN]; /* AGV_GROUP's group */
  wchar_t *group_name;           /* its name, as of the last reload */
  int hidden;                    /* playlists left out for having nothing to play here */
  tile *tiles;
  int count;
  UINT gen; /* bumped on every reload, so late thumbnails for old tiles are dropped */
  HFONT font, glyph_font, header_font, icon_font;

  /* Layout, in device pixels. */
  int cols, tile_w, row_h, pad, gap, text_gap, text_h;
  int top; /* of the first row: pad, plus a group's header */
  int client_w, client_h;
  int full_w; /* the width the tiles are sized for; see album_grid_set_full_width */
  scroller scroll;

  int hot;                          /* tile under the mouse, or -1 */
  int selected;                     /* or -1 */
  char selected_uid[MC_UID_STRLEN]; /* keeps the selection across reloads */
  BOOL tracking;                    /* asked for WM_MOUSELEAVE */
  int hot_part;                     /* PART_*: the header button under the mouse */
  BOOL navigated;                   /* the last click went into or out of a group, so its
                                     * double click's second click is ignored */

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
  wchar_t **dirs; /* snapshot of cover folders (for playlists and groups, their uids) for the loader, per gen */
  int dir_count;
  album_grid_view loader_view; /* what the snapshot is of */
  char *lib_dir;               /* the library's folder, for the loader's own connection, per gen */
  char *art_dir;               /* where playlists' and groups' pictures are cached */
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

/* The loader's own connection to the library, for playlists' and groups' pictures: making
 * one decodes four covers, which would hold up the UI thread's connection. */
typedef struct art_source {
  mc_library *lib;
  char *dir;
} art_source;

/* The picture file of playlist uid (see mc_playlist_art()), or for group of album group uid,
 * or NULL if it has none. Opens the library in lib_dir first, if it isn't the one open. */
static wchar_t *playlist_art(art_source *src, const char *lib_dir, const char *art_dir, const wchar_t *uid,
                             BOOL group) {
  if (!lib_dir || !art_dir || !uid) return NULL;
  if (src->lib && strcmp(src->dir, lib_dir)) {
    mc_library_close(src->lib);
    free(src->dir);
    src->lib = NULL;
    src->dir = NULL;
  }
  if (!src->lib && mc_library_open(lib_dir, &src->lib) == MC_OK) src->dir = _strdup(lib_dir);
  if (!src->lib || !src->dir) return NULL;
  char *u = wstr_to_utf8(uid), *path = NULL;
  if (u && group) mc_album_group_art(src->lib, u, art_dir, &path);
  else if (u) mc_playlist_art(src->lib, u, art_dir, &path);
  free(u);
  wchar_t *file = widen(path);
  mc_free(path);
  return file;
}

static DWORD WINAPI loader_main(void *arg) {
  (void)arg;
  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  IWICImagingFactory *wic = NULL;
  CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                   (void **)&wic);
  art_source art = {0};

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
    BOOL playlist = pictures(g.loader_view), group = g.loader_view == AGV_GROUPS;
    char *lib_dir = playlist && g.lib_dir ? _strdup(g.lib_dir) : NULL;
    char *art_dir = playlist && g.art_dir ? _strdup(g.art_dir) : NULL;
    g.busy_index = job.index;
    g.busy_gen = gen;
    LeaveCriticalSection(&g.lock);

    thumb_result *r = malloc(sizeof *r);
    if (!r) {
      free(dir);
      free(lib_dir);
      free(art_dir);
      continue;
    }
    r->index = job.index;
    r->size = job.size;
    r->gen = gen;
    r->pixels = NULL;
    r->file = playlist ? playlist_art(&art, lib_dir, art_dir, dir, group) : NULL;
    if (!wic) r->bmp = NULL;
    else if (playlist) r->bmp = r->file ? cover_decode_file(wic, r->file, job.size, job.radius, &r->pixels) : NULL;
    else r->bmp = dir ? cover_decode(wic, dir, job.size, job.radius, &r->pixels) : NULL;
    free(dir);
    free(lib_dir);
    free(art_dir);
    if (!PostMessageW(g.wnd, WM_APP_THUMB, 0, (LPARAM)r)) {
      if (r->bmp) DeleteObject(r->bmp);
      free(r->file);
      free(r);
    }
  }
  if (art.lib) mc_library_close(art.lib);
  free(art.dir);
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
  g.top = g.pad + (g.view == AGV_GROUP ? scale(HEADER_BUTTON + HEADER_GAP) : 0);
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
  /* A group's header: the buttons' icons, and its name larger and semibold, as the track
   * list's title. */
  if (g.icon_font) DeleteObject(g.icon_font);
  g.icon_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH, theme_icon_face());
  if (g.header_font) DeleteObject(g.header_font);
  g.header_font = NULL;
  LOGFONTW lf;
  if (g.font && GetObjectW(g.font, sizeof lf, &lf)) {
    lf.lfHeight = MulDiv(lf.lfHeight, 5, 3);
    lf.lfWeight = FW_SEMIBOLD;
    g.header_font = CreateFontIndirectW(&lf);
  }

  g.scroll.pos = g.scroll.target = (first / g.cols) * g.row_h;
  scroller_resize(&g.scroll, &rc, rows ? g.top + g.pad + rows * g.row_h - g.gap : g.top);
}

/* --- Thumbnails (UI thread) --- */

static void free_thumb(tile *t) {
  if (!t->thumb) return;
  DeleteObject(t->thumb);
  t->thumb = NULL;
  t->pixels = NULL;
  t->thumb_size = 0;
  t->stale = FALSE;
  g.loaded--;
}

/* Index range of tiles at least partly in view. */
static void visible_range(int *first, int *last) {
  int top_row = g.row_h ? (g.scroll.pos - g.top) / g.row_h : 0;
  int bottom_row = g.row_h ? (g.scroll.pos + g.client_h - g.top) / g.row_h : 0;
  if (top_row < 0) top_row = 0;
  *first = top_row * g.cols;
  *last = (bottom_row + 1) * g.cols - 1;
  if (*last >= g.count) *last = g.count - 1;
}

/* Every playlist and group is looked up: only the loader finds out whether it has a picture. */
static BOOL wants_thumb(int i) {
  const tile *t = &g.tiles[i];
  return (t->cover_dir || pictures(g.tiles_view)) && !t->failed && (!t->thumb || t->stale || t->thumb_size != g.tile_w);
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

static void notify_selection(void);

static void on_thumb(thumb_result *r) {
  if (r->gen != g.gen || r->index < 0 || r->index >= g.count) {
    if (r->bmp) DeleteObject(r->bmp);
    free(r->file);
    free(r);
    return;
  }
  int index = r->index;
  tile *t = &g.tiles[index];
  free_thumb(t);
  /* A selected playlist's new picture is news: the track list shows it too. */
  BOOL changed = !r->file != !t->cover_file || (r->file && wcscmp(r->file, t->cover_file));
  free(t->cover_file);
  t->cover_file = r->file;
  if (changed && index == g.selected) notify_selection();
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
  RECT tr = {g.pad + col * (g.tile_w + g.gap), g.top + row * g.row_h - g.scroll.pos, 0, 0};
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
  if (g.selected < 0 || g.selected >= g.count || g.tiles_view == AGV_GROUPS) return NULL; /* groups open instead */
  memcpy(uid, g.tiles[g.selected].uid, MC_UID_STRLEN);
  return g.tiles[g.selected].title;
}

const wchar_t *album_grid_selection_artist(void) {
  return g.selected >= 0 && g.selected < g.count ? g.tiles[g.selected].artist : NULL;
}

const wchar_t *album_grid_selection_cover(void) {
  if (g.selected < 0 || g.selected >= g.count) return NULL;
  return pictures(g.tiles_view) ? g.tiles[g.selected].cover_file : g.tiles[g.selected].cover_dir;
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
    free(g.tiles[i].cover_file);
  }
  free(g.tiles);
  g.tiles = NULL;
  g.count = 0;
}

/* The albums' tiles (all of them, or the shown group's), into tiles and dirs (n of each). */
static void read_albums(tile **tiles, wchar_t ***dirs, int *n) {
  mc_album_list *list = NULL;
  if (g.lib && g.view == AGV_GROUP) mc_library_list_album_group_albums(g.lib, g.group_uid, &list);
  else if (g.lib) mc_library_list_albums(g.lib, &list);
  *n = list ? list->count : 0;
  *tiles = *n ? calloc((size_t)*n, sizeof **tiles) : NULL;
  *dirs = *n ? calloc((size_t)*n, sizeof **dirs) : NULL;
  if (*n && (!*tiles || !*dirs)) *n = 0;
  for (int i = 0; i < *n; i++) {
    const mc_album_info *a = &list->items[i];
    tile *t = &(*tiles)[i];
    memcpy(t->uid, a->uid, sizeof t->uid);
    t->title = widen(a->title && *a->title ? a->title : mc_str(MC_STR_UNTITLED));
    t->artist = a->artist && *a->artist ? widen(a->artist) : NULL;
    t->year = a->year;
    t->cover_dir = (*dirs)[i] = a->has_art ? widen(a->folder) : NULL;
  }
  mc_album_list_free(list);
}

/* The playlists' tiles: the loader finds their pictures by uid, and the track count goes
 * beneath the name. Only those with a track to play here; the rest are counted in g.hidden. */
static void read_playlists(tile **tiles, wchar_t ***dirs, int *n) {
  mc_playlist_list *list = NULL;
  if (g.lib) mc_library_list_playlists(g.lib, MC_LIST_PLAYABLE, &list);
  g.hidden = list ? list->hidden : 0;
  *n = list ? list->count : 0;
  *tiles = *n ? calloc((size_t)*n, sizeof **tiles) : NULL;
  *dirs = *n ? calloc((size_t)*n, sizeof **dirs) : NULL;
  if (*n && (!*tiles || !*dirs)) *n = 0;
  for (int i = 0; i < *n; i++) {
    const mc_playlist_info *p = &list->items[i];
    tile *t = &(*tiles)[i];
    memcpy(t->uid, p->uid, sizeof t->uid);
    (*dirs)[i] = widen(p->uid);
    t->title = widen(p->name && *p->name ? p->name : mc_str(MC_STR_UNTITLED));
    mc_wformat(t->sub, sizeof t->sub / sizeof t->sub[0], mc_str_plural(MC_STR_TRACKS, p->track_count),
               MC_D(p->track_count));
  }
  mc_playlist_list_free(list);
}

/* The album groups' tiles, as the playlists' with the album count beneath the name. For
 * AGV_GROUP, finds the shown group's name instead (g.group_name), or leaves it NULL if the
 * group has gone; *n is then 0. */
static void read_groups(tile **tiles, wchar_t ***dirs, int *n) {
  mc_album_group_list *list = NULL;
  if (g.lib) mc_library_list_album_groups(g.lib, &list);
  *n = 0;
  *tiles = NULL;
  *dirs = NULL;
  if (g.view == AGV_GROUP) {
    free(g.group_name);
    g.group_name = NULL;
    for (int i = 0; list && i < list->count; i++)
      if (!strcmp(list->items[i].uid, g.group_uid))
        g.group_name = widen(list->items[i].name && *list->items[i].name ? list->items[i].name : mc_str(MC_STR_UNTITLED));
    mc_album_group_list_free(list);
    return;
  }
  *n = list ? list->count : 0;
  *tiles = *n ? calloc((size_t)*n, sizeof **tiles) : NULL;
  *dirs = *n ? calloc((size_t)*n, sizeof **dirs) : NULL;
  if (*n && (!*tiles || !*dirs)) *n = 0;
  for (int i = 0; i < *n; i++) {
    const mc_album_group_info *a = &list->items[i];
    tile *t = &(*tiles)[i];
    memcpy(t->uid, a->uid, sizeof t->uid);
    (*dirs)[i] = widen(a->uid);
    t->title = widen(a->name && *a->name ? a->name : mc_str(MC_STR_UNTITLED));
    mc_wformat(t->sub, sizeof t->sub / sizeof t->sub[0], mc_str_plural(MC_STR_COUNT_ALBUMS, a->album_count),
               MC_D(a->album_count));
  }
  mc_album_group_list_free(list);
}

static int old_tile_cmp(const void *key, const void *elem) {
  return strcmp(key, g.tiles[*(const int *)elem].uid);
}

static int old_tile_sort(const void *a, const void *b) {
  return strcmp(g.tiles[*(const int *)a].uid, g.tiles[*(const int *)b].uid);
}

/* Moves the old tiles' thumbnails to the new ones of the same album and cover folder (or the
 * same playlist), so a reload (as sync brings albums in) doesn't blank the covers. They're
 * marked stale and decoded again, since the cover may have changed: a playlist's picture
 * does whenever a track that changes it comes or goes. */
static void carry_thumbs(tile *tiles, int n, BOOL playlists) {
  int *order = g.count && n ? malloc((size_t)g.count * sizeof *order) : NULL;
  if (!order) return;
  for (int i = 0; i < g.count; i++) order[i] = i;
  qsort(order, (size_t)g.count, sizeof *order, old_tile_sort);
  for (int i = 0; i < n; i++) {
    if (!playlists && !tiles[i].cover_dir) continue;
    const int *at = bsearch(tiles[i].uid, order, (size_t)g.count, sizeof *order, old_tile_cmp);
    tile *old = at ? &g.tiles[*at] : NULL;
    if (!old || !old->thumb) continue;
    if (playlists) {
      tiles[i].cover_file = old->cover_file; /* until the loader looks again */
      old->cover_file = NULL;
    } else if (!old->cover_dir || wcscmp(old->cover_dir, tiles[i].cover_dir)) {
      continue;
    }
    tiles[i].thumb = old->thumb;
    tiles[i].pixels = old->pixels;
    tiles[i].thumb_size = old->thumb_size;
    tiles[i].stale = TRUE;
    old->thumb = NULL; /* still counted in g.loaded, now by the new tile */
    old->pixels = NULL;
  }
  free(order);
}

static void reload(void) {
  g.dirty = FALSE;
  tile *tiles;
  wchar_t **dirs;
  int n;
  if (g.view == AGV_GROUP) {
    read_groups(&tiles, &dirs, &n); /* its name */
    if (!g.group_name) { /* deleted, maybe on another device: back to the groups */
      g.view = AGV_GROUPS;
      g.scroll.pos = g.scroll.target = 0;
    }
  }
  if (g.view == AGV_PLAYLISTS) read_playlists(&tiles, &dirs, &n);
  else if (g.view == AGV_GROUPS) read_groups(&tiles, &dirs, &n);
  else read_albums(&tiles, &dirs, &n);
  char *lib_dir = g.lib ? _strdup(mc_library_dir(g.lib)) : NULL;
  int selected = -1;
  for (int i = 0; i < n; i++)
    if (g.selected_uid[0] && !strcmp(tiles[i].uid, g.selected_uid)) selected = i;
  /* A renamed selection is news too: the track list shows its name. */
  BOOL renamed = selected >= 0 && g.selected >= 0 && g.selected < g.count && g.tiles[g.selected].title &&
                 tiles[selected].title && wcscmp(tiles[selected].title, g.tiles[g.selected].title) != 0;
  /* So is a cover that came or went: the track list shows it too. */
  if (selected >= 0 && g.selected >= 0 && g.selected < g.count &&
      !tiles[selected].cover_dir != !g.tiles[g.selected].cover_dir)
    renamed = TRUE;
  if (pictures(g.view) == pictures(g.tiles_view)) carry_thumbs(tiles, n, pictures(g.view));
  g.tiles_view = g.view;

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
  g.loader_view = g.view;
  free(g.lib_dir);
  g.lib_dir = lib_dir;
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

/* Switches to view (and for AGV_GROUP, group), dropping the selection; select, if given,
 * is selected once it's loaded, without notifying the parent. */
static void switch_view(album_grid_view view, const char *group, const char *select) {
  if (view == g.view && (view != AGV_GROUP || !strcmp(group, g.group_uid))) return;
  BOOL had = g.selected_uid[0] != '\0';
  g.view = view;
  if (view == AGV_GROUP) snprintf(g.group_uid, sizeof g.group_uid, "%s", group);
  g.selected = g.hot = -1;
  snprintf(g.selected_uid, sizeof g.selected_uid, "%s", select ? select : "");
  g.hot_part = PART_NONE;
  g.scroll.pos = g.scroll.target = 0;
  g.dirty = TRUE;
  if (g.wnd) InvalidateRect(g.wnd, NULL, FALSE);
  if (had) notify_selection();
}

void album_grid_show(album_grid_view view) {
  if (view != AGV_GROUP) switch_view(view, NULL, NULL);
}

void album_grid_show_playlists(BOOL playlists) {
  album_grid_show(playlists ? AGV_PLAYLISTS : AGV_ALBUMS);
}

BOOL album_grid_showing_playlists(void) {
  return g.view == AGV_PLAYLISTS;
}

album_grid_view album_grid_shown(void) {
  return g.view;
}

void album_grid_open_group(const char *uid) {
  if (!uid || !*uid) return;
  switch_view(AGV_GROUP, uid, NULL);
  PostMessageW(GetParent(g.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(g.wnd), AGN_OPEN_GROUP), (LPARAM)g.wnd);
}

const char *album_grid_group(void) {
  return g.view == AGV_GROUP ? g.group_uid : NULL;
}

/* From a group back to the groups, with it selected. */
static void go_back(void) {
  if (g.view != AGV_GROUP) return;
  char uid[MC_UID_STRLEN];
  memcpy(uid, g.group_uid, sizeof uid);
  switch_view(AGV_GROUPS, NULL, uid);
}

BOOL album_grid_back(void) {
  if (g.view != AGV_GROUP) return FALSE;
  go_back();
  return TRUE;
}

void album_grid_set_playlist_art_dir(const char *dir) {
  EnterCriticalSection(&g.lock);
  free(g.art_dir);
  g.art_dir = dir ? _strdup(dir) : NULL;
  LeaveCriticalSection(&g.lock);
}

void album_grid_set_library(mc_library *lib) {
  g.lib = lib;
  g.dirty = TRUE;
  if (g.wnd) InvalidateRect(g.wnd, NULL, FALSE); /* reloads on the next paint, if visible */
}

/* --- Tiles under the mouse --- */

/* Where tile i is in the view: its cover, its title, and both with room for the hover fill. */
static void tile_rects(int i, RECT *art, RECT *text, RECT *outer) {
  int x = g.pad + (i % g.cols) * (g.tile_w + g.gap), y = g.top + (i / g.cols) * g.row_h - g.scroll.pos;
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
  int cx = x - g.pad, cy = y + g.scroll.pos - g.top;
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

/* The scroller already repainted; the tiles in view changed. */
static void on_scrolled(void) {
  request_thumbs();
  update_hot();
}

/* --- A group's header --- */

/* Its Back button, at the top left, or its More button (Rename, Delete), at the top right. */
static RECT header_button(int part) {
  int size = scale(HEADER_BUTTON), y = g.pad - g.scroll.pos;
  int x = part == PART_BACK ? g.pad : g.client_w - g.pad - size;
  RECT r = {x, y, x + size, y + size};
  return r;
}

static int header_part_at(int x, int y) {
  if (g.tiles_view != AGV_GROUP) return PART_NONE;
  POINT pt = {x, y};
  RECT back = header_button(PART_BACK), more = header_button(PART_MORE);
  return PtInRect(&back, pt) ? PART_BACK : PtInRect(&more, pt) ? PART_MORE : PART_NONE;
}

static void set_hot_part(int part) {
  if (part == g.hot_part) return;
  RECT r;
  if (g.hot_part != PART_NONE) InvalidateRect(g.wnd, (r = header_button(g.hot_part), &r), FALSE);
  if (part != PART_NONE) InvalidateRect(g.wnd, (r = header_button(part), &r), FALSE);
  g.hot_part = part;
}

static void paint_header(canvas *c) {
  RECT back = header_button(PART_BACK), more = header_button(PART_MORE);
  int radius = scale(HEADER_BUTTON) / 2;
  const wchar_t *glyphs[] = {L"\xE72B", L"\xE712"}; /* Back, More */
  RECT buttons[] = {back, more};
  for (int i = 0; i < 2; i++) {
    COLORREF color = theme_draw_state(c, &buttons[i], radius, g.hot_part == i + PART_BACK ? STATE_HOT : STATE_NORMAL);
    SelectObject(c->dc, g.icon_font);
    SetTextColor(c->dc, color);
    DrawTextW(c->dc, glyphs[i], 1, &buttons[i], DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  RECT name = {back.right + scale(HEADER_TEXT_GAP), back.top, more.left - scale(HEADER_TEXT_GAP), back.bottom};
  SelectObject(c->dc, g.header_font ? g.header_font : g.font);
  SetTextColor(c->dc, theme.text);
  if (g.group_name)
    DrawTextW(c->dc, g.group_name, -1, &name, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  SelectObject(c->dc, g.font ? (HGDIOBJ)g.font : GetStockObject(DEFAULT_GUI_FONT));
}

/* --- Menus --- */

/* Posted, as the parent reloads everything, this grid too. */
static void notify_changed(void) {
  PostMessageW(GetParent(g.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(g.wnd), AGN_LIBRARY_CHANGED), (LPARAM)g.wnd);
}

static UINT menu_flags(void) {
  return TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
}

/* Rename... and Delete Album Group... for group uid, named name, at pt (screen
 * coordinates). */
static void show_group_menu(const char *uid, const wchar_t *name, POINT pt) {
  /* Copies: the dialogs' message loops repaint the grid, which may reload it. */
  char group[MC_UID_STRLEN];
  snprintf(group, sizeof group, "%s", uid);
  wchar_t *title = _wcsdup(name ? name : L"");
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, CMD_RENAME_GROUP, mc_wstr_mnemonic(MC_STR_PLAYLIST_RENAME_MENU));
  AppendMenuW(menu, MF_STRING, CMD_DELETE_GROUP, mc_wstr_mnemonic(MC_STR_ALBUM_GROUP_DELETE_MENU));
  int cmd = TrackPopupMenu(menu, menu_flags(), pt.x, pt.y, 0, g.wnd, NULL);
  DestroyMenu(menu);
  HWND owner = GetAncestor(g.wnd, GA_ROOT);
  BOOL changed = FALSE;
  if (cmd == CMD_RENAME_GROUP) changed = album_group_rename_dialog(owner, g.lib, group, title);
  else if (cmd == CMD_DELETE_GROUP) changed = album_group_delete_dialog(owner, g.lib, group, title);
  free(title);
  if (changed) notify_changed();
}

/* The More button's menu: the shown group's, hanging from the button's right edge. */
static void show_more_menu(void) {
  if (g.tiles_view != AGV_GROUP) return;
  RECT r = header_button(PART_MORE);
  POINT pt = {r.right, r.bottom};
  ClientToScreen(g.wnd, &pt);
  show_group_menu(g.group_uid, g.group_name, pt);
}

/* Add to Album Group, inside a group Remove from Album Group, and Delete Album..., for album
 * tile i. */
static void show_album_menu(int i, POINT pt) {
  char album[MC_UID_STRLEN], group[MC_UID_STRLEN];
  memcpy(album, g.tiles[i].uid, sizeof album);
  memcpy(group, g.group_uid, sizeof group);
  wchar_t *title = _wcsdup(g.tiles[i].title ? g.tiles[i].title : L"");
  BOOL in_group = g.tiles_view == AGV_GROUP;
  HMENU menu = CreatePopupMenu();
  mc_album_group_list *groups = NULL;
  HMENU add = add_to_album_group_menu(g.lib, CMD_GROUP_FIRST, CMD_NEW_GROUP, &groups);
  if (add) AppendMenuW(menu, MF_POPUP, (UINT_PTR)add, mc_wstr_mnemonic(MC_STR_ADD_TO_ALBUM_GROUP));
  if (in_group) {
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_REMOVE_FROM_GROUP, mc_wstr_mnemonic(MC_STR_REMOVE_FROM_ALBUM_GROUP));
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_DELETE_ALBUM, mc_wstr_mnemonic(MC_STR_DELETE_ALBUM_MENU));
  int cmd = TrackPopupMenu(menu, menu_flags(), pt.x, pt.y, 0, g.wnd, NULL);
  DestroyMenu(menu); /* and the submenu */

  HWND owner = GetAncestor(g.wnd, GA_ROOT);
  BOOL changed = FALSE;
  if (cmd == CMD_DELETE_ALBUM) changed = delete_dialog(owner, g.lib, MC_DELETE_ALBUMS, album, title);
  free(title);
  if (cmd >= CMD_GROUP_FIRST && groups && cmd - CMD_GROUP_FIRST < groups->count)
    changed = add_to_album_group(owner, g.lib, album, groups->items[cmd - CMD_GROUP_FIRST].uid);
  mc_album_group_list_free(groups);
  if (cmd == CMD_NEW_GROUP) changed = add_to_album_group(owner, g.lib, album, NULL);
  if (cmd == CMD_REMOVE_FROM_GROUP) {
    changed = g.lib && mc_album_group_remove(g.lib, group, album) == MC_OK;
    if (!changed)
      MessageBoxW(owner, mc_wstr(MC_STR_ALBUM_GROUP_REMOVE_FAILED), mc_wstr(MC_STR_ALBUM_GROUP), MB_OK | MB_ICONERROR);
  }
  if (changed) notify_changed();
}

/* WM_CONTEXTMENU: a tile's menu, or the group's on its header. From the keyboard (lp -1),
 * the selected tile's, under its cover. Playlists have none here: theirs is in the track
 * list. */
static BOOL on_context_menu(LPARAM lp) {
  if (!g.lib) return FALSE;
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  int i;
  if (lp == -1) {
    i = g.selected;
    if (i < 0 && g.tiles_view == AGV_GROUP) {
      show_more_menu();
      return TRUE;
    }
    if (i < 0 || i >= g.count) return FALSE;
    RECT art, text, outer;
    tile_rects(i, &art, &text, &outer);
    pt.x = art.left;
    pt.y = art.bottom;
    ClientToScreen(g.wnd, &pt);
  } else {
    POINT client = pt;
    ScreenToClient(g.wnd, &client);
    i = tile_at(client.x, client.y);
    if (i < 0 && g.tiles_view == AGV_GROUP && client.y < g.top - g.scroll.pos) {
      show_group_menu(g.group_uid, g.group_name, pt);
      return TRUE;
    }
    if (i < 0) return FALSE;
  }
  if (g.tiles_view == AGV_GROUPS) show_group_menu(g.tiles[i].uid, g.tiles[i].title, pt);
  else if (g.tiles_view != AGV_PLAYLISTS) show_album_menu(i, pt);
  else return FALSE;
  return TRUE;
}

/* --- Scrolling and the mouse --- */

static void on_mouse_move(int x, int y) {
  if (!g.tracking) {
    TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, g.wnd, 0};
    TrackMouseEvent(&tme);
    g.tracking = TRUE;
  }
  BOOL on_bar = scroller_mouse_move(&g.scroll, x, y);
  set_hot(on_bar ? -1 : tile_at(x, y));
  set_hot_part(on_bar ? PART_NONE : header_part_at(x, y));
}

/* A click selects an album or playlist, or opens a group; a group's header buttons go back
 * or open its menu. */
static void on_mouse_down(int x, int y) {
  SetFocus(g.wnd); /* for keyboard scrolling */
  g.navigated = FALSE;
  if (scroller_mouse_down(&g.scroll, x, y)) return;
  int part = header_part_at(x, y), i = tile_at(x, y);
  if (part == PART_BACK) {
    go_back();
    g.navigated = TRUE;
  } else if (part == PART_MORE) {
    show_more_menu();
  } else if (g.tiles_view == AGV_GROUPS && i >= 0) {
    album_grid_open_group(g.tiles[i].uid);
    g.navigated = TRUE;
  } else {
    select_tile(i);
  }
}

/* The arrow keys, Home, End, Page Up and Page Down move the selection, as clicks would, and
 * scroll it into view; on the groups, Enter opens the selected one. In a group, Backspace
 * and Alt+Left go back. Returns FALSE for other keys. */
static BOOL on_key(WPARAM key) {
  if (key == VK_BACK || key == VK_BROWSER_BACK || (key == VK_LEFT && GetKeyState(VK_MENU) < 0)) {
    if (g.view != AGV_GROUP) return FALSE;
    go_back();
    return TRUE;
  }
  if (key == VK_RETURN && g.tiles_view == AGV_GROUPS && g.selected >= 0 && g.selected < g.count) {
    album_grid_open_group(g.tiles[g.selected].uid);
    return TRUE;
  }
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
  int top = g.top + (to / g.cols) * g.row_h;
  scroller_reveal(&g.scroll, top - g.pad, top + g.row_h - g.gap + g.pad);
  return TRUE;
}

BOOL album_grid_select(const char *uid) {
  if (g.dirty) reload(); /* e.g. just switched between albums and playlists */
  for (int i = 0; i < g.count; i++) {
    if (strcmp(g.tiles[i].uid, uid)) continue;
    select_tile(i);
    if (g.cols) {
      int top = g.top + (i / g.cols) * g.row_h;
      scroller_reveal(&g.scroll, top - g.pad, top + g.row_h - g.gap + g.pad);
    }
    return TRUE;
  }
  return FALSE;
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
      /* The sidebar's Playlists or Album Groups icon, or a note. */
      const wchar_t *glyph = g.tiles_view == AGV_PLAYLISTS ? L"\xE90B" : g.tiles_view == AGV_GROUPS ? L"\xE8F1" : L"\xE8D6";
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
  const wchar_t *sub = pictures(g.tiles_view) ? t->sub : t->artist ? t->artist : mc_wstr(MC_STR_UNKNOWN_ARTIST);
  DrawTextW(c->dc, sub, -1, &line, one_line);
}

/* Everything is drawn into a canvas, then copied in one go. GDI text leaves alpha at 0, so
 * it's made opaque at the end, except outside the rounded top-left corner, which shows
 * the sidebar's Mica. */
static void paint(HDC dc, const RECT *dirty) {
  if (!canvas_reserve(&g.buf, g.client_w, g.client_h)) return;
  canvas *c = &g.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, g.font ? (HGDIOBJ)g.font : GetStockObject(DEFAULT_GUI_FONT));

  if (g.tiles_view == AGV_GROUP) paint_header(c);
  if (g.count == 0) {
    RECT rc = {0, 0, g.client_w, g.client_h};
    SetTextColor(c->dc, theme.text3);
    wchar_t empty[512];
    BOOL playlists = g.tiles_view == AGV_PLAYLISTS;
    if (!g.lib) swprintf(empty, 512, L"%ls", mc_wstr(MC_STR_LIBRARY_OPEN_FAILED));
    else if (playlists && g.hidden)
      swprintf(empty, 512, L"%ls\n%ls", mc_wstr(MC_STR_NO_PLAYABLE_PLAYLISTS_TITLE),
               mc_wstr(MC_STR_NO_PLAYABLE_PLAYLISTS_TEXT));
    else if (playlists)
      swprintf(empty, 512, L"%ls\n%ls", mc_wstr(MC_STR_NO_PLAYLISTS_TITLE), mc_wstr(MC_STR_NO_PLAYLISTS_TEXT));
    else if (g.tiles_view == AGV_GROUPS)
      swprintf(empty, 512, L"%ls\n%ls", mc_wstr(MC_STR_NO_ALBUM_GROUPS_TITLE), mc_wstr(MC_STR_NO_ALBUM_GROUPS_TEXT));
    else if (g.tiles_view == AGV_GROUP)
      swprintf(empty, 512, L"%ls\n%ls", mc_wstr(MC_STR_ALBUM_GROUP_EMPTY_TITLE), mc_wstr(MC_STR_ALBUM_GROUP_EMPTY_TEXT));
    else swprintf(empty, 512, L"%ls\n%ls", mc_wstr(MC_STR_EMPTY_TITLE), mc_wstr(MC_STR_EMPTY_TEXT));
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
      set_hot_part(PART_NONE);
      return 0;
    case WM_LBUTTONDBLCLK:
      if (g.navigated) { /* the page under it changed with the first click */
        g.navigated = FALSE;
        return 0;
      }
      /* fall through */
    case WM_LBUTTONDOWN:
      on_mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_XBUTTONUP: /* the mouse's back button */
      if (GET_XBUTTON_WPARAM(wp) == XBUTTON1 && g.view == AGV_GROUP) {
        go_back();
        return TRUE;
      }
      break;
    case WM_CONTEXTMENU:
      if (on_context_menu(lp)) return 0;
      break;
    case WM_LBUTTONUP:
      scroller_mouse_up(&g.scroll);
      return 0;
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&g.scroll);
      return 0;
    case WM_KEYDOWN:
      if (on_key(wp)) return 0;
      break;
    case WM_SYSKEYDOWN: /* Alt+Left */
      if (wp == VK_LEFT && on_key(wp)) return 0;
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
      free(g.lib_dir);
      free(g.art_dir);
      free(g.queue);
      canvas_free(&g.buf);
      if (g.glyph_font) DeleteObject(g.glyph_font);
      if (g.icon_font) DeleteObject(g.icon_font);
      if (g.header_font) DeleteObject(g.header_font);
      free(g.group_name);
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
                    .lpszClassName = L"kithsong_albumgrid"};
  RegisterClassExW(&wc);

  memset(&g, 0, sizeof g);
  g.busy_index = -1;
  g.hot = g.selected = -1;
  g.scroll.moved = on_scrolled;
  g.dirty = TRUE;
  InitializeCriticalSection(&g.lock);
  InitializeConditionVariable(&g.wake);
  g.wnd = CreateWindowExW(0, L"kithsong_albumgrid", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (g.wnd) g.thread = CreateThread(NULL, 0, loader_main, NULL, 0, NULL);
  return g.wnd;
}
