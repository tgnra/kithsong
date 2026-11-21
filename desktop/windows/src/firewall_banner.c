#include "firewall_banner.h"

#include <commctrl.h>
#include <stdlib.h>
#include <uiautomation.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "firewall.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

/* Layout at 96 DPI. */
enum {
  PAD = 12, /* inside the strip */
  ICON = 16,
  ICON_GAP = 10,
  LINE_GAP = 2, /* heading to text */
  BUTTON_GAP = 10, /* text to buttons */
  BUTTON_H = 32,
  BUTTON_PAD = 32, /* a button's width over its label's */
  BUTTON_SPACE = 8,
  RADIUS = 8,
  TEXT_MAX = 1024,
};

/* What a button does. */
typedef enum action {
  ACT_NONE,
  ACT_ALLOW,
  ACT_NETWORK,
  ACT_SECURITY,
  ACT_ALLOWED_APPS,
} action;

#define NUM_BUTTONS 2
#define TEXT_FORMAT (DT_LEFT | DT_WORDBREAK | DT_NOPREFIX)

static struct {
  HWND wnd, parent, button[NUM_BUTTONS];
  action act[NUM_BUTTONS];
  BOOL shield[NUM_BUTTONS]; /* it shows the UAC shield */
  int first_id;             /* button[0]'s */
  HFONT font, heading_font, icon_font;
  UINT dpi;
  BOOL warn; /* the warning on the caution colour, not the info icon on the accent's */
  wchar_t heading[256], text[TEXT_MAX]; /* "" while there's nothing to say */
  wchar_t spoken[512];
  HBRUSH brush; /* the strip's colour, behind the buttons' corners */
  COLORREF brush_color;
  canvas c;
  uia_host ui;
} b;

static int scale(int px) {
  return MulDiv(px, (int)(b.dpi ? b.dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

/* Fluent's caution colour, as the device page's size estimate has it. */
static COLORREF tint(void) {
  if (!b.warn) return theme.accent;
  return theme.dark ? RGB(0xFC, 0xE1, 0x00) : RGB(0x9D, 0x5D, 0x00);
}

static int tint_alpha(void) {
  if (b.warn) return theme.dark ? 28 : 30;
  return theme.dark ? 60 : 36;
}

/* The tint over the page's background, as canvas_fill() leaves it. */
static COLORREF tinted_bg(void) {
  if (theme.high_contrast) return theme.bg;
  COLORREF over = tint(), under = theme.bg;
  int a = tint_alpha();
#define MIX(part) ((part(over) * a + part(under) * (255 - a)) / 255)
  return RGB(MIX(GetRValue), MIX(GetGValue), MIX(GetBValue));
#undef MIX
}

static int text_left(void) {
  return scale(PAD) + scale(ICON) + scale(ICON_GAP);
}

static int measure(HFONT font, const wchar_t *text, int w) {
  if (!text[0]) return 0;
  HDC dc = GetDC(b.parent);
  HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT));
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, text, -1, &rc, TEXT_FORMAT | DT_CALCRECT);
  SelectObject(dc, old);
  ReleaseDC(b.parent, dc);
  return rc.bottom;
}

static void paint(HDC dc) {
  RECT rc;
  GetClientRect(b.wnd, &rc);
  if (!canvas_reserve(&b.c, rc.right, rc.bottom)) return;
  canvas *c = &b.c;
  canvas_clear(c, &rc, theme.bg, 255);
  int r = scale(RADIUS);
  if (theme.high_contrast) {
    canvas_frame(c, &rc, r, 1, theme.text, 255);
  } else {
    canvas_fill(c, &rc, r, tint(), tint_alpha());
    canvas_frame(c, &rc, r, 1, tint(), 110);
  }
  int pad = scale(PAD), left = text_left(), w = rc.right - left - pad;
  int heading_h = measure(b.heading_font, b.heading, w);
  RECT ir = {pad, pad, pad + scale(ICON), pad + heading_h};
  if (b.icon_font) {
    /* Warning and Info, in the icon font. */
    const wchar_t *glyph = b.warn ? L"\xE7BA" : L"\xE946";
    canvas_text(c, glyph, 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, b.icon_font, theme.text);
  }
  RECT hr = {left, pad, left + w, pad + heading_h};
  canvas_text(c, b.heading, -1, &hr, TEXT_FORMAT, b.heading_font, theme.text);
  RECT tr = {left, hr.bottom + scale(LINE_GAP), left + w, rc.bottom};
  canvas_text(c, b.text, -1, &tr, TEXT_FORMAT, b.font ? b.font : (HFONT)GetStockObject(DEFAULT_GUI_FONT), theme.text);
  canvas_blit(c, dc, 0, 0, &rc);
}

