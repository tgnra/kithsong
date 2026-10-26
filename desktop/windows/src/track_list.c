#define COBJMACROS
#include "track_list.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "cover.h"
#include "scroller.h"
#include "theme.h"

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
  HEADER_BOTTOM = 18,
  ROW_H = 36,
  INSET = 8,   /* rows' fill from the edges */
  CELL_X = 12, /* text inside the fill */
  NUM_W = 28,  /* track number column */
  NUM_GAP = 12,
  DUR_W = 52,
  RADIUS = 4,
};

/* Context menu items. Stubs until playlists and a play queue exist. */
enum { CMD_ADD_TO_PLAYLIST = 1, CMD_PLAY_NEXT, CMD_PLAY_LAST };

typedef struct row {
  char uid[MC_UID_STRLEN];
  wchar_t *title;
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
  wchar_t *album_title;
  wchar_t *artist;     /* NULL if unknown */
  int year;            /* 0 if unknown */
  int total_ms;        /* the tracks' length */
  wchar_t summary[64]; /* "1997 · 12 tracks · 47 min" */
  wchar_t *cover_dir;  /* NULL if the album has no cover */
  HBITMAP art;         /* the cover, premultiplied, with rounded corners; NULL shows the placeholder */
  const uint32_t *art_px;
  int art_size;
  row *rows;
  int count;
  BOOL close_hot, close_pressed;

  int header_h; /* the fixed part above the rows */
  int hot, selected;
  BOOL tracking;
  int play_index; /* the track TLN_PLAY was last sent for */
  char playing_uid[MC_UID_STRLEN];
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
  RECT r = close_rect();
  POINT pt = {x, y};
  return PtInRect(&r, pt);
}

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int text_h = 2 * font_height(s.title_font) + font_height(s.font) + font_height(s.sub_font) + 2 * scale(LINE_GAP);
  s.header_h = scale(PAD_TOP) + max(scale(COVER), text_h) + scale(HEADER_BOTTOM);
  RECT view = {0, s.header_h + 1, rc.right, rc.bottom}; /* below the header's divider */
  scroller_resize(&s.scroll, &view, s.count * scale(ROW_H) + 2 * scale(INSET));
  if (s.tooltip) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = s.wnd, .uId = 1, .rect = close_rect()};
    SendMessageW(s.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  }
}

/* Row i's fill, in client coordinates. */
static RECT row_rect(int i) {
  int top = s.scroll.view.top + scale(INSET) + i * scale(ROW_H) - s.scroll.pos;
  RECT r = {scale(INSET), top, s.scroll.view.right - scale(INSET), top + scale(ROW_H)};
  return r;
}

static void invalidate_row(int i) {
  if (i < 0 || i >= s.count) return;
  RECT r = row_rect(i), isect;
  if (IntersectRect(&isect, &r, &s.scroll.view)) InvalidateRect(s.wnd, &isect, FALSE);
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

static void set_hot(int i) {
  if (i == s.hot) return;
  invalidate_row(s.hot);
  invalidate_row(i);
  s.hot = i;
}

static void select_row(int i) {
  if (i < 0 || i >= s.count) return;
  invalidate_row(s.selected);
  invalidate_row(i);
  s.selected = i;
  int top = scale(INSET) + i * scale(ROW_H);
  scroller_reveal(&s.scroll, top - scale(INSET), top + scale(ROW_H) + scale(INSET));
}

static void on_scrolled(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(s.wnd, &pt);
  set_hot(s.tracking && !s.scroll.hot && !s.scroll.dragging ? row_at(pt.x, pt.y) : -1);
}

/* --- Data --- */

static void clear_rows(void) {
  for (int i = 0; i < s.count; i++) {
    free(s.rows[i].title);
    free(s.rows[i].path);
  }
  free(s.rows);
  s.rows = NULL;
  s.count = 0;
}

/* "1997 · 12 tracks · 47 min" */
static void make_summary(void) {
  wchar_t year[16] = L"";
  if (s.year > 0) swprintf(year, 16, L"%d \x00B7 ", s.year);
  int n = s.count, min = (s.total_ms + 30000) / 60000;
  const wchar_t *tracks = n == 1 ? L"track" : L"tracks";
  if (!n) swprintf(s.summary, 64, L"%lsNo tracks", year);
  else if (min >= 60) swprintf(s.summary, 64, L"%ls%d %ls \x00B7 %d hr %d min", year, n, tracks, min / 60, min % 60);
  else swprintf(s.summary, 64, L"%ls%d %ls \x00B7 %d min", year, n, tracks, min);
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
  if (s.wic) s.art = cover_decode(s.wic, s.cover_dir, s.art_size, scale(COVER_RADIUS), &s.art_px);
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
  }
  if (s.wnd) InvalidateRect(s.wnd, NULL, FALSE);
}

