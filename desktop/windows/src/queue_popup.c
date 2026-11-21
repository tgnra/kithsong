#include "queue_popup.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <uiautomation.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "playback.h"
#include "scroller.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

/* Newer than some SDK headers. */
#define DWMWA_CORNERS 33 /* DWMWA_WINDOW_CORNER_PREFERENCE, Windows 11 */
#define CORNERS_ROUND_SMALL 3

/* Metrics at 96 DPI. */
enum {
  WIDTH = 360,
  PAD = 8,
  HEADER_H = 44,
  CLEAR_H = 28,
  CLEAR_TEXT_PAD = 12, /* each side of its label */
  ROW_H = 48,
  MIN_ROWS = 4, /* the popup's height, however few there are */
  MAX_ROWS = 8, /* ...and however many; the rest scroll */
  RADIUS = 4,
  GRIP_W = 28,
  DUR_W = 40,     /* at least: a long album's length takes more */
  DUR_GAP = 8,    /* between the text and the duration */
  REMOVE = 28,    /* the x on a row */
  REMOVE_PAD = 6, /* after it */
  DROP = 4,       /* between the toolbar button and the popup */
  DROP_LINE = 2,  /* where a dragged row would go */
};

enum { TIMER_DRAG_SCROLL = 103 }; /* the scroller has 101 and 102 */

/* An entry of the queue as it shows. */
typedef struct queue_row {
  uint32_t id;
  wchar_t *title;
  wchar_t *byline; /* its artist, what it is, or how many tracks are left; may be NULL */
  wchar_t duration[16];
  int duration_w; /* in device pixels */
} queue_row;

static struct {
  HWND wnd, anchor, tooltip;
  HWND refocus; /* had the focus in the main window before it opened */
  UINT dpi;
  HFONT title_font, font, sub_font, icon_font;
  int line_h, sub_line_h; /* of font and sub_font */
  canvas buf;
  scroller scroll;
  BOOL rounded; /* Windows rounds the corners and draws the border */

  queue_row *rows; /* the queue, in order */
  int count;

  int focus;           /* the row with the keyboard focus, or -1 with none */
  BOOL focus_clear;    /* Clear has it instead */
  RECT clear;          /* in device pixels */
  POINT mouse;         /* in the client area, as last seen */
  BOOL tracking;
  int hot;             /* the row under the mouse, or -1 */
  BOOL hot_remove;     /* ...and on its x */
  BOOL hot_clear;
  BOOL press_clear;
  int press_remove;    /* the row whose x is pressed, or -1 */
  int drag_row;        /* the row pressed, at drag_from, or -1 */
  POINT drag_from;
  BOOL dragging;       /* it has moved far enough */
  int drop_at;         /* where it would go (0 to count), or -1 */
  BOOL closing;        /* queue_popup_close() is handing back to the main window */
  BOOL anchor_closed;  /* a press on the queue button closed it, at closed_at */
  DWORD closed_at;
} s;

/* Outside s, which is cleared when the window goes: elements clients still hold use it. */
static uia_host ui;

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

BOOL queue_popup_is_open(void) {
  return s.wnd && IsWindowVisible(s.wnd);
}

