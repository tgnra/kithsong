/* The navigation list on the left: an owner-drawn list box of pages under section headers,
 * with Import and Settings at the bottom. Only pages can be selected, by mouse or keyboard;
 * selecting one sends the parent WM_COMMAND with LBN_SELCHANGE. */
#ifndef MUSICAPP_SIDEBAR_H
#define MUSICAPP_SIDEBAR_H

#include <windows.h>

typedef enum {
  PAGE_ALBUMS,
  PAGE_ARTISTS,
  PAGE_PLAYLISTS,
  PAGE_IMPORT,
  PAGE_SETTINGS,
} sidebar_page;

/* Creates the list as a child of parent, with Albums selected. Only one exists at a time. */
HWND sidebar_create(HWND parent, int id);

/* Rebuilds fonts, row heights and colours. Call after creating it, and on DPI or theme
 * changes (including theme.mica). */
void sidebar_update(UINT dpi);

sidebar_page sidebar_selection(void);
/* Selects a page without notifying the parent. */
void sidebar_select(sidebar_page page);
/* While locked, every page but the selected one is greyed out and can't be selected. */
void sidebar_set_locked(BOOL locked);

/* Paints a row: call from the parent's WM_DRAWITEM for the sidebar. */
void sidebar_draw(const DRAWITEMSTRUCT *dis);

/* The background brush for the parent's WM_CTLCOLORLISTBOX, or NULL if wnd isn't the sidebar. */
HBRUSH sidebar_ctlcolor(HWND wnd);

#endif
