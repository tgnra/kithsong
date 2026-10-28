#define COBJMACROS
#include "track_list.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "add_to_playlist.h"
#include "canvas.h"
#include "cover.h"
#include "scroller.h"
#include "theme.h"
#include "wstr.h"

/* Metrics at 96 DPI. */
enum {
  PAD_X = 18,   /* the cover from the left edge */
  PAD_TOP = 18, /* above the cover */
  COVER = 96,
  COVER_RADIUS = 8,
  COVER_GAP = 16, /* between the cover and the header's text */
  LINE_GAP = 4,   /* between the header's lines */
  CLOSE = 32,     /* the close button's square */
  CLOSE_TOP = 12,
  CLOSE_RIGHT = 12,
  ACTIONS_GAP = 16, /* between the cover and the Play and Shuffle buttons */
  ACTION_H = 32,
  ACTION_GAP = 8, /* between Play and Shuffle */
  HEADER_BOTTOM = 18,
  ROW_H = 36,
  INSET = 8,   /* rows' fill from the edges */
  CELL_X = 12, /* text inside the fill */
  NUM_W = 28,  /* track number column */
  NUM_GAP = 12,
  DUR_W = 52,
  RADIUS = 4,
  MORE_GAP = 4,  /* between the playlist menu's button and the close button */
  DROP_LINE = 2, /* where a dragged track would go */
};

/* Context menu items. Play Next and Play Last are stubs until there's a play queue. */
enum {
  CMD_NEW_PLAYLIST = 1,
  CMD_PLAY_NEXT,
  CMD_PLAY_LAST,
  CMD_MOVE_UP,
  CMD_MOVE_DOWN,
  CMD_REMOVE,
  CMD_RENAME,
  CMD_DELETE,
  CMD_DELETE_TRACK,
  CMD_PLAYLIST_FIRST = 100, /* Add to Playlist's playlists */
};

enum { TIMER_DRAG_SCROLL = 103 }; /* the scroller has 101 and 102 */

typedef struct row {
  char uid[MC_UID_STRLEN];
  char entry_uid[MC_UID_STRLEN]; /* its place in a playlist, else "" */
  wchar_t *title;
  wchar_t *artist, *album, *cover_dir;
  wchar_t num[16];
  wchar_t duration[16];
  int duration_ms;
  char *path;
} row;

static struct {
  HWND wnd;
  UINT dpi;
  HFONT font, title_font, sub_font, icon_font, glyph_font;
  canvas buf;
  scroller scroll;
  HWND tooltip;
  IWICImagingFactory *wic;

  mc_library *lib;
  char album_uid[MC_UID_STRLEN];
  BOOL playlist; /* the shown uid is a playlist's */
  BOOL smart;    /* a smart playlist's: no entries to edit, and no playlist menu */
  BOOL random;   /* a smart playlist in random order: the menu's button reshuffles it */
  BOOL by_artist; /* an artist's: no header (the artist page has its own), and an album column */
  wchar_t *album_title;
  wchar_t *artist;     /* NULL if unknown */
  int year;            /* 0 if unknown */
  int total_ms;        /* the tracks' length */
  wchar_t summary[128]; /* "1997 · 12 tracks · 47 min" */
  wchar_t *cover_dir;  /* NULL if the album has no cover; a playlist's picture file */
  HBITMAP art;         /* the cover, premultiplied, with rounded corners; NULL shows the placeholder */
  const uint32_t *art_px;
  int art_size;
  row *rows;
  int count;
  BOOL close_hot, close_pressed;

  int header_h;    /* the fixed part above the rows */
  int actions_top; /* the Play and Shuffle buttons' */
  int action_hot, action_pressed; /* 0 Play, 1 Shuffle, or -1 */
  int hot, selected;
  BOOL tracking;
  BOOL more_hot, more_open; /* the playlist menu's button */
  int play_index; /* the track TLN_PLAY was last sent for */
  char playing_uid[MC_UID_STRLEN];
  track_list_request req;

  /* Dragging a playlist's track: pressed on drag_row at drag_from, and once it has moved
   * far enough, dragging, with drop_at the place it would go (0 to count). */
  int drag_row;
  POINT drag_from;
  BOOL dragging;
  int drop_at;
} s;

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

static wchar_t *widen(const char *u) {
  if (!u) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n);
  return w;
}

static void format_duration(int ms, wchar_t *out, size_t n) {
  int sec = (ms + 500) / 1000;
  if (ms <= 0) out[0] = L'\0';
  else if (sec >= 3600) swprintf(out, n, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else swprintf(out, n, L"%d:%02d", sec / 60, sec % 60);
}

/* --- Layout --- */

static int font_height(HFONT font) {
  HDC dc = GetDC(s.wnd);
  HGDIOBJ old = SelectObject(dc, font);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(s.wnd, dc);
  return tm.tmHeight;
}

static RECT close_rect(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  RECT r = {rc.right - scale(CLOSE_RIGHT + CLOSE), scale(CLOSE_TOP), rc.right - scale(CLOSE_RIGHT),
            scale(CLOSE_TOP + CLOSE)};
  return r;
}

static void set_close_hot(BOOL hot) {
  if (hot == s.close_hot) return;
  s.close_hot = hot;
  RECT r = close_rect();
  InvalidateRect(s.wnd, &r, FALSE);
}

static BOOL in_close(int x, int y) {
  if (s.by_artist) return FALSE;
  RECT r = close_rect();
  POINT pt = {x, y};
  return PtInRect(&r, pt);
}

static RECT more_rect(void);

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int text_h = 2 * font_height(s.title_font) + font_height(s.font) + font_height(s.sub_font) + 2 * scale(LINE_GAP);
  /* An artist's list has only Play and Shuffle above it: the artist page has its own header. */
  s.actions_top = s.by_artist ? scale(PAD_TOP) : scale(PAD_TOP) + max(scale(COVER), text_h) + scale(ACTIONS_GAP);
  s.header_h = s.actions_top + scale(ACTION_H) + scale(HEADER_BOTTOM);
  RECT view = {0, s.header_h + 1, rc.right, rc.bottom}; /* below the header's divider */
  scroller_resize(&s.scroll, &view, s.count * scale(ROW_H) + 2 * scale(INSET));
  if (s.tooltip) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = s.wnd, .uId = 1, .rect = s.by_artist ? (RECT){0} : close_rect()};
    SendMessageW(s.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
    ti.uId = 2;
    ti.rect = s.smart && s.random ? more_rect() : (RECT){0};
    SendMessageW(s.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  }
}