static void format_duration(int64_t ms, wchar_t *out, size_t n) {
  int sec = (int)((ms + 500) / 1000);
  if (ms <= 0) out[0] = L'\0';
  else if (sec >= 3600) swprintf(out, n, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else swprintf(out, n, L"%d:%02d", sec / 60, sec % 60);
}

/* Whether the focus shows, which it does once the keyboard has been used. */
static BOOL focus_shows(void) {
  return GetFocus() == s.wnd && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
}

/* --- Layout --- */

static RECT row_rect(int i) {
  RECT client;
  GetClientRect(s.wnd, &client);
  int top = s.scroll.view.top + i * scale(ROW_H) - s.scroll.pos;
  RECT r = {scale(PAD), top, client.right - scale(PAD), top + scale(ROW_H)};
  return r;
}

static RECT remove_rect(int i) {
  RECT row = row_rect(i);
  int size = scale(REMOVE), right = row.right - scale(REMOVE_PAD), top = row.top + (row.bottom - row.top - size) / 2;
  RECT r = {right - size, top, right, top + size};
  return r;
}

static int row_at(int x, int y) {
  POINT pt = {x, y};
  if (!PtInRect(&s.scroll.view, pt)) return -1;
  int i = (y - s.scroll.view.top + s.scroll.pos) / scale(ROW_H);
  if (i >= s.count) return -1;
  RECT r = row_rect(i);
  return PtInRect(&r, pt) ? i : -1;
}

static BOOL on_remove(int i, POINT pt) {
  if (i < 0 || i >= s.count) return FALSE;
  RECT r = remove_rect(i);
  return PtInRect(&r, pt);
}

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  RECT view = {0, scale(HEADER_H), rc.right, rc.bottom - scale(PAD)};
  scroller_resize(&s.scroll, &view, s.count * scale(ROW_H));

  const wchar_t *label = mc_wstr(MC_STR_QUEUE_CLEAR);
  HDC dc = GetDC(s.wnd);
  HGDIOBJ old = SelectObject(dc, s.sub_font);
  SIZE text = {0};
  GetTextExtentPoint32W(dc, label, lstrlenW(label), &text);
  SelectObject(dc, old);
  ReleaseDC(s.wnd, dc);
  int h = scale(CLEAR_H), top = (scale(HEADER_H) - h) / 2, right = rc.right - scale(PAD) - scale(REMOVE_PAD);
  SetRect(&s.clear, right - text.cx - 2 * scale(CLEAR_TEXT_PAD), top, right, top + h);
}

/* --- What's under the mouse --- */

static void update_hover(void) {
  int hot = -1;
  BOOL hot_remove = FALSE, hot_clear = FALSE;
  if (s.tracking && !s.scroll.hot && !s.scroll.dragging && s.drag_row < 0) {
    hot = row_at(s.mouse.x, s.mouse.y);
    hot_remove = on_remove(hot, s.mouse);
    hot_clear = s.count && PtInRect(&s.clear, s.mouse);
  }
  if (hot == s.hot && hot_remove == s.hot_remove && hot_clear == s.hot_clear) return;
  s.hot = hot;
  s.hot_remove = hot_remove;
  s.hot_clear = hot_clear;
  RECT tip = {0};
  if (hot >= 0) tip = remove_rect(hot);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = s.wnd, .uId = 1, .rect = tip};
  SendMessageW(s.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  InvalidateRect(s.wnd, NULL, FALSE);
}

static void on_scrolled(void) {
  update_hover();
}

/* --- The queue, as the player has it --- */

static void free_rows(void) {
  for (int i = 0; i < s.count; i++) {
    free(s.rows[i].title);
    free(s.rows[i].byline);
  }
  free(s.rows);
  s.rows = NULL;
  s.count = 0;
}

static uint32_t id_of(int i) {
  return i >= 0 && i < s.count ? s.rows[i].id : 0;
}

static int row_of(uint32_t id) {
  for (int i = 0; id && i < s.count; i++)
    if (s.rows[i].id == id) return i;
  return -1;
}

static void end_press(void);
static void ui_changed(void);

/* Reads the queue again. The focus and a press go with their entries, wherever those have moved
 * to: if the focused one has gone, the focus stays where it was. */
