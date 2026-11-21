#include "scrobble_service.h"

#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "dlg_template.h"
#include "theme.h"
#include "wstr.h"

enum {
  ID_NOTE = 100,
  ID_KIND_LABEL,
  ID_KIND,
  ID_API_URL_LABEL,
  ID_API_URL,
  ID_AUTH_URL_LABEL,
  ID_AUTH_URL,
  ID_KEY_LABEL,
  ID_KEY,
  ID_SECRET_LABEL,
  ID_SECRET,
  ID_HINT,
};

/* Layout, in dialog units. */
enum { DLG_W = 240, DLG_H = 240, MARGIN = 7, BTN_W = 50, BTN_H = 14, FIELD_MAX = 1024 };

/* The combo box's items, in order: mc_scrobbler_service's. */
static const enum mc_str_id k_kinds[] = {MC_STR_SCROBBLE_SERVICE, MC_STR_SCROBBLE_SERVICE_LIBREFM,
                                          MC_STR_SCROBBLE_SERVICE_OTHER};

/* The boxes for another service, each after its label. */
static const struct {
  int label, box;
  enum mc_str_id text;
} k_fields[] = {
  {ID_API_URL_LABEL, ID_API_URL, MC_STR_SCROBBLE_SERVICE_API_URL},
  {ID_AUTH_URL_LABEL, ID_AUTH_URL, MC_STR_SCROBBLE_SERVICE_AUTH_URL},
  {ID_KEY_LABEL, ID_KEY, MC_STR_SCROBBLE_SERVICE_API_KEY},
  {ID_SECRET_LABEL, ID_SECRET, MC_STR_SCROBBLE_SERVICE_API_SECRET},
};
enum { NUM_FIELDS = sizeof k_fields / sizeof k_fields[0] };

typedef struct dialog {
  mc_scrobbler_service current;
  const char *api_url, *auth_url;
  scrobble_service_choice *out;
} dialog;

/* --- The template --- */

static void build(dlg_template *t) {
  dlg_template_header(t, 3 + 2 * NUM_FIELDS + 3, DLG_W, DLG_H, mc_wstr(MC_STR_SCROBBLE_SERVICE_TITLE));

  int w = DLG_W - 2 * MARGIN, y = MARGIN;
  wchar_t label[128];
  dlg_template_item(t, 0x0082, NULL, mc_wstr(MC_STR_SCROBBLE_SERVICE_TEXT), SS_LEFT | SS_NOPREFIX, MARGIN, y, w, 27,
                    ID_NOTE);
  y += 33;
  dlg_template_item(t, 0x0082, NULL, wstr_field_label(label, 128, MC_STR_SCROBBLE_SERVICE_TITLE), SS_LEFT, MARGIN, y,
                    w, 9, ID_KIND_LABEL);
  y += 11;
  /* A combo box's height includes its list. */
  dlg_template_item(t, 0x0085, NULL, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, MARGIN, y, w, 60, ID_KIND);
  y += 20;
  for (int i = 0; i < NUM_FIELDS; i++) {
    dlg_template_item(t, 0x0082, NULL, wstr_field_label(label, 128, k_fields[i].text), SS_LEFT, MARGIN, y, w, 9,
                      k_fields[i].label);
    y += 11;
    DWORD style = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (k_fields[i].box == ID_SECRET ? ES_PASSWORD : 0);
    dlg_template_item(t, 0x0081, NULL, L"", style, MARGIN, y, w, 14, k_fields[i].box);
    y += 20;
  }
  dlg_template_item(t, 0x0082, NULL, mc_wstr(MC_STR_SCROBBLE_SERVICE_INCOMPLETE), SS_LEFT | SS_NOPREFIX, MARGIN, y, w,
                    18, ID_HINT);
  int by = DLG_H - MARGIN - BTN_H;
  dlg_template_item(t, 0x0080, NULL, mc_wstr_mnemonic(MC_STR_OK), BS_DEFPUSHBUTTON | WS_TABSTOP,
                    DLG_W - MARGIN - 2 * BTN_W - 4, by, BTN_W, BTN_H, IDOK);
  dlg_template_item(t, 0x0080, NULL, mc_wstr_mnemonic(MC_STR_CANCEL), BS_PUSHBUTTON | WS_TABSTOP,
                    DLG_W - MARGIN - BTN_W, by, BTN_W, BTN_H, IDCANCEL);
}

