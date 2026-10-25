#include "sidebar.h"

#include <commctrl.h>
#include <string.h>

#include "canvas.h"
#include "theme.h"

typedef enum { ROW_NAV, ROW_HEADER, ROW_NOTE } row_kind;

static const struct {
  const wchar_t *text;
  wchar_t icon; /* Segoe Fluent Icons */
  row_kind kind;
} rows[] = {
  {L"Albums", 0xE93C, ROW_NAV},
  {L"Artists", 0xE77B, ROW_NAV},
  {L"Playlists", 0xE90B, ROW_NAV},
  {L"DEVICES", 0, ROW_HEADER},
  {L"No devices found", 0, ROW_NOTE},
};
#define NUM_ROWS ((int)(sizeof rows / sizeof rows[0]))

/* Metrics at 96 DPI. */
enum {
  NAV_H = 36,
  HEADER_H = 40, /* includes the gap above the section */
  NOTE_H = 32,
  INSET = 6,  /* between the list's edges and the selection fill */
  TEXT_X = 18,
  ICON_W = 28, /* icon plus the gap after it */
  RADIUS = 6,
};

static struct {
  HWND wnd;
  UINT dpi;
  HFONT font, header_font, icon_font;
  HBRUSH bg; /* below the rows; black (transparent) over Mica */
  canvas buf;
  int hot; /* navigation row under the mouse, or -1 */
} s;

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

/* The next navigation row after from in direction dir (+1 or -1), or -1 if there is none. */
static int next_nav(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < NUM_ROWS; i += dir)
    if (rows[i].kind == ROW_NAV) return i;
  return -1;
}

/* The navigation row at a WM_MOUSE* lParam, or -1. */
static int nav_at(LPARAM lp) {
  LRESULT r = SendMessageW(s.wnd, LB_ITEMFROMPOINT, 0, lp);
  int i = LOWORD(r);
  return !HIWORD(r) && i < NUM_ROWS && rows[i].kind == ROW_NAV ? i : -1;
}

static void invalidate_row(int i) {
  RECT rc;
  if (i >= 0 && SendMessageW(s.wnd, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&rc) != LB_ERR)
    InvalidateRect(s.wnd, &rc, FALSE);
}

static void set_hot(int i) {
  if (i == s.hot) return;
  invalidate_row(s.hot);
  invalidate_row(i);
  s.hot = i;
}

static void select_row(int i) {
  if (i < 0 || i == (int)SendMessageW(s.wnd, LB_GETCURSEL, 0, 0)) return;
  SendMessageW(s.wnd, LB_SETCURSEL, (WPARAM)i, 0);
  SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), LBN_SELCHANGE),
               (LPARAM)s.wnd);
}