/* Row i's fill, in client coordinates. */
static RECT row_rect(int i) {
  int top = s.scroll.view.top + scale(INSET) + i * scale(ROW_H) - s.scroll.pos;
  RECT r = {scale(INSET), top, s.scroll.view.right - scale(INSET), top + scale(ROW_H)};
  return r;
}

/* The playlist menu's button, just left of the close button. */
static RECT more_rect(void) {
  RECT r = close_rect();
  OffsetRect(&r, -(scale(CLOSE) + scale(MORE_GAP)), 0);
  return r;
}

/* Play (0) and Shuffle (1), side by side under the cover, half the width each. */
static RECT action_rect(int i) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int left = scale(PAD_X), right = max(left, rc.right - scale(PAD_X));
  int half = max(0, (right - left - scale(ACTION_GAP)) / 2);
  RECT r = {i ? right - half : left, s.actions_top, i ? right : left + half, s.actions_top + scale(ACTION_H)};
  return r;
}

/* Whether any shown track can be played here: Play and Shuffle are greyed out otherwise. */
static BOOL playable(void) {
  for (int i = 0; i < s.count; i++)
    if (s.rows[i].path) return TRUE;
  return FALSE;
}

static int action_at(int x, int y) {
  POINT pt = {x, y};
  for (int i = 0; i < 2; i++) {
    RECT r = action_rect(i);
    if (PtInRect(&r, pt)) return playable() ? i : -1;
  }
  return -1;
}

static void invalidate_actions(void) {
  RECT r = action_rect(0), shuffle = action_rect(1);
  UnionRect(&r, &r, &shuffle);
  InvalidateRect(s.wnd, &r, FALSE);
}

static void set_action_hot(int i) {
  if (i == s.action_hot) return;
  s.action_hot = i;
  invalidate_actions();
}

static void invalidate_row(int i) {
  if (i < 0 || i >= s.count) return;
  RECT r = row_rect(i), isect;
  if (IntersectRect(&isect, &r, &s.scroll.view)) InvalidateRect(s.wnd, &isect, FALSE);
}

static void invalidate_more(void) {
  RECT r = more_rect();
  InvalidateRect(s.wnd, &r, FALSE);
}

static int row_at(int x, int y) {
  POINT pt = {x, y};
  if (!PtInRect(&s.scroll.view, pt)) return -1;
  for (int i = 0; i < s.count; i++) {
    RECT r = row_rect(i);
    if (PtInRect(&r, pt)) return i;
  }
  return -1;
}

/* The button left of the close button: a playlist's menu, or a random smart playlist's
 * Reshuffle. */
static BOOL has_more(void) {
  return s.playlist && (!s.smart || s.random);
}

static BOOL on_more(int x, int y) {
  RECT r = more_rect();
  POINT pt = {x, y};
  return has_more() && PtInRect(&r, pt);
}

static void set_hot(int i) {
  if (i == s.hot) return;
  invalidate_row(s.hot);
  invalidate_row(i);
  s.hot = i;
}

static void set_more_hot(BOOL hot) {
  if (hot == s.more_hot) return;
  s.more_hot = hot;
  invalidate_more();
}

static void reveal_row(int i) {
  int top = scale(INSET) + i * scale(ROW_H);
  scroller_reveal(&s.scroll, top - scale(INSET), top + scale(ROW_H) + scale(INSET));
}

static void select_row(int i) {
  if (i < 0 || i >= s.count) return;
  invalidate_row(s.selected);
  invalidate_row(i);
  s.selected = i;
  reveal_row(i);
}

static void on_scrolled(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(s.wnd, &pt);
  set_hot(s.tracking && !s.scroll.hot && !s.scroll.dragging && !s.dragging ? row_at(pt.x, pt.y) : -1);
}

/* --- Data --- */

static void clear_rows(void) {
  for (int i = 0; i < s.count; i++) {
    free(s.rows[i].title);
    free(s.rows[i].artist);
    free(s.rows[i].album);
    free(s.rows[i].cover_dir);
    free(s.rows[i].path);
  }
  free(s.rows);
  s.rows = NULL;
  s.count = 0;
}

static void end_drag(void);

/* "1997 · 12 tracks · 47 min" */
static void make_summary(void) {
  int n = s.count, min = (s.total_ms + 30000) / 60000, count = 0;
  char parts[3][64];
  if (s.year > 0) snprintf(parts[count++], 64, "%d", s.year);
  if (!n) {
    snprintf(parts[count++], 64, "%s", mc_str(MC_STR_NO_TRACKS));
  } else {
    mc_format(parts[count++], 64, mc_str_plural(MC_STR_TRACKS, n), MC_D(n));
    if (min >= 60) mc_format(parts[count++], 64, mc_str(MC_STR_DURATION_HOURS_MINUTES), MC_D(min / 60), MC_D(min % 60));
    else mc_format(parts[count++], 64, mc_str(MC_STR_DURATION_MINUTES), MC_D(min));
  }
  char text[256];
  size_t len = 0;
  for (int i = 0; i < count && len < sizeof text; i++)
    len += (size_t)snprintf(text + len, sizeof text - len, "%s%s", i ? mc_str(MC_STR_SEPARATOR_DOT) : "", parts[i]);
  wstr_from_utf8(s.summary, sizeof s.summary / sizeof s.summary[0], text);
}

