/* See analysis_dialog.h. */
#include "analysis_dialog.h"

#include <commctrl.h>
#include <stdlib.h>

#include "dlg_template.h"
#include "kithcore/kithcore.h"
#include "theme.h"
#include "wstr.h"

enum { ID_TEXT = 100, ID_BAR, ID_STATUS, ID_BACKGROUND, ID_STOP };

/* Dialog units. */
enum { DLG_W = 220, DLG_H = 82, MARGIN = 7, BTN_W = 56, BTN_H = 14, TEXT_H = 18, BAR_Y = 29, STATUS_Y = 43 };

static struct {
  HWND wnd;
  const analysis_dialog_host *host;
} g;

static void apply_theme(HWND dlg) {
  theme_apply_window(dlg, FALSE);
  theme_apply_controls(dlg);
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG:
      apply_theme(dlg);
      return FALSE; /* the focus stays where it is */
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      return (INT_PTR)theme_ctlcolor(msg, (HDC)wp);
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update();
        apply_theme(dlg);
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_COMMAND: {
      const analysis_dialog_host *host = g.host;
      if (LOWORD(wp) == ID_STOP) {
        analysis_dialog_close();
        host->cancel();
        return TRUE;
      }
      if (LOWORD(wp) == ID_BACKGROUND || LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
        analysis_dialog_close();
        host->background();
        return TRUE;
      }
      break;
    }
    case WM_NCDESTROY:
      g.wnd = NULL;
      break;
  }
  return FALSE;
}

void analysis_dialog_show(HWND owner, const analysis_dialog_host *host) {
  if (g.wnd) return;
  dlg_template *t = malloc(sizeof *t);
  if (!t) return;
  int w = DLG_W - 2 * MARGIN, by = DLG_H - MARGIN - BTN_H;
  dlg_template_header(t, 5, DLG_W, DLG_H, mc_wstr(MC_STR_ANALYSIS_TITLE));
  dlg_template_item(t, 0x0082, NULL, mc_wstr(MC_STR_ANALYSIS_TEXT), SS_LEFT | SS_NOPREFIX, MARGIN, MARGIN, w, TEXT_H,
                    ID_TEXT);
  dlg_template_item(t, 0, PROGRESS_CLASSW, L"", PBS_SMOOTH, MARGIN, BAR_Y, w, 9, ID_BAR);
  dlg_template_item(t, 0x0082, NULL, L"", SS_LEFT | SS_NOPREFIX, MARGIN, STATUS_Y, w, 9, ID_STATUS);
  dlg_template_item(t, 0x0080, NULL, mc_wstr_mnemonic(MC_STR_ANALYSIS_BACKGROUND), BS_DEFPUSHBUTTON | WS_TABSTOP,
                    DLG_W - MARGIN - 2 * BTN_W - 4, by, BTN_W, BTN_H, ID_BACKGROUND);
  dlg_template_item(t, 0x0080, NULL, mc_wstr(MC_STR_CANCEL), BS_PUSHBUTTON | WS_TABSTOP, DLG_W - MARGIN - BTN_W, by,
                    BTN_W, BTN_H, ID_STOP);
  g.host = host;
  g.wnd = CreateDialogIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, owner, dialog_proc, 0);
  free(t);
  if (g.wnd) ShowWindow(g.wnd, SW_SHOWNOACTIVATE);
}

BOOL analysis_dialog_is_open(void) {
  return g.wnd != NULL;
}

void analysis_dialog_update(int done, int total) {
  if (!g.wnd) return;
  SendDlgItemMessageW(g.wnd, ID_BAR, PBM_SETRANGE32, 0, total);
  SendDlgItemMessageW(g.wnd, ID_BAR, PBM_SETPOS, (WPARAM)done, 0);
  wchar_t text[128];
  mc_wformat(text, 128, mc_str(MC_STR_ANALYSIS_PROGRESS), MC_D(done), MC_D(total));
  SetDlgItemTextW(g.wnd, ID_STATUS, text);
}

void analysis_dialog_close(void) {
  if (g.wnd) DestroyWindow(g.wnd);
}

BOOL analysis_dialog_translate(MSG *msg) {
  return g.wnd && IsDialogMessageW(g.wnd, msg);
}
