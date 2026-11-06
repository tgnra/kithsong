#define COBJMACROS
#include "settings_view.h"

#include <commctrl.h>
#include <oleacc.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "firewall.h"
#include "musiccore/source_run.h"
#include "playback.h"
#include "prompt_dialog.h"
#include "scrobble_service.h"
#include "scroller.h"
#include "sync_bridge.h"
#include "testing_view.h"
#include "theme.h"
#include "wstr.h"

enum {
  ID_SET_TITLE = 300,
  ID_SET_HEADING,
  ID_SET_PATH,
  ID_SET_MOVE,
  ID_SET_OPEN,
  ID_SET_NOTE,
  ID_SET_SYNC_HEADING,
  ID_SET_NAME_LABEL,
  ID_SET_NAME,
  ID_SET_ADDR_LABEL,
  ID_SET_ADDR,
  ID_SET_LEAVE,
  ID_SET_SYNC_NOTE,
  ID_SET_LFM_HEADING,
  ID_SET_LFM_STATUS,
  ID_SET_LFM_BUTTON,
  ID_SET_LFM_CHECK,
  ID_SET_LFM_CHECK_LABEL,
  ID_SET_LFM_NOTE,
  ID_SET_LFM_SERVICE,
  ID_SET_FIREWALL,
  ID_SET_PLAY_HEADING,
  ID_SET_ORDER_CHECK,
  ID_SET_ORDER_CHECK_LABEL,
  ID_SET_ORDER_NOTE,
  ID_SET_QUEUE_CHECK,
  ID_SET_QUEUE_CHECK_LABEL,
  ID_SET_QUEUE_NOTE,
  ID_SET_ANALYSIS_HEADING,
  ID_SET_ANALYSIS_STATUS,
  ID_SET_ANALYSIS_RESUME,
  ID_SET_TEST_HEADING,
  ID_SET_TEST_TOGGLE,
  ID_SET_RESET_HEADING,
  ID_SET_RESET,
  ID_SET_RESET_NOTE,
};

enum { MAX_ADDRESSES = 8 };

/* Signing in: the auth thread polls mc_scrobbler_auth_finish() every AUTH_POLL_MS until the
 * user has approved the token in the browser, for up to AUTH_LIMIT_MS (tokens last an hour,
 * but nobody waits that long). The status line is refreshed every STATUS_MS while shown,
 * since an account arriving by sync, or plays being sent, changes it. */
enum { AUTH_POLL_MS = 3000, AUTH_LIMIT_MS = 10 * 60 * 1000, STATUS_MS = 2000, STATUS_TIMER = 1 };
enum { WM_AUTH_URL = WM_APP + 1, WM_AUTH_DONE }; /* to the status label, from the auth thread */

/* At 96 DPI: a key's step, and what's kept in view around a control tabbed to, which takes in
 * its label or note. */
enum { LINE_STEP = 40, REVEAL_MARGIN = 40 };

static struct {
  HWND parent;  /* the main window */
  HWND page;    /* the window that clips the content, and has the scrollbar in its right margin */
  HWND content; /* as tall as the controls need, moved up and down in page; they're its children */
  scroller scroll;
  canvas canvas;
  BOOL tracking; /* the mouse's leaving page */
  HWND title, heading, path, move, open, note;
  HWND sync_heading, name_label, name, addr_label, addr, leave, firewall, sync_note;
  BOOL firewall_shown; /* Ask about Windows Firewall again is: firewall_can_ask_again() */
  HWND play_heading, order_check, order_check_label, order_note;
  HWND queue_check, queue_check_label, queue_note;
  HWND analysis_heading, analysis_status, analysis_resume;
  BOOL analysis_stopped;       /* Resume analysis is there */
  wchar_t analysis_shown[256]; /* the status line as last set */
  HWND lfm_heading, lfm_status, lfm_button, lfm_service, lfm_check, lfm_check_label, lfm_note;
  HWND test_heading, test_toggle; /* the Testing section's accordion: the rest is testing_view.c's */
  BOOL test_open;
  HWND reset_heading, reset, reset_note;
  const char *source_config_dir; /* running from a checkout: the settings folder; else NULL */
  BOOL reset_deletes_library;    /* so Reset takes the library with it, and says so */
  HFONT font, title_font, heading_font;
  IAccPropServices *acc; /* names order_check and queue_check for screen readers */
  BOOL shown;
  mc_settings *settings;
  void (*devices_changed)(void);
  char shown_name[MC_SYNC_DEVICE_NAME_MAX + 1]; /* what the name box was last set to */

  /* Signing in to Last.fm (or the service chosen instead). */
  HANDLE auth_thread, auth_cancel; /* while signing in; auth_cancel is an event */
  mc_status auth_result;           /* of the last sign-in, shown until the next action */
  BOOL auth_timed_out;             /* it wasn't approved in time */
  wchar_t lfm_shown[512];          /* the status line as last set */
  wchar_t lfm_button_shown[128];   /* the button's text */
  wchar_t lfm_note_shown[512];     /* and the note's, which names the service */
} g_set;

static void relayout(void);
static void update_scrobble(void);
static void update_firewall(void);
static void stop_auth(void);
static LRESULT CALLBACK lfm_status_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, g_set.content, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

static void apply_name(void);

/* Enter in the name box applies it (a plain window has no default button to take it). */
static LRESULT CALLBACK name_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        apply_name();
        SendMessageW(wnd, EM_SETSEL, 0, -1);
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r') return 0; /* no beep */
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, name_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* --- Scrolling --- */