static void free_art(void) {
  if (s.art) DeleteObject(s.art);
  s.art = NULL;
  s.art_px = NULL;
}

/* One small image, so it's decoded right here, as the now-playing strip does. */
static void load_art(void) {
  free_art();
  if (!s.cover_dir) return;
  if (!s.wic)
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                     (void **)&s.wic);
  s.art_size = scale(COVER);
  if (!s.wic) return;
  if (s.playlist) s.art = cover_decode_file(s.wic, s.cover_dir, s.art_size, scale(COVER_RADIUS), &s.art_px);
  else s.art = cover_decode(s.wic, s.cover_dir, s.art_size, scale(COVER_RADIUS), &s.art_px);
}

void track_list_set_details(const wchar_t *artist, int year, const wchar_t *cover_dir) {
  free(s.artist);
  s.artist = artist ? _wcsdup(artist) : NULL;
  s.year = year;
  make_summary();
  BOOL same_cover = (!cover_dir && !s.cover_dir) || (cover_dir && s.cover_dir && !wcscmp(cover_dir, s.cover_dir));
  if (!same_cover) {
    free(s.cover_dir);
    s.cover_dir = cover_dir ? _wcsdup(cover_dir) : NULL;
    load_art();
  } else if (s.cover_dir && !s.art) {
    load_art(); /* it wasn't there before; sync may have brought it since */
  }
  if (s.wnd) InvalidateRect(s.wnd, NULL, FALSE);
}

static void show(mc_library *lib, const char *uid, const wchar_t *title, BOOL playlist, BOOL artist, BOOL smart,
                 BOOL random) {
  BOOL same = uid && !strcmp(uid, s.album_uid) && !playlist == !s.playlist && !artist == !s.by_artist &&
              !smart == !s.smart;
  /* The selected track; in a playlist its entry, as a track can be in one twice. */
  char keep[MC_UID_STRLEN] = "";
  int was = s.selected;
  if (same && s.selected >= 0 && s.selected < s.count)
    strcpy(keep, playlist ? s.rows[s.selected].entry_uid : s.rows[s.selected].uid);

  end_drag();
  clear_rows();
  free(s.album_title);
  s.album_title = title ? _wcsdup(title) : NULL;
  s.lib = lib;
  s.album_uid[0] = '\0';
  s.playlist = playlist;
  s.by_artist = artist;
  s.smart = smart;
  s.random = random;
  s.hot = s.selected = s.action_hot = -1;
  s.more_hot = FALSE;
  if (!same) s.scroll.pos = s.scroll.target = 0;

  mc_track_list *list = NULL;
  if (lib && uid && strlen(uid) < MC_UID_STRLEN) {
    strcpy(s.album_uid, uid);
    if (smart) mc_library_list_smart_tracks(lib, uid, &list);
    else if (playlist) mc_library_list_playlist_tracks(lib, uid, &list);
    else if (artist) mc_library_list_artist_tracks(lib, uid, &list);
    else mc_library_list_album_tracks(lib, uid, &list);
  }
  int n = list ? list->count : 0, total_ms = 0;
  BOOL discs = FALSE;
  for (int i = 0; i < n; i++) discs |= list->items[i].disc_no > 1;
  s.rows = n ? calloc((size_t)n, sizeof *s.rows) : NULL;
  if (!s.rows) n = 0;
  for (int i = 0; i < n; i++) {
    const mc_track_info *t = &list->items[i];
    row *r = &s.rows[i];
    memcpy(r->uid, t->uid, sizeof r->uid);
    memcpy(r->entry_uid, t->entry_uid, sizeof r->entry_uid);
    r->title = widen(t->title && *t->title ? t->title : mc_str(MC_STR_UNTITLED));
    r->artist = t->artist && *t->artist ? widen(t->artist) : NULL;
    r->album = t->album && *t->album ? widen(t->album) : NULL;
    r->cover_dir = widen(t->cover_dir);
    if (playlist || artist) swprintf(r->num, 16, L"%d", i + 1);
    else if (t->track_no && discs) swprintf(r->num, 16, L"%d-%d", t->disc_no ? t->disc_no : 1, t->track_no);
    else if (t->track_no) swprintf(r->num, 16, L"%d", t->track_no);
    format_duration(t->duration_ms, r->duration, 16);
    r->duration_ms = t->duration_ms;
    r->path = t->path ? _strdup(t->path) : NULL;
    total_ms += t->duration_ms > 0 ? t->duration_ms : 0;
    if (keep[0] && s.selected < 0 && !strcmp(playlist ? r->entry_uid : r->uid, keep)) s.selected = i;
  }
  s.count = n;
  mc_track_list_free(list);
  /* A track taken out of a playlist leaves the selection where it was, on the next one. */
  if (playlist && keep[0] && s.selected < 0 && n) s.selected = was < n ? was : n - 1;

  s.total_ms = total_ms;
  if (!same) s.year = 0; /* until track_list_set_details */
  make_summary();

  relayout();
  if (same && s.selected >= 0) reveal_row(s.selected); /* a moved track stays in view */
  InvalidateRect(s.wnd, NULL, FALSE);
}

void track_list_show(mc_library *lib, const char *uid, const wchar_t *title, BOOL playlist) {
  show(lib, uid, title, playlist, FALSE, FALSE, FALSE);
}

void track_list_show_smart(mc_library *lib, const char *uid, const wchar_t *title, BOOL random) {
  show(lib, uid, title, TRUE, FALSE, TRUE, random);
}

void track_list_show_artist(mc_library *lib, const char *artist_uid) {
  show(lib, artist_uid, NULL, FALSE, TRUE, FALSE, FALSE);
}

int track_list_count(void) {
  return s.count;
}

track_list_item track_list_item_at(int i) {
  track_list_item item = {0};
  if (i < 0 || i >= s.count) return item;
  const row *r = &s.rows[i];
  item.uid = r->uid;
  item.title = r->title;
  item.artist = r->artist;
  item.album = r->album;
  item.cover_dir = r->cover_dir;
  item.duration_ms = r->duration_ms;
  item.path = r->path;
  return item;
}

