#include "waiting_banner.h"

#include <commctrl.h>
#include <oleacc.h>
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
  BUTTON_PAD = 24, /* the button's width over its label's */
  BUTTON_INSET = 4, /* from the pill's top and bottom */
  BUTTON_GAP = 6,   /* between the two buttons */
  CLOSE = 28,       /* the ✕'s width; it's as tall as the other buttons */
};

static struct {
  HWND wnd;
  HFONT font, icon_font, close_font;
  UINT dpi;
  int text_h; /* a line of font */
  wchar_t text[512]; /* "" while nothing waits */
  wchar_t problem[512]; /* the warning, above it: "" while there's nothing to warn of */
  wchar_t spoken[1024]; /* both, for screen readers */
  HWND remove;          /* Remove from Library, on the warning */
  BOOL removable;       /* tracks are missing for good, so it shows */
  HWND details;         /* Details, on the warning */
  BOOL detailed;        /* there are issues to show, so it shows */
  BOOL stress;       /* a stress test library is open: text says so, and a click goes back */
  HWND close_problem, close_text; /* the ✕ at the end of each pill */
  BOOL problem_dismissed, text_dismissed; /* ✕ pressed: hidden until it has nothing to say */
  mc_sync *sync;     /* as last given to waiting_banner_update(), to read again on a ✕ */
  BOOL above;        /* waiting_banner_set_above() */
  IAccPropServices *acc;
  canvas c;
  uia_host ui;
} b;

