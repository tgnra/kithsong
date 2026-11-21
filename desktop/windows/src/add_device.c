#include "add_device.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "dlg_template.h"
#include "theme.h"
#include "wstr.h"

enum { ID_NOTE = 100, ID_ADDRESS_LABEL, ID_ADDRESS, ID_PORT_LABEL, ID_PORT, ID_SPIN };

/* Layout, in dialog units. */
enum { DLG_W = 220, DLG_H = 114, MARGIN = 7, BTN_W = 50, BTN_H = 14 };

/* What Connect closed the dialog with. */
typedef struct result {
  char *host; /* malloc'd UTF-8 */
  uint16_t port;
} result;

/* --- The template --- */

static void build(dlg_template *t) {
  dlg_template_header(t, 8, DLG_W, DLG_H, mc_wstr(MC_STR_ADD_DEVICE));

  int w = DLG_W - 2 * MARGIN, y = MARGIN;
  dlg_template_item(t, 0x0082, NULL, mc_wstr(MC_STR_ADD_DEVICE_TEXT), SS_LEFT | SS_NOPREFIX, MARGIN, y, w, 18, ID_NOTE);
  y += 25;
  wchar_t label[128];
  dlg_template_item(t, 0x0082, NULL, wstr_field_label(label, 128, MC_STR_ADD_DEVICE_HOST), SS_LEFT, MARGIN, y, w, 9,
           ID_ADDRESS_LABEL);
  y += 11;
  dlg_template_item(t, 0x0081, NULL, L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, MARGIN, y, w, 14, ID_ADDRESS);
  y += 20;
  dlg_template_item(t, 0x0082, NULL, wstr_field_label(label, 128, MC_STR_ADD_DEVICE_PORT), SS_LEFT, MARGIN, y, w, 9,
           ID_PORT_LABEL);
  y += 11;
  dlg_template_item(t, 0x0081, NULL, L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, MARGIN, y, 54, 14, ID_PORT);
  /* Attaches itself to the port box, the control before it. */
  dlg_template_item(t, 0, UPDOWN_CLASSW, L"",
           UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_AUTOBUDDY | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, 0, 0,
           0, 0, ID_SPIN);
  int by = DLG_H - MARGIN - BTN_H;
  dlg_template_item(t, 0x0080, NULL, mc_wstr_mnemonic(MC_STR_CONNECT), BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED,
           DLG_W - MARGIN - 2 * BTN_W - 4, by, BTN_W, BTN_H, IDOK);
  dlg_template_item(t, 0x0080, NULL, mc_wstr_mnemonic(MC_STR_CANCEL), BS_PUSHBUTTON | WS_TABSTOP, DLG_W - MARGIN - BTN_W, by,
           BTN_W, BTN_H, IDCANCEL);
}

/* --- The dialog --- */

/* The trimmed address, as a malloc'd UTF-8 string; NULL if there's none, or it has spaces inside. */
static char *address_text(HWND dlg) {
  wchar_t buf[256];
  GetDlgItemTextW(dlg, ID_ADDRESS, buf, 256);
  wchar_t *start = buf, *end = buf + wcslen(buf);
  while (*start == L' ' || *start == L'\t') start++;
  while (end > start && (end[-1] == L' ' || end[-1] == L'\t')) end--;
  *end = L'\0';
  if (!*start || wcspbrk(start, L" \t")) return NULL;
  int n = WideCharToMultiByte(CP_UTF8, 0, start, -1, NULL, 0, NULL, NULL);
  char *host = n > 0 ? malloc((size_t)n) : NULL;
  if (host) WideCharToMultiByte(CP_UTF8, 0, start, -1, host, n, NULL, NULL);
  return host;
}

/* The port typed, or 0 if it isn't one. */
static uint16_t port_value(HWND dlg) {
  BOOL ok = FALSE;
  UINT port = GetDlgItemInt(dlg, ID_PORT, &ok, FALSE);
  return ok && port >= 1 && port <= 65535 ? (uint16_t)port : 0;
}

static void update_connect(HWND dlg) {
  char *host = address_text(dlg);
  EnableWindow(GetDlgItem(dlg, IDOK), host && port_value(dlg));
  free(host);
}

static void apply_theme(HWND dlg) {
  theme_apply_window(dlg, FALSE);
  theme_apply_controls(dlg);
  /* The arrows have no dark style of their own; the explorer one at least darkens them. */
  SetWindowTheme(GetDlgItem(dlg, ID_SPIN), theme.dark ? L"DarkMode_Explorer" : NULL, NULL);
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      SendDlgItemMessageW(dlg, ID_ADDRESS, EM_LIMITTEXT, 253, 0); /* the longest hostname */
      SendDlgItemMessageW(dlg, ID_PORT, EM_LIMITTEXT, 5, 0);
      SendDlgItemMessageW(dlg, ID_SPIN, UDM_SETRANGE32, 1, 65535);
      SendDlgItemMessageW(dlg, ID_SPIN, UDM_SETPOS32, 0, ADD_DEVICE_DEFAULT_PORT);
      apply_theme(dlg);
      update_connect(dlg);
      SetFocus(GetDlgItem(dlg, ID_ADDRESS));
      return FALSE; /* we set the focus */
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
      HBRUSH brush = theme_ctlcolor(msg, (HDC)wp);
      if (msg == WM_CTLCOLORSTATIC && (HWND)lp == GetDlgItem(dlg, ID_NOTE)) SetTextColor((HDC)wp, theme.text2);
      return (INT_PTR)brush;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update(); /* the main window may not have heard yet */
        apply_theme(dlg);
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case ID_ADDRESS:
        case ID_PORT:
          if (HIWORD(wp) == EN_CHANGE) update_connect(dlg);
          return TRUE;
        case IDOK: {
          /* Enter reaches here even while Connect is disabled. */
          char *host = address_text(dlg);
          uint16_t port = port_value(dlg);
          if (host && port) {
            result *r = (result *)GetWindowLongPtrW(dlg, DWLP_USER);
            r->host = host;
            r->port = port;
            EndDialog(dlg, IDOK);
          } else {
            free(host);
          }
          return TRUE;
        }
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

void add_device_show(HWND owner, add_device_connect_fn connect, void *user) {
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_UPDOWN_CLASS};
  InitCommonControlsEx(&icc);
  dlg_template *t = malloc(sizeof *t);
  if (!t) return;
  build(t);
  result r = {NULL, 0};
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, owner, dialog_proc, (LPARAM)&r);
  free(t);
  /* Called once the dialog has gone: connect may show the next one (the code entry). */
  if (r.host && connect) connect(user, r.host, r.port);
  free(r.host);
}