void track_list_show(mc_library *lib, const char *album_uid, const wchar_t *title) {
  BOOL same = album_uid && !strcmp(album_uid, s.album_uid);
  char keep[MC_UID_STRLEN] = "";
  if (same && s.selected >= 0 && s.selected < s.count) strcpy(keep, s.rows[s.selected].uid);

  clear_rows();
  free(s.album_title);
  s.album_title = title ? _wcsdup(title) : NULL;
  s.lib = lib;
  s.album_uid[0] = '\0';
  s.hot = s.selected = -1;
  if (!same) s.scroll.pos = s.scroll.target = 0;

  mc_track_list *list = NULL;
  if (lib && album_uid && strlen(album_uid) < MC_UID_STRLEN) {
    strcpy(s.album_uid, album_uid);
    mc_library_list_album_tracks(lib, album_uid, &list);
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
    r->title = widen(t->title && *t->title ? t->title : "Untitled");
    if (t->track_no && discs) swprintf(r->num, 16, L"%d-%d", t->disc_no ? t->disc_no : 1, t->track_no);
    else if (t->track_no) swprintf(r->num, 16, L"%d", t->track_no);
    format_duration(t->duration_ms, r->duration, 16);
    r->duration_ms = t->duration_ms;
    r->path = t->path ? _strdup(t->path) : NULL;
    total_ms += t->duration_ms > 0 ? t->duration_ms : 0;
    if (keep[0] && !strcmp(r->uid, keep)) s.selected = i;
  }
  s.count = n;
  mc_track_list_free(list);

  s.total_ms = total_ms;
  if (!same) s.year = 0; /* until track_list_set_details */
  make_summary();

  relayout();
  InvalidateRect(s.wnd, NULL, FALSE);
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
  item.duration_ms = r->duration_ms;
  item.path = r->path;
  return item;
}

int track_list_play_index(void) {
  return s.play_index;
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

/* The menu for the selected track, at pt (screen coordinates). */
static void show_menu(POINT pt) {
  if (s.selected < 0) return;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, CMD_ADD_TO_PLAYLIST, L"Add to Playlist");
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_PLAY_NEXT, L"Play Next");
  AppendMenuW(menu, MF_STRING, CMD_PLAY_LAST, L"Play Last");
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
  int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, s.wnd, NULL);
  DestroyMenu(menu);
  switch (cmd) {
    case CMD_ADD_TO_PLAYLIST: /* not yet: needs playlists */
    case CMD_PLAY_NEXT:       /* not yet: needs a play queue */
    case CMD_PLAY_LAST:
      break;
  }
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
    control_state state = i == s.selected ? STATE_SELECTED : i == s.hot ? STATE_HOT : STATE_NORMAL;
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
  SetTextColor(c->dc, r->path || playing ? text : theme.text3); /* greyed out: not on this device */
  DrawTextW(c->dc, r->title, -1, &title, one_line | DT_END_ELLIPSIS);
}

