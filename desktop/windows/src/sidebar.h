/* The navigation list on the left: an owner-drawn list box of navigation entries under
 * section headers. Only navigation entries can be selected, by mouse or keyboard; selecting
 * one sends the parent WM_COMMAND with LBN_SELCHANGE. */
#ifndef MUSICAPP_SIDEBAR_H
#define MUSICAPP_SIDEBAR_H

#include <windows.h>

/* Creates the list as a child of parent. Only one exists at a time. */
HWND sidebar_create(HWND parent, int id);

/* Rebuilds fonts, row heights and colours. Call after creating it, and on DPI or theme
 * changes (including theme.mica). */
void sidebar_update(UINT dpi);

/* Paints a row: call from the parent's WM_DRAWITEM for the sidebar. */
void sidebar_draw(const DRAWITEMSTRUCT *dis);

/* The background brush for the parent's WM_CTLCOLORLISTBOX, or NULL if wnd isn't the sidebar. */
HBRUSH sidebar_ctlcolor(HWND wnd);

#endif