static void reload(void) {
  uint32_t focus = id_of(s.focus), drag = id_of(s.drag_row), remove = id_of(s.press_remove);
  int was = s.focus;
  mc_up_next_list *list = playback_up_next();
  free_rows();
  int n = list ? list->count : 0;
  s.rows = n ? calloc((size_t)n, sizeof *s.rows) : NULL;
  if (!s.rows) n = 0;
  HDC dc = GetDC(s.wnd);
  HGDIOBJ old = SelectObject(dc, s.sub_font);
  for (int i = 0; i < n; i++) {
    const mc_up_next_entry *e = &list->items[i];
    queue_row *r = &s.rows[i];
    r->id = e->id;
    r->title = wstr_dup_from_utf8(e->title ? e->title : "");
    if (e->artist) {
      r->byline = wstr_dup_from_utf8(e->artist);
    } else if (e->block) {
      wchar_t tracks[64];
      mc_wformat(tracks, 64, mc_str_plural(MC_STR_TRACKS, e->tracks), MC_D(e->tracks));
      r->byline = _wcsdup(tracks);
    }
    format_duration(e->duration_ms, r->duration, 16);
    SIZE text = {0};
    GetTextExtentPoint32W(dc, r->duration, lstrlenW(r->duration), &text);
    r->duration_w = max(scale(DUR_W), text.cx);
  }
  SelectObject(dc, old);
  ReleaseDC(s.wnd, dc);
  s.count = n;
  mc_up_next_list_free(list);

  s.focus = row_of(focus);
  if (s.focus < 0) s.focus = max(min(was, s.count - 1), s.count ? 0 : -1);
  if (!s.count) s.focus_clear = FALSE;
  if (s.drop_at > s.count) s.drop_at = s.count;
  s.press_remove = row_of(remove);
  int dragged = row_of(drag);
  if (s.drag_row >= 0 && dragged < 0) end_press(); /* it played, or went */
  else s.drag_row = dragged;
  relayout();
  update_hover();
  InvalidateRect(s.wnd, NULL, FALSE);
  ui_changed();
  if (id_of(s.focus) != focus) uia_focus(&ui);
}

void queue_popup_update(void) {
  if (queue_popup_is_open()) reload();
}

static void remove_row(int i) {
  playback_up_next_remove(id_of(i));
}

/* to is a place between rows: 0 before the first, count after the last. */
static void move_row(int from, int to) {
  to = min(to, s.count);
  if (to > from) to--;
  if (to != from) playback_up_next_move(id_of(from), to);
}

/* --- Dragging a row into another order --- */

static int drop_index(int y) {
  int row_h = scale(ROW_H), i = (y - s.scroll.view.top + s.scroll.pos + row_h / 2) / row_h;
  return max(0, min(i, s.count));
}

static void set_drop(int i) {
  if (i == s.drop_at) return;
  s.drop_at = i;
  InvalidateRect(s.wnd, NULL, FALSE);
}

/* Near the top or bottom of the view, the list scrolls while dragging: how far per tick. */
static int drag_scroll_step(int y) {
  int edge = scale(ROW_H) / 2, step = scale(ROW_H) / 3;
  if (y < s.scroll.view.top + edge) return -step;
  if (y > s.scroll.view.bottom - edge) return step;
  return 0;
}

static void drag_to(int x, int y) {
  if (!s.dragging) {
    if (abs(x - s.drag_from.x) < GetSystemMetrics(SM_CXDRAG) && abs(y - s.drag_from.y) < GetSystemMetrics(SM_CYDRAG))
      return;
    s.dragging = TRUE;
    update_hover();
    InvalidateRect(s.wnd, NULL, FALSE);
  }
  set_drop(drop_index(y));
  if (drag_scroll_step(y)) SetTimer(s.wnd, TIMER_DRAG_SCROLL, 30, NULL);
}

static void on_drag_timer(void) {
  int step = s.dragging ? drag_scroll_step(s.mouse.y) : 0;
  if (!step) {
    KillTimer(s.wnd, TIMER_DRAG_SCROLL);
    return;
  }
  scroller_jump(&s.scroll, s.scroll.pos + step);
  set_drop(drop_index(s.mouse.y));
}