static void scrolled(void) {
  SetWindowPos(g_set.content, NULL, 0, -g_set.scroll.pos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The controls' messages go on to the main window, as they did when it was their parent. */
static LRESULT CALLBACK content_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
      return SendMessageW(g_set.parent, msg, wp, lp);
    case WM_ERASEBKGND: /* also how themed controls draw their parent's background */
    case WM_PRINTCLIENT: {
      RECT client;
      GetClientRect(wnd, &client);
      FillRect((HDC)wp, &client, theme.bg_brush);
      return 1;
    }
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static LRESULT CALLBACK page_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      RECT client;
      GetClientRect(wnd, &client);
      if (canvas_reserve(&g_set.canvas, client.right, client.bottom)) {
        canvas_clear(&g_set.canvas, &ps.rcPaint, theme.bg, 255);
        scroller_paint(&g_set.scroll, &g_set.canvas);
        canvas_blit(&g_set.canvas, dc, ps.rcPaint.left, ps.rcPaint.top, &ps.rcPaint);
      }
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_TIMER:
      scroller_timer(&g_set.scroll, wp);
      return 0;
    case WM_MOUSEMOVE:
      if (!g_set.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        g_set.tracking = TrackMouseEvent(&tme);
      }
      scroller_mouse_move(&g_set.scroll, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      g_set.tracking = FALSE;
      scroller_mouse_leave(&g_set.scroll);
      return 0;
    case WM_LBUTTONDOWN:
      scroller_mouse_down(&g_set.scroll, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
      scroller_mouse_up(&g_set.scroll);
      return 0;
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&g_set.scroll);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* Scrolls ctl into view, with what's around it. */
static void reveal(HWND ctl) {
  RECT r;
  GetWindowRect(ctl, &r);
  MapWindowPoints(NULL, g_set.content, (POINT *)&r, 2);
  int margin = MulDiv(REVEAL_MARGIN, (int)GetDpiForWindow(g_set.page), USER_DEFAULT_SCREEN_DPI);
  scroller_reveal(&g_set.scroll, r.top - margin, r.bottom + margin);
}

/* Gives ctl the focus as Tab does in a dialog, which the main window isn't. */
static void tab_to(HWND ctl) {
  if (!ctl) return; /* nothing can take the focus */
  SendMessageW(g_set.parent, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  SetFocus(ctl);
  if (SendMessageW(ctl, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(ctl, EM_SETSEL, 0, -1);
  reveal(ctl);
}

void settings_view_focus(void) {
  if (g_set.shown) tab_to(GetNextDlgTabItem(g_set.content, NULL, FALSE));
}

/* A key for the page: Tab from anywhere in the window, the others from one of its controls,
 * unless that control uses them. */
static BOOL on_key(const MSG *msg) {
  if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return FALSE;
  BOOL in_page = IsChild(g_set.content, msg->hwnd);
  if (msg->wParam == VK_TAB) {
    tab_to(GetNextDlgTabItem(g_set.content, in_page ? msg->hwnd : NULL, GetKeyState(VK_SHIFT) < 0));
    return TRUE;
  }
  if (!in_page) return FALSE;
  int code;
  switch (msg->wParam) {
    case VK_PRIOR: code = SB_PAGEUP; break;
    case VK_NEXT: code = SB_PAGEDOWN; break;
    case VK_UP: code = SB_LINEUP; break;
    case VK_DOWN: code = SB_LINEDOWN; break;
    case VK_HOME: code = SB_TOP; break;
    case VK_END: code = SB_BOTTOM; break;
    default: return FALSE;
  }
  const LRESULT keeps = DLGC_HASSETSEL | DLGC_WANTARROWS | DLGC_WANTALLKEYS;
  if (SendMessageW(msg->hwnd, WM_GETDLGCODE, msg->wParam, (LPARAM)msg) & keeps) return FALSE;
  scroller_command(&g_set.scroll, code, MulDiv(LINE_STEP, (int)GetDpiForWindow(g_set.page), USER_DEFAULT_SCREEN_DPI));
  return TRUE;
}

BOOL settings_view_translate(const MSG *msg) {
  if (!g_set.shown || GetAncestor(msg->hwnd, GA_ROOT) != g_set.parent || !IsWindowEnabled(g_set.parent)) return FALSE;
  switch (msg->message) {
    case WM_KEYDOWN:
      return on_key(msg);
    case WM_MOUSEWHEEL: { /* whichever control is under the mouse, or has the focus */
      RECT r;
      GetWindowRect(g_set.page, &r);
      POINT pt = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
      if (!PtInRect(&r, pt)) return FALSE;
      scroller_wheel(&g_set.scroll, GET_WHEEL_DELTA_WPARAM(msg->wParam));
      return TRUE;
    }
    case WM_MOUSEMOVE: /* over the controls, which page doesn't hear of: the bar shows */
      if (IsChild(g_set.page, msg->hwnd)) {
        POINT pt = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        MapWindowPoints(msg->hwnd, g_set.page, &pt, 1);
        scroller_mouse_move(&g_set.scroll, pt.x, pt.y);
      }
      return FALSE;
  }
  return FALSE;
}

void settings_view_create(HWND parent) {
  g_set.parent = parent;
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = page_proc, .hInstance = inst,
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_settings"};
  RegisterClassExW(&wc);
  wc.lpfnWndProc = content_proc;
  wc.lpszClassName = L"kithsong_settings_content";
  RegisterClassExW(&wc);
  g_set.page = CreateWindowExW(0, L"kithsong_settings", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, NULL,
                               inst, NULL);
  g_set.content = CreateWindowExW(0, L"kithsong_settings_content", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0,
                                  0, 0, 0, g_set.page, NULL, inst, NULL);
  g_set.scroll.wnd = g_set.page;
  g_set.scroll.moved = scrolled;

  DWORD label = SS_LEFT | SS_NOPREFIX;
  g_set.title = child(WC_STATICW, mc_wstr(MC_STR_SETTINGS), label, ID_SET_TITLE);
  g_set.heading = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_LOCATION), label, ID_SET_HEADING);
  /* One line: a long path loses folders from its middle, rather than wrapping mid-name. */
  g_set.path = child(WC_STATICW, NULL, label | SS_PATHELLIPSIS, ID_SET_PATH);
  g_set.move = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_MOVE), BS_PUSHBUTTON | WS_TABSTOP, ID_SET_MOVE);
  g_set.open = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_OPEN_ANOTHER), BS_PUSHBUTTON | WS_TABSTOP,
                     ID_SET_OPEN);
  g_set.note = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_LOCATION_TEXT), label, ID_SET_NOTE);

  g_set.sync_heading = child(WC_STATICW, mc_wstr(MC_STR_SYNC), label, ID_SET_SYNC_HEADING);
  g_set.name_label = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_THIS_PC_NAME), label, ID_SET_NAME_LABEL);
  g_set.name = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, NULL, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0,
                               0, g_set.content, (HMENU)(INT_PTR)ID_SET_NAME, GetModuleHandleW(NULL), NULL);
  SendMessageW(g_set.name, EM_LIMITTEXT, MC_SYNC_DEVICE_NAME_MAX, 0);
  SetWindowSubclass(g_set.name, name_proc, 0, 0);
  theme_center_edit(g_set.name);
  g_set.addr_label = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_THIS_PC_ADDRESS), label, ID_SET_ADDR_LABEL);
  g_set.addr = child(WC_STATICW, NULL, label | SS_EDITCONTROL, ID_SET_ADDR);
  g_set.leave = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LEAVE_GROUP_BUTTON),
                      BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_SET_LEAVE);
  g_set.firewall = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_WINDOWS_FIREWALL_ASK_AGAIN), BS_PUSHBUTTON | WS_TABSTOP,
                         ID_SET_FIREWALL); /* shown by update_firewall() */
  g_set.sync_note = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SYNC_SETTINGS_NOTE), label, ID_SET_SYNC_NOTE);

  g_set.play_heading = child(WC_STATICW, mc_wstr(MC_STR_SETTINGS_PLAYBACK), label, ID_SET_PLAY_HEADING);
  /* Like Scrobbling's checkbox below: the label beside it says it and toggles it. A button
   * without text has no name for screen readers, so it's given the label's. */
  g_set.order_check = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_SET_ORDER_CHECK);
  g_set.order_check_label =
    child(WC_STATICW, mc_wstr(MC_STR_ALBUM_IN_ORDER), label | SS_NOTIFY, ID_SET_ORDER_CHECK_LABEL);
  g_set.order_note = child(WC_STATICW, mc_wstr(MC_STR_ALBUM_IN_ORDER_TEXT), label, ID_SET_ORDER_NOTE);
  g_set.queue_check = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_SET_QUEUE_CHECK);
  g_set.queue_check_label =
    child(WC_STATICW, mc_wstr(MC_STR_QUEUE_TRACKS), label | SS_NOTIFY, ID_SET_QUEUE_CHECK_LABEL);
  g_set.queue_note = child(WC_STATICW, mc_wstr(MC_STR_QUEUE_TRACKS_TEXT), label, ID_SET_QUEUE_NOTE);
  if (SUCCEEDED(CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER, &IID_IAccPropServices,
                                 (void **)&g_set.acc))) {
    IAccPropServices_SetHwndPropStr(g_set.acc, g_set.order_check, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME,
                                    mc_wstr(MC_STR_ALBUM_IN_ORDER));
    IAccPropServices_SetHwndPropStr(g_set.acc, g_set.queue_check, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME,
                                    mc_wstr(MC_STR_QUEUE_TRACKS));
  } else {
    g_set.acc = NULL;
  }

  g_set.analysis_heading = child(WC_STATICW, mc_wstr(MC_STR_ANALYSIS_HEADING), label, ID_SET_ANALYSIS_HEADING);
  g_set.analysis_status = child(WC_STATICW, NULL, label, ID_SET_ANALYSIS_STATUS);
  g_set.analysis_resume = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_ANALYSIS_RESUME), BS_PUSHBUTTON | WS_TABSTOP,
                                ID_SET_ANALYSIS_RESUME); /* shown by settings_view_update_analysis() */

  g_set.lfm_heading = child(WC_STATICW, mc_wstr(MC_STR_SCROBBLING), label, ID_SET_LFM_HEADING);
  g_set.lfm_status = child(WC_STATICW, NULL, label, ID_SET_LFM_STATUS);
  SetWindowSubclass(g_set.lfm_status, lfm_status_proc, 0, 0); /* the auth thread's news, the timer */
  g_set.lfm_button = child(WC_BUTTONW, NULL, BS_PUSHBUTTON | WS_TABSTOP, ID_SET_LFM_BUTTON); /* update_scrobble() */
  g_set.lfm_service = child(WC_BUTTONW, mc_wstr(MC_STR_SCROBBLE_CHANGE_SERVICE), BS_PUSHBUTTON | WS_TABSTOP,
                            ID_SET_LFM_SERVICE);
  /* No text of its own: a themed checkbox's text stays black in dark mode, so the label
   * beside it says it, and toggles it too (as on the device page). */
  g_set.lfm_check = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_SET_LFM_CHECK);
  g_set.lfm_check_label = child(WC_STATICW, mc_wstr(MC_STR_SCROBBLE_SWITCH), label | SS_NOTIFY, ID_SET_LFM_CHECK_LABEL);
  g_set.lfm_note = child(WC_STATICW, NULL, label, ID_SET_LFM_NOTE); /* names the service: update_scrobble() */

  g_set.test_heading = child(WC_STATICW, mc_wstr(MC_STR_TESTING_HEADING), label, ID_SET_TEST_HEADING);
  g_set.test_toggle = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_TESTING_SHOW), BS_PUSHBUTTON | WS_TABSTOP, ID_SET_TEST_TOGGLE);
  testing_view_create(g_set.content, relayout); /* here, for the tab order */

  g_set.reset_heading = child(WC_STATICW, mc_wstr(MC_STR_SETTINGS_RESET), label, ID_SET_RESET_HEADING);
  g_set.reset = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_RESET_APP_BUTTON), BS_PUSHBUTTON | WS_TABSTOP, ID_SET_RESET);
  g_set.reset_note = child(WC_STATICW, mc_wstr(MC_STR_RESET_APP_TEXT), label, ID_SET_RESET_NOTE);
  playback_on_scrobbler_closing(stop_auth);
}

