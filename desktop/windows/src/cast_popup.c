#include "cast_popup.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomation.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "playback.h"
#include "scroller.h"
#include "theme.h"
#include "uia.h"
#include "volume_popup.h"
#include "wstr.h"

/* Newer than some SDK headers. */
#define DWMWA_CORNERS 33 /* DWMWA_WINDOW_CORNER_PREFERENCE, Windows 11 */
#define CORNERS_ROUND_SMALL 3

/* Segoe Fluent Icons (and MDL2 Assets). */
#define GLYPH_CAST 0xEC15 /* MiracastLogoSmall: a screen with waves, as the toolbar's button */
#define GLYPH_STOP 0xE711 /* Cancel */

/* Metrics at 96 DPI. */
enum {
  WIDTH = 320,
  PAD = 8,
  HEADER_H = 44,
  TEXT_PAD = 12, /* the heading and the looking text, in from the rows' edges */
  ROW_H = 48,
  MIN_ROWS = 3, /* the list's height, however few there are */
  MAX_ROWS = 6, /* ...and however many; the rest scroll */
  RADIUS = 4,
  ICON_W = 40, /* the glyph at the start of a row */
  VOLUME_H = 48,
  SLIDER_END = 16, /* after the slider, as in the volume drop-down */
  DROP = 4,        /* between the toolbar button and the popup */
  RAIL_H = 4,
  THUMB = 20,
  KNOB = 12,
  KNOB_HOT = 14,
  KNOB_PRESSED = 10,
  KEY_STEP = 5, /* as the volume shortcuts */
  PAGE_STEP = 20,
};

/* What the mouse or the keyboard can be on: a device's row (0 up), or these. */
enum { T_NONE = -1, T_SLIDER = -2, T_STOP = -3 };

typedef struct cast_row {
  char id[MC_CAST_ID_LEN];
  wchar_t name[MC_CAST_NAME_LEN];  /* its name, else its model, else MC_STR_CAST_DEVICE_UNNAMED */
  wchar_t model[MC_CAST_NAME_LEN]; /* under it, if the name isn't the model already; may be "" */
} cast_row;

static struct {
  HWND wnd, anchor;
  HWND refocus; /* had the focus in the main window before it opened */
  cast_popup_host host;
  UINT dpi;
  HFONT title_font, font, sub_font, icon_font;
  int line_h, sub_line_h; /* of font and sub_font */
  canvas buf;
  scroller scroll; /* the list of devices */
  BOOL rounded;    /* Windows rounds the corners and draws the border */

  /* What it shows. */
  BOOL session;       /* connecting or casting: the device, not the list */
  BOOL casting;       /* ...casting, with its volume */
  wchar_t heading[256];
  cast_row *rows;     /* the devices, while not in a session */
  int count;

  /* Layout, in device pixels, while in a session. */
  RECT volume_row, bar, stop; /* bar: the row the slider's rail and thumb sit in */

  int focus;          /* a target: a row, T_SLIDER, T_STOP, or T_NONE */
  int hot;            /* ...under the mouse */
  int pressed;        /* ...held down with the mouse (not the slider, which drags) */
  BOOL dragging;      /* the slider */
  int held;           /* the volume it was dragged to, shown until the device says otherwise */
  POINT mouse;        /* in the client area, as last seen */
  BOOL tracking;
  int wheel;          /* the wheel's turn short of a notch (WHEEL_DELTA), not acted on yet */
  BOOL closing;       /* cast_popup_close() is handing back to the main window */
  BOOL anchor_closed; /* a press on the cast button closed it, at closed_at */
  DWORD closed_at;
} s;

/* Outside s, which is cleared when the window goes: elements clients still hold use it. */
static uia_host ui;

static void ui_changed(void);

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

BOOL cast_popup_is_open(void) {
  return s.wnd && IsWindowVisible(s.wnd);
}

/* Whether the focus shows, which it does once the keyboard has been used. */
static BOOL focus_shows(void) {
  return GetFocus() == s.wnd && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}

/* --- Layout --- */