/* Ends a press or drag, without doing anything about it. */
static void end_press(void) {
  BOOL was = s.drag_row >= 0 || s.press_remove >= 0 || s.press_clear;
  s.drag_row = s.drop_at = s.press_remove = -1;
  s.dragging = s.press_clear = FALSE;
  KillTimer(s.wnd, TIMER_DRAG_SCROLL);
  if (GetCapture() == s.wnd) ReleaseCapture();
  if (was) {
    update_hover();
    InvalidateRect(s.wnd, NULL, FALSE);
  }
}

/* --- Painting --- */

static void paint_row(canvas *c, int i) {
  RECT r = row_rect(i);
  if (r.bottom <= s.scroll.view.top || r.top >= s.scroll.view.bottom) return;
  BOOL held = s.dragging && i == s.drag_row, focused = i == s.focus && !s.focus_clear && focus_shows();
  control_state state = held ? STATE_PRESSED : i == s.hot ? STATE_HOT : STATE_NORMAL;
  COLORREF text = theme_draw_state(c, &r, scale(RADIUS), state);
  COLORREF dim = theme.high_contrast && state != STATE_NORMAL ? text : theme.text2;
  UINT one_line = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
  if (focused) canvas_frame(c, &r, scale(RADIUS), scale(2), theme.text, 255);

  RECT grip = {r.left, r.top, r.left + scale(GRIP_W), r.bottom};
  if (i == s.hot || held || focused)
    canvas_text(c, L"\xE76F", 1, &grip, one_line | DT_CENTER, s.icon_font, dim); /* GripperBarHorizontal */

  RECT x = remove_rect(i);
  if (i == s.hot || focused) {
    control_state remove = s.press_remove == i ? STATE_PRESSED : i == s.hot && s.hot_remove ? STATE_HOT : STATE_NORMAL;
    COLORREF color = remove == STATE_NORMAL ? dim : theme_draw_state(c, &x, scale(RADIUS), remove);
    canvas_text(c, L"\xE711", 1, &x, one_line | DT_CENTER, s.icon_font, color); /* Cancel */
  }

  const queue_row *rw = &s.rows[i];
  RECT dur = {x.left - rw->duration_w, r.top, x.left, r.bottom};
  canvas_text(c, rw->duration, -1, &dur, one_line | DT_RIGHT, s.sub_font, dim);

  int top = r.top + (r.bottom - r.top - s.line_h - s.sub_line_h) / 2;
  RECT title = {grip.right, top, dur.left - scale(DUR_GAP), top + s.line_h};
  RECT byline = {grip.right, title.bottom, title.right, title.bottom + s.sub_line_h};
  canvas_text(c, rw->title ? rw->title : L"", -1, &title, one_line | DT_END_ELLIPSIS, s.font, text);
  if (rw->byline) canvas_text(c, rw->byline, -1, &byline, one_line | DT_END_ELLIPSIS, s.sub_font, dim);
}

/* A standard button, as the track list's Shuffle is. */
static void paint_clear(canvas *c) {
  int radius = scale(RADIUS);
  BOOL enabled = s.count > 0;
  control_state state = !enabled ? STATE_NORMAL : s.press_clear && s.hot_clear ? STATE_PRESSED : s.hot_clear ? STATE_HOT : STATE_NORMAL;
  COLORREF ink = theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  if (!theme.high_contrast) canvas_fill(c, &s.clear, radius, ink, theme.dark ? 15 : 6);
  COLORREF color = theme_draw_state(c, &s.clear, radius, state);
  canvas_frame(c, &s.clear, radius, 1, theme.high_contrast ? theme.text : theme.line, 255);
  if (s.focus_clear && focus_shows()) canvas_frame(c, &s.clear, radius, scale(2), theme.text, 255);
  canvas_text(c, mc_wstr(MC_STR_QUEUE_CLEAR), -1, &s.clear, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
              s.sub_font, enabled ? color : theme.text3);
}