static int scale(int px) {
  return MulDiv(px, (int)(b.dpi ? b.dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

/* A pill's height: a line of text with its padding. */
static int pill_height(void) {
  return (b.text_h > scale(ICON) ? b.text_h : scale(ICON)) + 2 * scale(PAD_Y);
}

/* A button's width for its label, 0 while it doesn't show. */
static int label_width(HWND button, BOOL on) {
  if (!on || !button) return 0;
  wchar_t label[128];
  int len = GetWindowTextW(button, label, 128);
  HDC dc = GetDC(b.wnd);
  HGDIOBJ old = SelectObject(dc, b.font ? (HGDIOBJ)b.font : GetStockObject(DEFAULT_GUI_FONT));
  RECT rc = {0, 0, 0, 0};
  DrawTextW(dc, label, len, &rc, DT_CALCRECT | DT_SINGLELINE); /* without the mnemonic's '&' */
  SelectObject(dc, old);
  ReleaseDC(b.wnd, dc);
  return rc.right + scale(BUTTON_PAD);
}

static int remove_width(void) {
  return label_width(b.remove, b.removable);
}

static int details_width(void) {
  return label_width(b.details, b.detailed);
}

/* Both buttons' width with the gap between, and the ✕'s after them. */
static int button_width(void) {
  int r = remove_width(), d = details_width();
  return r + d + (r && d ? scale(BUTTON_GAP) : 0) + (r || d ? scale(BUTTON_GAP) : 0) + scale(CLOSE);
}

/* One sentence in its pill, with its icon: the warning on the caution colour (Fluent's, as the
 * firewall banner has it), else the info icon on the accent's tint. It's cut short with an
 * ellipsis if it doesn't fit (screen readers get all of it), and stops short of right_gap. */
static void paint_pill(canvas *c, const RECT *pill, BOOL warn, const wchar_t *text, int right_gap) {
  int r = scale(RADIUS);
  COLORREF tint = !warn ? theme.accent : theme.dark ? RGB(0xFC, 0xE1, 0x00) : RGB(0x9D, 0x5D, 0x00);
  int alpha = warn ? (theme.dark ? 28 : 30) : (theme.dark ? 60 : 36);
  if (theme.high_contrast) {
    canvas_frame(c, pill, r, 1, theme.text, 255);
  } else {
    canvas_fill(c, pill, r, tint, alpha);
    canvas_frame(c, pill, r, 1, tint, 110);
  }
  int pad = scale(PAD_X), icon = scale(ICON);
  RECT ir = {pill->left + pad, pill->top, pill->left + pad + icon, pill->bottom};
  if (b.icon_font)
    canvas_text(c, warn ? L"\xE7BA" : L"\xE946", 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                b.icon_font, theme.text);
  RECT tr = {ir.right + pad * 3 / 4, pill->top, pill->right - pad - right_gap, pill->bottom};
  canvas_text(c, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
              b.font ? b.font : (HFONT)GetStockObject(DEFAULT_GUI_FONT), theme.text);
}

static void paint(HDC dc) {
  RECT rc;
  GetClientRect(b.wnd, &rc);
  if (!canvas_reserve(&b.c, rc.right, rc.bottom)) return;
  canvas *c = &b.c;
  canvas_clear(c, &rc, theme.bg, 255);

  RECT pill = {rc.left + scale(MARGIN_X), rc.top + scale(MARGIN_TOP), rc.right - scale(MARGIN_X),
               rc.top + scale(MARGIN_TOP) + pill_height()};
  if (b.problem[0]) {
    paint_pill(c, &pill, TRUE, b.problem, button_width() + scale(PAD_X) / 2);
    OffsetRect(&pill, 0, pill_height() + scale(MARGIN_TOP));
  }
  if (b.text[0]) paint_pill(c, &pill, FALSE, b.text, scale(CLOSE) + scale(PAD_X) / 2);

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
  lstrcpynW(n->name, b.spoken, (int)(sizeof n->name / sizeof *n->name));
  n->automation_id = L"WaitingBanner";
  n->live = Polite;
  return TRUE;
}

static int ui_focus(void) {
  return UIA_ROOT;
}

static const uia_ops ui_ops = {.node = ui_node, .focus = ui_focus};

/* Puts the buttons at the end of the warning's pill, the ✕ last and Details before it, and
 * the other ✕ at the end of the second pill; or hides them. */
static void place_button(void) {
  if (!b.remove || !b.details || !b.close_problem || !b.close_text) return;
  RECT rc;
  GetClientRect(b.wnd, &rc);
  int end = rc.right - scale(MARGIN_X) - scale(PAD_X) / 2, inset = scale(BUTTON_INSET);
  int right = end, top = scale(MARGIN_TOP) + inset, h = pill_height() - 2 * inset;
  HWND buttons[] = {b.close_problem, b.details, b.remove};
  int widths[] = {scale(CLOSE), details_width(), remove_width()};
  for (int i = 0; i < 3; i++) {
    if (!widths[i] || !b.problem[0]) {
      ShowWindow(buttons[i], SW_HIDE);
      continue;
    }
    SetWindowPos(buttons[i], NULL, right - widths[i], top, widths[i], h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    right -= widths[i] + scale(BUTTON_GAP);
  }
  if (b.text[0]) {
    int text_top = top + (b.problem[0] ? pill_height() + scale(MARGIN_TOP) : 0);
    SetWindowPos(b.close_text, NULL, end - scale(CLOSE), text_top, scale(CLOSE), h,
                 SWP_NOZORDER | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  } else {
    ShowWindow(b.close_text, SW_HIDE);
  }
}

static BOOL refresh(void);

static LRESULT CALLBACK banner_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!b.wnd) { /* messages arrive before CreateWindowExW returns */
    b.wnd = wnd;
    uia_init(&b.ui, wnd, &ui_ops);
  }
  LRESULT result;
  if (uia_message(&b.ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
      place_button();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_CTLCOLORBTN: return (LRESULT)theme_ctlcolor(msg, (HDC)wp);
    case WM_COMMAND:
      if ((HWND)lp == b.remove && HIWORD(wp) == BN_CLICKED)
        SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), WAITING_BANNER_REMOVE), (LPARAM)wnd);
      else if ((HWND)lp == b.details && HIWORD(wp) == BN_CLICKED)
        SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), WAITING_BANNER_DETAILS), (LPARAM)wnd);
      else if (((HWND)lp == b.close_problem || (HWND)lp == b.close_text) && HIWORD(wp) == BN_CLICKED) {
        if ((HWND)lp == b.close_problem) b.problem_dismissed = TRUE;
        else b.text_dismissed = TRUE;
        if (GetFocus() == (HWND)lp) SetFocus(GetParent(wnd));
        refresh();
        SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), WAITING_BANNER_DISMISSED), (LPARAM)wnd);
      }
      return 0;
    case WM_LBUTTONUP:
      if (b.stress)
        SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), BN_CLICKED), (LPARAM)wnd);
      return 0;
    case WM_SETCURSOR:
      if (b.stress) {
        SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_HAND));
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (b.acc) {
        MSAAPROPID props[] = {PROPID_ACC_NAME};
        IAccPropServices_ClearHwndProps(b.acc, b.close_problem, OBJID_CLIENT, CHILDID_SELF, props, 1);
        IAccPropServices_ClearHwndProps(b.acc, b.close_text, OBJID_CLIENT, CHILDID_SELF, props, 1);
        IAccPropServices_Release(b.acc);
        b.acc = NULL;
      }
      uia_destroy(&b.ui);
      canvas_free(&b.c);
      if (b.icon_font) DeleteObject(b.icon_font);
      b.icon_font = NULL;
      if (b.close_font) DeleteObject(b.close_font);
      b.close_font = NULL;
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
  HWND wnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"kithsong_waiting", NULL, WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                             0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  b.remove = CreateWindowExW(0, WC_BUTTONW, mc_wstr(MC_STR_SYNC_LOST_REMOVE), WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,
                             0, 0, 0, 0, wnd, (HMENU)(INT_PTR)1, GetModuleHandleW(NULL), NULL);
  b.details = CreateWindowExW(0, WC_BUTTONW, mc_wstr_mnemonic(MC_STR_SYNC_ISSUES_DETAILS),
                              WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, wnd, (HMENU)(INT_PTR)2,
                              GetModuleHandleW(NULL), NULL);
  /* The ✕s: the icon font's Cancel (waiting_banner_set_font()), named Close for screen readers. */
  CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER, &IID_IAccPropServices, (void **)&b.acc);
  for (int i = 0; i < 2; i++) {
    HWND close = CreateWindowExW(0, WC_BUTTONW, L"\xE711", WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, wnd,
                                 (HMENU)(INT_PTR)(3 + i), GetModuleHandleW(NULL), NULL);
    if (b.acc && close)
      IAccPropServices_SetHwndPropStr(b.acc, close, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, mc_wstr(MC_STR_CLOSE));
    if (i == 0) b.close_problem = close;
    else b.close_text = close;
  }
  return wnd;
}

