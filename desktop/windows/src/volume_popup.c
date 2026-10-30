#include "volume_popup.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uiautomation.h>
#include <windowsx.h>

#include "canvas.h"
#include "playback.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

/* Newer than some SDK headers. */
#define DWMWA_CORNERS 33 /* DWMWA_WINDOW_CORNER_PREFERENCE, Windows 11 */
#define CORNERS_ROUND_SMALL 3

/* Metrics at 96 DPI. */
enum {
  PAD = 8,
  BUTTON = 36, /* the speaker, as big as the toolbar's buttons */
  GAP = 8,     /* between the speaker and the slider */
  SLIDER_W = 200,
  END_PAD = 16, /* after the slider, so the thumb at 100 stands in as far as the speaker's glyph */
  DROP = 4,     /* between the toolbar button and the popup */
  RAIL_H = 4,
  THUMB = 20,        /* Windows 11's slider's; the seekbar's is smaller, to fit the toolbar */
  KNOB = 12,         /* the accent dot inside it */
  KNOB_HOT = 14,     /* ...under the mouse */
  KNOB_PRESSED = 10, /* ...while dragging */
  KEY_STEP = 5,      /* as the volume shortcuts */
  PAGE_STEP = 20,
};

/* The two parts, which are also their UI Automation node numbers. */
enum { PART_NONE = -1, PART_SPEAKER, PART_SLIDER };

static struct {
  HWND wnd, anchor, tooltip;
  HWND refocus; /* had the focus in the main window before it opened */
  volume_popup_host host;
  UINT dpi;
  HFONT icon_font;
  canvas buf;
  BOOL rounded; /* Windows rounds the corners and draws the border */

  /* Layout, in device pixels. */
  RECT speaker, bar;  /* bar: the row the rail and thumb sit in */
  int focus, hot;     /* parts */
  BOOL pressed;       /* on the speaker */
  BOOL dragging, tracking;
  int wheel;          /* the wheel's turn short of a notch (WHEEL_DELTA), not acted on yet */
  BOOL closing;       /* volume_popup_close() is handing back to the main window */
  BOOL anchor_closed; /* a press on the speaker button closed it, at closed_at */
  DWORD closed_at;
} s;

/* Outside s, which is cleared when the window goes: elements clients still hold use it. */
static uia_host ui;

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

wchar_t volume_popup_glyph(void) {
  int volume = playback_volume();
  if (playback_muted()) return 0xE74F; /* Mute */
  if (volume == 0) return 0xE992;      /* Volume0 */
  return volume < 34 ? 0xE993 : volume < 67 ? 0xE994 : 0xE995; /* Volume1 to Volume3 */
}

BOOL volume_popup_is_open(void) {
  return s.wnd && IsWindowVisible(s.wnd);
}

