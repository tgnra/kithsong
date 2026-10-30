/* Dialog templates built in memory (DLGTEMPLATEEX), for dialogs laid out at run time rather
 * than in app.rc: WORDs, with items aligned to DWORDs. Fill one with dlg_template_header()
 * and dlg_template_item(), then pass buf to CreateDialogIndirectParamW() or
 * DialogBoxIndirectParamW(). What doesn't fit is dropped. */
#ifndef KITHSONG_DLG_TEMPLATE_H
#define KITHSONG_DLG_TEMPLATE_H

#include <windows.h>

typedef struct dlg_template {
  WORD buf[2048];
  int n;
} dlg_template;

/* Starts the template over: a centred dialog with a modal frame, caption and system menu, w by
 * h dialog units, called title, in 9 pt Segoe UI, with num_items items to follow. */
void dlg_template_header(dlg_template *t, int num_items, int w, int h, const wchar_t *title);

/* Adds an item (visible, a child): atom is a predefined class (0x0080 button, 0x0081 edit,
 * 0x0082 static), or 0 for the class called cls. */
void dlg_template_item(dlg_template *t, WORD atom, const wchar_t *cls, const wchar_t *text, DWORD style, int x,
                       int y, int w, int h, int id);

#endif