static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  scroller_paint_start(&s.scroll);
  canvas_clear(c, &client, theme.bg, 255);

  for (int i = 0; i < s.count; i++) paint_row(c, i);
  if (!s.count)
    canvas_text(c, mc_wstr(MC_STR_QUEUE_EMPTY), -1, &s.scroll.view, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                s.font, theme.text2);
  if (s.dragging && s.drop_at >= 0) { /* where the dragged row would go */
    int y = s.scroll.view.top + s.drop_at * scale(ROW_H) - s.scroll.pos;
    RECT line = {scale(PAD), y - scale(DROP_LINE) / 2, client.right - scale(PAD), y - scale(DROP_LINE) / 2 + scale(DROP_LINE)}, isect;
    if (IntersectRect(&isect, &line, &s.scroll.view)) canvas_fill(c, &isect, 0, theme.accent, 255);
  }

  /* Over the ends of the rows that scrolled under them. */
  RECT header = {0, 0, client.right, s.scroll.view.top}, foot = {0, s.scroll.view.bottom, client.right, client.bottom};
  canvas_clear(c, &header, theme.bg, 255);
  canvas_clear(c, &foot, theme.bg, 255);
  RECT title = {scale(PAD) + scale(REMOVE_PAD), 0, s.clear.left - scale(PAD), header.bottom};
  canvas_text(c, mc_wstr(MC_STR_QUEUE), -1, &title, DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
              s.title_font, theme.text);
  paint_clear(c);
  RECT rule = {0, header.bottom - 1, client.right, header.bottom};
  canvas_clear(c, &rule, theme.line, 255);

  scroller_paint(&s.scroll, c);
  if (!s.rounded) canvas_frame(c, &client, 0, 1, theme.high_contrast ? theme.text : theme.line, 255);
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Opening and closing --- */

/* Something else is active now: activated, or NULL for another app's window. Unless that's
 * the main window, it stops looking active, as it only did for the popup's sake. */
static void hide(HWND activated) {
  if (!queue_popup_is_open()) return;
  HWND owner = GetWindow(s.wnd, GW_OWNER);
  end_press();
  ShowWindow(s.wnd, SW_HIDE);
  if (activated != owner) SendMessageW(owner, WM_NCACTIVATE, FALSE, 0);
  InvalidateRect(s.anchor, NULL, FALSE);
}

void queue_popup_close(void) {
  if (!queue_popup_is_open()) return;
  HWND owner = GetWindow(s.wnd, GW_OWNER), refocus = s.refocus;
  s.closing = TRUE;
  SetActiveWindow(owner); /* which hides it (WM_ACTIVATE) */
  s.closing = FALSE;
  hide(owner);
  if (refocus && IsWindow(refocus) && GetActiveWindow() == owner) SetFocus(refocus);
}

/* --- UI Automation (uia.h) ---
 *
 * The nodes: Clear, then the list, whose rows come next, then each row's x, its only child. */

enum { UI_CLEAR, UI_LIST, UI_ROWS };

static int ui_row(int i) {
  return UI_ROWS + i;
}

static int ui_remove(int i) {
  return UI_ROWS + s.count + i;
}