int track_list_play_index(void) {
  return s.play_index;
}

const track_list_request *track_list_last_request(void) {
  return &s.req;
}

void track_list_select(const char *uid) {
  for (int i = 0; uid && i < s.count; i++)
    if (!strcmp(s.rows[i].uid, uid)) {
      select_row(i);
      return;
    }
}

void track_list_set_playing(const char *uid) {
  const char *now = uid ? uid : "";
  if (!strcmp(now, s.playing_uid) || strlen(now) >= MC_UID_STRLEN) return;
  strcpy(s.playing_uid, now);
  if (s.wnd) InvalidateRect(s.wnd, &s.scroll.view, FALSE);
}

static void notify(int code) {
  SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), code), (LPARAM)s.wnd);
}

/* The parent starts playback, which then marks the track through track_list_set_playing. */
static void play(int i) {
  if (i < 0 || i >= s.count) return;
  s.play_index = i;
  notify(TLN_PLAY);
}

/* --- Playlist requests --- */

static BOOL in_playlist(int i) {
  return s.playlist && i >= 0 && i < s.count && s.rows[i].entry_uid[0];
}

/* Moves row from to place to (0 to count, counted before the move): after the row above
 * that place, or to the start. Nothing if that's where it is. */
static void request_move(int from, int to) {
  if (!in_playlist(from) || to < 0 || to > s.count || to == from || to == from + 1) return;
  memset(&s.req, 0, sizeof s.req);
  strcpy(s.req.entry_uid, s.rows[from].entry_uid);
  if (to > 0) strcpy(s.req.after_uid, s.rows[to - 1].entry_uid);
  notify(TLN_MOVE);
}

static void request_remove(int i) {
  if (!in_playlist(i)) return;
  memset(&s.req, 0, sizeof s.req);
  strcpy(s.req.entry_uid, s.rows[i].entry_uid);
  notify(TLN_REMOVE);
}

static void request_playlist(int code) {
  /* A smart playlist can only be reshuffled here, and only a smart one can be. */
  if (!s.playlist || (s.smart ? code != TLN_RESHUFFLE : code == TLN_RESHUFFLE) || !s.album_uid[0]) return;
  memset(&s.req, 0, sizeof s.req);
  strcpy(s.req.playlist_uid, s.album_uid);
  notify(code);
}

/* --- Menus --- */

static UINT menu_flags(void) {
  return TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
}

/* Rename... and Delete Playlist..., under the [...] button. */
static void show_playlist_menu(void) {
  if (!s.playlist || s.smart) return;
  RECT r = more_rect();
  MapWindowPoints(s.wnd, NULL, (POINT *)&r, 2);
  HMENU menu = CreatePopupMenu();
  wchar_t item[128];
  AppendMenuW(menu, MF_STRING, CMD_RENAME,
              wstr_menu_item(item, 128, MC_STR_PLAYLIST_RENAME_MENU, MC_STR_WINDOWS_SHORTCUT_RENAME));
  AppendMenuW(menu, MF_STRING, CMD_DELETE, mc_wstr_mnemonic(MC_STR_PLAYLIST_DELETE_MENU));
  s.more_open = TRUE;
  invalidate_more();
  /* Hangs from the button's right edge, as the button sits at the window's. */
  TPMPARAMS tpm = {sizeof tpm, r};
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_VERTICAL | TPM_RIGHTALIGN;
  int cmd = TrackPopupMenuEx(menu, flags, r.right, r.bottom, s.wnd, &tpm);
  DestroyMenu(menu);
  s.more_open = FALSE;
  invalidate_more();
  if (cmd == CMD_RENAME) request_playlist(TLN_RENAME_PLAYLIST);
  else if (cmd == CMD_DELETE) request_playlist(TLN_DELETE_PLAYLIST);
}

/* The menu for the selected track, at pt (screen coordinates). */
static void show_menu(POINT pt) {
  int i = s.selected;
  if (i < 0 || i >= s.count) return;
  HMENU menu = CreatePopupMenu();
  mc_playlist_list *lists = NULL;
  HMENU add = add_to_playlist_menu(s.lib, CMD_PLAYLIST_FIRST, CMD_NEW_PLAYLIST, &lists);
  if (add) AppendMenuW(menu, MF_POPUP, (UINT_PTR)add, mc_wstr_mnemonic(MC_STR_ADD_TO_PLAYLIST));
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_PLAY_NEXT, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_NEXT));
  AppendMenuW(menu, MF_STRING, CMD_PLAY_LAST, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_LAST));
  if (in_playlist(i)) {
    wchar_t item[128];
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (i > 0 ? 0 : MF_GRAYED), CMD_MOVE_UP,
                wstr_menu_item(item, 128, MC_STR_MOVE_UP, MC_STR_WINDOWS_SHORTCUT_MOVE_UP));
    AppendMenuW(menu, MF_STRING | (i + 1 < s.count ? 0 : MF_GRAYED), CMD_MOVE_DOWN,
                wstr_menu_item(item, 128, MC_STR_MOVE_DOWN, MC_STR_WINDOWS_SHORTCUT_MOVE_DOWN));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_REMOVE,
                wstr_menu_item(item, 128, MC_STR_REMOVE_FROM_PLAYLIST, MC_STR_WINDOWS_SHORTCUT_REMOVE));
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_DELETE_TRACK, mc_wstr_mnemonic(MC_STR_DELETE_TRACK_MENU));
  int cmd = TrackPopupMenu(menu, menu_flags(), pt.x, pt.y, 0, s.wnd, NULL);
  DestroyMenu(menu); /* and the submenu */

  memset(&s.req, 0, sizeof s.req);
  if (cmd >= CMD_PLAYLIST_FIRST && lists && cmd - CMD_PLAYLIST_FIRST < lists->count) {
    strcpy(s.req.track_uid, s.rows[i].uid);
    strcpy(s.req.playlist_uid, lists->items[cmd - CMD_PLAYLIST_FIRST].uid);
    notify(TLN_ADD_TO_PLAYLIST);
  }
  mc_playlist_list_free(lists);
  switch (cmd) {
    case CMD_NEW_PLAYLIST:
      strcpy(s.req.track_uid, s.rows[i].uid);
      notify(TLN_ADD_TO_PLAYLIST);
      break;
    case CMD_MOVE_UP: request_move(i, i - 1); break;
    case CMD_MOVE_DOWN: request_move(i, i + 2); break;
    case CMD_REMOVE: request_remove(i); break;
    case CMD_DELETE_TRACK:
      strcpy(s.req.track_uid, s.rows[i].uid);
      notify(TLN_DELETE_TRACK);
      break;
    case CMD_PLAY_NEXT: /* not yet: needs a play queue */
    case CMD_PLAY_LAST:
      break;
  }
}

