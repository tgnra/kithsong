#include "add_device.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "theme.h"

enum { ID_NOTE = 100, ID_ADDRESS_LABEL, ID_ADDRESS, ID_PORT_LABEL, ID_PORT, ID_SPIN };

/* Layout, in dialog units. */
enum { DLG_W = 220, DLG_H = 114, MARGIN = 7, BTN_W = 50, BTN_H = 14 };

/* What Connect closed the dialog with. */
typedef struct result {
  char *host; /* malloc'd UTF-8 */
  uint16_t port;
} result;

/* --- The template --- */

/* A DLGTEMPLATEEX being written: WORDs, with items aligned to DWORDs. */
typedef struct tmpl {
  WORD buf[1024];
  int n;
} tmpl;

static void put_word(tmpl *t, WORD w) {
  if (t->n < (int)(sizeof t->buf / sizeof t->buf[0])) t->buf[t->n++] = w;
}

static void put_dword(tmpl *t, DWORD d) {
  put_word(t, LOWORD(d));
  put_word(t, HIWORD(d));
}

static void put_string(tmpl *t, const wchar_t *s) {
  do put_word(t, *s); while (*s++);
}

static void align_dword(tmpl *t) {
  if (t->n % 2) put_word(t, 0);
}

/* cls is a predefined class atom (0x0080 button, 0x0081 edit, 0x0082 static) or 0 with name. */
static void put_item(tmpl *t, WORD atom, const wchar_t *cls_name, const wchar_t *text, DWORD style, int x, int y,
                     int w, int h, int id) {
  align_dword(t);
  put_dword(t, 0); /* help id */
  put_dword(t, 0); /* extended style */
  put_dword(t, WS_CHILD | WS_VISIBLE | style);
  put_word(t, (WORD)x);
  put_word(t, (WORD)y);
  put_word(t, (WORD)w);
  put_word(t, (WORD)h);
  put_dword(t, (DWORD)id);
  if (atom) {
    put_word(t, 0xFFFF);
    put_word(t, atom);
  } else {
    put_string(t, cls_name);
  }
  put_string(t, text);
  put_word(t, 0); /* no creation data */
}

static void build(tmpl *t) {
  enum { NUM_ITEMS = 8 };
  t->n = 0;
  put_word(t, 1);      /* version */
  put_word(t, 0xFFFF); /* DLGTEMPLATEEX */
  put_dword(t, 0);     /* help id */
  put_dword(t, 0);     /* extended style */
  put_dword(t, DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
  put_word(t, NUM_ITEMS);
  put_word(t, 0);
  put_word(t, 0);
  put_word(t, DLG_W);
  put_word(t, DLG_H);
  put_word(t, 0); /* no menu */
  put_word(t, 0); /* the standard dialog class */
  put_string(t, L"Add Device");
  put_word(t, 9);         /* point size */
  put_word(t, FW_NORMAL); /* weight */
  put_word(t, 0);         /* italic, charset */
  put_string(t, L"Segoe UI");

  int w = DLG_W - 2 * MARGIN, y = MARGIN;
  put_item(t, 0x0082, NULL, L"Enter the address of a device on your network. It will show a code to enter next.",
           SS_LEFT | SS_NOPREFIX, MARGIN, y, w, 18, ID_NOTE);
  y += 25;
  put_item(t, 0x0082, NULL, L"&IP address or hostname:", SS_LEFT, MARGIN, y, w, 9, ID_ADDRESS_LABEL);
  y += 11;
  put_item(t, 0x0081, NULL, L"", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, MARGIN, y, w, 14, ID_ADDRESS);
  y += 20;
  put_item(t, 0x0082, NULL, L"&Port:", SS_LEFT, MARGIN, y, w, 9, ID_PORT_LABEL);
  y += 11;
  put_item(t, 0x0081, NULL, L"", WS_BORDER | WS_TABSTOP | ES_NUMBER, MARGIN, y, 54, 14, ID_PORT);
  /* Attaches itself to the port box, the control before it. */
  put_item(t, 0, UPDOWN_CLASSW, L"",
           UDS_SETBUDDYINT | UDS_ALIGNRIGHT | UDS_AUTOBUDDY | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, 0, 0,
           0, 0, ID_SPIN);
  int by = DLG_H - MARGIN - BTN_H;
  put_item(t, 0x0080, NULL, L"Connect", BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED,
           DLG_W - MARGIN - 2 * BTN_W - 4, by, BTN_W, BTN_H, IDOK);
  put_item(t, 0x0080, NULL, L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, DLG_W - MARGIN - BTN_W, by, BTN_W, BTN_H,
           IDCANCEL);
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
  tmpl *t = malloc(sizeof *t);
  if (!t) return;
  build(t);
  result r = {NULL, 0};
  DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, owner, dialog_proc, (LPARAM)&r);
  free(t);
  /* Called once the dialog has gone: connect may show the next one (the code entry). */
  if (r.host && connect) connect(user, r.host, r.port);
  free(r.host);
}
