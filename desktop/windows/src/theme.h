/* Colours and system theming. Follows the Windows app mode (light or dark), high contrast
 * and the accent colour; gives the main window Mica on Windows 11, and dark mode to the
 * standard controls.
 *
 * Dark standard controls rely on undocumented parts of Windows: the "DarkMode_*" visual
 * style classes and uxtheme's SetPreferredAppMode (for menus). Where those are missing the
 * controls stay light; everything we draw ourselves still follows the theme.
 *
 * MUSICAPP_THEME=light or MUSICAPP_THEME=dark in the environment overrides the app mode.
 */
#ifndef MUSICAPP_THEME_H
#define MUSICAPP_THEME_H

#include <windows.h>

#include "canvas.h"

/* Corner radius of the content area's top-left corner, at 96 DPI. */
#define CONTENT_RADIUS 8

typedef struct theme_colors {
  BOOL dark, high_contrast;
  BOOL mica;      /* the main window shows Mica behind the toolbar and sidebar */
  COLORREF bg;    /* content: album grid, import view, dialogs */
  COLORREF frame; /* toolbar and sidebar when there's no Mica */
  COLORREF field; /* text boxes */
  COLORREF text, text2, text3; /* primary, secondary, tertiary (hints) */
  COLORREF line;               /* dividers and borders */
  COLORREF accent, on_accent;  /* accent fills, and text on them */
  HBRUSH bg_brush, field_brush;
} theme_colors;

extern theme_colors theme;

typedef enum { STATE_NORMAL, STATE_HOT, STATE_PRESSED, STATE_SELECTED } control_state;

/* Reads the Windows settings. Call at startup and whenever theme_is_change says so. */
void theme_update(void);
/* TRUE for a message that means the theme may have changed. */
BOOL theme_is_change(UINT msg, WPARAM wp, LPARAM lp);

/* A dark or light title bar to match. With mica, also asks for a Mica backdrop and sets
 * theme.mica to whether there is one. */
void theme_apply_window(HWND wnd, BOOL mica);
/* Themes the standard controls among wnd's children: buttons, list views, edits, progress
 * bars and scrollbars. Call again after a theme change. */
void theme_apply_controls(HWND wnd);
void theme_apply_control(HWND ctl);
/* For WM_CTLCOLOR* messages: sets dc's colours and returns the background brush, or NULL
 * for other messages. */
HBRUSH theme_ctlcolor(UINT msg, HDC dc);

/* The backdrop of a hovered, pressed or selected item. Returns the colour for text on it. */
COLORREF theme_draw_state(canvas *c, const RECT *r, int radius, control_state state);

/* Segoe Fluent Icons, or Segoe MDL2 Assets (same code points) on Windows 10. */
const wchar_t *theme_icon_face(void);

#endif
