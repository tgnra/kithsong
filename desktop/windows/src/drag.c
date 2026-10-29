#include "drag.h"

enum { MAX_TARGETS = 4 };

static struct {
  struct {
    HWND wnd;
    drag_hover_fn hover;
  } targets[MAX_TARGETS];
  int count;
  int over; /* the target that last answered, or -1 */
} d = {.over = -1};

void drag_add_target(HWND wnd, drag_hover_fn hover) {
  for (int i = 0; i < d.count; i++)
    if (d.targets[i].wnd == wnd) {
      d.targets[i].hover = hover;
      return;
    }
  if (d.count == MAX_TARGETS) return;
  d.targets[d.count].wnd = wnd;
  d.targets[d.count++].hover = hover;
}

/* The target that wnd is, or is inside, or -1. */
static int target_of(HWND wnd) {
  for (; wnd; wnd = GetParent(wnd))
    for (int i = 0; i < d.count; i++)
      if (d.targets[i].wnd == wnd && IsWindowVisible(wnd)) return i;
  return -1;
}

void drag_leave(void) {
  if (d.over >= 0 && d.over < d.count && IsWindow(d.targets[d.over].wnd)) d.targets[d.over].hover(DRAG_TRACK, NULL, NULL);
  d.over = -1;
}

void drag_set_cursor(BOOL allowed) {
  SetCursor(LoadCursorW(NULL, allowed ? (LPCWSTR)IDC_ARROW : (LPCWSTR)IDC_NO));
}

BOOL drag_over(drag_kind kind, POINT screen, char uid[MC_UID_STRLEN]) {
  uid[0] = '\0';
  int i = target_of(WindowFromPoint(screen));
  if (i != d.over) drag_leave();
  BOOL ok = FALSE;
  if (i >= 0) {
    d.over = i;
    POINT pt = screen;
    ScreenToClient(d.targets[i].wnd, &pt);
    ok = d.targets[i].hover(kind, &pt, uid);
    if (!ok) uid[0] = '\0';
  }
  drag_set_cursor(ok);
  return ok;
}

void drag_done(void) {
  drag_leave();
  drag_set_cursor(TRUE);
}