void settings_view_destroy(void) {
  testing_view_destroy();
  stop_auth();
  playback_on_scrobbler_closing(NULL);
  if (g_set.acc) {
    MSAAPROPID props[] = {PROPID_ACC_NAME};
    IAccPropServices_ClearHwndProps(g_set.acc, g_set.order_check, OBJID_CLIENT, CHILDID_SELF, props, 1);
    IAccPropServices_ClearHwndProps(g_set.acc, g_set.queue_check, OBJID_CLIENT, CHILDID_SELF, props, 1);
    IAccPropServices_Release(g_set.acc);
    g_set.acc = NULL;
  }
  if (g_set.title_font) DeleteObject(g_set.title_font);
  if (g_set.heading_font) DeleteObject(g_set.heading_font);
  g_set.title_font = g_set.heading_font = NULL;
  canvas_free(&g_set.canvas);
}

void settings_view_set_source_run(const char *config_dir) {
  g_set.source_config_dir = config_dir;
}

void settings_view_set_library_dir(const char *dir) {
  wchar_t buf[1024];
  if (!dir || !MultiByteToWideChar(CP_UTF8, 0, dir, -1, buf, 1024)) buf[0] = L'\0';
  buf[1023] = L'\0';
  SetWindowTextW(g_set.path, buf);
  if (g_set.source_config_dir) {
    g_set.reset_deletes_library = mc_source_reset_deletes_library(1, g_set.source_config_dir, dir);
    SetWindowTextW(g_set.reset_note, mc_wstr(g_set.reset_deletes_library ? MC_STR_RESET_APP_TEXT_SOURCE : MC_STR_RESET_APP_TEXT));
  }
  SendMessageW(g_set.parent, WM_SIZE, 0, 0); /* the path may be cut differently */
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
  SendMessageW(g_set.sync_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.play_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.analysis_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.lfm_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.test_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.reset_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.test_toggle, WM_SETFONT, (WPARAM)font, TRUE);
  testing_view_set_font(font, dpi);
  HWND body[] = {g_set.path,       g_set.move,       g_set.open,       g_set.note,
                 g_set.name_label, g_set.name,       g_set.addr_label, g_set.addr,
                 g_set.leave,      g_set.sync_note,  g_set.order_check, g_set.order_check_label,
                 g_set.order_note, g_set.queue_check, g_set.queue_check_label, g_set.queue_note,
                 g_set.lfm_status, g_set.lfm_button,
                 g_set.lfm_service, g_set.lfm_check, g_set.lfm_check_label, g_set.lfm_note,
                 g_set.firewall, g_set.reset, g_set.reset_note, g_set.analysis_status, g_set.analysis_resume};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
}

/* After opening or closing it the page is as tall as it now needs (WM_SIZE lays it out again). */
static void relayout(void) {
  if (g_set.shown) SendMessageW(g_set.parent, WM_SIZE, 0, 0);
}

void settings_view_open_testing(BOOL open) {
  g_set.test_open = open;
  SetWindowTextW(g_set.test_toggle, mc_wstr_mnemonic(open ? MC_STR_TESTING_HIDE : MC_STR_TESTING_SHOW));
  testing_view_show(g_set.shown, open);
  relayout();
}