/* --- The dialog --- */

static mc_scrobbler_service chosen_kind(HWND dlg) {
  LRESULT i = SendDlgItemMessageW(dlg, ID_KIND, CB_GETCURSEL, 0, 0);
  return i >= 0 && i < 3 ? (mc_scrobbler_service)i : MC_SCROBBLER_SERVICE_LASTFM;
}

/* A box's text, trimmed, as malloc'd UTF-8 (NULL if it's empty). */
static char *field_text(HWND dlg, int id) {
  wchar_t buf[FIELD_MAX];
  GetDlgItemTextW(dlg, id, buf, FIELD_MAX);
  wchar_t *start = buf, *end = buf + wcslen(buf);
  while (*start == L' ' || *start == L'\t') start++;
  while (end > start && (end[-1] == L' ' || end[-1] == L'\t')) end--;
  *end = L'\0';
  return *start ? wstr_to_utf8(start) : NULL;
}

/* Whether s is an http:// or https:// address, with more after that. */
static BOOL web_address(const char *s) {
  if (!s) return FALSE;
  if (!_strnicmp(s, "http://", 7)) return s[7] != '\0';
  return !_strnicmp(s, "https://", 8) && s[8] != '\0';
}

/* Whether OK can close the dialog: always for Last.fm and Libre.fm; for another service once
 * every box is filled in and both addresses are web ones. */
static BOOL valid(HWND dlg) {
  if (chosen_kind(dlg) != MC_SCROBBLER_SERVICE_OTHER) return TRUE;
  char *text[NUM_FIELDS];
  BOOL ok = TRUE;
  for (int i = 0; i < NUM_FIELDS; i++) {
    text[i] = field_text(dlg, k_fields[i].box);
    if (!text[i]) ok = FALSE;
  }
  ok = ok && web_address(text[0]) && web_address(text[1]);
  for (int i = 0; i < NUM_FIELDS; i++) free(text[i]);
  return ok;
}

static void update(HWND dlg) {
  BOOL other = chosen_kind(dlg) == MC_SCROBBLER_SERVICE_OTHER, ok = valid(dlg);
  for (int i = 0; i < NUM_FIELDS; i++) {
    EnableWindow(GetDlgItem(dlg, k_fields[i].label), other);
    EnableWindow(GetDlgItem(dlg, k_fields[i].box), other);
  }
  ShowWindow(GetDlgItem(dlg, ID_HINT), ok ? SW_HIDE : SW_SHOW);
  EnableWindow(GetDlgItem(dlg, IDOK), ok);
}

static char *dup_or_null(const char *s) {
  char *d = s ? malloc(strlen(s) + 1) : NULL;
  if (d) strcpy(d, s);
  return d;
}

