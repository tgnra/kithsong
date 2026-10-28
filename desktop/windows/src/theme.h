/* Colours and system theming. Follows the Windows app mode (light or dark), high contrast
 * and the accent colour; gives the main window Mica on Windows 11, and dark mode to the
 * standard controls.
 *
 * Dark standard controls rely on undocumented parts of Windows: the "DarkMode_*" visual
 * style classes and uxtheme's SetPreferredAppMode (for menus). Where those are missing the
 * controls stay light; everything we draw ourselves still follows the theme.
 *
 * KITHSONG_THEME=light or KITHSONG_THEME=dark in the environment overrides the app mode.
 */
#ifndef KITHSONG_THEME_H
#define KITHSONG_THEME_H

#include <windows.h>
#include <commctrl.h>

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
/* Themes the standard controls among wnd's children: buttons, list views, edits, combo
 * boxes, progress bars and scrollbars. Call again after a theme change. */
void theme_apply_controls(HWND wnd);
void theme_apply_control(HWND ctl);
/* For WM_CTLCOLOR* messages: sets dc's colours and returns the background brush, or NULL
 * for other messages. */
HBRUSH theme_ctlcolor(UINT msg, HDC dc);

/* The backdrop of a hovered, pressed or selected item. Returns the colour for text on it. */
COLORREF theme_draw_state(canvas *c, const RECT *r, int radius, control_state state);

/* One button of a segmented control (a row of joined buttons, one of them selected), filling
 * r: first and last say which ends of the row it is, which are rounded. state is the
 * button's own (hot, pressed). */
void theme_draw_segment(canvas *c, const RECT *r, int radius, const wchar_t *text, HFONT font, BOOL first,
                        BOOL last, BOOL selected, control_state state);

/* State images for a list view's LVS_EX_CHECKBOXES, size pixels square, drawn as Windows 11
 * draws checkboxes in the current theme: unchecked, then checked. Set them with
 * ListView_SetImageList(..., LVSIL_STATE) after the style, and again after theme and DPI
 * changes; the list view frees the ones it has when it goes. */
HIMAGELIST theme_checkboxes(int size);

/* A single-line edit draws its text at the top of its client area. This keeps the client
 * area as tall as the text, in the middle of the edit, so the text is centred however tall
 * the edit is. Call once, after creating it. */
void theme_center_edit(HWND edit);

/* Segoe Fluent Icons, or Segoe MDL2 Assets (same code points) on Windows 10. */
const wchar_t *theme_icon_face(void);

#endif