void settings_view_reveal_testing(void) {
  settings_view_open_testing(TRUE);
  /* It may be taller than the page, so its heading goes to the top, not just into view. */
  RECT r;
  GetWindowRect(g_set.test_heading, &r);
  MapWindowPoints(NULL, g_set.content, (POINT *)&r, 2);
  scroller_jump(&g_set.scroll, r.top);
}

void settings_view_show(BOOL show) {
  g_set.shown = show;
  HWND all[] = {g_set.title,      g_set.heading,      g_set.path,  g_set.move,       g_set.open,
                g_set.note,       g_set.sync_heading, g_set.name_label, g_set.name,  g_set.addr_label,
                g_set.addr,       g_set.leave,        g_set.sync_note, g_set.play_heading, g_set.order_check,
                g_set.order_check_label, g_set.order_note, g_set.queue_check, g_set.queue_check_label,
                g_set.queue_note, g_set.analysis_heading, g_set.analysis_status,
                g_set.lfm_heading, g_set.lfm_status,
                g_set.lfm_button, g_set.lfm_service,  g_set.lfm_check, g_set.lfm_check_label, g_set.lfm_note,
                g_set.test_heading, g_set.test_toggle, g_set.reset_heading, g_set.reset, g_set.reset_note};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(all[i], show ? SW_SHOW : SW_HIDE);
  ShowWindow(g_set.analysis_resume, show && g_set.analysis_stopped ? SW_SHOW : SW_HIDE);
  testing_view_show(show, g_set.test_open); /* its controls show only while it's open */
  ShowWindow(g_set.page, show ? SW_SHOW : SW_HIDE);
  update_firewall();
  if (show) {
    scroller_show(&g_set.scroll); /* a moment's sign that there's more below */
    settings_view_update_sync();
    update_scrobble();
    SetTimer(g_set.lfm_status, STATUS_TIMER, STATUS_MS, NULL);
  } else {
    KillTimer(g_set.lfm_status, STATUS_TIMER);
  }
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

/* Lays the controls out down content, from its top: how far they reach is what scrolls, so
 * anything that changes that only has to come through here (WM_SIZE to the main window). */
void settings_view_layout(int x, int y, int w, int h, UINT dpi) {
  if (!g_set.shown || !g_set.font) return;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int pad = S(32), text_w = min(w - 2 * pad, S(720));
  int band = S(CONTENT_RADIUS), left = 0, top = S(24) - band;

  int th = canvas_text_height(g_set.title, g_set.title_font, text_w, 0);
  MoveWindow(g_set.title, left, top, text_w, th, TRUE);
  top += th + S(24);

  int hh = canvas_text_height(g_set.heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int ph = canvas_text_height(g_set.path, g_set.font, text_w, DT_SINGLELINE);
  MoveWindow(g_set.path, left, top, text_w, ph, TRUE);
  top += ph + S(12);

  int btn_h = S(32), move_w = button_width(g_set.move, S(16)), open_w = button_width(g_set.open, S(16));
  MoveWindow(g_set.move, left, top, move_w, btn_h, TRUE);
  MoveWindow(g_set.open, left + move_w + S(8), top, open_w, btn_h, TRUE);
  top += btn_h + S(12);

  int nh = canvas_text_height(g_set.note, g_set.font, text_w, 0);
  MoveWindow(g_set.note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Sync */
  hh = canvas_text_height(g_set.sync_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.sync_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int lh = canvas_text_height(g_set.name_label, g_set.font, text_w, 0);
  MoveWindow(g_set.name_label, left, top, text_w, lh, TRUE);
  top += lh + S(4);
  MoveWindow(g_set.name, left, top, min(text_w, S(320)), S(28), TRUE);
  top += S(28) + S(12);

  lh = canvas_text_height(g_set.addr_label, g_set.font, text_w, 0);
  MoveWindow(g_set.addr_label, left, top, text_w, lh, TRUE);
  top += lh + S(4);
  int ah = canvas_text_height(g_set.addr, g_set.font, text_w, DT_EDITCONTROL);
  MoveWindow(g_set.addr, left, top, text_w, ah, TRUE);
  top += ah + S(12);

  int leave_w = button_width(g_set.leave, S(16));
  MoveWindow(g_set.leave, left, top, leave_w, btn_h, TRUE);
  MoveWindow(g_set.firewall, left + leave_w + S(8), top, button_width(g_set.firewall, S(16)), btn_h, TRUE);
  top += btn_h + S(12);

  nh = canvas_text_height(g_set.sync_note, g_set.font, text_w, 0);
  MoveWindow(g_set.sync_note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Playback */
  hh = canvas_text_height(g_set.play_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.play_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int box = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi);
  lh = canvas_text_height(g_set.order_check_label, g_set.font, text_w - box - S(8), 0);
  int row_h = max(box, lh);
  MoveWindow(g_set.order_check, left, top + (row_h - box) / 2, box, box, TRUE);
  MoveWindow(g_set.order_check_label, left + box + S(8), top + (row_h - lh) / 2, text_w - box - S(8), lh, TRUE);
  top += row_h + S(12);

  nh = canvas_text_height(g_set.order_note, g_set.font, text_w, 0);
  MoveWindow(g_set.order_note, left, top, text_w, nh, TRUE);
  top += nh + S(20);

  lh = canvas_text_height(g_set.queue_check_label, g_set.font, text_w - box - S(8), 0);
  row_h = max(box, lh);
  MoveWindow(g_set.queue_check, left, top + (row_h - box) / 2, box, box, TRUE);
  MoveWindow(g_set.queue_check_label, left + box + S(8), top + (row_h - lh) / 2, text_w - box - S(8), lh, TRUE);
  top += row_h + S(12);

  nh = canvas_text_height(g_set.queue_note, g_set.font, text_w, 0);
  MoveWindow(g_set.queue_note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Audio analysis */
  hh = canvas_text_height(g_set.analysis_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.analysis_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  nh = canvas_text_height(g_set.analysis_status, g_set.font, text_w, 0);
  MoveWindow(g_set.analysis_status, left, top, text_w, nh, TRUE);
  top += nh;
  if (g_set.analysis_stopped) {
    top += S(12);
    MoveWindow(g_set.analysis_resume, left, top, button_width(g_set.analysis_resume, S(16)), btn_h, TRUE);
    top += btn_h;
  }
  top += S(32);

  /* Scrobbling */
  hh = canvas_text_height(g_set.lfm_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.lfm_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int sh = canvas_text_height(g_set.lfm_status, g_set.font, text_w, 0);
  MoveWindow(g_set.lfm_status, left, top, text_w, sh, TRUE);
  top += sh + S(12);

  int sign_in_w = button_width(g_set.lfm_button, S(16));
  MoveWindow(g_set.lfm_button, left, top, sign_in_w, btn_h, TRUE);
  MoveWindow(g_set.lfm_service, left + sign_in_w + S(8), top, button_width(g_set.lfm_service, S(16)), btn_h, TRUE);
  top += btn_h + S(12);

  lh = canvas_text_height(g_set.lfm_check_label, g_set.font, text_w - box - S(8), 0);
  row_h = max(box, lh);
  MoveWindow(g_set.lfm_check, left, top + (row_h - box) / 2, box, box, TRUE);
  MoveWindow(g_set.lfm_check_label, left + box + S(8), top + (row_h - lh) / 2, text_w - box - S(8), lh, TRUE);
  top += row_h + S(12);

  nh = canvas_text_height(g_set.lfm_note, g_set.font, text_w, 0);
  MoveWindow(g_set.lfm_note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Testing */
  hh = canvas_text_height(g_set.test_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.test_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);
  MoveWindow(g_set.test_toggle, left, top, button_width(g_set.test_toggle, S(16)), btn_h, TRUE);
  top += btn_h + S(12);
  if (g_set.test_open) top = testing_view_layout(left, top, text_w, dpi) + S(20);
  top += S(20);

  /* Reset */
  hh = canvas_text_height(g_set.reset_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.reset_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);
  MoveWindow(g_set.reset, left, top, button_width(g_set.reset, S(16)), btn_h, TRUE);
  top += btn_h + S(12);
  nh = canvas_text_height(g_set.reset_note, g_set.font, text_w, 0);
  MoveWindow(g_set.reset_note, left, top, text_w, nh, TRUE);
  top += nh + S(24);

  /* The page leaves the main window the left margin and the band at the top, with the content
   * area's border and rounded corner: Mica's frame reaches down that far, and shows through
   * what GDI draws, so the controls mustn't scroll through it. The right margin is the page's,
   * for the scrollbar. */
  RECT view = {0, 0, max(w - pad, 0), max(h - band, 0)};
  MoveWindow(g_set.page, x + pad, y + band, view.right, view.bottom, TRUE);
  scroller_resize(&g_set.scroll, &view, top);
  SetWindowPos(g_set.content, NULL, 0, -g_set.scroll.pos, max(w - 2 * pad, 0), top, SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(g_set.page, NULL, FALSE); /* the bar's size, or that it's gone */
#undef S
}

/* --- Audio analysis --- */

void settings_view_update_analysis(BOOL stopped, int left) {
  wchar_t text[256];
  if (left > 0) {
    enum mc_str_id id = stopped ? MC_STR_ANALYSIS_STOPPED_LEFT : MC_STR_ANALYSIS_LEFT;
    mc_wformat(text, 256, mc_str_plural(id, left), MC_D(left));
  } else {
    wcscpy(text, mc_wstr(stopped ? MC_STR_ANALYSIS_STOPPED : MC_STR_ANALYSIS_NONE_LEFT));
  }
  BOOL changed = !stopped != !g_set.analysis_stopped;
  if (changed) {
    g_set.analysis_stopped = stopped;
    if (!stopped && GetFocus() == g_set.analysis_resume) SetFocus(g_set.name);
    ShowWindow(g_set.analysis_resume, g_set.shown && stopped ? SW_SHOW : SW_HIDE);
  }
  if (wcscmp(text, g_set.analysis_shown)) {
    wcscpy(g_set.analysis_shown, text);
    SetWindowTextW(g_set.analysis_status, text);
    changed = TRUE; /* it may take more lines */
  }
  if (changed && g_set.shown) SendMessageW(g_set.parent, WM_SIZE, 0, 0);
}

/* --- Sync --- */

void settings_view_init_sync(mc_settings *settings, void (*devices_changed)(void)) {
  g_set.settings = settings;
  g_set.devices_changed = devices_changed;
  BOOL on = !settings || mc_settings_scrobble(settings);
  playback_set_scrobble(on);
  SendMessageW(g_set.lfm_check, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  BOOL in_order = !settings || mc_settings_album_in_order(settings);
  SendMessageW(g_set.order_check, BM_SETCHECK, in_order ? BST_CHECKED : BST_UNCHECKED, 0);
  BOOL queue_tracks = settings && mc_settings_queue_tracks(settings);
  playback_set_queue_tracks(queue_tracks);
  SendMessageW(g_set.queue_check, BM_SETCHECK, queue_tracks ? BST_CHECKED : BST_UNCHECKED, 0);
  settings_view_update_sync();
}

/* This PC's name as sync has it, else as saved, else the host name; UTF-8 into out. */
static void current_name(char *out, size_t cap) {
  mc_sync *sync = sync_bridge_get();
  out[0] = '\0';
  if (sync && mc_sync_get_device_name(sync, out, cap) == MC_OK && out[0]) return;
  const char *saved = g_set.settings ? mc_settings_device_name(g_set.settings) : NULL;
  if (saved && *saved) {
    snprintf(out, cap, "%s", saved);
    return;
  }
  wchar_t host[256];
  DWORD n = 256;
  if (GetComputerNameExW(ComputerNameDnsHostname, host, &n) && n)
    WideCharToMultiByte(CP_UTF8, 0, host, -1, out, (int)cap, NULL, NULL);
  out[cap - 1] = '\0';
}

/* "addr:port" for each of this PC's addresses, IPv6 ones in brackets. */
static void format_addresses(wchar_t *out, size_t cap) {
  mc_sync *sync = sync_bridge_get();
  out[0] = L'\0';
  if (!sync) {
    mc_wformat(out, cap, mc_str(MC_STR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
    return;
  }
  char addrs[MAX_ADDRESSES][MC_SYNC_ADDR_STRLEN];
  int n = mc_sync_local_addresses(addrs, MAX_ADDRESSES);
  unsigned port = mc_sync_port(sync);
  if (n <= 0) {
    mc_wformat(out, cap, mc_str(MC_STR_WINDOWS_PORT_NO_NETWORK), MC_D(port));
    return;
  }
  size_t len = 0;
  for (int i = 0; i < n && len < cap; i++) {
    BOOL v6 = strchr(addrs[i], ':') != NULL;
    int w = swprintf(out + len, cap - len, L"%ls%hs%hs%hs:%u", i ? mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR) : L"",
                     v6 ? "[" : "", addrs[i],
                     v6 ? "]" : "", port);
    if (w < 0) break;
    len += (size_t)w;
  }
}

/* Whether this PC is in a group: listed devices, its own row included, that aren't removed. */
static BOOL in_group(void) {
  mc_sync *sync = sync_bridge_get();
  mc_sync_device_list *list = NULL;
  if (!sync || mc_sync_list_devices(sync, &list) != MC_OK) return FALSE;
  BOOL member = FALSE;
  for (int i = 0; i < list->count && !member; i++) member = !list->items[i].removed;
  mc_sync_device_list_free(list);
  return member;
}

/* Ask about Windows Firewall again: shown once the user said not to ask, enabled while sync
 * runs (the check needs it). */
static void update_firewall(void) {
  if (!g_set.firewall) return;
  BOOL show = g_set.shown && firewall_can_ask_again(g_set.settings);
  EnableWindow(g_set.firewall, sync_bridge_get() != NULL);
  if (show == g_set.firewall_shown) return;
  g_set.firewall_shown = show;
  if (!show && GetFocus() == g_set.firewall) SetFocus(IsWindowEnabled(g_set.leave) ? g_set.leave : g_set.name);
  ShowWindow(g_set.firewall, show ? SW_SHOW : SW_HIDE);
}

static void ask_firewall_again(void) {
  firewall_ask_again(g_set.parent, g_set.settings);
  update_firewall();
}

void settings_view_update_sync(void) {
  if (!g_set.name) return;
  if (GetFocus() != g_set.name) {
    current_name(g_set.shown_name, sizeof g_set.shown_name);
    wchar_t w[MC_SYNC_DEVICE_NAME_MAX + 1];
    if (!MultiByteToWideChar(CP_UTF8, 0, g_set.shown_name, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1)) w[0] = L'\0';
    SetWindowTextW(g_set.name, w);
  }
  wchar_t addr[1024], old[1024];
  format_addresses(addr, 1024);
  GetWindowTextW(g_set.addr, old, 1024);
  if (wcscmp(addr, old)) {
    SetWindowTextW(g_set.addr, addr);
    if (g_set.shown) SendMessageW(g_set.parent, WM_SIZE, 0, 0); /* it may wrap differently */
  }
  EnableWindow(g_set.leave, in_group());
  update_firewall();
}

/* Trims spaces at either end, in place. */
static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
  return s;
}

/* Renames this PC to what's in the name box, if it changed. An empty box goes back to the
 * current name. */
static void apply_name(void) {
  wchar_t w[MC_SYNC_DEVICE_NAME_MAX + 1];
  char buf[4 * (MC_SYNC_DEVICE_NAME_MAX + 1)];
  GetWindowTextW(g_set.name, w, MC_SYNC_DEVICE_NAME_MAX + 1);
  if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL)) buf[0] = '\0';
  char *name = trim(buf);
  if (!*name || !strcmp(name, g_set.shown_name)) {
    current_name(g_set.shown_name, sizeof g_set.shown_name);
    if (MultiByteToWideChar(CP_UTF8, 0, g_set.shown_name, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1))
      SetWindowTextW(g_set.name, w);
    return;
  }

  mc_sync *sync = sync_bridge_get();
  mc_status st = sync ? mc_sync_set_device_name(sync, name) : MC_OK;
  if (st != MC_OK && st != MC_ERR_UNIMPLEMENTED) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_RENAME_SELF_FAILED), MC_S(mc_status_str(st)));
    message_dialog(g_set.parent, mc_wstr(MC_STR_SETTINGS), text, MB_ICONERROR);
    if (MultiByteToWideChar(CP_UTF8, 0, g_set.shown_name, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1))
      SetWindowTextW(g_set.name, w); /* back to the name it has, so it isn't tried again */
  } else if (g_set.settings) {
    /* sync may have cut it to fit; save what it now goes by */
    char saved[MC_SYNC_DEVICE_NAME_MAX + 1];
    if (!sync || mc_sync_get_device_name(sync, saved, sizeof saved) != MC_OK || !saved[0])
      snprintf(saved, sizeof saved, "%s", name);
    mc_settings_set_device_name(g_set.settings, saved);
    if (mc_settings_save(g_set.settings) != MC_OK)
      message_dialog(g_set.parent, mc_wstr(MC_STR_SETTINGS), mc_wstr(MC_STR_NAME_SAVE_FAILED), MB_ICONWARNING);
    snprintf(g_set.shown_name, sizeof g_set.shown_name, "%s", saved);
    if (MultiByteToWideChar(CP_UTF8, 0, saved, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1)) SetWindowTextW(g_set.name, w);
  }
  if (g_set.devices_changed) g_set.devices_changed();
}

static void leave_group(void) {
  mc_sync *sync = sync_bridge_get();
  if (!sync) return;
  if (!confirm_dialog(g_set.parent, mc_wstr(MC_STR_LEAVE_GROUP), mc_wstr(MC_STR_WINDOWS_PC_LEAVE_GROUP_CONFIRM),
                      MB_ICONWARNING, mc_wstr_mnemonic(MC_STR_LEAVE_GROUP), mc_wstr_mnemonic(MC_STR_CANCEL), TRUE))
    return;
  HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
  mc_status st = mc_sync_leave_group(sync); /* blocks for a few seconds at most */
  SetCursor(old);
  if (st == MC_ERR_LOCKED)
    message_dialog(g_set.parent, mc_wstr(MC_STR_LEAVE_GROUP), mc_wstr(MC_STR_WINDOWS_LEAVE_ONLY_ARCHIVE),
                   MB_ICONINFORMATION);
  else if (st != MC_OK && st != MC_ERR_NOT_FOUND) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_LEAVE_GROUP_FAILED), MC_S(mc_status_str(st)));
    message_dialog(g_set.parent, mc_wstr(MC_STR_LEAVE_GROUP), text, MB_ICONERROR);
  }
  if (g_set.devices_changed) g_set.devices_changed();
  settings_view_update_sync();
}

/* --- Reset --- */

/* Reset Kithsong…: once confirmed, the main window marks the settings folder and closes
 * (WM_APP_RESET); the next start clears the folder. */
static void reset_app(void) {
  if (!confirm_dialog(g_set.parent, mc_wstr(MC_STR_RESET_APP_TITLE),
                      mc_wstr(g_set.reset_deletes_library ? MC_STR_RESET_APP_CONFIRM_SOURCE : MC_STR_RESET_APP_CONFIRM),
                      MB_ICONWARNING, mc_wstr_mnemonic(MC_STR_RESET_APP_CONFIRM_BUTTON), mc_wstr_mnemonic(MC_STR_CANCEL),
                      TRUE))
    return;
  PostMessageW(g_set.parent, WM_APP_RESET, 0, 0);
}

/* --- Scrobbling --- */

/* Brings the Scrobbling section up to date with the scrobbler: the service, signed in or
 * not, whether the build can sign in at all, plays waiting and the last error. */
static void update_scrobble(void) {
  if (!g_set.lfm_status) return;
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st = {0};
  BOOL have = sc && mc_scrobbler_status_get(sc, &st) == MC_OK;
  BOOL waiting = g_set.auth_thread != NULL;
  const char *service = have && st.service_name ? st.service_name : mc_str(MC_STR_SCROBBLE_SERVICE);
  wchar_t text[512], line[256], button[128], note[512];
  mc_wformat(text, 511, mc_str(MC_STR_WINDOWS_SCROBBLE_SERVICE_CURRENT), MC_S(service));
  wcscat(text, L"\n"); /* room was left for it */
  size_t len = wcslen(text);
  wstr_mnemonic_format(button, 128, MC_STR_SCROBBLE_SIGN_IN, service);
  BOOL enabled = have;

  if (waiting) {
    mc_wformat(text + len, 512 - len, mc_str(MC_STR_SCROBBLE_WAITING), MC_S(service));
    wcscpy(button, mc_wstr_mnemonic(MC_STR_SCROBBLE_CANCEL_SIGN_IN));
    enabled = TRUE;
  } else if (have && st.username) {
    char *name = st.username;
    mc_wformat(text + len, 512 - len, mc_str(MC_STR_SCROBBLE_SIGNED_IN), MC_S(name));
    wcscpy(button, mc_wstr_mnemonic(MC_STR_SCROBBLE_SIGN_OUT));
    line[0] = L'\0';
    if (st.last_error == MC_ERR_AUTH) mc_wformat(line, 256, mc_str(MC_STR_SCROBBLE_REFUSED), MC_S(service));
    else if (st.last_error == MC_ERR_NET) mc_wformat(line, 256, mc_str(MC_STR_SCROBBLE_OFFLINE), MC_S(service));
    else if (st.pending > 0) mc_wformat(line, 256, mc_str_plural(MC_STR_SCROBBLE_PENDING, st.pending), MC_D(st.pending));
    if (line[0]) {
      wcsncat(text, L"\n", 511 - wcslen(text));
      wcsncat(text, line, 511 - wcslen(text));
    }
  } else if (have && !st.has_key) {
    wcsncat(text, mc_wstr(MC_STR_SCROBBLE_NO_KEY), 511 - len);
    enabled = FALSE;
  } else if (have && st.last_error == MC_ERR_AUTH) {
    mc_wformat(text + len, 512 - len, mc_str(MC_STR_SCROBBLE_ENDED), MC_S(service));
  } else {
    wcsncat(text, mc_wstr(MC_STR_SCROBBLE_SIGNED_OUT), 511 - len);
  }
  if (!waiting && g_set.auth_result != MC_OK) {
    if (g_set.auth_timed_out)
      wcscpy(line, mc_wstr(MC_STR_SCROBBLE_SIGN_IN_TIMED_OUT));
    else
      mc_wformat(line, 256, mc_str(MC_STR_SCROBBLE_SIGN_IN_FAILED), MC_S(mc_status_str(g_set.auth_result)));
    wcsncat(text, L"\n", 511 - wcslen(text));
    wcsncat(text, line, 511 - wcslen(text));
  }
  mc_wformat(note, 512, mc_str(MC_STR_SCROBBLE_NOTE), MC_S(service));
  if (have) mc_scrobbler_status_clear(&st); /* not before: service may point into it */

  EnableWindow(g_set.lfm_button, enabled);
  EnableWindow(g_set.lfm_service, sc != NULL);
  BOOL relayout = FALSE;
  if (wcscmp(button, g_set.lfm_button_shown)) {
    wcscpy(g_set.lfm_button_shown, button);
    SetWindowTextW(g_set.lfm_button, button);
    relayout = TRUE; /* its width */
  }
  if (wcscmp(note, g_set.lfm_note_shown)) {
    wcscpy(g_set.lfm_note_shown, note);
    SetWindowTextW(g_set.lfm_note, note);
    relayout = TRUE; /* it may take more lines */
  }
  if (wcscmp(text, g_set.lfm_shown)) {
    wcscpy(g_set.lfm_shown, text);
    SetWindowTextW(g_set.lfm_status, text);
    relayout = TRUE; /* it may take more lines */
  }
  if (relayout && g_set.shown) SendMessageW(g_set.parent, WM_SIZE, 0, 0);
}

typedef struct auth_job {
  mc_scrobbler *sc;
  HWND notify;
  HANDLE cancel;
} auth_job;

/* The auth thread: gets a token and the page to approve it on (posted as WM_AUTH_URL, the
 * URL to free with mc_free()), then polls until it's approved, cancelled or too late; posts
 * WM_AUTH_DONE with the result. */
static DWORD WINAPI auth_main(void *arg) {
  auth_job *job = arg;
  char *url = NULL;
  mc_status st = mc_scrobbler_auth_begin(job->sc, &url);
  BOOL polled = FALSE;
  if (st == MC_OK) {
    polled = TRUE;
    if (!PostMessageW(job->notify, WM_AUTH_URL, 0, (LPARAM)url)) mc_free(url);
    ULONGLONG end = GetTickCount64() + AUTH_LIMIT_MS;
    do {
      if (WaitForSingleObject(job->cancel, AUTH_POLL_MS) == WAIT_OBJECT_0) break; /* stop_auth() */
      st = mc_scrobbler_auth_finish(job->sc);
    } while (st == MC_ERR_AUTH && GetTickCount64() < end);
  }
  PostMessageW(job->notify, WM_AUTH_DONE, (WPARAM)(INT_PTR)st, polled && st == MC_ERR_AUTH);
  free(job);
  return 0;
}

static void start_auth(void) {
  mc_scrobbler *sc = playback_scrobbler();
  if (!sc || g_set.auth_thread) return;
  auth_job *job = malloc(sizeof *job);
  HANDLE cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!job || !cancel) {
    free(job);
    if (cancel) CloseHandle(cancel);
    return;
  }
  *job = (auth_job){sc, g_set.lfm_status, cancel};
  g_set.auth_result = MC_OK;
  g_set.auth_timed_out = FALSE;
  g_set.auth_cancel = cancel;
  g_set.auth_thread = CreateThread(NULL, 0, auth_main, job, 0, NULL);
  if (!g_set.auth_thread) {
    free(job);
    CloseHandle(cancel);
    g_set.auth_cancel = NULL;
  }
  update_scrobble();
}

/* Cancels a sign-in in progress and waits for its thread, which may be in the middle of a
 * call to the service (cut short by mc_scrobbler_auth_cancel()). Also called just before
 * the scrobbler is freed. */
static void stop_auth(void) {
  if (!g_set.auth_thread) return;
  SetEvent(g_set.auth_cancel);
  mc_scrobbler_auth_cancel(playback_scrobbler());
  WaitForSingleObject(g_set.auth_thread, INFINITE);
  CloseHandle(g_set.auth_thread);
  CloseHandle(g_set.auth_cancel);
  g_set.auth_thread = g_set.auth_cancel = NULL;
  /* Its WM_AUTH_URL may still be queued: drop it, and the URL with it. */
  MSG msg;
  while (PeekMessageW(&msg, g_set.lfm_status, WM_AUTH_URL, WM_AUTH_DONE, PM_REMOVE))
    if (msg.message == WM_AUTH_URL) mc_free((void *)msg.lParam);
  update_scrobble();
}

static void sign_out(const char *service) {
  mc_scrobbler *sc = playback_scrobbler();
  if (!sc) return;
  wchar_t title[256];
  mc_wformat(title, 256, mc_str(MC_STR_SCROBBLE_SIGN_OUT_TITLE), MC_S(service));
  if (!confirm_dialog(g_set.parent, title, mc_wstr(MC_STR_SCROBBLE_SIGN_OUT_TEXT), MB_ICONQUESTION,
                      mc_wstr_mnemonic(MC_STR_SCROBBLE_SIGN_OUT), mc_wstr_mnemonic(MC_STR_CANCEL), TRUE))
    return;
  g_set.auth_result = mc_scrobbler_sign_out(sc);
  g_set.auth_timed_out = FALSE;
  update_scrobble();
}

/* Use a Different Service: the dialog, then the service it chose, which signs out if it's
 * another one. A sign-in under way is given up first, since it's for the old service. */
static void change_service(void) {
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st;
  if (!sc || mc_scrobbler_status_get(sc, &st) != MC_OK) return;
  scrobble_service_choice c;
  BOOL chosen = scrobble_service_show(g_set.parent, st.service, st.api_url, st.auth_url, &c);
  mc_scrobbler_status_clear(&st);
  if (!chosen) return;
  stop_auth();
  sc = playback_scrobbler(); /* the library may have changed while the dialog was open */
  mc_status r = sc ? mc_scrobbler_set_service(sc, c.api_url, c.auth_url, c.api_key, c.api_secret) : MC_OK;
  scrobble_service_choice_clear(&c);
  g_set.auth_result = MC_OK;
  g_set.auth_timed_out = FALSE;
  if (r != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_SCROBBLE_SERVICE_FAILED), MC_S(mc_status_str(r)));
    message_dialog(g_set.parent, mc_wstr(MC_STR_SCROBBLE_SERVICE_TITLE), text, MB_ICONERROR);
  }
  update_scrobble();
}