static void set_utf8(HWND dlg, int id, const char *s) {
  wchar_t buf[FIELD_MAX];
  SetDlgItemTextW(dlg, id, wstr_from_utf8(buf, FIELD_MAX, s));
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      dialog *d = (dialog *)lp;
      HWND kind = GetDlgItem(dlg, ID_KIND);
      for (int i = 0; i < 3; i++) SendMessageW(kind, CB_ADDSTRING, 0, (LPARAM)mc_wstr(k_kinds[i]));
      SendMessageW(kind, CB_SETCURSEL, (WPARAM)d->current, 0);
      for (int i = 0; i < NUM_FIELDS; i++) SendDlgItemMessageW(dlg, k_fields[i].box, EM_LIMITTEXT, FIELD_MAX - 1, 0);
      if (d->current == MC_SCROBBLER_SERVICE_OTHER) {
        /* The key and secret aren't shown again: they're typed afresh. */
        set_utf8(dlg, ID_API_URL, d->api_url);
        set_utf8(dlg, ID_AUTH_URL, d->auth_url);
      }
      theme_apply_window(dlg, FALSE);
      theme_apply_controls(dlg);
      update(dlg);
      SetFocus(kind);
      return FALSE; /* we set the focus */
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
      HBRUSH brush = theme_ctlcolor(msg, (HDC)wp);
      HWND w = (HWND)lp;
      if (msg == WM_CTLCOLORSTATIC && (w == GetDlgItem(dlg, ID_NOTE) || w == GetDlgItem(dlg, ID_HINT)))
        SetTextColor((HDC)wp, theme.text2);
      else if (msg == WM_CTLCOLORSTATIC && !IsWindowEnabled(w))
        SetTextColor((HDC)wp, theme.text3); /* a disabled label */
      return (INT_PTR)brush;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update(); /* the main window may not have heard yet */
        theme_apply_window(dlg, FALSE);
        theme_apply_controls(dlg);
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case ID_KIND:
          if (HIWORD(wp) == CBN_SELCHANGE) update(dlg);
          return TRUE;
        case ID_API_URL:
        case ID_AUTH_URL:
        case ID_KEY:
        case ID_SECRET:
          if (HIWORD(wp) == EN_CHANGE) update(dlg);
          return TRUE;
        case IDOK: {
          /* Enter reaches here even while OK is disabled. */
          if (!valid(dlg)) return TRUE;
          dialog *d = (dialog *)GetWindowLongPtrW(dlg, DWLP_USER);
          scrobble_service_choice *c = d->out;
          switch (chosen_kind(dlg)) {
            case MC_SCROBBLER_SERVICE_LASTFM: break;
            case MC_SCROBBLER_SERVICE_LIBREFM:
              c->api_url = dup_or_null(MC_SCROBBLER_LIBREFM_API_URL);
              c->auth_url = dup_or_null(MC_SCROBBLER_LIBREFM_AUTH_URL);
              c->api_key = dup_or_null(MC_SCROBBLER_LIBREFM_API_KEY);
              c->api_secret = dup_or_null(MC_SCROBBLER_LIBREFM_API_SECRET);
              break;
            case MC_SCROBBLER_SERVICE_OTHER:
              c->api_url = field_text(dlg, ID_API_URL);
              c->auth_url = field_text(dlg, ID_AUTH_URL);
              c->api_key = field_text(dlg, ID_KEY);
              c->api_secret = field_text(dlg, ID_SECRET);
              break;
          }
          EndDialog(dlg, IDOK);
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

BOOL scrobble_service_show(HWND owner, mc_scrobbler_service current, const char *api_url, const char *auth_url,
                           scrobble_service_choice *out) {
  memset(out, 0, sizeof *out);
  dlg_template *t = malloc(sizeof *t);
  if (!t) return FALSE;
  build(t);
  if ((unsigned)current > MC_SCROBBLER_SERVICE_OTHER) current = MC_SCROBBLER_SERVICE_LASTFM;
  dialog d = {current, api_url, auth_url, out};
  INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, owner, dialog_proc,
                                      (LPARAM)&d);
  free(t);
  if (r != IDOK) scrobble_service_choice_clear(out);
  return r == IDOK;
}

void scrobble_service_choice_clear(scrobble_service_choice *c) {
  if (!c) return;
  free(c->api_url);
  free(c->auth_url);
  free(c->api_key);
  if (c->api_secret) SecureZeroMemory(c->api_secret, strlen(c->api_secret));
  free(c->api_secret);
  memset(c, 0, sizeof *c);
}