/* --- Layout --- */

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int pad = scale(PAD), button = scale(BUTTON), thumb = scale(THUMB), cy = rc.bottom / 2;
  SetRect(&s.speaker, pad, cy - button / 2, pad + button, cy - button / 2 + button);
  SetRect(&s.bar, s.speaker.right + scale(GAP), cy - thumb / 2, rc.right - scale(END_PAD), cy - thumb / 2 + thumb);
  if (s.bar.right < s.bar.left) s.bar.right = s.bar.left;
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = s.wnd, .uId = PART_SPEAKER + 1, .rect = s.speaker};
  SendMessageW(s.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

/* The thumb's centre for a volume, and the volume for an x: the thumb stays inside the bar. */
static int thumb_x(int volume) {
  int travel = s.bar.right - s.bar.left - scale(THUMB);
  return s.bar.left + scale(THUMB) / 2 + (travel > 0 ? MulDiv(travel, volume, 100) : 0);
}

static int volume_at(int x) {
  int travel = s.bar.right - s.bar.left - scale(THUMB);
  if (travel <= 0) return 0;
  int along = x - s.bar.left - scale(THUMB) / 2;
  return MulDiv(max(0, min(along, travel)), 100, travel);
}

/* The bar takes clicks from the whole height of the popup. */
static int part_at(int x, int y) {
  POINT pt = {x, y};
  RECT client, bar;
  GetClientRect(s.wnd, &client);
  SetRect(&bar, s.bar.left, client.top, s.bar.right, client.bottom);
  return PtInRect(&s.speaker, pt) ? PART_SPEAKER : PtInRect(&bar, pt) ? PART_SLIDER : PART_NONE;
}

/* --- What it changes --- */

static void set_volume(int volume) {
  volume = max(0, min(volume, 100));
  if ((volume != playback_volume() || playback_muted()) && s.host.set_volume) s.host.set_volume(volume);
}

static void toggle_muted(void) {
  if (s.host.set_muted) s.host.set_muted(!playback_muted());
}

static void set_focus(int part) {
  if (part == s.focus) return;
  s.focus = part;
  InvalidateRect(s.wnd, NULL, FALSE);
  uia_focus(&ui);
}

static void set_hot(int part) {
  if (part == s.hot) return;
  s.hot = part;
  InvalidateRect(s.wnd, NULL, FALSE);
}

/* "Mute (Ctrl+M)", or Unmute. */
static void update_tip(void) {
  wchar_t tip[128];
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = s.wnd, .uId = PART_SPEAKER + 1,
                    .lpszText = wstr_tooltip(tip, 128, playback_muted() ? MC_STR_UNMUTE : MC_STR_MUTE,
                                             MC_STR_WINDOWS_SHORTCUT_MUTE)};
  SendMessageW(s.tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

/* --- Painting --- */

/* Windows 11's slider, as the seekbar draws it (now_playing.c), in grey while muted. */
static void paint_bar(canvas *c) {
  int rail_h = scale(RAIL_H), cy = (s.bar.top + s.bar.bottom) / 2, x = thumb_x(playback_volume());
  RECT rail = {s.bar.left, cy - rail_h / 2, s.bar.right, cy - rail_h / 2 + rail_h};
  COLORREF ink = theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  COLORREF fill = playback_muted() ? theme.text3 : theme.accent;
  canvas_fill(c, &rail, rail_h / 2, theme.high_contrast ? theme.text : ink, theme.high_contrast ? 255 : theme.dark ? 139 : 114);
  RECT done = rail;
  done.right = x;
  canvas_fill(c, &done, rail_h / 2, fill, 255);

  BOOL hot = s.hot == PART_SLIDER;
  int thumb = scale(THUMB), knob = scale(s.dragging ? KNOB_PRESSED : hot ? KNOB_HOT : KNOB);
  RECT outer = {x - thumb / 2, cy - thumb / 2, x - thumb / 2 + thumb, cy - thumb / 2 + thumb};
  RECT inner = {x - knob / 2, cy - knob / 2, x - knob / 2 + knob, cy - knob / 2 + knob};
  if (theme.high_contrast) {
    canvas_fill(c, &outer, thumb / 2, theme.bg, 255);
    canvas_frame(c, &outer, thumb / 2, 1, theme.text, 255);
  } else {
    canvas_fill(c, &outer, thumb / 2, theme.dark ? RGB(0x45, 0x45, 0x45) : RGB(255, 255, 255), 255);
    canvas_frame(c, &outer, thumb / 2, 1, ink, theme.dark ? 24 : 30);
  }
  canvas_fill(c, &inner, knob / 2, fill, 255);
}

static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  canvas_clear(c, &client, theme.bg, 255);
  if (!s.rounded) canvas_frame(c, &client, 0, 1, theme.high_contrast ? theme.text : theme.line, 255);
  BOOL cues = GetFocus() == s.wnd && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);

  /* The speaker, as the toolbar's buttons are drawn. */
  control_state state = s.hot != PART_SPEAKER ? STATE_NORMAL : s.pressed ? STATE_PRESSED : STATE_HOT;
  int radius = scale(6);
  COLORREF color = theme_draw_state(c, &s.speaker, radius, state);
  wchar_t glyph = volume_popup_glyph();
  canvas_text(c, &glyph, 1, &s.speaker, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, s.icon_font, color);
  if (cues && s.focus == PART_SPEAKER) canvas_frame(c, &s.speaker, radius, scale(2), theme.text, 255);

  paint_bar(c);
  if (cues && s.focus == PART_SLIDER) {
    RECT ring = s.bar;
    InflateRect(&ring, scale(4), scale(3));
    canvas_frame(c, &ring, scale(4), scale(2), theme.text, 255);
  }
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Opening and closing --- */

/* Something else is active now: activated, or NULL for another app's window. Unless that's
 * the main window, it stops looking active, as it only did for the popup's sake. */
static void hide(HWND activated) {
  if (!volume_popup_is_open()) return;
  HWND owner = GetWindow(s.wnd, GW_OWNER);
  if (s.dragging || s.pressed) ReleaseCapture();
  ShowWindow(s.wnd, SW_HIDE);
  if (activated != owner) SendMessageW(owner, WM_NCACTIVATE, FALSE, 0);
  InvalidateRect(s.anchor, NULL, FALSE);
}

void volume_popup_close(void) {
  if (!volume_popup_is_open()) return;
  HWND owner = GetWindow(s.wnd, GW_OWNER), refocus = s.refocus;
  s.closing = TRUE;
  SetActiveWindow(owner); /* which hides it (WM_ACTIVATE) */
  s.closing = FALSE;
  hide(owner);
  if (refocus && IsWindow(refocus) && GetActiveWindow() == owner) SetFocus(refocus);
}

void volume_popup_update(void) {
  if (!s.wnd) return;
  update_tip();
  InvalidateRect(s.wnd, NULL, FALSE);
  uia_refresh(&ui);
}

/* --- UI Automation: the speaker is a button, the slider is named by its level --- */

static BOOL ui_node(int i, uia_node *n) {
  if (i == UIA_ROOT) {
    lstrcpynW(n->name, mc_wstr(MC_STR_VOLUME), 512);
    n->first = PART_SPEAKER;
    n->count = 2;
    return TRUE;
  }
  if (i != PART_SPEAKER && i != PART_SLIDER) return FALSE;
  n->parent = UIA_ROOT;
  n->flags = UIA_FOCUSABLE;
  if (i == PART_SPEAKER) {
    n->type = UIA_ButtonControlTypeId;
    n->flags |= UIA_INVOKABLE;
    n->rect = s.speaker;
    n->automation_id = L"Mute";
    lstrcpynW(n->name, mc_wstr(playback_muted() ? MC_STR_UNMUTE : MC_STR_MUTE), 512);
    wstr_tooltip(n->help, 256, playback_muted() ? MC_STR_UNMUTE : MC_STR_MUTE, MC_STR_WINDOWS_SHORTCUT_MUTE);
  } else {
    n->type = UIA_SliderControlTypeId;
    n->rect = s.bar;
    n->automation_id = L"Volume";
    mc_wformat(n->name, 512, mc_str(MC_STR_VOLUME_LEVEL), MC_D(playback_volume()));
    if (playback_muted()) lstrcpynW(n->status, mc_wstr(MC_STR_VOLUME_MUTED), 64);
  }
  return TRUE;
}

static int ui_focus(void) {
  return s.focus;
}

static int ui_at(POINT pt) {
  int part = part_at(pt.x, pt.y);
  return part == PART_NONE ? UIA_ROOT : part;
}

static void ui_invoke(int i) {
  if (i == PART_SPEAKER) toggle_muted();
}

static void ui_set_focus(int i) {
  set_focus(i);
}

static const uia_ops ui_ops = {.node = ui_node, .focus = ui_focus, .at = ui_at, .invoke = ui_invoke,
                               .set_focus = ui_set_focus};

/* --- Window --- */

/* The arrow keys, + and - move the volume by as much as the shortcuts do, wherever the focus
 * is; Space and Enter press the speaker. */
static void on_key(WPARAM key) {
  SendMessageW(s.wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  int volume = playback_volume();
  switch (key) {
    case VK_ESCAPE: volume_popup_close(); break;
    case VK_TAB: set_focus(s.focus == PART_SPEAKER ? PART_SLIDER : PART_SPEAKER); break;
    case VK_SPACE:
    case VK_RETURN:
      if (s.focus == PART_SPEAKER) toggle_muted();
      else if (key == VK_RETURN) volume_popup_close();
      break;
    case 'M':
      if (GetKeyState(VK_CONTROL) < 0) toggle_muted();
      break;
    case VK_LEFT:
    case VK_DOWN:
    case VK_OEM_MINUS:
    case VK_SUBTRACT: set_volume(volume - KEY_STEP); break;
    case VK_RIGHT:
    case VK_UP:
    case VK_OEM_PLUS:
    case VK_ADD: set_volume(volume + KEY_STEP); break;
    case VK_NEXT: set_volume(volume - PAGE_STEP); break;
    case VK_PRIOR: set_volume(volume + PAGE_STEP); break;
    case VK_HOME: set_volume(0); break;
    case VK_END: set_volume(100); break;
  }
}

static LRESULT CALLBACK popup_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!s.wnd) { /* messages arrive before CreateWindowExW returns */
    s.wnd = wnd;
    uia_init(&ui, wnd, &ui_ops);
  }
  LRESULT result;
  if (uia_message(&ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_ACTIVATE: {
      if (LOWORD(wp) != WA_INACTIVE) {
        SendMessageW(GetWindow(wnd, GW_OWNER), WM_NCACTIVATE, TRUE, 0); /* the main window still looks active */
        break; /* DefWindowProc gives us the focus */
      }
      /* A click elsewhere, or another window. If it was on the speaker button, that click
       * mustn't open it again straight away. */
      POINT pt;
      GetCursorPos(&pt);
      s.anchor_closed = !s.closing && WindowFromPoint(pt) == s.anchor;
      s.closed_at = GetTickCount();
      hide((HWND)lp);
      return 0;
    }
    case WM_CLOSE: /* Alt+F4 */
      volume_popup_close();
      return 0;
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
      if (s.dragging) set_volume(volume_at(x));
      set_hot(s.dragging ? PART_SLIDER : part_at(x, y));
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      set_hot(s.dragging ? PART_SLIDER : PART_NONE);
      return 0;
    case WM_LBUTTONDOWN: {
      int x = GET_X_LPARAM(lp), part = part_at(x, GET_Y_LPARAM(lp));
      if (part == PART_NONE) return 0;
      set_focus(part);
      SetCapture(wnd);
      if (part == PART_SPEAKER) {
        s.pressed = TRUE;
      } else {
        s.dragging = TRUE;
        set_volume(volume_at(x));
      }
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      BOOL click = s.pressed && part_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)) == PART_SPEAKER;
      if (s.pressed || s.dragging) ReleaseCapture(); /* WM_CAPTURECHANGED ends the press */
      if (click) toggle_muted();
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (s.pressed || s.dragging) {
        s.pressed = s.dragging = FALSE;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(wnd, &pt);
        s.hot = s.tracking ? part_at(pt.x, pt.y) : PART_NONE;
        InvalidateRect(wnd, NULL, FALSE);
      }
      return 0;
    case WM_MOUSEWHEEL: /* a notch is a key's step; a touchpad's smaller turns add up to one */
      s.wheel += GET_WHEEL_DELTA_WPARAM(wp);
      if (s.wheel / WHEEL_DELTA) set_volume(playback_volume() + s.wheel / WHEEL_DELTA * KEY_STEP);
      s.wheel %= WHEEL_DELTA;
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_DESTROY: /* with the main window */
      uia_destroy(&ui);
      canvas_free(&s.buf);
      if (s.icon_font) DeleteObject(s.icon_font);
      memset(&s, 0, sizeof s);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* Once, the first time it opens; it's hidden rather than destroyed when it closes. */
static BOOL create(HWND owner) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DROPSHADOW, .lpfnWndProc = popup_proc,
                    .hInstance = GetModuleHandleW(NULL), .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"kithsong_volume"};
  RegisterClassExW(&wc);
  /* Its text names it for screen readers. */
  CreateWindowExW(WS_EX_TOOLWINDOW, L"kithsong_volume", mc_wstr(MC_STR_VOLUME), WS_POPUP, 0, 0, 0, 0, owner, NULL,
                  GetModuleHandleW(NULL), NULL);
  if (!s.wnd) return FALSE;
  int corners = CORNERS_ROUND_SMALL;
  s.rounded = SUCCEEDED(DwmSetWindowAttribute(s.wnd, DWMWA_CORNERS, &corners, sizeof corners));
  s.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s.wnd, NULL,
                              GetModuleHandleW(NULL), NULL);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = s.wnd, .uId = PART_SPEAKER + 1,
                    .lpszText = (wchar_t *)mc_wstr(MC_STR_MUTE)};
  SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  return TRUE;
}