static void on_scrobble_button(void) {
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st;
  if (g_set.auth_thread) {
    stop_auth();
  } else if (sc && mc_scrobbler_status_get(sc, &st) == MC_OK) {
    if (st.username) sign_out(st.service_name ? st.service_name : mc_str(MC_STR_SCROBBLE_SERVICE));
    else start_auth();
    mc_scrobbler_status_clear(&st);
  }
}

static void on_scrobble_check(void) {
  BOOL on = SendMessageW(g_set.lfm_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
  playback_set_scrobble(on);
  if (g_set.settings) {
    mc_settings_set_scrobble(g_set.settings, on);
    mc_settings_save(g_set.settings);
  }
}

/* Only what's added from now on is affected. */
static void on_queue_tracks_check(void) {
  BOOL on = SendMessageW(g_set.queue_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
  playback_set_queue_tracks(on);
  if (g_set.settings) {
    mc_settings_set_queue_tracks(g_set.settings, on);
    mc_settings_save(g_set.settings); /* failing, it's saved with the next setting */
  }
}

/* Playing a track reads it from the settings main.c keeps, these ones. */
static void on_album_in_order_check(void) {
  BOOL on = SendMessageW(g_set.order_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
  if (g_set.settings) {
    mc_settings_set_album_in_order(g_set.settings, on);
    mc_settings_save(g_set.settings); /* failing, it's saved with the next setting */
  }
}

static LRESULT CALLBACK lfm_status_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_AUTH_URL: {
      char *url = (char *)lp;
      wchar_t w[2048];
      if (wstr_from_utf8(w, 2048, url)) ShellExecuteW(g_set.parent, L"open", w, NULL, NULL, SW_SHOWNORMAL);
      mc_free(url);
      return 0;
    }
    case WM_AUTH_DONE: {
      mc_status st = (mc_status)(INT_PTR)wp;
      if (g_set.auth_thread) {
        WaitForSingleObject(g_set.auth_thread, INFINITE); /* it's just posted this */
        CloseHandle(g_set.auth_thread);
        CloseHandle(g_set.auth_cancel);
        g_set.auth_thread = g_set.auth_cancel = NULL;
      }
      g_set.auth_result = st;
      g_set.auth_timed_out = lp != 0;
      if (st == MC_OK) SetForegroundWindow(g_set.parent); /* back from the browser */
      update_scrobble();
      return 0;
    }
    case WM_TIMER:
      if (wp == STATUS_TIMER) {
        update_scrobble();
        update_firewall(); /* "Don't ask again" may just have been ticked */
        return 0;
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, lfm_status_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

BOOL settings_view_on_command(int id, int code) {
  switch (id) {
    case ID_SET_OPEN: PostMessageW(g_set.parent, WM_APP_OPEN_LIBRARY, 0, 0); return TRUE;
    case ID_SET_MOVE: PostMessageW(g_set.parent, WM_APP_MOVE_LIBRARY, 0, 0); return TRUE;
    case ID_SET_NAME:
      if (code == EN_KILLFOCUS) apply_name();
      return TRUE;
    case ID_SET_LEAVE: leave_group(); return TRUE;
    case ID_SET_ANALYSIS_RESUME: PostMessageW(g_set.parent, WM_APP_RESUME_ANALYSIS, 0, 0); return TRUE;
    case ID_SET_RESET: reset_app(); return TRUE;
    case ID_SET_TEST_TOGGLE: settings_view_open_testing(!g_set.test_open); return TRUE;
    case ID_SET_FIREWALL: ask_firewall_again(); return TRUE;
    case ID_SET_LFM_BUTTON: on_scrobble_button(); return TRUE;
    case ID_SET_LFM_SERVICE: change_service(); return TRUE;
    case ID_SET_LFM_CHECK:
      if (code == BN_CLICKED) on_scrobble_check();
      return TRUE;
    case ID_SET_LFM_CHECK_LABEL:
      if (code == STN_CLICKED) {
        BOOL on = SendMessageW(g_set.lfm_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(g_set.lfm_check, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        on_scrobble_check();
      }
      return TRUE;
    case ID_SET_ORDER_CHECK:
      if (code == BN_CLICKED) on_album_in_order_check();
      return TRUE;
    case ID_SET_ORDER_CHECK_LABEL:
      if (code == STN_CLICKED) {
        BOOL on = SendMessageW(g_set.order_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(g_set.order_check, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        on_album_in_order_check();
      }
      return TRUE;
    case ID_SET_QUEUE_CHECK:
      if (code == BN_CLICKED) on_queue_tracks_check();
      return TRUE;
    case ID_SET_QUEUE_CHECK_LABEL:
      if (code == STN_CLICKED) {
        BOOL on = SendMessageW(g_set.queue_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(g_set.queue_check, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        on_queue_tracks_check();
      }
      return TRUE;
    default: return testing_view_on_command(id, code);
  }
}

HBRUSH settings_view_ctlcolor(HWND wnd, HDC dc) {
  if (wnd == g_set.lfm_check || wnd == g_set.order_check || wnd == g_set.queue_check) { /* WM_CTLCOLORBTN: just its background */
    SetBkColor(dc, theme.bg);
    return theme.bg_brush;
  }
  HBRUSH testing = testing_view_ctlcolor(wnd, dc);
  if (testing) return testing;
  BOOL note = wnd == g_set.note || wnd == g_set.sync_note || wnd == g_set.order_note || wnd == g_set.queue_note ||
              wnd == g_set.lfm_note;
  if (!note && wnd != g_set.title && wnd != g_set.heading && wnd != g_set.path && wnd != g_set.sync_heading &&
      wnd != g_set.name_label && wnd != g_set.addr_label && wnd != g_set.addr && wnd != g_set.play_heading &&
      wnd != g_set.order_check_label && wnd != g_set.queue_check_label && wnd != g_set.analysis_heading &&
      wnd != g_set.analysis_status &&
      wnd != g_set.lfm_heading && wnd != g_set.lfm_status &&
      wnd != g_set.lfm_check_label && wnd != g_set.test_heading)
    return NULL;
  SetTextColor(dc, note ? theme.text2 : theme.text);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