static int sentences(void) {
  return (b.text[0] != L'\0') + (b.problem[0] != L'\0');
}

/* TRUE if the strip's height changed. */
static BOOL set_text(const wchar_t *wide, const wchar_t *problem, BOOL removable, BOOL detailed) {
  if (!wcscmp(wide, b.text) && !wcscmp(problem, b.problem) && removable == b.removable && detailed == b.detailed)
    return FALSE;
  int was = sentences();
  wcscpy(b.text, wide);
  wcscpy(b.problem, problem);
  b.removable = removable;
  b.detailed = detailed;
  swprintf(b.spoken, sizeof b.spoken / sizeof *b.spoken, L"%ls%ls%ls", b.problem,
           b.problem[0] && b.text[0] ? L" " : L"", b.text);
  if (b.wnd) {
    place_button();
    InvalidateRect(b.wnd, NULL, FALSE);
    uia_refresh(&b.ui);
    if (b.spoken[0]) uia_announce(&b.ui, UIA_ROOT);
  }
  return was != sentences();
}

/* What each sentence says ("" for nothing), less any whose ✕ was pressed. A dismissed one
 * stays hidden until it has nothing to say, then comes back with what's next. TRUE if the
 * strip's height changed. */
static BOOL show(const wchar_t *text, const wchar_t *problem, BOOL removable, BOOL detailed) {
  if (!text[0]) b.text_dismissed = FALSE;
  if (!problem[0]) b.problem_dismissed = FALSE;
  return set_text(b.text_dismissed ? L"" : text, b.problem_dismissed ? L"" : problem, removable, detailed);
}