/* --- UI Automation: a live region whose name is the heading and the text --- */

static BOOL ui_node(int i, uia_node *n) {
  if (i != UIA_ROOT) return FALSE;
  n->type = UIA_TextControlTypeId;
  lstrcpynW(n->name, b.spoken, (int)(sizeof n->name / sizeof *n->name));
  n->automation_id = L"FirewallBanner";
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
      HGDIOBJ owned[] = {b.icon_font, b.heading_font, b.brush};
      for (size_t i = 0; i < sizeof owned / sizeof owned[0]; i++)
        if (owned[i]) DeleteObject(owned[i]);
      b.icon_font = b.heading_font = NULL;
      b.brush = NULL;
      b.wnd = NULL;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

void firewall_banner_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = banner_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_firewall_banner"};
  RegisterClassExW(&wc);
  b.parent = parent;
  b.first_id = id + 1;
  b.dpi = GetDpiForWindow(parent);
  CreateWindowExW(0, wc.lpszClassName, NULL, WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                  wc.hInstance, NULL);
  for (int i = 0; i < NUM_BUTTONS; i++)
    b.button[i] = CreateWindowExW(0, WC_BUTTONW, NULL, WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, parent,
                                  (HMENU)(INT_PTR)(b.first_id + i), wc.hInstance, NULL);
  /* The strip goes under its buttons, and under the page: they're siblings. */
  SetWindowPos(b.wnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void firewall_banner_set_font(HFONT font, UINT dpi) {
  b.font = font;
  b.dpi = dpi;
  LOGFONTW lf;
  if (GetObjectW(font ? (HGDIOBJ)font : GetStockObject(DEFAULT_GUI_FONT), sizeof lf, &lf)) {
    lf.lfWeight = FW_SEMIBOLD;
    if (b.heading_font) DeleteObject(b.heading_font);
    b.heading_font = CreateFontIndirectW(&lf);
  }
  if (b.icon_font) DeleteObject(b.icon_font);
  b.icon_font = CreateFontW(-scale(ICON), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());
  for (int i = 0; i < NUM_BUTTONS; i++) SendMessageW(b.button[i], WM_SETFONT, (WPARAM)font, TRUE);
  if (b.wnd) InvalidateRect(b.wnd, NULL, FALSE);
}

void firewall_banner_theme_changed(void) {
  if (b.wnd) InvalidateRect(b.wnd, NULL, FALSE);
  for (int i = 0; i < NUM_BUTTONS; i++) InvalidateRect(b.button[i], NULL, TRUE);
}

/* "Home Wi-Fi (Private network), Airport Wi-Fi (Public network)", into buf. */
static void network_list(const firewall_status *st, wchar_t *buf, size_t n) {
  buf[0] = L'\0';
  for (int i = 0; i < st->nnets; i++) {
    const firewall_net *net = &st->nets[i];
    enum mc_str_id kind = net->profile == FIREWALL_PRIVATE  ? MC_STR_WINDOWS_FIREWALL_BANNER_NET_PRIVATE
                          : net->profile == FIREWALL_DOMAIN ? MC_STR_WINDOWS_FIREWALL_BANNER_NET_DOMAIN
                                                            : MC_STR_WINDOWS_FIREWALL_BANNER_NET_PUBLIC;
    wchar_t item[128];
    char *name = net->name[0] ? wstr_to_utf8(net->name) : NULL;
    if (name)
      mc_wformat(item, 128, mc_str(MC_STR_WINDOWS_FIREWALL_BANNER_NET_NAMED), MC_S(name), MC_S(mc_str(kind)));
    else
      lstrcpynW(item, mc_wstr(kind), 128);
    free(name);
    if (i) wcsncat(buf, mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR), n - wcslen(buf) - 1);
    wcsncat(buf, item, n - wcslen(buf) - 1);
  }
}

static void add_sentence(wchar_t *text, const wchar_t *more) {
  if (text[0]) wcsncat(text, L" ", TEXT_MAX - wcslen(text) - 1);
  wcsncat(text, more, TEXT_MAX - wcslen(text) - 1);
}

/* A sentence with the network list for its {0}, into text. */
static void networks_sentence(wchar_t *text, enum mc_str_id id, const wchar_t *nets) {
  char *list = wstr_to_utf8(nets);
  mc_wformat(text, TEXT_MAX, mc_str(id), MC_S(list ? list : ""));
  free(list);
}

static void set_button(int i, action act, const wchar_t *label, BOOL shield) {
  b.act[i] = act;
  if (act == ACT_NONE) return;
  SetWindowTextW(b.button[i], label);
  b.shield[i] = shield;
  Button_SetElevationRequiredState(b.button[i], shield);
}

/* Button wnd is about to go away or grey out: the keyboard's focus mustn't go with it. */
static void drop_focus(HWND wnd) {
  if (GetFocus() == wnd) SetFocus(b.parent);
}

/* Heading, text, icon and buttons for st. */
static void compose(const firewall_status *st, wchar_t *heading, wchar_t *text) {
  wchar_t nets[512];
  network_list(st, nets, sizeof nets / sizeof *nets);
  b.act[0] = b.act[1] = ACT_NONE;
  heading[0] = text[0] = L'\0';
  b.warn = TRUE;
  switch (st->state) {
    case FIREWALL_CLEAR:
      break;
    case FIREWALL_BLOCKED:
      wcscpy(heading, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_BLOCKED_TITLE));
      networks_sentence(text, MC_STR_WINDOWS_FIREWALL_BANNER_BLOCKED_TEXT, nets);
      if (st->moved) add_sentence(text, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_MOVED_TEXT));
      if (st->public_only) {
        add_sentence(text, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_PUBLIC_TEXT));
        set_button(0, ACT_NETWORK, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_BANNER_NETWORK_SETTINGS), FALSE);
        set_button(1, ACT_ALLOW, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_BANNER_ALLOW_PUBLIC), TRUE);
      } else {
        add_sentence(text, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_ALLOW_TEXT));
        set_button(0, ACT_ALLOW, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_ALLOW), TRUE);
      }
      break;
    case FIREWALL_RULE:
      wcscpy(heading, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_RULE_TITLE));
      networks_sentence(text, MC_STR_WINDOWS_FIREWALL_BANNER_RULE_TEXT, nets);
      set_button(0, ACT_ALLOWED_APPS, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_BANNER_OPEN_ALLOWED_APPS), FALSE);
      break;
    case FIREWALL_SHUT:
      wcscpy(heading, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_SHUT_TITLE));
      networks_sentence(text, MC_STR_WINDOWS_FIREWALL_BANNER_SHUT_TEXT, nets);
      set_button(0, ACT_SECURITY, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_BANNER_OPEN_SECURITY), FALSE);
      break;
    case FIREWALL_MANAGED:
      wcscpy(heading, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_MANAGED_TITLE));
      networks_sentence(text, MC_STR_WINDOWS_FIREWALL_BANNER_MANAGED_TEXT, nets);
      break;
    case FIREWALL_THIRD_PARTY: {
      b.warn = FALSE;
      char *product = wstr_to_utf8(st->product);
      const char *name = product && *product ? product : mc_str(MC_STR_WINDOWS_FIREWALL_BANNER_ANOTHER_FIREWALL);
      mc_wformat(heading, 256, mc_str(MC_STR_WINDOWS_FIREWALL_BANNER_THIRD_PARTY_TITLE), MC_S(name));
      mc_wformat(text, TEXT_MAX, mc_str(MC_STR_WINDOWS_FIREWALL_BANNER_THIRD_PARTY_TEXT), MC_S(name));
      free(product);
      break;
    }
    case FIREWALL_UNREADABLE:
      b.warn = FALSE;
      wcscpy(heading, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_UNREADABLE_TITLE));
      wcscpy(text, mc_wstr(MC_STR_WINDOWS_FIREWALL_BANNER_UNREADABLE_TEXT));
      break;
  }
}

