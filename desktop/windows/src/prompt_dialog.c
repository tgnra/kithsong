#include "prompt_dialog.h"

#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <uxtheme.h>
#include <vsstyle.h>

#include "canvas.h"
#include "resource.h"
#include "theme.h"
#include "wstr.h"

enum {
  WM_APP_REFONT = WM_APP + 1,
  HEADING_PT = 12, /* the dialog's 9 pt, a little larger */
};

/* Layout at 96 DPI. */
enum {
  PAD = 16,
  ICON = 32,
  ICON_GAP = 12,
  TEXT_W = 380,
  HEADING_GAP = 8,
  CHECK_GAP = 12,
  CHECK_H = 20,
  CHECK_TEXT_GAP = 4, /* between the checkbox's box and its label, as Windows draws it */
  LINK_GAP = 12,
  LINK_H = 20,
  BUTTONS_GAP = 16,
  BTN_W = 84, /* at least */
  BTN_H = 26,
  BTN_GAP = 8,
};

/* One dialog's state, passed in through DialogBoxParamW. */
typedef struct state {
  prompt_dialog *p;
  HFONT heading_font;
  HICON icon;
} state;

static int scale(HWND wnd, int px) {
  return MulDiv(px, (int)GetDpiForWindow(wnd), USER_DEFAULT_SCREEN_DPI);
}

static void apply_theme(HWND dlg) {
  theme_apply_window(dlg, FALSE);
  theme_apply_controls(dlg);
}

