/* Settings' Testing section, below its heading and the button that opens it (settings_view.c
 * has those): the profiler (kithcore/perf.h) with its report, and stress test libraries
 * (kithcore/stress.h).
 *
 *   [x] Profile the app's speed
 *   Startup is measured from the next start ...
 *   [Run Scroll Test] [Copy] [Save...] [Clear]
 *
 *   Stress test library
 *   [10,000 tracks v] [Generate and Open] [Cancel]
 *   Generating... 40%
 *   [Back to My Library] [Delete Stress Libraries...]
 *   Generated libraries are kept apart from your music ...
 *
 *   +--------------------------+
 *   | the report               |
 *   +--------------------------+
 *
 * The controls are children of the Settings page's content window, so they scroll with it, and
 * their messages reach the main window, which passes them on through settings_view.c. The
 * report grows to fit, so the page scrolls rather than a box inside it. */
#ifndef KITHSONG_TESTING_VIEW_H
#define KITHSONG_TESTING_VIEW_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* What the main window does for it. */
typedef struct testing_host {
  void (*open_stress)(const char *dir); /* opens stress library dir, remembering the way back */
  void (*back)(void);                   /* goes back to the library the user came from */
  void (*run_scroll_test)(void);
  mc_library *(*library)(void);         /* the open one; NULL if none */
} testing_host;

/* parent is the window the controls go in; relayout is told when they need more or less room.
 * Call between the page's sections, for the tab order. */
void testing_view_create(HWND parent, void (*relayout)(void));
void testing_view_destroy(void);
/* Call once, before the section can be opened. */
void testing_view_init(mc_settings *settings, const char *config_dir, const testing_host *host);
void testing_view_set_font(HFONT font, UINT dpi);
/* Whether the page is showing, and whether the section is open: its controls show when both are. */
void testing_view_show(BOOL page_shown, BOOL open);
/* Lays the open section out from top at left, text_w wide, and returns where it ends. */
int testing_view_layout(int left, int top, int text_w, UINT dpi);
/* A library opened, or a scroll test ended: brings the buttons and the report up to date. */
void testing_view_update(void);
BOOL testing_view_on_command(int id, int code);
/* As settings_view_ctlcolor(). */
HBRUSH testing_view_ctlcolor(HWND wnd, HDC dc);

#endif