/* --- Dragging a playlist's tracks into another order --- */

/* The place (0 to count) nearest y: between the rows either side of it. */
static int drop_index(int y) {
  int rel = y - s.scroll.view.top - scale(INSET) + s.scroll.pos + scale(ROW_H) / 2;
  int i = rel > 0 ? rel / scale(ROW_H) : 0;
  return i < s.count ? i : s.count;
}

/* The line marking place i. */
static RECT drop_rect(int i) {
  int y = i < s.count ? row_rect(i).top : row_rect(s.count - 1).bottom;
  RECT r = {scale(INSET), y - scale(DROP_LINE) / 2 - 1, s.scroll.view.right - scale(INSET),
            y + scale(DROP_LINE) / 2 + 1};
  return r;
}

static void set_drop(int i) {
  if (i == s.drop_at) return;
  if (s.drop_at >= 0) {
    RECT r = drop_rect(s.drop_at);
    InvalidateRect(s.wnd, &r, FALSE);
  }
  s.drop_at = i;
  if (i >= 0) {
    RECT r = drop_rect(i);
    InvalidateRect(s.wnd, &r, FALSE);
  }
}

/* Near the top or bottom of the view, the list scrolls while dragging: how far per tick. */
static int drag_scroll_step(int y) {
  int edge = scale(ROW_H), step = scale(ROW_H) / 3;
  if (y < s.scroll.view.top + edge) return -step;
  if (y > s.scroll.view.bottom - edge) return step;
  return 0;
}

static void drag_to(int x, int y) {
  if (!s.dragging) {
    if (abs(x - s.drag_from.x) < GetSystemMetrics(SM_CXDRAG) && abs(y - s.drag_from.y) < GetSystemMetrics(SM_CYDRAG))
      return;
    s.dragging = TRUE;
    set_hot(-1);
    invalidate_row(s.drag_row);
  }
  set_drop(drop_index(y));
  if (drag_scroll_step(y)) SetTimer(s.wnd, TIMER_DRAG_SCROLL, 30, NULL);
}

static void on_drag_timer(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(s.wnd, &pt);
  int step = s.dragging ? drag_scroll_step(pt.y) : 0;
  if (!step) {
    KillTimer(s.wnd, TIMER_DRAG_SCROLL);
    return;
  }
  set_drop(-1); /* it moves with the rows */
  scroller_jump(&s.scroll, s.scroll.pos + step);
  set_drop(drop_index(pt.y));
}

/* Stops dragging, without moving anything. */
static void end_drag(void) {
  BOOL was = s.drag_row >= 0;
  int row_was = s.drag_row;
  s.drag_row = -1;
  s.dragging = FALSE;
  set_drop(-1);
  if (s.wnd) KillTimer(s.wnd, TIMER_DRAG_SCROLL);
  if (was) {
    invalidate_row(row_was);
    if (GetCapture() == s.wnd) ReleaseCapture();
  }
}

/* The mouse button came up: moves the track if it was dragged somewhere else. */
static void drop(void) {
  int from = s.drag_row, to = s.drop_at;
  BOOL moved = s.dragging;
  end_drag();
  if (moved) request_move(from, to);
}

/* --- Painting --- */

static void paint_row(canvas *c, int i, BOOL focused) {
  const row *r = &s.rows[i];
  RECT fill = row_rect(i);
  BOOL playing = s.playing_uid[0] && !strcmp(r->uid, s.playing_uid);
  COLORREF text = theme.text, dim = theme.text2;

  if (playing && theme.high_contrast) {
    canvas_fill(c, &fill, scale(RADIUS), theme.accent, 255);
    text = dim = theme.on_accent;
  } else {
    control_state state = s.dragging && i == s.drag_row ? STATE_PRESSED
                          : i == s.selected            ? STATE_SELECTED
                          : i == s.hot                 ? STATE_HOT
                                                       : STATE_NORMAL;
    text = theme_draw_state(c, &fill, scale(RADIUS), state);
    if (state == STATE_SELECTED && theme.high_contrast) dim = text;
    if (playing) {
      canvas_fill(c, &fill, scale(RADIUS), theme.accent, theme.dark ? 40 : 30);
      text = dim = theme.accent;
    }
  }
  if (focused && i == s.selected) canvas_frame(c, &fill, scale(RADIUS), scale(2), theme.text, 255);

  UINT one_line = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
  RECT num = {fill.left + scale(CELL_X), fill.top, fill.left + scale(CELL_X + NUM_W), fill.bottom};
  RECT dur = {fill.right - scale(CELL_X + DUR_W), fill.top, fill.right - scale(CELL_X), fill.bottom};
  RECT title = {num.right + scale(NUM_GAP), fill.top, dur.left - scale(NUM_GAP), fill.bottom};
  if (playing) { /* a speaker in place of the number */
    SelectObject(c->dc, s.icon_font);
    SetTextColor(c->dc, text);
    DrawTextW(c->dc, L"\xE767", 1, &num, one_line | DT_RIGHT);
  } else {
    SelectObject(c->dc, s.font);
    SetTextColor(c->dc, dim);
    DrawTextW(c->dc, r->num, -1, &num, one_line | DT_RIGHT);
  }
  SelectObject(c->dc, s.font);
  SetTextColor(c->dc, dim);
  DrawTextW(c->dc, r->duration, -1, &dur, one_line | DT_RIGHT);
  if (s.by_artist && r->album) { /* the tracks come from many albums: each one's, beside it */
    RECT album = title;
    title.right = album.left = title.left + (title.right - title.left) * 3 / 5;
    album.left += scale(NUM_GAP);
    SetTextColor(c->dc, r->path || playing ? dim : theme.text3);
    DrawTextW(c->dc, r->album, -1, &album, one_line | DT_END_ELLIPSIS);
  }
  SetTextColor(c->dc, r->path || playing ? text : theme.text3); /* greyed out: not on this device */
  DrawTextW(c->dc, r->title, -1, &title, one_line | DT_END_ELLIPSIS);
}

