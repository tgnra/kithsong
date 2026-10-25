/* The Settings page, beside the sidebar:
 *
 *   +--------------------------------------------------+
 *   | Settings                                         |
 *   |                                                  |
 *   | Library location                                 |
 *   | C:\Users\me\Music                                |
 *   | [Move Library...] [Open Another Library...]      |
 *   | Opening another library switches to ...          |
 *   +--------------------------------------------------+
 *
 * Moving the library isn't implemented yet, so its button is disabled. Open Another Library
 * posts WM_APP_OPEN_LIBRARY to the main window, which asks for the folder.
 */
#ifndef MUSICAPP_SETTINGS_VIEW_H
#define MUSICAPP_SETTINGS_VIEW_H

#include <windows.h>

#define WM_APP_OPEN_LIBRARY (WM_APP + 5)

void settings_view_create(HWND parent);
void settings_view_destroy(void);
/* The library folder to show (UTF-8; NULL shows nothing). */
void settings_view_set_library_dir(const char *dir);
/* font is the main window's; the headings are made from it. */
void settings_view_set_font(HFONT font, UINT dpi);
void settings_view_show(BOOL show);
void settings_view_layout(int x, int y, int w, int h, UINT dpi);

/* Returns TRUE if cmd belonged to the settings view. */
BOOL settings_view_on_command(int id);
/* For the parent's WM_CTLCOLORSTATIC: sets dc's colours and returns the background brush,
 * or NULL if wnd isn't one of the view's labels. */
HBRUSH settings_view_ctlcolor(HWND wnd, HDC dc);

#endif
