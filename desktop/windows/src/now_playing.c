#define COBJMACROS
#include "now_playing.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "cover.h"
#include "theme.h"

/* Metrics at 96 DPI. */
enum {
  ART = 36, /* the cover's square, as tall as the playback buttons */
  ART_RADIUS = 4,
  ART_GAP = 12,  /* between the cover and the first time */
  TIME_GAP = 10, /* between each time and the bar */
  TITLE_GAP = 2, /* between the title and the bar's row */
  RAIL_H = 4,
  THUMB = 14,       /* the thumb's outer circle, which also sets the bar's row height */
  KNOB = 8,         /* the accent dot inside it */
  KNOB_HOT = 10,    /* ...under the mouse */
  KNOB_PRESSED = 6, /* ...while dragging */
  HIT_PAD = 5,      /* the bar takes clicks this far above and below its row */
  KEY_STEP_MS = 5000,
};

static struct {
  HWND wnd;
  UINT dpi;
  HFONT font, icon_font;
  canvas buf;
  IWICImagingFactory *wic;

  wchar_t *title;     /* NULL when nothing is playing */
  wchar_t *cover_dir; /* NULL when the album has no cover */
  HBITMAP art;        /* premultiplied, with rounded corners; NULL shows the placeholder */
  const uint32_t *art_px;
  int art_size;
  int duration_ms, pos_ms;

  /* Layout, in device pixels. */
  RECT art_rect, title_rect, cur_rect, left_rect, bar; /* bar: the row the rail and thumb sit in */
  BOOL hot, dragging, tracking;
} s;

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

static void format_time(int ms, wchar_t *out, size_t n) {
  int sec = ms > 0 ? ms / 1000 : 0;
  if (sec >= 3600) swprintf(out, n, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else swprintf(out, n, L"%d:%02d", sec / 60, sec % 60);
}

/* "1:02" and "-5:21". Whole seconds, so the two always add up to the length. With no
 * length, both are "0:00". */
static void format_times(wchar_t *cur, wchar_t *left, size_t n) {
  int total = (s.duration_ms + 500) / 1000 * 1000, pos = s.pos_ms / 1000 * 1000;
  format_time(pos, cur, n);
  if (total > 0) left[0] = L'-';
  format_time(total - pos, left + (total > 0), n - 1);
}

static BOOL seekable(void) {
  return s.title && s.duration_ms > 0;
}

/* --- Layout --- */

static int text_width(HDC dc, const wchar_t *t) {
  SIZE sz = {0};
  GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
  return sz.cx;
}

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  HDC dc = GetDC(s.wnd);
  HGDIOBJ old = SelectObject(dc, s.font);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  /* Both times get room for the longest the track can show, so the bar doesn't move. */
  wchar_t widest[24] = L"-", zero[] = L"-0:00";
  format_time(s.duration_ms, widest + 1, 23);
  int time_w = max(text_width(dc, widest), text_width(dc, zero));
  SelectObject(dc, old);
  ReleaseDC(s.wnd, dc);

  int art = scale(ART);
  SetRect(&s.art_rect, 0, (rc.bottom - art) / 2, art, (rc.bottom - art) / 2 + art);
  int row_h = scale(THUMB), top = (rc.bottom - (tm.tmHeight + scale(TITLE_GAP) + row_h)) / 2;
  int row_top = top + tm.tmHeight + scale(TITLE_GAP);
  int cur_left = art + scale(ART_GAP), left_right = rc.right;
  SetRect(&s.cur_rect, cur_left, row_top, cur_left + time_w, row_top + row_h);
  SetRect(&s.left_rect, left_right - time_w, row_top, left_right, row_top + row_h);
  SetRect(&s.bar, s.cur_rect.right + scale(TIME_GAP), row_top, s.left_rect.left - scale(TIME_GAP), row_top + row_h);
  if (s.bar.right < s.bar.left) s.bar.right = s.bar.left;
  SetRect(&s.title_rect, s.bar.left, top, s.bar.right, top + tm.tmHeight);
}

/* The thumb's centre for a position, and the position for an x: the thumb stays inside the bar. */
static int thumb_x(int ms) {
  int travel = s.bar.right - s.bar.left - scale(THUMB);
  if (travel <= 0 || s.duration_ms <= 0) return s.bar.left + scale(THUMB) / 2;
  return s.bar.left + scale(THUMB) / 2 + MulDiv(travel, min(ms, s.duration_ms), s.duration_ms);
}

