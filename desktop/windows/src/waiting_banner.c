#include "waiting_banner.h"

#include <uiautomation.h>
#include <wchar.h>

#include "canvas.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

/* Layout at 96 DPI. */
enum {
  MARGIN_X = 12,   /* the pill's distance from the strip's sides */
  MARGIN_TOP = 10, /* and from its top; the grid's own padding is below it */
  PAD_X = 12,      /* inside the pill */
  PAD_Y = 9,
  ICON = 16,
  RADIUS = 8,
};

static struct {
  HWND wnd;
  HFONT font, icon_font;
  UINT dpi;
  int text_h; /* a line of font */
  wchar_t text[512]; /* "" while nothing waits */
  canvas c;
  uia_host ui;
} b;

static int scale(int px) {
  return MulDiv(px, (int)(b.dpi ? b.dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

static void paint(HDC dc) {
  RECT rc;
  GetClientRect(b.wnd, &rc);
  if (!canvas_reserve(&b.c, rc.right, rc.bottom)) return;
  canvas *c = &b.c;
  canvas_clear(c, &rc, theme.bg, 255);

  RECT pill = {rc.left + scale(MARGIN_X), rc.top + scale(MARGIN_TOP), rc.right - scale(MARGIN_X), rc.bottom};
  int r = scale(RADIUS);
  if (theme.high_contrast) {
    canvas_frame(c, &pill, r, 1, theme.text, 255);
  } else {
    canvas_fill(c, &pill, r, theme.accent, theme.dark ? 60 : 36);
    canvas_frame(c, &pill, r, 1, theme.accent, 110);
  }
  /* The info icon, then the sentence, cut short with an ellipsis if it doesn't fit (screen
   * readers get all of it). */
  int pad = scale(PAD_X), icon = scale(ICON);
  RECT ir = {pill.left + pad, pill.top, pill.left + pad + icon, pill.bottom};
  if (b.icon_font)
    canvas_text(c, L"\xE946", 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, b.icon_font, theme.text);
  RECT tr = {ir.right + pad * 3 / 4, pill.top, pill.right - pad, pill.bottom};
  canvas_text(c, b.text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
              b.font ? b.font : (HFONT)GetStockObject(DEFAULT_GUI_FONT), theme.text);

  /* The content area's top border and rounded corner, as the album grid draws them when
   * it's at the top; its left border carries on into the grid below. */
  int radius = scale(CONTENT_RADIUS);
  RECT border = {0, 0, rc.right + radius, rc.bottom + 2 * radius};
  canvas_frame(c, &border, radius, 1, theme.line, 255);
  canvas_round_corners(c, &rc, radius, CORNER_TL, theme.frame, theme.mica ? 0 : 255);
  canvas_blit(c, dc, 0, 0, &rc);
}

/* --- UI Automation: a live region whose name is the sentence --- */

static BOOL ui_node(int i, uia_node *n) {
  if (i != UIA_ROOT) return FALSE;
  n->type = UIA_TextControlTypeId;
  lstrcpynW(n->name, b.text, (int)(sizeof n->name / sizeof *n->name));
  n->automation_id = L"WaitingBanner";
  n->live = Polite;
  return TRUE;
}

static int ui_focus(void) {
  return UIA_ROOT;
}

static const uia_ops ui_ops = {.node = ui_node, .focus = ui_focus};

static LRESULT CALLBACK banner_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!b.wnd) { /* messages arrive before CreateWindowExW returns */
    b.wnd = wnd;
    uia_init(&b.ui, wnd, &ui_ops);
  }
  LRESULT result;
  if (uia_message(&b.ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(wnd, NULL, FALSE); return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      uia_destroy(&b.ui);
      canvas_free(&b.c);
      if (b.icon_font) DeleteObject(b.icon_font);
      b.icon_font = NULL;
      b.wnd = NULL;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND waiting_banner_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = banner_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_waiting"};
  RegisterClassExW(&wc);
  b.dpi = GetDpiForWindow(parent);
  return CreateWindowExW(0, L"kithsong_waiting", NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

BOOL waiting_banner_update(mc_sync *sync) {
  char text[512] = "";
  mc_sync_waiting *w = NULL;
  if (sync && mc_sync_get_waiting(sync, &w) == MC_OK) mc_sync_waiting_text(w, text, sizeof text);
  mc_sync_waiting_free(w);
  wchar_t wide[512];
  wstr_from_utf8(wide, sizeof wide / sizeof *wide, text);
  if (!wcscmp(wide, b.text)) return FALSE;
  BOOL was = b.text[0] != L'\0';
  wcscpy(b.text, wide);
  if (b.wnd) {
    InvalidateRect(b.wnd, NULL, FALSE);
    uia_refresh(&b.ui);
    if (b.text[0]) uia_announce(&b.ui, UIA_ROOT);
  }
  return was != (b.text[0] != L'\0');
}

BOOL waiting_banner_shown(void) {
  return b.text[0] != L'\0';
}

int waiting_banner_height(void) {
  int line = b.text_h > scale(ICON) ? b.text_h : scale(ICON);
  return scale(MARGIN_TOP) + line + 2 * scale(PAD_Y);
}

void waiting_banner_set_font(HFONT font, UINT dpi) {
  b.font = font;
  b.dpi = dpi;
  if (b.icon_font) DeleteObject(b.icon_font);
  b.icon_font = CreateFontW(-scale(ICON), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());
  HDC dc = GetDC(b.wnd);
  HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  b.text_h = GetTextMetricsW(dc, &tm) ? (int)tm.tmHeight : scale(16);
  SelectObject(dc, old);
  ReleaseDC(b.wnd, dc);
  if (b.wnd) InvalidateRect(b.wnd, NULL, FALSE);
}

void waiting_banner_theme_changed(void) {
  if (b.wnd) InvalidateRect(b.wnd, NULL, FALSE);
}
