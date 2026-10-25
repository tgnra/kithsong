#include "scroller.h"

#include "theme.h"

/* Scrollbar metrics at 96 DPI. */
enum {
  BAR_THIN = 3, /* width at rest, and under the mouse */
  BAR_WIDE = 6,
  BAR_MARGIN = 3,
  BAR_HIT = 14, /* how close to the right edge the mouse takes the scrollbar */
  BAR_MIN = 32, /* shortest thumb */
  HIDE_MS = 1500,
};

static int scale(const scroller *s, int px) {
  return MulDiv(px, (int)GetDpiForWindow(s->wnd), USER_DEFAULT_SCREEN_DPI);
}

static int view_h(const scroller *s) {
  return s->view.bottom - s->view.top;
}

int scroller_max(const scroller *s) {
  return s->content > view_h(s) ? s->content - view_h(s) : 0;
}

static int clamp(const scroller *s, int pos) {
  return pos < 0 ? 0 : pos > scroller_max(s) ? scroller_max(s) : pos;
}

void scroller_resize(scroller *s, const RECT *view, int content) {
  s->view = *view;
  s->content = content;
  s->pos = clamp(s, s->pos);
  s->target = clamp(s, s->target);
}

/* The thumb, or FALSE if everything fits. */
static BOOL thumb_rect(const scroller *s, RECT *out) {
  int max = scroller_max(s);
  if (max <= 0) return FALSE;
  int margin = scale(s, BAR_MARGIN), track_h = view_h(s) - 2 * margin;
  int thumb_h = MulDiv(track_h, view_h(s), s->content);
  if (thumb_h < scale(s, BAR_MIN)) thumb_h = scale(s, BAR_MIN);
  int top = s->view.top + margin + MulDiv(track_h - thumb_h, s->pos, max);
  int width = scale(s, s->hot || s->dragging ? BAR_WIDE : BAR_THIN), right = s->view.right - margin;
  SetRect(out, right - width, top, right, top + thumb_h);
  return TRUE;
}

static void invalidate_bar(const scroller *s) {
  RECT strip = {s->view.right - scale(s, BAR_HIT), s->view.top, s->view.right, s->view.bottom};
  InvalidateRect(s->wnd, &strip, FALSE);
}

static void show(scroller *s) {
  if (!s->shown) invalidate_bar(s);
  s->shown = TRUE;
  SetTimer(s->wnd, SCROLLER_TIMER_HIDE, HIDE_MS, NULL);
}

static void set_pos(scroller *s, int pos) {
  pos = clamp(s, pos);
  if (pos == s->pos) return;
  s->pos = pos;
  InvalidateRect(s->wnd, &s->view, FALSE);
  show(s);
  if (s->moved) s->moved();
}

void scroller_jump(scroller *s, int pos) {
  if (s->animating) KillTimer(s->wnd, SCROLLER_TIMER_ANIMATE);
  s->animating = FALSE;
  set_pos(s, pos);
  s->target = s->pos;
}

void scroller_animate(scroller *s, int pos) {
  s->target = clamp(s, pos);
  if (!s->animating && s->target != s->pos) {
    s->animating = TRUE;
    SetTimer(s->wnd, SCROLLER_TIMER_ANIMATE, USER_TIMER_MINIMUM, NULL);
  }
}

void scroller_command(scroller *s, int code, int line) {
  int page = view_h(s) * 9 / 10;
  switch (code) {
    case SB_LINEUP: scroller_animate(s, s->target - line); break;
    case SB_LINEDOWN: scroller_animate(s, s->target + line); break;
    case SB_PAGEUP: scroller_animate(s, s->target - page); break;
    case SB_PAGEDOWN: scroller_animate(s, s->target + page); break;
    case SB_TOP: scroller_jump(s, 0); break;
    case SB_BOTTOM: scroller_jump(s, scroller_max(s)); break;
  }
}

void scroller_wheel(scroller *s, int delta) {
  UINT lines = 3;
  SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
  int px = lines == WHEEL_PAGESCROLL ? view_h(s) * 9 / 10 : (int)lines * scale(s, 40);
  scroller_animate(s, s->target - MulDiv(delta, px, WHEEL_DELTA));
}

void scroller_reveal(scroller *s, int top, int bottom) {
  if (top < s->target) scroller_animate(s, top);
  else if (bottom > s->target + view_h(s)) scroller_animate(s, bottom - view_h(s));
}

BOOL scroller_mouse_move(scroller *s, int x, int y) {
  if (s->dragging) {
    RECT thumb;
    if (!thumb_rect(s, &thumb)) return TRUE;
    int margin = scale(s, BAR_MARGIN), travel = view_h(s) - 2 * margin - (thumb.bottom - thumb.top);
    if (travel > 0) scroller_jump(s, MulDiv(y - s->drag_dy - s->view.top - margin, scroller_max(s), travel));
    return TRUE;
  }
  POINT pt = {x, y};
  BOOL hot = scroller_max(s) > 0 && PtInRect(&s->view, pt) && x >= s->view.right - scale(s, BAR_HIT);
  if (hot != s->hot) {
    s->hot = hot;
    invalidate_bar(s);
  }
  show(s);
  return hot;
}

BOOL scroller_mouse_down(scroller *s, int x, int y) {
  RECT thumb;
  if (!scroller_mouse_move(s, x, y) || !thumb_rect(s, &thumb)) return FALSE;
  if (y >= thumb.top && y < thumb.bottom) {
    s->dragging = TRUE;
    s->drag_dy = y - thumb.top;
    SetCapture(s->wnd);
  } else {
    scroller_command(s, y < thumb.top ? SB_PAGEUP : SB_PAGEDOWN, 0);
  }
  return TRUE;
}

void scroller_mouse_up(scroller *s) {
  if (s->dragging) ReleaseCapture();
}

void scroller_mouse_leave(scroller *s) {
  if (s->dragging) return;
  s->hot = FALSE;
  invalidate_bar(s);
}

void scroller_capture_changed(scroller *s) {
  s->dragging = FALSE;
  invalidate_bar(s);
}

BOOL scroller_timer(scroller *s, UINT_PTR id) {
  if (id == SCROLLER_TIMER_ANIMATE) {
    int diff = s->target - s->pos, step = diff / 4;
    if (step == 0) step = diff;
    set_pos(s, s->pos + step);
    if (s->pos == s->target || step == 0) {
      KillTimer(s->wnd, SCROLLER_TIMER_ANIMATE);
      s->animating = FALSE;
    }
    return TRUE;
  }
  if (id == SCROLLER_TIMER_HIDE) {
    if (!s->hot && !s->dragging) {
      KillTimer(s->wnd, SCROLLER_TIMER_HIDE);
      s->shown = FALSE;
      invalidate_bar(s);
    }
    return TRUE;
  }
  return FALSE;
}

void scroller_paint(scroller *s, canvas *c) {
  RECT thumb;
  if (s->shown && thumb_rect(s, &thumb))
    canvas_fill(c, &thumb, thumb.right - thumb.left, theme.text, s->hot || s->dragging ? 140 : 110);
}