void volume_popup_toggle(HWND anchor, UINT dpi, const volume_popup_host *host) {
  if (volume_popup_is_open()) {
    volume_popup_close();
    return;
  }
  if (s.anchor_closed && GetTickCount() - s.closed_at < GetDoubleClickTime()) { /* the click that closed it */
    s.anchor_closed = FALSE;
    return;
  }
  s.anchor_closed = FALSE;
  HWND owner = GetAncestor(anchor, GA_ROOT);
  if (!s.wnd && !create(owner)) return;
  s.anchor = anchor;
  s.host = *host;
  s.refocus = GetFocus();
  if (dpi != s.dpi || !s.icon_font) {
    s.dpi = dpi;
    if (s.icon_font) DeleteObject(s.icon_font);
    s.icon_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                              theme_icon_face());
  }
  theme_apply_window(s.wnd, FALSE); /* Windows 11's border, dark or light */
  theme_apply_control(s.tooltip);

  /* Under the button, their right edges in line, kept on the screen. */
  RECT a;
  GetWindowRect(anchor, &a);
  int w = scale(PAD + BUTTON + GAP + SLIDER_W + END_PAD), h = scale(PAD + BUTTON + PAD);
  int x = a.right - w, y = a.bottom + scale(DROP);
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &mi)) {
    x = max(mi.rcWork.left, min(x, mi.rcWork.right - w));
    y = max(mi.rcWork.top, min(y, mi.rcWork.bottom - h));
  }
  s.focus = PART_SLIDER;
  s.hot = PART_NONE;
  s.wheel = 0;
  SetWindowPos(s.wnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
  relayout();
  update_tip();
  /* Focus rectangles as the main window has them: shown once the keyboard has been used. */
  BOOL hide_focus = (SendMessageW(owner, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
  SendMessageW(s.wnd, WM_CHANGEUISTATE, MAKEWPARAM(hide_focus ? UIS_SET : UIS_CLEAR, UISF_HIDEFOCUS), 0);
  ShowWindow(s.wnd, SW_SHOW);
  InvalidateRect(anchor, NULL, FALSE);
}