static BOOL ui_node(int i, uia_node *n) {
  n->parent = UIA_ROOT;
  if (i == UIA_ROOT) {
    n->type = UIA_PaneControlTypeId;
    lstrcpynW(n->name, mc_wstr(MC_STR_QUEUE), 512);
    n->first = UI_CLEAR;
    n->count = 2;
    return TRUE;
  }
  if (i == UI_CLEAR) {
    n->type = UIA_ButtonControlTypeId;
    n->flags = UIA_FOCUSABLE | UIA_INVOKABLE | (s.count ? 0 : UIA_DISABLED);
    n->rect = s.clear;
    n->automation_id = L"Clear";
    lstrcpynW(n->name, mc_wstr(MC_STR_QUEUE_CLEAR), 512);
    return TRUE;
  }
  if (i == UI_LIST) {
    n->type = UIA_ListControlTypeId;
    n->flags = UIA_SELECTION;
    n->rect = s.scroll.view;
    n->first = UI_ROWS;
    n->count = s.count;
    n->automation_id = L"Queue";
    lstrcpynW(n->name, mc_wstr(MC_STR_QUEUE), 512);
    if (!s.count) lstrcpynW(n->status, mc_wstr(MC_STR_QUEUE_EMPTY), 64);
    return TRUE;
  }
  int r = i - UI_ROWS;
  if (r < 0 || r >= 2 * s.count) return FALSE;
  if (r >= s.count) { /* an x */
    r -= s.count;
    RECT x = remove_rect(r);
    n->parent = ui_row(r);
    n->type = UIA_ButtonControlTypeId;
    n->flags = UIA_INVOKABLE;
    IntersectRect(&n->rect, &x, &s.scroll.view);
    n->automation_id = L"Remove";
    lstrcpynW(n->name, mc_wstr(MC_STR_QUEUE_REMOVE), 512);
    return TRUE;
  }
  const queue_row *rw = &s.rows[r];
  RECT fill = row_rect(r);
  n->parent = UI_LIST;
  n->type = UIA_ListItemControlTypeId;
  n->flags = UIA_FOCUSABLE | UIA_SELECTABLE | (r == s.focus ? UIA_SELECTED : 0);
  n->first = ui_remove(r);
  n->count = 1;
  IntersectRect(&n->rect, &fill, &s.scroll.view);
  uia_name(n, rw->title ? rw->title : L"", rw->byline ? rw->byline : L"", rw->duration, NULL);
  lstrcpynW(n->help, mc_wstr(MC_STR_QUEUE_ROW_HELP), 256);
  return TRUE;
}

static int ui_focus(void) {
  return s.focus_clear ? UI_CLEAR : s.focus >= 0 ? ui_row(s.focus) : UIA_ROOT;
}

static int ui_at(POINT pt) {
  if (PtInRect(&s.clear, pt)) return UI_CLEAR;
  int r = row_at(pt.x, pt.y);
  return r < 0 ? UIA_ROOT : on_remove(r, pt) ? ui_remove(r) : ui_row(r);
}

static int ui_selection(int i) {
  return i == UI_LIST && s.focus >= 0 ? ui_row(s.focus) : UIA_NONE;
}

static void reveal_row(int i) {
  scroller_reveal(&s.scroll, i * scale(ROW_H), (i + 1) * scale(ROW_H));
}

static void focus_row(int i) {
  if (!s.count) return;
  s.focus = max(0, min(i, s.count - 1));
  s.focus_clear = FALSE;
  reveal_row(s.focus);
  InvalidateRect(s.wnd, NULL, FALSE);
  uia_selected(&ui, ui_row(s.focus));
  uia_focus(&ui);
}

static void focus_clear_button(void) {
  if (!s.count) return;
  s.focus_clear = TRUE;
  InvalidateRect(s.wnd, NULL, FALSE);
  uia_focus(&ui);
}

/* The row of a node of a row or its x, or -1. */
static int ui_row_of(int i) {
  int r = i - UI_ROWS;
  return r < 0 || r >= 2 * s.count ? -1 : r % s.count;
}

static void ui_select(int i) {
  if (ui_row_of(i) >= 0) focus_row(ui_row_of(i));
}

static void ui_invoke(int i) {
  if (i == UI_CLEAR) playback_up_next_clear();
  else if (i >= ui_remove(0) && ui_row_of(i) >= 0) remove_row(ui_row_of(i));
}

static void ui_reveal(int i) {
  if (ui_row_of(i) >= 0) reveal_row(ui_row_of(i));
}

static void ui_set_focus(int i) {
  if (i == UI_CLEAR) focus_clear_button();
  else ui_select(i);
}

