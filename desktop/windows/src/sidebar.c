#include "sidebar.h"

#include <commctrl.h>
#include <string.h>

#include "canvas.h"
#include "theme.h"

typedef enum { ROW_NAV, ROW_HEADER, ROW_NOTE, ROW_SPACER } row_kind;

/* The list box holds one item per row, except the spacer, which takes as many items as it
 * needs to fill the height (an item can't be taller than 255 pixels). Each item's data is
 * its index in rows. */
static const struct {
  const wchar_t *text;
  wchar_t icon; /* Segoe Fluent Icons */
  row_kind kind;
  sidebar_page page; /* navigation rows */
} rows[] = {
  {L"Albums", 0xE93C, ROW_NAV, PAGE_ALBUMS},
  {L"Artists", 0xE77B, ROW_NAV, PAGE_ARTISTS},
  {L"Playlists", 0xE90B, ROW_NAV, PAGE_PLAYLISTS},
  {L"DEVICES", 0, ROW_HEADER, 0},
  {L"No devices found", 0, ROW_NOTE, 0},
  {L"", 0, ROW_SPACER, 0}, /* pushes the rest to the bottom */
  {L"Import", 0xE8B5, ROW_NAV, PAGE_IMPORT},
  {L"Settings", 0xE713, ROW_NAV, PAGE_SETTINGS},
};
#define NUM_ROWS ((int)(sizeof rows / sizeof rows[0]))
#define MAX_ITEM_H 255

/* Metrics at 96 DPI. */
enum {
  NAV_H = 36,
  HEADER_H = 40, /* includes the gap above the section */
  NOTE_H = 32,
  BOTTOM = 6, /* below the last row */
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
  int hot; /* navigation item under the mouse, or -1 */
  BOOL locked;
} s;

static void set_hot(int i);

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

static int item_count(void) {
  return (int)SendMessageW(s.wnd, LB_GETCOUNT, 0, 0);
}

static int row_of(int item) {
  return (int)SendMessageW(s.wnd, LB_GETITEMDATA, (WPARAM)item, 0);
}

static int selected_item(void) {
  return (int)SendMessageW(s.wnd, LB_GETCURSEL, 0, 0);
}

/* A navigation item that can be selected now. */
static BOOL enabled(int item) {
  return rows[row_of(item)].kind == ROW_NAV && (!s.locked || item == selected_item());
}

/* The next enabled navigation item after from in direction dir (+1 or -1), or -1 if there is none. */
static int next_nav(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < item_count(); i += dir)
    if (enabled(i)) return i;
  return -1;
}

/* The enabled navigation item at a WM_MOUSE* lParam, or -1. */
static int nav_at(LPARAM lp) {
  LRESULT r = SendMessageW(s.wnd, LB_ITEMFROMPOINT, 0, lp);
  int i = LOWORD(r);
  return !HIWORD(r) && i < item_count() && enabled(i) ? i : -1;
}

static int row_height(int row) {
  static const int heights[] = {[ROW_NAV] = NAV_H, [ROW_HEADER] = HEADER_H, [ROW_NOTE] = NOTE_H, [ROW_SPACER] = 0};
  return scale(heights[rows[row].kind]);
}

static void add_item(int at, int row) {
  int item = (int)SendMessageW(s.wnd, LB_INSERTSTRING, (WPARAM)at, (LPARAM)rows[row].text);
  SendMessageW(s.wnd, LB_SETITEMDATA, (WPARAM)item, row);
  SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)item, row_height(row));
}

/* Gives the spacer the height left over, so the rows after it sit at the bottom. */
static void fit_spacer(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int first = -1, n = 0, used = scale(BOTTOM), spacer_row = 0;
  for (int i = 0; i < item_count(); i++) {
    int row = row_of(i);
    if (rows[row].kind != ROW_SPACER) {
      used += row_height(row);
    } else {
      if (first < 0) first = i;
      spacer_row = row;
      n++;
    }
  }
  if (first < 0) return;

  int gap = max(0, rc.bottom - used), want = max(1, (gap + MAX_ITEM_H - 1) / MAX_ITEM_H);
  set_hot(-1); /* item indexes shift */
  for (; n < want; n++) add_item(first, spacer_row);
  for (; n > want; n--) SendMessageW(s.wnd, LB_DELETESTRING, (WPARAM)first, 0);
  for (int k = 0; k < n; k++)
    SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)(first + k), gap / n + (k < gap % n));
  SendMessageW(s.wnd, LB_SETTOPINDEX, 0, 0);
  InvalidateRect(s.wnd, NULL, TRUE);
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

static void select_item(int i) {
  if (i < 0 || i == selected_item()) return;
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
      select_item(nav_at(lp));
      return 0;
    case WM_KEYDOWN: {
      /* Using the keyboard shows focus rectangles, as in a dialog. */
      SendMessageW(GetAncestor(wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
      int cur = selected_item();
      switch (wp) {
        case VK_UP: select_item(next_nav(cur, -1)); return 0;
        case VK_DOWN: select_item(next_nav(cur, +1)); return 0;
        case VK_HOME:
        case VK_PRIOR: select_item(next_nav(-1, +1)); return 0;
        case VK_END:
        case VK_NEXT: select_item(next_nav(item_count(), -1)); return 0;
      }
      break;
    }
    case WM_CHAR:
      return 0; /* type-ahead would land on headers */
    case WM_SIZE: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      fit_spacer();
      return r;
    }
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
  for (int i = 0; i < NUM_ROWS; i++) add_item(i, i);
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

  for (int i = 0; i < item_count(); i++)
    if (rows[row_of(i)].kind != ROW_SPACER) SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)i, row_height(row_of(i)));
  fit_spacer();
}

sidebar_page sidebar_selection(void) {
  int i = selected_item();
  return i >= 0 ? rows[row_of(i)].page : PAGE_ALBUMS;
}

void sidebar_select(sidebar_page page) {
  for (int i = 0; i < item_count(); i++)
    if (rows[row_of(i)].kind == ROW_NAV && rows[row_of(i)].page == page) SendMessageW(s.wnd, LB_SETCURSEL, (WPARAM)i, 0);
}

void sidebar_set_locked(BOOL locked) {
  if (locked == s.locked) return;
  s.locked = locked;
  set_hot(-1);
  InvalidateRect(s.wnd, NULL, FALSE);
}

void sidebar_draw(const DRAWITEMSTRUCT *dis) {
  int item = (int)dis->itemID, i = (int)dis->itemData;
  int w = dis->rcItem.right - dis->rcItem.left, h = dis->rcItem.bottom - dis->rcItem.top;
  if (item < 0 || i < 0 || i >= NUM_ROWS || !canvas_reserve(&s.buf, w, h)) return;

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
                               selected ? STATE_SELECTED : item == s.hot ? STATE_HOT : STATE_NORMAL);
      if (!enabled(item)) color = theme.text3;
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
    case ROW_SPACER:
      break;
  }
  canvas_text(c, rows[i].text, -1, &text, format, font, color);
  canvas_blit(c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
}

HBRUSH sidebar_ctlcolor(HWND wnd) {
  return wnd == s.wnd ? s.bg : NULL;
}