/* The playlist menu's button: round, like the close button beside it. */
static void paint_more(canvas *c) {
  RECT r = more_rect();
  control_state state = s.more_open ? STATE_PRESSED : s.more_hot ? STATE_HOT : STATE_NORMAL;
  COLORREF color = theme_draw_state(c, &r, scale(CLOSE) / 2, state);
  SelectObject(c->dc, s.icon_font);
  SetTextColor(c->dc, color);
  const wchar_t *glyph = s.smart ? L"\xE72C" : L"\xE712"; /* Refresh (for Reshuffle), More */
  DrawTextW(c->dc, glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

/* Play, an accent button as Windows 11's main action is, and Shuffle, a standard one beside
 * it: an icon and a label, centred together. */
static void paint_action(canvas *c, int i) {
  RECT r = action_rect(i);
  if (r.right <= r.left) return;
  int radius = scale(RADIUS);
  BOOL enabled = playable();
  control_state state = !enabled ? STATE_NORMAL
                        : s.action_pressed == i && s.action_hot == i ? STATE_PRESSED
                        : s.action_hot == i || s.action_pressed == i ? STATE_HOT
                                                                     : STATE_NORMAL;
  COLORREF ink = theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), fg;
  if (!enabled) {
    canvas_fill(c, &r, radius, ink, theme.high_contrast ? 0 : 15);
    if (theme.high_contrast) canvas_frame(c, &r, radius, 1, theme.text3, 255);
    fg = theme.text3;
  } else if (i == 0) {
    int alpha = theme.high_contrast ? 255 : state == STATE_PRESSED ? 205 : state == STATE_HOT ? 230 : 255;
    canvas_fill(c, &r, radius, theme.accent, alpha);
    if (theme.high_contrast && state != STATE_NORMAL) canvas_frame(c, &r, radius, scale(2), theme.text, 255);
    fg = theme.on_accent;
  } else {
    if (!theme.high_contrast) canvas_fill(c, &r, radius, ink, theme.dark ? 15 : 6);
    fg = theme_draw_state(c, &r, radius, state);
    canvas_frame(c, &r, radius, 1, theme.high_contrast ? theme.text : theme.line, 255);
  }

  const wchar_t *glyph = i ? L"\xE8B1" : L"\xE768"; /* Shuffle, Play */
  const wchar_t *label = mc_wstr(i ? MC_STR_SHUFFLE : MC_STR_PLAY);
  SIZE icon = {0}, text = {0};
  SelectObject(c->dc, s.icon_font);
  GetTextExtentPoint32W(c->dc, glyph, 1, &icon);
  SelectObject(c->dc, s.font);
  GetTextExtentPoint32W(c->dc, label, (int)wcslen(label), &text);
  int gap = scale(8), x = r.left + max(0, (int)(r.right - r.left - icon.cx - gap - text.cx) / 2);
  UINT one_line = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
  RECT at = {x, r.top, x + icon.cx, r.bottom};
  SetTextColor(c->dc, fg);
  SelectObject(c->dc, s.icon_font);
  DrawTextW(c->dc, glyph, 1, &at, one_line);
  SetRect(&at, at.right + gap, r.top, r.right - scale(4), r.bottom);
  SelectObject(c->dc, s.font);
  DrawTextW(c->dc, label, -1, &at, one_line | DT_END_ELLIPSIS);
}

/* The cover, the title (up to two lines), artist and summary beside it, the playlist menu's
 * button for a playlist, the close button, and Play and Shuffle under them. */
static void paint_header(canvas *c, int w) {
  RECT cover = {scale(PAD_X), scale(PAD_TOP), scale(PAD_X + COVER), scale(PAD_TOP + COVER)};
  if (s.art_px) {
    canvas_draw_image(c, &cover, s.art_px, s.art_size, s.art_size);
  } else { /* the album grid's placeholder */
    canvas_fill(c, &cover, scale(COVER_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
    SelectObject(c->dc, s.glyph_font);
    SetTextColor(c->dc, theme.text3);
    /* As the album grid's: a smart playlist's bolt, the sidebar's Playlists icon, or a note. */
    const wchar_t *glyph = s.smart ? L"\xE945" : s.playlist ? L"\xE90B" : L"\xE8D6";
    DrawTextW(c->dc, glyph, 1, &cover, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }

  RECT close = close_rect();
  control_state state = s.close_pressed && s.close_hot ? STATE_PRESSED
                        : s.close_hot || s.close_pressed ? STATE_HOT
                                                         : STATE_NORMAL;
  COLORREF close_color = theme_draw_state(c, &close, scale(CLOSE) / 2, state);
  SelectObject(c->dc, s.icon_font);
  SetTextColor(c->dc, close_color);
  DrawTextW(c->dc, L"\xE711", 1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* Cancel */
  if (has_more()) paint_more(c);

  /* The text, centred against the cover. The title wraps onto a second line if it must. */
  int buttons = has_more() ? more_rect().left : close.left;
  int left = cover.right + scale(COVER_GAP), right = max(left, buttons - scale(4));
  int title_line = font_height(s.title_font), artist_h = font_height(s.font), sub_h = font_height(s.sub_font);
  UINT wrap = DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX;
  RECT title = {left, 0, right, 0};
  SelectObject(c->dc, s.title_font);
  if (s.album_title) DrawTextW(c->dc, s.album_title, -1, &title, wrap | DT_CALCRECT);
  int title_h = min(max(title.bottom, title_line), 2 * title_line);
  int block = title_h + scale(LINE_GAP) + artist_h + scale(LINE_GAP) + sub_h;
  int top = cover.top + max(0, (scale(COVER) - block) / 2);
  SetRect(&title, left, top, right, top + title_h);
  SetTextColor(c->dc, theme.text);
  if (s.album_title) DrawTextW(c->dc, s.album_title, -1, &title, wrap);
  UINT one_line = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
  RECT artist = {left, title.bottom + scale(LINE_GAP), right, title.bottom + scale(LINE_GAP) + artist_h};
  SelectObject(c->dc, s.font);
  const wchar_t *byline = s.smart      ? mc_wstr(MC_STR_SMART_PLAYLIST)
                          : s.playlist ? mc_wstr(MC_STR_PLAYLIST)
                          : s.artist   ? s.artist
                                       : mc_wstr(MC_STR_UNKNOWN_ARTIST);
  DrawTextW(c->dc, byline, -1, &artist, one_line);
  RECT sub = {left, artist.bottom + scale(LINE_GAP), right, artist.bottom + scale(LINE_GAP) + sub_h};
  SelectObject(c->dc, s.sub_font);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, s.summary, -1, &sub, one_line);
  paint_action(c, 0);
  paint_action(c, 1);

  RECT divider = {0, s.header_h, w, s.header_h + 1};
  canvas_clear(c, &divider, theme.line, 255);
}

/* Like the album grid, everything goes into a canvas; GDI text needs its alpha set after. */
static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, s.font);

  /* Rows. A row half scrolled under the header spills into it, so the header goes on top. */
  HRGN clip = CreateRectRgnIndirect(&s.scroll.view);
  SelectClipRgn(c->dc, clip);
  BOOL focused = GetFocus() == s.wnd && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
  for (int i = 0; i < s.count; i++) {
    RECT r = row_rect(i), isect;
    if (IntersectRect(&isect, &r, dirty) && IntersectRect(&isect, &r, &s.scroll.view)) paint_row(c, i, focused);
  }
  SelectClipRgn(c->dc, NULL);
  DeleteObject(clip);

  /* Header. */
  RECT header = {0, 0, client.right, s.scroll.view.top};
  canvas_clear(c, &header, theme.bg, 255);
  if (!s.by_artist) {
    paint_header(c, client.right);
  } else {
    paint_action(c, 0);
    paint_action(c, 1);
  }
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  if (s.dragging && s.drop_at >= 0 && s.count) { /* where the dragged track would go */
    RECT line = drop_rect(s.drop_at), isect;
    line.top += 1;
    line.bottom -= 1;
    if (IntersectRect(&isect, &line, &s.scroll.view)) canvas_fill(c, &isect, 0, theme.accent, 255);
  }

  scroller_paint(&s.scroll, c);
  RECT top = {0, 0, client.right, 1}; /* continues the album grid's top border */
  RECT left = {0, 0, 1, client.bottom}; /* on the artist page, beside the sidebar: its left border */
  canvas_clear(c, s.by_artist ? &left : &top, theme.line, 255);
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Window --- */

static void on_key(WPARAM key) {
  /* Using the keyboard shows the focus rectangle. */
  SendMessageW(GetAncestor(s.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  int page = (s.scroll.view.bottom - s.scroll.view.top) / scale(ROW_H);
  int cur = s.selected;
  switch (key) {
    case VK_UP: select_row(cur > 0 ? cur - 1 : 0); break;
    case VK_DOWN: select_row(cur < s.count - 1 ? cur + 1 : s.count - 1); break;
    case VK_PRIOR: select_row(cur - page > 0 ? cur - page : 0); break;
    case VK_NEXT: select_row(cur + page < s.count ? cur + page : s.count - 1); break;
    case VK_HOME: select_row(0); break;
    case VK_END: select_row(s.count - 1); break;
    case VK_RETURN: play(s.selected); break;
    case VK_DELETE: request_remove(s.selected); break;
    case VK_F2: request_playlist(TLN_RENAME_PLAYLIST); break;
    case VK_ESCAPE: /* cancels a drag, else closes the pane */
      if (s.drag_row >= 0) end_drag();
      else notify(TLN_CLOSE);
      break;
  }
}

static LRESULT CALLBACK track_list_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!s.wnd) s.wnd = s.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  switch (msg) {
    case WM_SIZE:
      relayout();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_MOUSEWHEEL:
      scroller_wheel(&s.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_TIMER:
      if (wp == TIMER_DRAG_SCROLL) on_drag_timer();
      else scroller_timer(&s.scroll, wp);
      return 0;
    case WM_MOUSEMOVE: {
      if (!s.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        s.tracking = TRUE;
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (s.drag_row >= 0) {
        drag_to(x, y);
        return 0;
      }
      set_close_hot(in_close(x, y));
      set_more_hot(on_more(x, y));
      set_action_hot(action_at(x, y));
      set_hot(scroller_mouse_move(&s.scroll, x, y) ? -1 : row_at(x, y));
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      scroller_mouse_leave(&s.scroll);
      set_hot(-1);
      set_more_hot(FALSE);
      if (!s.close_pressed) set_close_hot(FALSE);
      if (s.action_pressed < 0) set_action_hot(-1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp), action = action_at(x, y);
      if (msg != WM_RBUTTONDOWN && action >= 0) { /* Play or Shuffle: acts on release */
        s.action_pressed = action;
        SetCapture(wnd);
        invalidate_actions();
        return 0;
      }
      if (msg == WM_LBUTTONDBLCLK) {
        int i = row_at(x, y);
        if (i >= 0) play(i);
        return 0;
      }
      if (msg == WM_LBUTTONDOWN && in_close(x, y)) { /* acts on release, like a button */
        s.close_pressed = TRUE;
        SetCapture(wnd);
        RECT r = close_rect();
        InvalidateRect(wnd, &r, FALSE);
        return 0;
      }
      if (on_more(x, y)) {
        if (msg == WM_LBUTTONDOWN && s.smart) request_playlist(TLN_RESHUFFLE);
        else if (msg == WM_LBUTTONDOWN) show_playlist_menu();
        return 0;
      }
      SetFocus(wnd);
      if (msg == WM_RBUTTONDOWN || !scroller_mouse_down(&s.scroll, x, y)) {
        int i = row_at(x, y);
        select_row(i);
        if (msg == WM_LBUTTONDOWN && in_playlist(i)) { /* maybe the start of a drag */
          s.drag_row = i;
          s.drag_from = (POINT){x, y};
          SetCapture(wnd);
        }
      }
      return 0;
    }
    case WM_LBUTTONUP:
      if (s.action_pressed >= 0) {
        int action = s.action_pressed;
        BOOL clicked = action_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == action;
        ReleaseCapture(); /* WM_CAPTURECHANGED clears action_pressed */
        if (clicked) notify(action ? TLN_SHUFFLE_ALL : TLN_PLAY_ALL);
        return 0;
      }
      if (s.close_pressed) {
        BOOL clicked = in_close(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        ReleaseCapture(); /* WM_CAPTURECHANGED clears close_pressed */
        if (clicked) notify(TLN_CLOSE);
        return 0;
      }
      if (s.drag_row >= 0) drop();
      scroller_mouse_up(&s.scroll);
      return 0;
    case WM_CAPTURECHANGED:
      if (s.drag_row >= 0 && (HWND)lp != wnd) end_drag(); /* lost it: e.g. a window came up */
      if (s.close_pressed) {
        s.close_pressed = FALSE;
        RECT r = close_rect();
        InvalidateRect(wnd, &r, FALSE);
      }
      if (s.action_pressed >= 0) {
        s.action_pressed = -1;
        invalidate_actions();
      }
      scroller_capture_changed(&s.scroll);
      return 0;
    case WM_CONTEXTMENU: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (lp == -1) { /* from the keyboard: under the selected track, or the playlist menu */
        if (s.selected < 0) {
          show_playlist_menu();
          return 0;
        }
        RECT r = row_rect(s.selected);
        pt.x = r.left + scale(CELL_X);
        pt.y = r.bottom;
        ClientToScreen(wnd, &pt);
      } else {
        POINT client = pt;
        ScreenToClient(wnd, &client);
        if (client.y < s.scroll.view.top) { /* the header */
          show_playlist_menu();
          return 0;
        }
        if (row_at(client.x, client.y) < 0) return 0;
      }
      show_menu(pt);
      return 0;
    }
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_SYSKEYDOWN: /* Alt+Up and Alt+Down move a playlist's track */
      if ((wp == VK_UP || wp == VK_DOWN) && in_playlist(s.selected)) {
        SendMessageW(GetAncestor(wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        request_move(s.selected, wp == VK_UP ? s.selected - 1 : s.selected + 2);
        return 0;
      }
      break;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      invalidate_row(s.selected);
      break;
    case WM_CREATE: {
      s.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, wnd, NULL,
                                  GetModuleHandleW(NULL), NULL);
      TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = wnd, .uId = 1,
                        .lpszText = (wchar_t *)mc_wstr(MC_STR_CLOSE)};
      SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      ti.uId = 2; /* a random smart playlist's Reshuffle; placed by relayout() */
      ti.lpszText = (wchar_t *)mc_wstr(MC_STR_RESHUFFLE);
      SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
      return 0;
    }
    case WM_DESTROY:
      clear_rows();
      free(s.album_title);
      free(s.artist);
      free(s.cover_dir);
      free_art();
      if (s.wic) IWICImagingFactory_Release(s.wic);
      if (s.tooltip) DestroyWindow(s.tooltip);
      canvas_free(&s.buf);
      HFONT fonts[] = {s.font, s.title_font, s.sub_font, s.icon_font, s.glyph_font};
      for (int i = 0; i < 5; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
      memset(&s, 0, sizeof s);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND track_list_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DBLCLKS, .lpfnWndProc = track_list_proc,
                    .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"kithsong_tracklist"};
  RegisterClassExW(&wc);

  memset(&s, 0, sizeof s);
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  s.hot = s.selected = s.drag_row = s.drop_at = s.action_hot = s.action_pressed = -1;
  s.scroll.moved = on_scrolled;
  return CreateWindowExW(0, L"kithsong_tracklist", NULL, WS_CHILD, 0, 0, 0, 0, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

void track_list_update(UINT dpi) {
  s.dpi = dpi;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  HFONT old[] = {s.font, s.title_font, s.sub_font, s.icon_font, s.glyph_font};

  LOGFONTW lf = ncm.lfMessageFont;
  s.sub_font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 7, 6); /* 9pt -> 10.5pt, as in the sidebar */
  s.font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 5, 3); /* 15pt */
  lf.lfWeight = FW_SEMIBOLD;
  s.title_font = CreateFontIndirectW(&lf);
  s.icon_font = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());
  s.glyph_font = CreateFontW(-scale(COVER) / 4, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             theme_icon_face());
  for (int i = 0; i < 5; i++)
    if (old[i]) DeleteObject(old[i]);
  if (s.tooltip) theme_apply_control(s.tooltip);

  if (s.wnd) {
    if (s.art_size != scale(COVER)) load_art();
    relayout();
    InvalidateRect(s.wnd, NULL, FALSE);
  }
}