BOOL firewall_banner_update(void) {
  const firewall_status *st = firewall_get_status();
  wchar_t heading[256], text[TEXT_MAX];
  action before[NUM_BUTTONS] = {b.act[0], b.act[1]};
  BOOL warn = b.warn;
  compose(st, heading, text);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    BOOL enable = !(st->adding && b.act[i] == ACT_ALLOW);
    if (!enable) drop_focus(b.button[i]);
    EnableWindow(b.button[i], enable);
  }
  BOOL same_text = !wcscmp(heading, b.heading) && !wcscmp(text, b.text);
  if (same_text && warn == b.warn && before[0] == b.act[0] && before[1] == b.act[1]) return FALSE;
  wcscpy(b.heading, heading);
  wcscpy(b.text, text);
  _snwprintf(b.spoken, sizeof b.spoken / sizeof *b.spoken - 1, L"%ls %ls", heading, text);
  if (b.wnd) {
    InvalidateRect(b.wnd, NULL, FALSE);
    uia_refresh(&b.ui);
    if (b.heading[0] && !same_text) uia_announce(&b.ui, UIA_ROOT);
  }
  return TRUE;
}

BOOL firewall_banner_shown(void) {
  return b.heading[0] != L'\0';
}

static int label_width(HWND button) {
  wchar_t label[128];
  int n = GetWindowTextW(button, label, 128);
  HDC dc = GetDC(button);
  HGDIOBJ old = SelectObject(dc, b.font ? (HGDIOBJ)b.font : GetStockObject(DEFAULT_GUI_FONT));
  RECT rc = {0, 0, 0, 0};
  DrawTextW(dc, label, n, &rc, DT_SINGLELINE | DT_CALCRECT); /* '&' as the button shows it */
  SelectObject(dc, old);
  ReleaseDC(button, dc);
  return rc.right;
}