/* The popup's size for what it shows. */
static SIZE wanted_size(void) {
  SIZE size = {scale(WIDTH), 0};
  if (s.session) size.cy = scale(HEADER_H + (s.casting ? VOLUME_H : 0) + ROW_H + PAD);
  else size.cy = scale(HEADER_H + max(MIN_ROWS, min(s.count, MAX_ROWS)) * ROW_H + PAD);
  return size;
}

static RECT row_rect(int i) {
  RECT client;
  GetClientRect(s.wnd, &client);
  int top = s.scroll.view.top + i * scale(ROW_H) - s.scroll.pos;
  RECT r = {scale(PAD), top, client.right - scale(PAD), top + scale(ROW_H)};
  return r;
}

static int row_at(POINT pt) {
  if (s.session || !PtInRect(&s.scroll.view, pt)) return T_NONE;
  int i = (pt.y - s.scroll.view.top + s.scroll.pos) / scale(ROW_H);
  if (i < 0 || i >= s.count) return T_NONE;
  RECT r = row_rect(i);
  return PtInRect(&r, pt) ? i : T_NONE;
}

/* The slider takes clicks from its whole row. */
static int target_at(POINT pt) {
  if (!s.session) return row_at(pt);
  if (s.casting && PtInRect(&s.volume_row, pt)) return T_SLIDER;
  if (PtInRect(&s.stop, pt)) return T_STOP;
  return T_NONE;
}

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int pad = scale(PAD), top = scale(HEADER_H);
  RECT view = {0, top, rc.right, max(top, rc.bottom - pad)};
  scroller_resize(&s.scroll, &view, s.session ? 0 : s.count * scale(ROW_H));
  SetRectEmpty(&s.volume_row);
  SetRectEmpty(&s.bar);
  if (s.casting) {
    SetRect(&s.volume_row, pad, top, rc.right - pad, top + scale(VOLUME_H));
    int thumb = scale(THUMB), cy = (s.volume_row.top + s.volume_row.bottom) / 2;
    SetRect(&s.bar, s.volume_row.left + scale(ICON_W), cy - thumb / 2, s.volume_row.right - scale(SLIDER_END),
            cy - thumb / 2 + thumb);
    if (s.bar.right < s.bar.left) s.bar.right = s.bar.left;
    top = s.volume_row.bottom;
  }
  SetRect(&s.stop, pad, top, rc.right - pad, top + scale(ROW_H));
  if (!s.session) SetRectEmpty(&s.stop);
}

/* Under the button, their right edges in line, kept on the screen. */
static void place(void) {
  RECT a;
  GetWindowRect(s.anchor, &a);
  SIZE size = wanted_size();
  int x = a.right - size.cx, y = a.bottom + scale(DROP);
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromWindow(s.anchor, MONITOR_DEFAULTTONEAREST), &mi)) {
    x = max(mi.rcWork.left, min(x, mi.rcWork.right - size.cx));
    y = max(mi.rcWork.top, min(y, mi.rcWork.bottom - size.cy));
  }
  SetWindowPos(s.wnd, HWND_TOP, x, y, size.cx, size.cy, SWP_NOACTIVATE);
  relayout(); /* WM_SIZE did, unless the size stayed */
}

/* --- The slider --- */