static const uia_ops ui_ops = {.node = ui_node, .focus = ui_focus, .at = ui_at, .selection = ui_selection,
                               .select = ui_select, .invoke = ui_invoke, .reveal = ui_reveal,
                               .set_focus = ui_set_focus};

/* After the rows changed: what the nodes are, so clients only start over when they did. */
static void ui_changed(void) {
  unsigned long sig = uia_hash(0, &s.count, sizeof s.count);
  for (int i = 0; i < s.count; i++) sig = uia_hash(sig, &s.rows[i].id, sizeof s.rows[i].id);
  uia_changed(&ui, sig);
}

/* --- Window --- */

/* The arrow keys, Page Up and Down, Home and End go through the rows, and up past the first to
 * Clear, as does Tab, and back; Delete takes out the row, Space or Enter press Clear. Alt+Up and
 * Alt+Down move the row (WM_SYSKEYDOWN). */
static void on_key(WPARAM key) {
  SendMessageW(s.wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  InvalidateRect(s.wnd, NULL, FALSE);
  if (key == VK_ESCAPE) {
    if (s.drag_row >= 0) end_press();
    else queue_popup_close();
    return;
  }
  if (s.drag_row >= 0 || !s.count) return;
  int page = max(1, (s.scroll.view.bottom - s.scroll.view.top) / scale(ROW_H));
  switch (key) {
    case VK_TAB:
      if (s.focus_clear) focus_row(s.focus);
      else focus_clear_button();
      break;
    case VK_UP:
      if (s.focus_clear) break;
      if (s.focus <= 0) focus_clear_button();
      else focus_row(s.focus - 1);
      break;
    case VK_DOWN: focus_row(s.focus_clear ? 0 : s.focus + 1); break;
    case VK_PRIOR: focus_row(s.focus - page); break;
    case VK_NEXT: focus_row(s.focus + page); break;
    case VK_HOME: focus_row(0); break;
    case VK_END: focus_row(s.count - 1); break;
    case VK_DELETE:
      if (!s.focus_clear) remove_row(s.focus);
      break;
    case VK_SPACE:
    case VK_RETURN:
      if (s.focus_clear) playback_up_next_clear();
      break;
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
      /* A click elsewhere, or another window. If it was on the queue button, that click
       * mustn't open it again straight away. */
      POINT pt;
      GetCursorPos(&pt);
      s.anchor_closed = !s.closing && WindowFromPoint(pt) == s.anchor;
      s.closed_at = GetTickCount();
      hide((HWND)lp);
      return 0;
    }
    case WM_CLOSE: /* Alt+F4 */
      queue_popup_close();
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
      if (wp == TIMER_DRAG_SCROLL) on_drag_timer();
      else scroller_timer(&s.scroll, wp);
      return 0;
    case WM_MOUSEMOVE: {
      if (!s.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        s.tracking = TRUE;
      }
      s.mouse.x = GET_X_LPARAM(lp);
      s.mouse.y = GET_Y_LPARAM(lp);
      if (s.drag_row >= 0) {
        drag_to(s.mouse.x, s.mouse.y);
        return 0;
      }
      scroller_mouse_move(&s.scroll, s.mouse.x, s.mouse.y);
      update_hover();
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      scroller_mouse_leave(&s.scroll);
      update_hover();
      return 0;
    case WM_MOUSEWHEEL:
      scroller_wheel(&s.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      s.mouse = pt;
      if (scroller_mouse_down(&s.scroll, pt.x, pt.y)) return 0;
      int i = row_at(pt.x, pt.y);
      if (s.count && PtInRect(&s.clear, pt)) s.press_clear = TRUE;
      else if (on_remove(i, pt)) s.press_remove = i;
      else if (i >= 0) {
        s.drag_row = i;
        s.drag_from = pt;
      } else return 0;
      if (i >= 0) {
        s.focus = i;
        s.focus_clear = FALSE;
      }
      SetCapture(wnd);
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      s.mouse = pt;
      int from = s.drag_row, to = s.drop_at, remove = s.press_remove;
      BOOL moved = s.dragging, clear = s.press_clear;
      scroller_mouse_up(&s.scroll);
      end_press();
      if (moved) move_row(from, to);
      else if (on_remove(remove, pt)) remove_row(remove);
      else if (clear && PtInRect(&s.clear, pt)) playback_up_next_clear();
      return 0;
    }
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&s.scroll);
      end_press();
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_SYSKEYDOWN:
      if ((wp == VK_UP || wp == VK_DOWN) && !s.focus_clear && s.focus >= 0 && s.drag_row < 0) {
        SendMessageW(wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        move_row(s.focus, wp == VK_UP ? max(s.focus - 1, 0) : s.focus + 2);
        reveal_row(s.focus);
        return 0;
      }
      break;
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

/* The track list's sizes, at the window's dpi. */
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
  s.icon_font = CreateFontW(-scale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
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
                    .lpszClassName = L"kithsong_queue"};
  RegisterClassExW(&wc);
  /* Its text names it for screen readers. */
  CreateWindowExW(WS_EX_TOOLWINDOW, L"kithsong_queue", mc_wstr(MC_STR_QUEUE), WS_POPUP, 0, 0, 0, 0, owner, NULL,
                  GetModuleHandleW(NULL), NULL);
  if (!s.wnd) return FALSE;
  s.scroll.moved = on_scrolled;
  s.hot = s.drag_row = s.drop_at = s.press_remove = -1;
  int corners = CORNERS_ROUND_SMALL;
  s.rounded = SUCCEEDED(DwmSetWindowAttribute(s.wnd, DWMWA_CORNERS, &corners, sizeof corners));
  s.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s.wnd, NULL,
                              GetModuleHandleW(NULL), NULL);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = s.wnd, .uId = 1,
                    .lpszText = (wchar_t *)mc_wstr(MC_STR_QUEUE_REMOVE)};
  SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  return TRUE;
}