/* The stress test library's sentence, or the waiting tracks and the problems from b.sync. */
static BOOL refresh(void) {
  if (b.stress) return show(mc_wstr(MC_STR_STRESS_BANNER), L"", FALSE, FALSE);
  mc_sync *sync = b.sync;
  char text[512] = "";
  mc_sync_waiting *w = NULL;
  if (sync && mc_sync_get_waiting(sync, &w) == MC_OK) mc_sync_waiting_text(w, text, sizeof text);
  mc_sync_waiting_free(w);
  char warning[512] = "";
  mc_sync_problems *p = NULL;
  if (sync && mc_sync_get_problems(sync, &p) == MC_OK) mc_sync_problems_text(p, warning, sizeof warning);
  BOOL removable = p && p->lost > 0, detailed = p && p->issue_count > 0;
  mc_sync_problems_free(p);
  wchar_t wide[512], problem[512];
  wstr_from_utf8(wide, sizeof wide / sizeof *wide, text);
  wstr_from_utf8(problem, sizeof problem / sizeof *problem, warning);
  return show(wide, problem, removable, detailed);
}

BOOL waiting_banner_set_stress(BOOL stress) {
  if (stress == b.stress) return FALSE;
  b.stress = stress;
  b.text_dismissed = FALSE;
  if (stress) return show(mc_wstr(MC_STR_STRESS_BANNER), L"", FALSE, FALSE);
  return set_text(L"", L"", FALSE, FALSE);
}

BOOL waiting_banner_update(mc_sync *sync) {
  b.sync = sync;
  if (b.stress) return FALSE;
  return refresh();
}

void waiting_banner_set_above(BOOL above) {
  b.above = above;
}

BOOL waiting_banner_above(void) {
  return b.above;
}

BOOL waiting_banner_shown(void) {
  return sentences() > 0;
}

int waiting_banner_height(void) {
  int n = sentences() ? sentences() : 1;
  return n * (scale(MARGIN_TOP) + pill_height());
}

void waiting_banner_set_font(HFONT font, UINT dpi) {
  b.font = font;
  b.dpi = dpi;
  if (b.remove) SendMessageW(b.remove, WM_SETFONT, (WPARAM)font, TRUE);
  if (b.details) SendMessageW(b.details, WM_SETFONT, (WPARAM)font, TRUE);
  if (b.icon_font) DeleteObject(b.icon_font);
  b.icon_font = CreateFontW(-scale(ICON), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());
  /* Smaller than the pill's icon, as a ✕ on a button is. */
  if (b.close_font) DeleteObject(b.close_font);
  b.close_font = CreateFontW(-scale(10), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                             theme_icon_face());
  if (b.close_problem) SendMessageW(b.close_problem, WM_SETFONT, (WPARAM)b.close_font, TRUE);
  if (b.close_text) SendMessageW(b.close_text, WM_SETFONT, (WPARAM)b.close_font, TRUE);
  HDC dc = GetDC(b.wnd);
  HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
  TEXTMETRICW tm;
  b.text_h = GetTextMetricsW(dc, &tm) ? (int)tm.tmHeight : scale(16);
  SelectObject(dc, old);
  ReleaseDC(b.wnd, dc);
  if (b.wnd) {
    place_button();
    InvalidateRect(b.wnd, NULL, FALSE);
  }
}

void waiting_banner_theme_changed(void) {
  if (b.wnd) InvalidateRect(b.wnd, NULL, FALSE);
  if (b.remove) InvalidateRect(b.remove, NULL, TRUE);
  if (b.details) InvalidateRect(b.details, NULL, TRUE);
  if (b.close_problem) InvalidateRect(b.close_problem, NULL, TRUE);
  if (b.close_text) InvalidateRect(b.close_text, NULL, TRUE);
}