/* Takes over the mouse and keyboard so headers and notes can't be selected. */
static LRESULT CALLBACK sidebar_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                     DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
      TrackMouseEvent(&tme);
      set_hot(nav_at(lp));
      return 0;
    }
    case WM_MOUSELEAVE:
      set_hot(-1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
      SetFocus(wnd);
      select_row(nav_at(lp));
      return 0;
    case WM_KEYDOWN: {
      /* Using the keyboard shows focus rectangles, as in a dialog. */
      SendMessageW(GetAncestor(wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
      int cur = (int)SendMessageW(wnd, LB_GETCURSEL, 0, 0);
      switch (wp) {
        case VK_UP: select_row(next_nav(cur, -1)); return 0;
        case VK_DOWN: select_row(next_nav(cur, +1)); return 0;
        case VK_HOME:
        case VK_PRIOR: select_row(next_nav(-1, +1)); return 0;
        case VK_END:
        case VK_NEXT: select_row(next_nav(NUM_ROWS, -1)); return 0;
      }
      break;
    }
    case WM_CHAR:
      return 0; /* type-ahead would land on headers */
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, sidebar_proc, id);
      if (s.font) DeleteObject(s.font);
      if (s.header_font) DeleteObject(s.header_font);
      if (s.icon_font) DeleteObject(s.icon_font);
      if (s.bg) DeleteObject(s.bg);
      canvas_free(&s.buf);
      memset(&s, 0, sizeof s);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

HWND sidebar_create(HWND parent, int id) {
  memset(&s, 0, sizeof s);
  s.hot = -1;
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  s.wnd = CreateWindowExW(0, WC_LISTBOXW, NULL,
                          WS_CHILD | WS_VISIBLE | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS |
                            LBS_NOINTEGRALHEIGHT,
                          0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (!s.wnd) return NULL;
  for (int i = 0; i < NUM_ROWS; i++) SendMessageW(s.wnd, LB_ADDSTRING, 0, (LPARAM)rows[i].text);
  SendMessageW(s.wnd, LB_SETCURSEL, 0, 0);
  SetWindowSubclass(s.wnd, sidebar_proc, 0, 0);
  return s.wnd;
}

void sidebar_update(UINT dpi) {
  s.dpi = dpi;

  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 7, 6); /* 9pt -> 10.5pt */
  if (s.font) DeleteObject(s.font);
  s.font = CreateFontIndirectW(&lf);
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 11, 12);
  lf.lfWeight = FW_SEMIBOLD;
  if (s.header_font) DeleteObject(s.header_font);
  s.header_font = CreateFontIndirectW(&lf);
  if (s.icon_font) DeleteObject(s.icon_font);
  s.icon_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());

  if (s.bg) DeleteObject(s.bg);
  s.bg = CreateSolidBrush(theme.mica ? RGB(0, 0, 0) : theme.frame);

  static const int heights[] = {[ROW_NAV] = NAV_H, [ROW_HEADER] = HEADER_H, [ROW_NOTE] = NOTE_H};
  for (int i = 0; i < NUM_ROWS; i++)
    SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)i, scale(heights[rows[i].kind]));
  InvalidateRect(s.wnd, NULL, TRUE);
}

void sidebar_draw(const DRAWITEMSTRUCT *dis) {
  int i = (int)dis->itemID;
  int w = dis->rcItem.right - dis->rcItem.left, h = dis->rcItem.bottom - dis->rcItem.top;
  if (i < 0 || i >= NUM_ROWS || !canvas_reserve(&s.buf, w, h)) return;

  /* Drawn off-screen, which also gets the alpha right over Mica. */
  canvas *c = &s.buf;
  RECT rc = {0, 0, w, h};
  canvas_clear(c, &rc, theme.frame, theme.mica ? 0 : 255);

  HFONT font = s.font;
  COLORREF color = theme.text;
  RECT text = {scale(TEXT_X), 0, w - scale(INSET), h};
  UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
  switch (rows[i].kind) {
    case ROW_NAV: {
      BOOL selected = (dis->itemState & ODS_SELECTED) != 0;
      RECT fill = {scale(INSET), scale(2), w - scale(INSET), h - scale(2)};
      color = theme_draw_state(c, &fill, scale(RADIUS),
                               selected ? STATE_SELECTED : i == s.hot ? STATE_HOT : STATE_NORMAL);
      if (selected && !theme.high_contrast) { /* accent marker at the left edge */
        int mark_h = scale(16);
        RECT mark = {fill.left, (h - mark_h) / 2, fill.left + scale(3), (h + mark_h) / 2};
        canvas_fill(c, &mark, scale(2), theme.accent, 255);
      }
      if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT))
        canvas_frame(c, &fill, scale(RADIUS), scale(2), theme.text, 255);
      RECT icon = {text.left, 0, text.left + scale(ICON_W), h};
      canvas_text(c, &rows[i].icon, 1, &icon, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.icon_font,
                  color);
      text.left += scale(ICON_W);
      break;
    }
    case ROW_HEADER:
      font = s.header_font;
      color = theme.text2;
      format = (format & ~DT_VCENTER) | DT_BOTTOM;
      text.bottom -= scale(6);
      break;
    case ROW_NOTE:
      color = theme.text3;
      break;
  }
  canvas_text(c, rows[i].text, -1, &text, format, font, color);
  canvas_blit(c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
}

HBRUSH sidebar_ctlcolor(HWND wnd) {
  return wnd == s.wnd ? s.bg : NULL;
}