/* Moves wnd unless it's there already: the page is laid out on every refresh. */
static void move(HWND wnd, int x, int y, int w, int h) {
  RECT old;
  GetWindowRect(wnd, &old);
  MapWindowPoints(NULL, b.parent, (POINT *)&old, 2);
  if (old.left != x || old.top != y || old.right != x + w || old.bottom != y + h) MoveWindow(wnd, x, y, w, h, TRUE);
}

int firewall_banner_place(BOOL visible, int x, int y, int w) {
  if (!visible || !firewall_banner_shown()) {
    ShowWindow(b.wnd, SW_HIDE);
    for (int i = 0; i < NUM_BUTTONS; i++) {
      drop_focus(b.button[i]);
      ShowWindow(b.button[i], SW_HIDE);
    }
    return 0;
  }
  int pad = scale(PAD), left = text_left(), text_w = w - left - pad;
  int heading_h = measure(b.heading_font, b.heading, text_w);
  int text_h = measure(b.font, b.text, text_w);
  int h = pad + heading_h + scale(LINE_GAP) + text_h, button_y = y + h + scale(BUTTON_GAP);
  int button_x = x + left;
  BOOL any = FALSE;
  for (int i = 0; i < NUM_BUTTONS; i++) {
    BOOL on = b.act[i] != ACT_NONE;
    if (on) {
      /* The shield takes its room in the button. */
      int shield = b.shield[i] ? scale(ICON + 4) : 0;
      int bw = label_width(b.button[i]) + scale(BUTTON_PAD) + shield;
      move(b.button[i], button_x, button_y, bw, scale(BUTTON_H));
      button_x += bw + scale(BUTTON_SPACE);
      any = TRUE;
    }
    if (!on) drop_focus(b.button[i]);
    ShowWindow(b.button[i], on ? SW_SHOWNA : SW_HIDE);
  }
  if (any) h += scale(BUTTON_GAP) + scale(BUTTON_H);
  h += pad;
  move(b.wnd, x, y, w, h);
  ShowWindow(b.wnd, SW_SHOWNA);
  return h;
}

BOOL firewall_banner_on_command(int id, int code) {
  int i = id - b.first_id;
  if (i < 0 || i >= NUM_BUTTONS) return FALSE;
  if (code != BN_CLICKED) return TRUE;
  switch (b.act[i]) {
    case ACT_ALLOW: firewall_allow(); break;
    case ACT_NETWORK: firewall_open(FIREWALL_PAGE_NETWORK); break;
    case ACT_SECURITY: firewall_open(FIREWALL_PAGE_SECURITY); break;
    case ACT_ALLOWED_APPS: firewall_open(FIREWALL_PAGE_ALLOWED_APPS); break;
    case ACT_NONE: break;
  }
  return TRUE;
}

HBRUSH firewall_banner_ctlcolor(HWND wnd, HDC dc) {
  if (wnd != b.button[0] && wnd != b.button[1]) return NULL;
  COLORREF color = tinted_bg();
  if (!b.brush || color != b.brush_color) {
    if (b.brush) DeleteObject(b.brush);
    b.brush = CreateSolidBrush(color);
    b.brush_color = color;
  }
  SetBkColor(dc, color);
  return b.brush;
}
