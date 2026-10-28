/* Pixel scrolling for the custom-drawn views (album grid, track list): wheel and keyboard
 * scrolling eased over a few frames, and a thin overlay scrollbar, Windows 11 style, that
 * shows while the mouse moves over the view or it scrolls, and widens under the mouse.
 *
 * The view owns the window and forwards its mouse, wheel and timer messages here; it draws
 * the bar with scroller_paint. The timer ids below are the scroller's.
 */
#ifndef KITHSONG_SCROLLER_H
#define KITHSONG_SCROLLER_H

#include <windows.h>

#include "canvas.h"

enum { SCROLLER_TIMER_ANIMATE = 101, SCROLLER_TIMER_HIDE = 102 };

typedef struct scroller {
  HWND wnd;
  void (*moved)(void); /* after pos changes; the view area is already invalidated */
  RECT view;           /* the area that scrolls, in client coordinates */
  int content;         /* height of what scrolls */
  int pos, target;     /* pixels scrolled, and where an animation is heading */
  BOOL animating, shown, hot, dragging;
  int drag_dy; /* where on the thumb it was grabbed */
} scroller;

/* Call when the view or the content changes size. Keeps pos in range, without calling moved. */
void scroller_resize(scroller *s, const RECT *view, int content);
int scroller_max(const scroller *s);

void scroller_jump(scroller *s, int pos);
void scroller_animate(scroller *s, int pos);
/* SB_LINEUP, SB_LINEDOWN, SB_PAGEUP, SB_PAGEDOWN, SB_TOP or SB_BOTTOM; line is the step for
 * a line. */
void scroller_command(scroller *s, int code, int line);
void scroller_wheel(scroller *s, int delta);
/* Scrolls as little as needed to bring content [top, bottom) into view. */
void scroller_reveal(scroller *s, int top, int bottom);

/* Mouse messages. They return TRUE when the mouse is on the scrollbar (or dragging it), so
 * the view shouldn't treat it as over its content. */
BOOL scroller_mouse_move(scroller *s, int x, int y);
BOOL scroller_mouse_down(scroller *s, int x, int y);
void scroller_mouse_up(scroller *s);
void scroller_mouse_leave(scroller *s);
void scroller_capture_changed(scroller *s);
/* WM_TIMER: TRUE if it was one of the scroller's. */
BOOL scroller_timer(scroller *s, UINT_PTR id);

void scroller_paint(scroller *s, canvas *c);

#endif