static int shown_volume(void) {
  return s.dragging ? s.held : playback_volume();
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

static void set_volume(int volume) {
  volume = max(0, min(volume, 100));
  if (s.dragging) s.held = volume;
  if (s.casting && (volume != playback_volume() || playback_muted()) && s.host.set_volume) s.host.set_volume(volume);
  InvalidateRect(s.wnd, NULL, FALSE);
}

/* --- What it does --- */

static void connect_row(int i) {
  if (s.session || i < 0 || i >= s.count) return;
  char id[MC_CAST_ID_LEN];
  memcpy(id, s.rows[i].id, sizeof id); /* the rows are read again meanwhile */
  playback_cast_connect(id); /* the main window hears of it, and calls cast_popup_update() */
}

static void stop_casting(void) {
  if (s.session) playback_cast_disconnect();
}

/* --- Focus and hover --- */

static int ui_node_of(int target);

static void reveal_row(int i) {
  scroller_reveal(&s.scroll, i * scale(ROW_H), (i + 1) * scale(ROW_H));
}

static void set_focus(int target) {
  if (target >= 0) {
    if (!s.count) return;
    target = max(0, min(target, s.count - 1));
    reveal_row(target);
  }
  BOOL moved = target != s.focus;
  s.focus = target;
  InvalidateRect(s.wnd, NULL, FALSE);
  if (!moved) return;
  if (target >= 0) uia_selected(&ui, ui_node_of(target));
  uia_focus(&ui);
}

static void update_hover(void) {
  int hot = s.tracking && !s.scroll.hot && !s.scroll.dragging ? target_at(s.mouse) : T_NONE;
  if (s.dragging) hot = T_SLIDER;
  if (hot == s.hot) return;
  s.hot = hot;
  InvalidateRect(s.wnd, NULL, FALSE);
}

static void on_scrolled(void) {
  update_hover();
}

static void end_press(void) {
  BOOL was = s.pressed != T_NONE || s.dragging;
  s.pressed = T_NONE;
  s.dragging = FALSE;
  if (GetCapture() == s.wnd) ReleaseCapture();
  if (was) {
    update_hover();
    InvalidateRect(s.wnd, NULL, FALSE);
  }
}

/* --- What the cast has --- */

static void free_rows(void) {
  free(s.rows);
  s.rows = NULL;
  s.count = 0;
}

static int row_of(const char *id) {
  for (int i = 0; id[0] && i < s.count; i++)
    if (!strcmp(s.rows[i].id, id)) return i;
  return T_NONE;
}

/* "Casting to Living Room TV", or for the list, "Cast to". */
static void make_heading(const mc_cast_status *st) {
  if (!s.session) {
    lstrcpynW(s.heading, mc_wstr(MC_STR_CAST_TO), 256);
    return;
  }
  const char *name = st->device_name[0] ? st->device_name : mc_str(MC_STR_CAST_DEVICE_UNNAMED);
  mc_wformat(s.heading, 256, mc_str(s.casting ? MC_STR_CAST_CASTING_TO : MC_STR_CAST_CONNECTING_TO), MC_S(name));
}

/* Reads the cast again. The focus stays on its device, wherever that's moved to; going into a
 * session or out of one, it starts afresh, and the popup takes the new size. */
static void reload(void) {
  const mc_cast_status *st = playback_cast_status();
  BOOL was_session = s.session, was_casting = s.casting;
  char focus_id[MC_CAST_ID_LEN] = "";
  if (s.focus >= 0 && s.focus < s.count) memcpy(focus_id, s.rows[s.focus].id, sizeof focus_id);
  int was_focus = s.focus;
  wchar_t was_heading[256];
  lstrcpynW(was_heading, s.heading, 256);

  s.session = st->state != MC_CAST_IDLE;
  s.casting = st->state == MC_CAST_CASTING;
  free_rows();
  mc_cast_device_list *list = s.session ? NULL : playback_cast_devices();
  int n = list ? list->count : 0;
  s.rows = n ? calloc((size_t)n, sizeof *s.rows) : NULL;
  if (!s.rows) n = 0;
  for (int i = 0; i < n; i++) {
    const mc_cast_device *d = &list->items[i];
    cast_row *r = &s.rows[i];
    memcpy(r->id, d->id, sizeof r->id);
    r->id[MC_CAST_ID_LEN - 1] = '\0';
    if (d->name[0]) {
      wstr_from_utf8(r->name, MC_CAST_NAME_LEN, d->name);
      wstr_from_utf8(r->model, MC_CAST_NAME_LEN, d->model);
    } else if (d->model[0]) {
      wstr_from_utf8(r->name, MC_CAST_NAME_LEN, d->model);
    } else {
      lstrcpynW(r->name, mc_wstr(MC_STR_CAST_DEVICE_UNNAMED), MC_CAST_NAME_LEN);
    }
  }
  s.count = n;
  mc_cast_device_list_free(list);
  make_heading(st);
  SetWindowTextW(s.wnd, s.heading); /* names the window for screen readers */

  if (s.session != was_session || s.casting != was_casting) {
    end_press();
    s.focus = s.session ? (s.casting ? T_SLIDER : T_STOP) : (s.count ? 0 : T_NONE);
    s.hot = T_NONE;
    if (cast_popup_is_open()) place();
    scroller_jump(&s.scroll, 0);
  } else if (!s.session) {
    s.focus = row_of(focus_id);
    if (s.focus < 0 && s.count) s.focus = max(0, min(was_focus, s.count - 1));
    if (s.pressed >= s.count) end_press();
    if (cast_popup_is_open()) place(); /* the list may be longer or shorter */
  } else {
    relayout();
  }
  update_hover();
  InvalidateRect(s.wnd, NULL, FALSE);
  ui_changed();
  if (s.focus != was_focus || s.session != was_session) uia_focus(&ui);
  if (cast_popup_is_open() && wcscmp(was_heading, s.heading)) uia_announce(&ui, UIA_ROOT);
}

void cast_popup_update(void) {
  if (cast_popup_is_open()) reload();
}

/* --- Painting --- */

static void paint_glyph(canvas *c, wchar_t glyph, const RECT *row, COLORREF color) {
  RECT r = {row->left, row->top, row->left + scale(ICON_W), row->bottom};
  canvas_text(c, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, s.icon_font, color);
}

static void paint_row(canvas *c, int i) {
  RECT r = row_rect(i);
  if (r.bottom <= s.scroll.view.top || r.top >= s.scroll.view.bottom) return;
  control_state state = s.pressed == i && s.hot == i ? STATE_PRESSED : s.hot == i ? STATE_HOT : STATE_NORMAL;
  COLORREF text = theme_draw_state(c, &r, scale(RADIUS), state);
  COLORREF dim = theme.high_contrast && state != STATE_NORMAL ? text : theme.text2;
  if (i == s.focus && focus_shows()) canvas_frame(c, &r, scale(RADIUS), scale(2), theme.text, 255);
  paint_glyph(c, GLYPH_CAST, &r, dim);

  const cast_row *rw = &s.rows[i];
  UINT one_line = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
  int left = r.left + scale(ICON_W), right = r.right - scale(TEXT_PAD);
  if (rw->model[0]) {
    int top = r.top + (r.bottom - r.top - s.line_h - s.sub_line_h) / 2;
    RECT name = {left, top, right, top + s.line_h}, model = {left, name.bottom, right, name.bottom + s.sub_line_h};
    canvas_text(c, rw->name, -1, &name, one_line, s.font, text);
    canvas_text(c, rw->model, -1, &model, one_line, s.sub_font, dim);
  } else {
    RECT name = {left, r.top, right, r.bottom};
    canvas_text(c, rw->name, -1, &name, one_line, s.font, text);
  }
}

/* Nothing found yet: what it's doing, and why a device may not show. */
static void paint_looking(canvas *c) {
  int inset = scale(PAD + TEXT_PAD);
  RECT title = {inset, s.scroll.view.top + scale(PAD), s.scroll.view.right - inset, 0};
  title.bottom = title.top + s.line_h;
  canvas_text(c, mc_wstr(MC_STR_CAST_LOOKING), -1, &title, DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, s.font,
              theme.text);
  RECT text = {title.left, title.bottom + scale(4), title.right, s.scroll.view.bottom};
  canvas_text(c, mc_wstr(MC_STR_CAST_LOOKING_TEXT), -1, &text, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS,
              s.sub_font, theme.text2);
}

/* Windows 11's slider, as the volume drop-down draws it, in grey while muted. */
static void paint_volume(canvas *c) {
  BOOL cues = focus_shows();
  wchar_t glyph = volume_popup_glyph(); /* the device's, while casting */
  paint_glyph(c, glyph, &s.volume_row, theme.text2);

  int rail_h = scale(RAIL_H), cy = (s.bar.top + s.bar.bottom) / 2, x = thumb_x(shown_volume());
  RECT rail = {s.bar.left, cy - rail_h / 2, s.bar.right, cy - rail_h / 2 + rail_h};
  COLORREF ink = theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  COLORREF fill = playback_muted() && !s.dragging ? theme.text3 : theme.accent;
  canvas_fill(c, &rail, rail_h / 2, theme.high_contrast ? theme.text : ink,
              theme.high_contrast ? 255 : theme.dark ? 139 : 114);
  RECT done = rail;
  done.right = x;
  canvas_fill(c, &done, rail_h / 2, fill, 255);

  int thumb = scale(THUMB), knob = scale(s.dragging ? KNOB_PRESSED : s.hot == T_SLIDER ? KNOB_HOT : KNOB);
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
  if (cues && s.focus == T_SLIDER) {
    RECT ring = s.bar;
    InflateRect(&ring, scale(4), scale(3));
    canvas_frame(c, &ring, scale(4), scale(2), theme.text, 255);
  }
}

static void paint_stop(canvas *c) {
  control_state state = s.pressed == T_STOP && s.hot == T_STOP ? STATE_PRESSED
                        : s.hot == T_STOP                       ? STATE_HOT
                                                                : STATE_NORMAL;
  COLORREF text = theme_draw_state(c, &s.stop, scale(RADIUS), state);
  if (s.focus == T_STOP && focus_shows()) canvas_frame(c, &s.stop, scale(RADIUS), scale(2), theme.text, 255);
  paint_glyph(c, GLYPH_STOP, &s.stop, text);
  RECT label = {s.stop.left + scale(ICON_W), s.stop.top, s.stop.right - scale(TEXT_PAD), s.stop.bottom};
  canvas_text(c, mc_wstr(MC_STR_CAST_STOP), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS,
              s.font, text);
}

static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  scroller_paint_start(&s.scroll);
  canvas_clear(c, &client, theme.bg, 255);

  if (s.session) {
    if (s.casting) paint_volume(c);
    paint_stop(c);
  } else {
    for (int i = 0; i < s.count; i++) paint_row(c, i);
    if (!s.count) paint_looking(c);
  }

  /* Over the ends of the rows that scrolled under them. */
  RECT header = {0, 0, client.right, scale(HEADER_H)}, foot = {0, s.scroll.view.bottom, client.right, client.bottom};
  canvas_clear(c, &header, theme.bg, 255);
  if (!s.session) canvas_clear(c, &foot, theme.bg, 255);
  RECT title = {scale(PAD + TEXT_PAD), 0, client.right - scale(PAD + TEXT_PAD), header.bottom};
  canvas_text(c, s.heading, -1, &title, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, s.title_font,
              theme.text);
  RECT rule = {0, header.bottom - 1, client.right, header.bottom};
  canvas_clear(c, &rule, theme.line, 255);

  if (!s.session) scroller_paint(&s.scroll, c);
  if (!s.rounded) canvas_frame(c, &client, 0, 1, theme.high_contrast ? theme.text : theme.line, 255);
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Opening and closing --- */

/* Something else is active now: activated, or NULL for another app's window. Unless that's
 * the main window, it stops looking active, as it only did for the popup's sake. */
static void hide(HWND activated) {
  if (!cast_popup_is_open()) return;
  HWND owner = GetWindow(s.wnd, GW_OWNER);
  end_press();
  ShowWindow(s.wnd, SW_HIDE);
  if (activated != owner) SendMessageW(owner, WM_NCACTIVATE, FALSE, 0);
  InvalidateRect(s.anchor, NULL, FALSE);
  if (s.host.closed) s.host.closed();
}

void cast_popup_close(void) {
  if (!cast_popup_is_open()) return;
  HWND owner = GetWindow(s.wnd, GW_OWNER), refocus = s.refocus;
  s.closing = TRUE;
  SetActiveWindow(owner); /* which hides it (WM_ACTIVATE) */
  s.closing = FALSE;
  hide(owner);
  if (refocus && IsWindow(refocus) && GetActiveWindow() == owner) SetFocus(refocus);
}

/* --- UI Automation (uia.h) ---
 *
 * The nodes: the slider and Stop casting, in a session, or the list of devices and its rows.
 * The window is named by the heading, and is a live region, so a change of state is read out. */

enum { UI_SLIDER, UI_STOP, UI_LIST, UI_ROWS };

static int ui_node_of(int target) {
  return target == T_SLIDER ? UI_SLIDER : target == T_STOP ? UI_STOP : target >= 0 ? UI_ROWS + target : UIA_ROOT;
}

static BOOL ui_node(int i, uia_node *n) {
  n->parent = UIA_ROOT;
  if (i == UIA_ROOT) {
    n->type = UIA_PaneControlTypeId;
    n->live = Polite;
    lstrcpynW(n->name, s.heading, 512);
    n->first = s.session ? (s.casting ? UI_SLIDER : UI_STOP) : UI_LIST;
    n->count = s.session && s.casting ? 2 : 1;
    return TRUE;
  }
  if (i == UI_SLIDER) {
    if (!s.casting) return FALSE;
    n->type = UIA_SliderControlTypeId;
    n->flags = UIA_FOCUSABLE | UIA_RANGE;
    n->rect = s.bar;
    n->automation_id = L"CastVolume";
    lstrcpynW(n->name, mc_wstr(MC_STR_VOLUME), 512);
    n->value = shown_volume();
    n->maximum = 100;
    n->small_step = KEY_STEP;
    n->large_step = PAGE_STEP;
    if (playback_muted()) lstrcpynW(n->status, mc_wstr(MC_STR_VOLUME_MUTED), 64);
    return TRUE;
  }
  if (i == UI_STOP) {
    if (!s.session) return FALSE;
    n->type = UIA_ButtonControlTypeId;
    n->flags = UIA_FOCUSABLE | UIA_INVOKABLE;
    n->rect = s.stop;
    n->automation_id = L"StopCasting";
    lstrcpynW(n->name, mc_wstr(MC_STR_CAST_STOP), 512);
    return TRUE;
  }
  if (s.session) return FALSE;
  if (i == UI_LIST) {
    n->type = UIA_ListControlTypeId;
    n->flags = UIA_SELECTION;
    n->rect = s.scroll.view;
    n->first = UI_ROWS;
    n->count = s.count;
    n->automation_id = L"CastDevices";
    lstrcpynW(n->name, mc_wstr(MC_STR_CAST_TO), 512);
    if (!s.count) {
      lstrcpynW(n->status, mc_wstr(MC_STR_CAST_LOOKING), 64);
      lstrcpynW(n->help, mc_wstr(MC_STR_CAST_LOOKING_TEXT), 256);
    }
    return TRUE;
  }
  int r = i - UI_ROWS;
  if (r < 0 || r >= s.count) return FALSE;
  RECT fill = row_rect(r);
  n->parent = UI_LIST;
  n->type = UIA_ListItemControlTypeId;
  n->flags = UIA_FOCUSABLE | UIA_SELECTABLE | UIA_INVOKABLE | (r == s.focus ? UIA_SELECTED : 0);
  IntersectRect(&n->rect, &fill, &s.scroll.view);
  uia_name(n, s.rows[r].name, s.rows[r].model, NULL);
  lstrcpynW(n->help, mc_wstr(MC_STR_CONNECT), 256);
  return TRUE;
}

static int ui_focus(void) {
  return ui_node_of(s.focus);
}

static int ui_at(POINT pt) {
  return ui_node_of(target_at(pt));
}

static int ui_selection(int i) {
  return i == UI_LIST && !s.session && s.focus >= 0 ? UI_ROWS + s.focus : UIA_NONE;
}

static int ui_row_of(int i) {
  int r = i - UI_ROWS;
  return !s.session && r >= 0 && r < s.count ? r : T_NONE;
}

static void ui_select(int i) {
  if (ui_row_of(i) >= 0) set_focus(ui_row_of(i));
}

static void ui_invoke(int i) {
  if (i == UI_STOP) stop_casting();
  else if (ui_row_of(i) >= 0) connect_row(ui_row_of(i));
}

static void ui_reveal(int i) {
  if (ui_row_of(i) >= 0) reveal_row(ui_row_of(i));
}

static void ui_set_focus(int i) {
  if (i == UI_SLIDER && s.casting) set_focus(T_SLIDER);
  else if (i == UI_STOP && s.session) set_focus(T_STOP);
  else ui_select(i);
}

static void ui_set_value(int i, double value) {
  if (i == UI_SLIDER) set_volume((int)(value + 0.5));
}

static const uia_ops ui_ops = {.node = ui_node, .focus = ui_focus, .at = ui_at, .selection = ui_selection,
                               .select = ui_select, .invoke = ui_invoke, .reveal = ui_reveal,
                               .set_focus = ui_set_focus, .set_value = ui_set_value};

/* After reloading: what the nodes are, so clients only start over when they did. */
static void ui_changed(void) {
  int shape[3] = {s.session, s.casting, s.count};
  unsigned long sig = uia_hash(0, shape, sizeof shape);
  for (int i = 0; i < s.count; i++) sig = uia_hash(sig, s.rows[i].id, strlen(s.rows[i].id));
  uia_changed(&ui, sig);
}

/* --- Window --- */

/* In the list, the arrow keys, Page Up and Down, Home and End go through the devices, and
 * Space or Enter casts to one. In a session, Tab and Up and Down (from Stop casting) move
 * between the slider and Stop casting; on the slider, the arrow keys, + and -, Page Up and
 * Down, Home and End set the volume, as in the volume drop-down. */
static void on_key(WPARAM key) {
  SendMessageW(s.wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  InvalidateRect(s.wnd, NULL, FALSE);
  if (key == VK_ESCAPE) {
    if (s.dragging || s.pressed != T_NONE) end_press();
    else cast_popup_close();
    return;
  }
  if (s.dragging || s.pressed != T_NONE) return;
  if (!s.session) {
    if (!s.count) return;
    int page = max(1, (s.scroll.view.bottom - s.scroll.view.top) / scale(ROW_H)), at = max(s.focus, 0);
    switch (key) {
      case VK_UP: set_focus(s.focus < 0 ? 0 : at - 1); break;
      case VK_DOWN: set_focus(s.focus < 0 ? 0 : at + 1); break;
      case VK_PRIOR: set_focus(at - page); break;
      case VK_NEXT: set_focus(at + page); break;
      case VK_HOME: set_focus(0); break;
      case VK_END: set_focus(s.count - 1); break;
      case VK_SPACE:
      case VK_RETURN: connect_row(s.focus); break;
    }
    return;
  }
  if (s.focus == T_STOP || !s.casting) {
    switch (key) {
      case VK_TAB:
      case VK_UP:
        if (s.casting) set_focus(T_SLIDER);
        break;
      case VK_SPACE:
      case VK_RETURN: stop_casting(); break;
    }
    return;
  }
  int volume = playback_volume();
  switch (key) {
    case VK_TAB: set_focus(T_STOP); break;
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
    s.wnd = s.scroll.wnd = wnd;
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
      /* A click elsewhere, or another window. If it was on the cast button, that click
       * mustn't open it again straight away. */
      POINT pt;
      GetCursorPos(&pt);
      s.anchor_closed = !s.closing && WindowFromPoint(pt) == s.anchor;
      s.closed_at = GetTickCount();
      hide((HWND)lp);
      return 0;
    }
    case WM_CLOSE: /* Alt+F4 */
      cast_popup_close();
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
    case WM_TIMER:
      scroller_timer(&s.scroll, wp);
      return 0;
    case WM_MOUSEMOVE: {
      if (!s.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        s.tracking = TRUE;
      }
      s.mouse.x = GET_X_LPARAM(lp);
      s.mouse.y = GET_Y_LPARAM(lp);
      if (s.dragging) {
        set_volume(volume_at(s.mouse.x));
        return 0;
      }
      if (!s.session) scroller_mouse_move(&s.scroll, s.mouse.x, s.mouse.y);
      update_hover();
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      if (!s.session) scroller_mouse_leave(&s.scroll);
      update_hover();
      return 0;
    case WM_MOUSEWHEEL:
      if (!s.session) {
        scroller_wheel(&s.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      } else if (s.casting) { /* a notch is a key's step; a touchpad's smaller turns add up to one */
        s.wheel += GET_WHEEL_DELTA_WPARAM(wp);
        if (s.wheel / WHEEL_DELTA) set_volume(playback_volume() + s.wheel / WHEEL_DELTA * KEY_STEP);
        s.wheel %= WHEEL_DELTA;
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      s.mouse = pt;
      if (!s.session && scroller_mouse_down(&s.scroll, pt.x, pt.y)) return 0;
      int target = target_at(pt);
      if (target == T_NONE) return 0;
      set_focus(target);
      SetCapture(wnd);
      if (target == T_SLIDER) {
        s.dragging = TRUE;
        s.held = playback_volume();
        set_volume(volume_at(pt.x));
      } else {
        s.pressed = target;
      }
      update_hover();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      s.mouse = pt;
      int pressed = s.pressed;
      BOOL click = pressed != T_NONE && target_at(pt) == pressed;
      if (!s.session) scroller_mouse_up(&s.scroll);
      end_press();
      if (click && pressed == T_STOP) stop_casting();
      else if (click) connect_row(pressed);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (!s.session) scroller_capture_changed(&s.scroll);
      if (s.dragging || s.pressed != T_NONE) {
        s.dragging = FALSE;
        s.pressed = T_NONE;
        update_hover();
        InvalidateRect(wnd, NULL, FALSE);
      }
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_DESTROY: { /* with the main window */
      uia_destroy(&ui);
      canvas_free(&s.buf);
      free_rows();
      HFONT fonts[] = {s.title_font, s.font, s.sub_font, s.icon_font};
      for (int i = 0; i < 4; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
      memset(&s, 0, sizeof s);
      return 0;
    }
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static int font_height(HFONT font) {
  HDC dc = GetDC(s.wnd);
  HGDIOBJ old = SelectObject(dc, font);
  TEXTMETRICW tm = {0};
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(s.wnd, dc);
  return tm.tmHeight;
}

/* The queue drop-down's sizes, at the window's dpi; the glyphs as big as the toolbar's. */
static void make_fonts(UINT dpi) {
  s.dpi = dpi;
  HFONT old[] = {s.title_font, s.font, s.sub_font, s.icon_font};
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  s.sub_font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 7, 6);
  s.font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 4, 3);
  lf.lfWeight = FW_SEMIBOLD;
  s.title_font = CreateFontIndirectW(&lf);
  s.icon_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
  for (int i = 0; i < 4; i++)
    if (old[i]) DeleteObject(old[i]);
  s.line_h = font_height(s.font);
  s.sub_line_h = font_height(s.sub_font);
}

/* Once, the first time it opens; it's hidden rather than destroyed when it closes. */
static BOOL create(HWND owner) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DROPSHADOW, .lpfnWndProc = popup_proc,
                    .hInstance = GetModuleHandleW(NULL), .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"kithsong_cast"};
  RegisterClassExW(&wc);
  /* Its text names it for screen readers: the heading, once it's read (reload()). */
  CreateWindowExW(WS_EX_TOOLWINDOW, L"kithsong_cast", mc_wstr(MC_STR_CAST_TO), WS_POPUP, 0, 0, 0, 0, owner, NULL,
                  GetModuleHandleW(NULL), NULL);
  if (!s.wnd) return FALSE;
  s.scroll.moved = on_scrolled;
  s.focus = s.hot = s.pressed = T_NONE;
  int corners = CORNERS_ROUND_SMALL;
  s.rounded = SUCCEEDED(DwmSetWindowAttribute(s.wnd, DWMWA_CORNERS, &corners, sizeof corners));
  return TRUE;
}

void cast_popup_toggle(HWND anchor, UINT dpi, const cast_popup_host *host) {
  if (cast_popup_is_open()) {
    cast_popup_close();
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
  if (dpi != s.dpi || !s.font) make_fonts(dpi);
  theme_apply_window(s.wnd, FALSE); /* Windows 11's border, dark or light */
  end_press();
  s.hot = T_NONE;
  s.wheel = 0;
  /* Read as new: the focus starts on the first device, or the slider. */
  s.session = s.casting = FALSE;
  free_rows();
  s.focus = T_NONE;
  reload();
  s.focus = s.session ? (s.casting ? T_SLIDER : T_STOP) : (s.count ? 0 : T_NONE);
  place();
  scroller_jump(&s.scroll, 0);
  /* Focus rectangles as the main window has them: shown once the keyboard has been used. */
  BOOL hide_focus = (SendMessageW(owner, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
  SendMessageW(s.wnd, WM_CHANGEUISTATE, MAKEWPARAM(hide_focus ? UIS_SET : UIS_CLEAR, UISF_HIDEFOCUS), 0);
  ShowWindow(s.wnd, SW_SHOW);
  InvalidateRect(anchor, NULL, FALSE);
}