/* The cover, the title (up to two lines), artist and summary beside it, and the close button. */
static void paint_header(canvas *c, int w) {
  RECT cover = {scale(PAD_X), scale(PAD_TOP), scale(PAD_X + COVER), scale(PAD_TOP + COVER)};
  if (s.art_px) {
    canvas_draw_image(c, &cover, s.art_px, s.art_size, s.art_size);
  } else { /* the album grid's placeholder */
    canvas_fill(c, &cover, scale(COVER_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
    SelectObject(c->dc, s.glyph_font);
    SetTextColor(c->dc, theme.text3);
    DrawTextW(c->dc, L"\xE8D6", 1, &cover, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* music note */
  }

  RECT close = close_rect();
  control_state state = s.close_pressed && s.close_hot ? STATE_PRESSED
                        : s.close_hot || s.close_pressed ? STATE_HOT
                                                         : STATE_NORMAL;
  COLORREF close_color = theme_draw_state(c, &close, scale(CLOSE) / 2, state);
  SelectObject(c->dc, s.icon_font);
  SetTextColor(c->dc, close_color);
  DrawTextW(c->dc, L"\xE711", 1, &close, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX); /* Cancel */

  /* The text, centred against the cover. The title wraps onto a second line if it must. */
  int left = cover.right + scale(COVER_GAP), right = max(left, close.left - scale(4));
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
  DrawTextW(c->dc, s.artist ? s.artist : L"Unknown Artist", -1, &artist, one_line);
  RECT sub = {left, artist.bottom + scale(LINE_GAP), right, artist.bottom + scale(LINE_GAP) + sub_h};
  SelectObject(c->dc, s.sub_font);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, s.summary, -1, &sub, one_line);

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
  paint_header(c, client.right);
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  scroller_paint(&s.scroll, c);
  RECT top = {0, 0, client.right, 1}; /* continues the album grid's top border */
  canvas_clear(c, &top, theme.line, 255);
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
    case VK_ESCAPE: notify(TLN_CLOSE); break;
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
      scroller_timer(&s.scroll, wp);
      return 0;
    case WM_MOUSEMOVE: {
      if (!s.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        s.tracking = TRUE;
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      set_close_hot(in_close(x, y));
      set_hot(scroller_mouse_move(&s.scroll, x, y) ? -1 : row_at(x, y));
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      scroller_mouse_leave(&s.scroll);
      set_hot(-1);
      if (!s.close_pressed) set_close_hot(FALSE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (msg == WM_LBUTTONDOWN && in_close(x, y)) { /* acts on release, like a button */
        s.close_pressed = TRUE;
        SetCapture(wnd);
        RECT r = close_rect();
        InvalidateRect(wnd, &r, FALSE);
        return 0;
      }
      SetFocus(wnd);
      if (msg == WM_RBUTTONDOWN || !scroller_mouse_down(&s.scroll, x, y)) select_row(row_at(x, y));
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      int i = row_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (i >= 0) play(i);
      return 0;
    }
    case WM_LBUTTONUP:
      if (s.close_pressed) {
        BOOL clicked = in_close(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        ReleaseCapture(); /* WM_CAPTURECHANGED clears close_pressed */
        if (clicked) notify(TLN_CLOSE);
        return 0;
      }
      scroller_mouse_up(&s.scroll);
      return 0;
    case WM_CAPTURECHANGED:
      if (s.close_pressed) {
        s.close_pressed = FALSE;
        RECT r = close_rect();
        InvalidateRect(wnd, &r, FALSE);
      }
      scroller_capture_changed(&s.scroll);
      return 0;
    case WM_CONTEXTMENU: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (lp == -1) { /* from the keyboard: under the selected track */
        if (s.selected < 0) return 0;
        RECT r = row_rect(s.selected);
        pt.x = r.left + scale(CELL_X);
        pt.y = r.bottom;
        ClientToScreen(wnd, &pt);
      } else {
        POINT client = pt;
        ScreenToClient(wnd, &client);
        if (row_at(client.x, client.y) < 0) return 0;
      }
      show_menu(pt);
      return 0;
    }
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
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
      TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = wnd, .uId = 1, .lpszText = L"Close"};
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
                    .lpszClassName = L"musicapp_tracklist"};
  RegisterClassExW(&wc);

  memset(&s, 0, sizeof s);
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  s.hot = s.selected = -1;
  s.scroll.moved = on_scrolled;
  return CreateWindowExW(0, L"musicapp_tracklist", NULL, WS_CHILD, 0, 0, 0, 0, parent,
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