void queue_popup_toggle(HWND anchor, UINT dpi) {
  if (queue_popup_is_open()) {
    queue_popup_close();
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
  s.refocus = GetFocus();
  if (dpi != s.dpi || !s.font) make_fonts(dpi);
  theme_apply_window(s.wnd, FALSE); /* Windows 11's border, dark or light */
  theme_apply_control(s.tooltip);
  s.drag_row = s.drop_at = s.press_remove = -1;
  s.focus_clear = FALSE;
  s.focus = -1;
  reload();

  /* Under the button, their right edges in line, kept on the screen. */
  RECT a;
  GetWindowRect(anchor, &a);
  int rows = max(MIN_ROWS, min(s.count, MAX_ROWS));
  int w = scale(WIDTH), h = scale(HEADER_H + rows * ROW_H + PAD);
  int x = a.right - w, y = a.bottom + scale(DROP);
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), &mi)) {
    x = max(mi.rcWork.left, min(x, mi.rcWork.right - w));
    y = max(mi.rcWork.top, min(y, mi.rcWork.bottom - h));
  }
  s.hot = -1;
  s.hot_remove = s.hot_clear = FALSE;
  SetWindowPos(s.wnd, HWND_TOP, x, y, w, h, SWP_NOACTIVATE);
  relayout();
  scroller_jump(&s.scroll, 0);
  /* Focus rectangles as the main window has them: shown once the keyboard has been used. */
  BOOL hide_focus = (SendMessageW(owner, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) != 0;
  SendMessageW(s.wnd, WM_CHANGEUISTATE, MAKEWPARAM(hide_focus ? UIS_SET : UIS_CLEAR, UISF_HIDEFOCUS), 0);
  ShowWindow(s.wnd, SW_SHOW);
  InvalidateRect(anchor, NULL, FALSE);
}
