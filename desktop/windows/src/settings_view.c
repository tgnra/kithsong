#include "settings_view.h"

#include <commctrl.h>
#include <stdlib.h>

#include "theme.h"
#include "wstr.h"

enum {
  ID_SET_TITLE = 300,
  ID_SET_HEADING,
  ID_SET_PATH,
  ID_SET_MOVE,
  ID_SET_OPEN,
  ID_SET_NOTE,
};

static struct {
  HWND parent;
  HWND title, heading, path, move, open, note;
  HFONT font, title_font, heading_font;
  BOOL shown;
} g_set;

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, g_set.parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

void settings_view_create(HWND parent) {
  g_set.parent = parent;
  DWORD label = SS_LEFT | SS_NOPREFIX;
  g_set.title = child(WC_STATICW, mc_wstr(MC_STR_SETTINGS), label, ID_SET_TITLE);
  g_set.heading = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_LOCATION), label, ID_SET_HEADING);
  /* SS_EDITCONTROL wraps a long path wherever it has to, as an edit box would. */
  g_set.path = child(WC_STATICW, NULL, label | SS_EDITCONTROL, ID_SET_PATH);
  g_set.move = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_MOVE), BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED,
                     ID_SET_MOVE);
  g_set.open = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_OPEN_ANOTHER), BS_PUSHBUTTON | WS_TABSTOP,
                     ID_SET_OPEN);
  g_set.note = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_LOCATION_TEXT), label, ID_SET_NOTE);
}

void settings_view_destroy(void) {
  if (g_set.title_font) DeleteObject(g_set.title_font);
  if (g_set.heading_font) DeleteObject(g_set.heading_font);
  g_set.title_font = g_set.heading_font = NULL;
}

void settings_view_set_library_dir(const char *dir) {
  wchar_t buf[1024];
  if (!dir || !MultiByteToWideChar(CP_UTF8, 0, dir, -1, buf, 1024)) buf[0] = L'\0';
  buf[1023] = L'\0';
  SetWindowTextW(g_set.path, buf);
  SendMessageW(g_set.parent, WM_SIZE, 0, 0); /* the path may wrap differently */
}

void settings_view_set_font(HFONT font, UINT dpi) {
  g_set.font = font;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 20, 9); /* 9pt -> 20pt, Windows 11's title size */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_set.title_font) DeleteObject(g_set.title_font);
  g_set.title_font = CreateFontIndirectW(&lf);
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 14, 9); /* subtitle */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_set.heading_font) DeleteObject(g_set.heading_font);
  g_set.heading_font = CreateFontIndirectW(&lf);

  SendMessageW(g_set.title, WM_SETFONT, (WPARAM)g_set.title_font, TRUE);
  SendMessageW(g_set.heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  HWND body[] = {g_set.path, g_set.move, g_set.open, g_set.note};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
}

void settings_view_show(BOOL show) {
  g_set.shown = show;
  HWND all[] = {g_set.title, g_set.heading, g_set.path, g_set.move, g_set.open, g_set.note};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(all[i], show ? SW_SHOW : SW_HIDE);
}

/* The height wnd's text needs at width w, wrapped as its static control wraps it. */
static int text_height(HWND wnd, HFONT font, int w, UINT format) {
  wchar_t buf[1024];
  int n = GetWindowTextW(wnd, buf, 1024);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, font);
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, n ? buf : L" ", n ? n : 1, &rc, format | DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return rc.bottom;
}

static int button_width(HWND wnd, int pad) {
  wchar_t buf[128];
  int n = GetWindowTextW(wnd, buf, 128);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, g_set.font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return size.cx + 2 * pad;
}

void settings_view_layout(int x, int y, int w, int h, UINT dpi) {
  (void)h;
  if (!g_set.shown || !g_set.font) return;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int pad = S(32), text_w = min(w - 2 * pad, S(720));
  int left = x + pad, top = y + S(24);

  int th = text_height(g_set.title, g_set.title_font, text_w, 0);
  MoveWindow(g_set.title, left, top, text_w, th, TRUE);
  top += th + S(24);

  int hh = text_height(g_set.heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int ph = text_height(g_set.path, g_set.font, text_w, DT_EDITCONTROL);
  MoveWindow(g_set.path, left, top, text_w, ph, TRUE);
  top += ph + S(12);

  int btn_h = S(32), move_w = button_width(g_set.move, S(16)), open_w = button_width(g_set.open, S(16));
  MoveWindow(g_set.move, left, top, move_w, btn_h, TRUE);
  MoveWindow(g_set.open, left + move_w + S(8), top, open_w, btn_h, TRUE);
  top += btn_h + S(12);

  int nh = text_height(g_set.note, g_set.font, text_w, 0);
  MoveWindow(g_set.note, left, top, text_w, nh, TRUE);
#undef S
}

BOOL settings_view_on_command(int id) {
  switch (id) {
    case ID_SET_OPEN: PostMessageW(g_set.parent, WM_APP_OPEN_LIBRARY, 0, 0); return TRUE;
    case ID_SET_MOVE: return TRUE; /* not implemented yet; the button is disabled */
    default: return FALSE;
  }
}

HBRUSH settings_view_ctlcolor(HWND wnd, HDC dc) {
  if (wnd != g_set.title && wnd != g_set.heading && wnd != g_set.path && wnd != g_set.note) return NULL;
  SetTextColor(dc, wnd == g_set.note ? theme.text2 : theme.text);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