static int position_at(int x) {
  int travel = s.bar.right - s.bar.left - scale(THUMB);
  if (travel <= 0) return 0;
  int along = x - s.bar.left - scale(THUMB) / 2;
  return MulDiv(max(0, min(along, travel)), s.duration_ms, travel);
}

static BOOL in_bar(int x, int y) {
  RECT hit = s.bar;
  InflateRect(&hit, 0, scale(HIT_PAD));
  POINT pt = {x, y};
  return seekable() && PtInRect(&hit, pt);
}

static void set_position(int ms, BOOL notify) {
  ms = max(0, min(ms, s.duration_ms));
  if (ms == s.pos_ms) return;
  s.pos_ms = ms;
  InvalidateRect(s.wnd, NULL, FALSE);
  if (notify) PostMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), NPN_SEEK), (LPARAM)s.wnd);
}

static void set_hot(BOOL hot) {
  if (hot == s.hot) return;
  s.hot = hot;
  InvalidateRect(s.wnd, NULL, FALSE);
}

/* --- Cover --- */

static void free_art(void) {
  if (s.art) DeleteObject(s.art);
  s.art = NULL;
  s.art_px = NULL;
}

/* One small image, so it's decoded right here rather than on a thread like the grid's. */
static void load_art(void) {
  free_art();
  if (!s.cover_dir) return;
  if (!s.wic)
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                     (void **)&s.wic);
  s.art_size = scale(ART);
  if (s.wic) s.art = cover_decode(s.wic, s.cover_dir, s.art_size, scale(ART_RADIUS), &s.art_px);
}

/* --- Painting --- */

/* Windows 11's slider: a thin rail, accent up to the thumb, and a thumb that's an accent dot
 * in a light (or in dark mode, grey) circle. */
static void paint_bar(canvas *c) {
  int rail_h = scale(RAIL_H), cy = (s.bar.top + s.bar.bottom) / 2;
  RECT rail = {s.bar.left, cy - rail_h / 2, s.bar.right, cy - rail_h / 2 + rail_h};
  COLORREF ink = theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  if (!seekable()) {
    canvas_fill(c, &rail, rail_h / 2, theme.high_contrast ? theme.text3 : ink, theme.high_contrast ? 255 : 40);
    return;
  }
  int x = thumb_x(s.pos_ms);
  canvas_fill(c, &rail, rail_h / 2, theme.high_contrast ? theme.text : ink, theme.high_contrast ? 255 : theme.dark ? 139 : 114);
  RECT done = rail;
  done.right = x;
  canvas_fill(c, &done, rail_h / 2, theme.accent, 255);

  int thumb = scale(THUMB), knob = scale(s.dragging ? KNOB_PRESSED : s.hot ? KNOB_HOT : KNOB);
  RECT outer = {x - thumb / 2, cy - thumb / 2, x - thumb / 2 + thumb, cy - thumb / 2 + thumb};
  RECT inner = {x - knob / 2, cy - knob / 2, x - knob / 2 + knob, cy - knob / 2 + knob};
  if (theme.high_contrast) {
    canvas_fill(c, &outer, thumb / 2, theme.bg, 255);
    canvas_frame(c, &outer, thumb / 2, 1, theme.text, 255);
  } else {
    canvas_fill(c, &outer, thumb / 2, theme.dark ? RGB(0x45, 0x45, 0x45) : RGB(255, 255, 255), 255);
    canvas_frame(c, &outer, thumb / 2, 1, ink, theme.dark ? 24 : 30);
  }
  canvas_fill(c, &inner, knob / 2, theme.accent, 255);
}