/* What goes with the DPI: the heading's font, the icon and the shield. */
static void make_scaled(HWND dlg, state *s) {
  HFONT heading = !s->p->heading ? NULL
                                 : CreateFontW(-MulDiv(HEADING_PT, (int)GetDpiForWindow(dlg), 72), 0, 0, 0, FW_SEMIBOLD,
                                               FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                               CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
  if (heading) {
    SendDlgItemMessageW(dlg, IDC_PROMPT_HEADING, WM_SETFONT, (WPARAM)heading, FALSE);
    if (s->heading_font) DeleteObject(s->heading_font);
    s->heading_font = heading;
  }
  HICON icon = NULL;
  int size = scale(dlg, ICON);
  UINT kind = s->p->icon;
  const wchar_t *name = kind == MB_ICONERROR      ? IDI_ERROR
                        : kind == MB_ICONWARNING  ? IDI_WARNING
                        : kind == MB_ICONQUESTION ? IDI_QUESTION
                        : kind                    ? IDI_INFORMATION
                                                  : MAKEINTRESOURCEW(IDI_APP);
  if (SUCCEEDED(LoadIconWithScaleDown(kind ? NULL : GetModuleHandleW(NULL), name, size, size, &icon))) {
    SendDlgItemMessageW(dlg, IDC_PROMPT_ICON, STM_SETICON, (WPARAM)icon, 0);
    if (s->icon) DestroyIcon(s->icon);
    s->icon = icon;
  }
  if (s->p->shield) Button_SetElevationRequiredState(GetDlgItem(dlg, IDOK), TRUE);
}

static int button_width(HWND dlg, HWND button) {
  SIZE ideal = {0, 0};
  Button_GetIdealSize(button, &ideal);
  int min = scale(dlg, BTN_W);
  return ideal.cx > min ? ideal.cx : min;
}

static void place(HWND ctl, int x, int y, int w, int h) {
  SetWindowPos(ctl, NULL, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Lays the controls out for the text and the DPI, and sizes the dialog to them about its
 * centre. */
static void layout(HWND dlg, const state *s) {
  int pad = scale(dlg, PAD), icon = scale(dlg, ICON), text_w = scale(dlg, TEXT_W);
  int x = pad + icon + scale(dlg, ICON_GAP), y = pad;
  place(GetDlgItem(dlg, IDC_PROMPT_ICON), pad, pad, icon, icon);
  if (s->p->heading) {
    HWND heading = GetDlgItem(dlg, IDC_PROMPT_HEADING);
    int h = canvas_text_height(heading, NULL, text_w, 0);
    place(heading, x, y, text_w, h);
    y += h + scale(dlg, HEADING_GAP);
  }
  HWND text = GetDlgItem(dlg, IDC_PROMPT_TEXT);
  int h = canvas_text_height(text, NULL, text_w, 0);
  place(text, x, y, text_w, h);
  y += h;
  if (s->p->link) {
    y += scale(dlg, LINK_GAP);
    h = scale(dlg, LINK_H);
    place(GetDlgItem(dlg, IDC_PROMPT_LINK), x, y, text_w, h);
    y += h;
  }
  if (s->p->check) {
    y += scale(dlg, CHECK_GAP);
    h = scale(dlg, CHECK_H);
    place(GetDlgItem(dlg, IDC_PROMPT_CHECK), x, y, text_w, h);
    y += h;
  }
  if (y < pad + icon) y = pad + icon;
  y += scale(dlg, BUTTONS_GAP);

  int client_w = x + text_w + pad, right = client_w - pad, btn_h = scale(dlg, BTN_H);
  if (s->p->cancel) {
    HWND cancel = GetDlgItem(dlg, IDCANCEL);
    int w = button_width(dlg, cancel);
    place(cancel, right - w, y, w, btn_h);
    right -= w + scale(dlg, BTN_GAP);
  }
  if (s->p->other) {
    HWND other = GetDlgItem(dlg, IDNO);
    int w = button_width(dlg, other);
    place(other, right - w, y, w, btn_h);
    right -= w + scale(dlg, BTN_GAP);
  }
  HWND ok = GetDlgItem(dlg, IDOK);
  int w = button_width(dlg, ok);
  place(ok, right - w, y, w, btn_h);

  RECT frame = {0, 0, client_w, y + btn_h + pad}, was;
  AdjustWindowRectExForDpi(&frame, (DWORD)GetWindowLongW(dlg, GWL_STYLE), FALSE,
                           (DWORD)GetWindowLongW(dlg, GWL_EXSTYLE), GetDpiForWindow(dlg));
  GetWindowRect(dlg, &was);
  int new_w = frame.right - frame.left, new_h = frame.bottom - frame.top;
  SetWindowPos(dlg, NULL, was.left + (was.right - was.left - new_w) / 2, was.top + (was.bottom - was.top - new_h) / 2,
               new_w, new_h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Over the middle of the owner, as far as the screen allows. A minimized owner has no place
 * to be over, so DS_CENTER's stands. */
static void center_on_owner(HWND dlg) {
  HWND owner = GetWindow(dlg, GW_OWNER);
  RECT o, r;
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (!owner || IsIconic(owner) || !GetWindowRect(owner, &o) || !GetWindowRect(dlg, &r) ||
      !GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &mi))
    return;
  int w = r.right - r.left, h = r.bottom - r.top;
  int x = o.left + (o.right - o.left - w) / 2, y = o.top + (o.bottom - o.top - h) / 2;
  if (x + w > mi.rcWork.right) x = mi.rcWork.right - w;
  if (y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
  if (x < mi.rcWork.left) x = mi.rcWork.left;
  if (y < mi.rcWork.top) y = mi.rcWork.top;
  SetWindowPos(dlg, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* A themed checkbox's text stays black in dark mode, whatever WM_CTLCOLORBTN says. The other
 * pages put a label beside the box, but a static's mnemonic only moves the focus to it and
 * this one's should tick it, so it stays a real checkbox, painted here while dark. */
static LRESULT draw_check(const NMCUSTOMDRAW *cd) {
  HWND ctl = cd->hdr.hwndFrom;
  if (!theme.dark || cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
  HTHEME ht = OpenThemeData(ctl, L"Button");
  if (!ht) return CDRF_DODEFAULT;
  int part_state = cd->uItemState & CDIS_SELECTED ? CBS_UNCHECKEDPRESSED
                   : cd->uItemState & CDIS_HOT    ? CBS_UNCHECKEDHOT
                                                  : CBS_UNCHECKEDNORMAL;
  if (SendMessageW(ctl, BM_GETCHECK, 0, 0) == BST_CHECKED) part_state += CBS_CHECKEDNORMAL - CBS_UNCHECKEDNORMAL;
  RECT rc;
  GetClientRect(ctl, &rc);
  FillRect(cd->hdc, &rc, theme.bg_brush);
  SIZE box = {0, 0};
  GetThemePartSize(ht, cd->hdc, BP_CHECKBOX, part_state, NULL, TS_DRAW, &box);
  RECT box_rc = {rc.left, (rc.bottom - box.cy) / 2, rc.left + box.cx, (rc.bottom - box.cy) / 2 + box.cy};
  DrawThemeBackground(ht, cd->hdc, BP_CHECKBOX, part_state, &box_rc, NULL);
  CloseThemeData(ht);

  wchar_t label[128];
  GetWindowTextW(ctl, label, 128);
  LRESULT hidden = SendMessageW(ctl, WM_QUERYUISTATE, 0, 0);
  HGDIOBJ old = SelectObject(cd->hdc, (HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0));
  RECT text = rc;
  text.left = box_rc.right + scale(ctl, CHECK_TEXT_GAP);
  UINT format = DT_SINGLELINE | DT_VCENTER | (hidden & UISF_HIDEACCEL ? DT_HIDEPREFIX : 0);
  SetBkMode(cd->hdc, TRANSPARENT);
  SetTextColor(cd->hdc, theme.text);
  DrawTextW(cd->hdc, label, -1, &text, format);
  if (cd->uItemState & CDIS_FOCUS && !(hidden & UISF_HIDEFOCUS)) {
    DrawTextW(cd->hdc, label, -1, &text, format | DT_CALCRECT);
    int h = text.bottom - text.top;
    text.top = (rc.bottom - h) / 2;
    text.bottom = text.top + h;
    InflateRect(&text, 2, 1);
    DrawFocusRect(cd->hdc, &text);
  }
  SelectObject(cd->hdc, old);
  return CDRF_SKIPDEFAULT;
}

/* The link, made here as most dialogs have none, and its class needs asking for. */
static void add_link(HWND dlg, const wchar_t *address) {
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_LINK_CLASS};
  InitCommonControlsEx(&icc);
  wchar_t text[512];
  swprintf(text, sizeof text / sizeof text[0], L"<a>%ls</a>", address);
  HWND link = CreateWindowExW(0, WC_LINK, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | LWS_TRANSPARENT, 0, 0, 0, 0, dlg,
                              (HMENU)(INT_PTR)IDC_PROMPT_LINK, GetModuleHandleW(NULL), NULL);
  SendMessageW(link, WM_SETFONT, SendMessageW(dlg, WM_GETFONT, 0, 0), FALSE);
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  state *s = (state *)GetWindowLongPtrW(dlg, DWLP_USER);
  switch (msg) {
    case WM_INITDIALOG: {
      s = (state *)lp;
      SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)s);
      const prompt_dialog *p = s->p;
      SetWindowTextW(dlg, p->caption);
      SetDlgItemTextW(dlg, IDC_PROMPT_HEADING, p->heading ? p->heading : L"");
      SetDlgItemTextW(dlg, IDC_PROMPT_TEXT, p->text);
      SetDlgItemTextW(dlg, IDC_PROMPT_CHECK, p->check ? p->check : L"");
      SetDlgItemTextW(dlg, IDOK, p->ok);
      SetDlgItemTextW(dlg, IDNO, p->other ? p->other : L"");
      SetDlgItemTextW(dlg, IDCANCEL, p->cancel ? p->cancel : L"");
      if (!p->heading) ShowWindow(GetDlgItem(dlg, IDC_PROMPT_HEADING), SW_HIDE);
      if (!p->check) ShowWindow(GetDlgItem(dlg, IDC_PROMPT_CHECK), SW_HIDE);
      if (!p->other) ShowWindow(GetDlgItem(dlg, IDNO), SW_HIDE);
      if (!p->cancel) ShowWindow(GetDlgItem(dlg, IDCANCEL), SW_HIDE);
      if (p->link) add_link(dlg, p->link);
      make_scaled(dlg, s);
      apply_theme(dlg);
      layout(dlg, s);
      center_on_owner(dlg);
      if (p->shown) *p->shown = dlg;
      if (p->cancel_default) {
        SendMessageW(dlg, DM_SETDEFID, IDCANCEL, 0);
        SendDlgItemMessageW(dlg, IDOK, BM_SETSTYLE, BS_PUSHBUTTON, TRUE);
        SendDlgItemMessageW(dlg, IDCANCEL, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);
      }
      SetFocus(GetDlgItem(dlg, p->cancel_default ? IDCANCEL : IDOK));
      return FALSE; /* focus is set */
    }
    case WM_CTLCOLORSTATIC: {
      HBRUSH brush = theme_ctlcolor(msg, (HDC)wp);
      /* Under a heading the text is the detail; alone, it's what the dialog says. */
      if (s && s->p->heading && (HWND)lp == GetDlgItem(dlg, IDC_PROMPT_TEXT)) SetTextColor((HDC)wp, theme.text2);
      return (INT_PTR)brush;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
      return (INT_PTR)theme_ctlcolor(msg, (HDC)wp);
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update(); /* the main window may not have heard yet */
        apply_theme(dlg);
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_DPICHANGED:
      PostMessageW(dlg, WM_APP_REFONT, 0, 0); /* once the dialog has rescaled itself */
      break;
    case WM_APP_REFONT:
      if (s) {
        make_scaled(dlg, s);
        layout(dlg, s);
      }
      return TRUE;
    case WM_NOTIFY: {
      const NMHDR *n = (const NMHDR *)lp;
      if (n->idFrom == IDC_PROMPT_CHECK && n->code == NM_CUSTOMDRAW) {
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, draw_check((const NMCUSTOMDRAW *)lp));
        return TRUE;
      }
      if (n->idFrom == IDC_PROMPT_LINK && (n->code == NM_CLICK || n->code == NM_RETURN)) {
        ShellExecuteW(NULL, L"open", s->p->link, NULL, NULL, SW_SHOWNORMAL);
        return TRUE;
      }
      break;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDNO) {
        s->p->checked = IsDlgButtonChecked(dlg, IDC_PROMPT_CHECK) == BST_CHECKED;
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (s && s->p->shown) *s->p->shown = NULL;
      if (s && s->heading_font) DeleteObject(s->heading_font);
      if (s && s->icon) DestroyIcon(s->icon);
      break;
  }
  return FALSE;
}

INT_PTR prompt_dialog_show(HWND owner, prompt_dialog *p) {
  state s = {.p = p};
  return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PROMPT), owner, dialog_proc, (LPARAM)&s);
}

void prompt_dialog_set_text(HWND dlg, const wchar_t *text) {
  SetDlgItemTextW(dlg, IDC_PROMPT_TEXT, text);
  layout(dlg, (const state *)GetWindowLongPtrW(dlg, DWLP_USER));
}

void message_dialog(HWND owner, const wchar_t *caption, const wchar_t *text, UINT icon) {
  prompt_dialog p = {.caption = caption, .icon = icon, .text = text, .ok = mc_wstr_mnemonic(MC_STR_OK)};
  MessageBeep(icon);
  prompt_dialog_show(owner, &p);
}

BOOL confirm_dialog(HWND owner, const wchar_t *caption, const wchar_t *text, UINT icon, const wchar_t *ok,
                    const wchar_t *cancel, BOOL cancel_default) {
  prompt_dialog p = {.caption = caption, .icon = icon, .text = text, .ok = ok, .cancel = cancel,
                     .cancel_default = cancel_default};
  MessageBeep(icon);
  return prompt_dialog_show(owner, &p) == IDOK;
}