static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  canvas_clear(c, &client, theme.frame, theme.mica ? 0 : 255);

  if (s.art_px) {
    canvas_draw_image(c, &s.art_rect, s.art_px, s.art_size, s.art_size);
  } else { /* the album grid's placeholder */
    canvas_fill(c, &s.art_rect, scale(ART_RADIUS), theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), 15);
    canvas_text(c, L"\xE8D6", 1, &s.art_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                s.icon_font, theme.text3); /* music note */
  }

  UINT one_line = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
  wchar_t cur[24], left[24];
  format_times(cur, left, 24);
  COLORREF time_color = s.title ? theme.text2 : theme.text3;
  canvas_text(c, cur, -1, &s.cur_rect, one_line | DT_RIGHT, s.font, time_color);
  canvas_text(c, left, -1, &s.left_rect, one_line | DT_LEFT, s.font, time_color);
  if (s.title) canvas_text(c, s.title, -1, &s.title_rect, one_line | DT_CENTER | DT_END_ELLIPSIS, s.font, theme.text);
  paint_bar(c);

  if (GetFocus() == s.wnd && seekable() && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) {
    RECT ring = s.bar;
    InflateRect(&ring, scale(4), scale(3));
    canvas_frame(c, &ring, scale(4), scale(2), theme.text, 255);
  }
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Window --- */

static void on_key(WPARAM key) {
  SendMessageW(GetAncestor(s.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  if (!seekable() || s.dragging) return;
  switch (key) {
    case VK_LEFT: set_position(s.pos_ms - KEY_STEP_MS, TRUE); break;
    case VK_RIGHT: set_position(s.pos_ms + KEY_STEP_MS, TRUE); break;
    case VK_HOME: set_position(0, TRUE); break;
    case VK_END: set_position(s.duration_ms, TRUE); break;
  }
}

static LRESULT CALLBACK now_playing_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!s.wnd) s.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  switch (msg) {
    case WM_NCHITTEST: { /* only the bar is ours; the rest is title bar */
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(wnd, &pt);
      return in_bar(pt.x, pt.y) || s.dragging ? HTCLIENT : HTTRANSPARENT;
    }
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
    case WM_MOUSEMOVE: {
      if (!s.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        s.tracking = TRUE;
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (s.dragging) set_position(position_at(x), FALSE);
      set_hot(s.dragging || in_bar(x, y));
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      set_hot(s.dragging);
      return 0;
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (!in_bar(x, y)) return 0;
      SetFocus(wnd);
      SetCapture(wnd);
      s.dragging = TRUE;
      set_position(position_at(x), FALSE);
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_LBUTTONUP:
      if (s.dragging) {
        ReleaseCapture(); /* WM_CAPTURECHANGED ends the drag */
        PostMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), NPN_SEEK), (LPARAM)wnd);
      }
      return 0;
    case WM_CAPTURECHANGED:
      if (s.dragging) {
        s.dragging = FALSE;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(wnd, &pt);
        s.hot = s.tracking && in_bar(pt.x, pt.y);
        InvalidateRect(wnd, NULL, FALSE);
      }
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_DESTROY:
      free_art();
      free(s.title);
      free(s.cover_dir);
      canvas_free(&s.buf);
      if (s.wic) IWICImagingFactory_Release(s.wic);
      if (s.font) DeleteObject(s.font);
      if (s.icon_font) DeleteObject(s.icon_font);
      memset(&s, 0, sizeof s);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND now_playing_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = now_playing_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"musicapp_nowplaying"};
  RegisterClassExW(&wc);

  memset(&s, 0, sizeof s);
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  return CreateWindowExW(0, L"musicapp_nowplaying", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

void now_playing_update(UINT dpi) {
  s.dpi = dpi;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  if (s.font) DeleteObject(s.font);
  if (s.icon_font) DeleteObject(s.icon_font);
  s.font = CreateFontIndirectW(&ncm.lfMessageFont);
  s.icon_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());
  if (s.wnd) {
    if (s.art_size != scale(ART)) load_art();
    relayout();
    InvalidateRect(s.wnd, NULL, FALSE);
  }
}

void now_playing_show(const wchar_t *title, int duration_ms, const wchar_t *cover_dir) {
  if (s.dragging) ReleaseCapture();
  free(s.title);
  free(s.cover_dir);
  s.title = title ? _wcsdup(title) : NULL;
  s.cover_dir = title && cover_dir ? _wcsdup(cover_dir) : NULL;
  s.duration_ms = title && duration_ms > 0 ? duration_ms : 0;
  s.pos_ms = 0;
  SetWindowTextW(s.wnd, title); /* for screen readers */
  load_art();
  relayout();
  InvalidateRect(s.wnd, NULL, FALSE);
}

void now_playing_set_position(int ms) {
  if (!s.dragging) set_position(ms, FALSE);
}

int now_playing_position(void) {
  return s.pos_ms;
}
