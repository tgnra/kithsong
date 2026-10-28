#include "playlist_dialogs.h"

#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#include "resource.h"
#include "theme.h"
#include "wstr.h"

enum { MAX_NAME = 255 };

/* One dialog's state, passed in through DialogBoxParamW. */
typedef struct ask {
  const wchar_t *caption, *ok; /* the name dialog's title and OK label; Delete's title */
  const wchar_t *note;         /* Delete's note under the heading */
  const wchar_t *name;         /* the current name; NULL for a new playlist */
  wchar_t entered[MAX_NAME + 1];
  HFONT heading_font; /* Delete's */
} ask;

static char *utf8(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = n > 0 ? malloc((size_t)n) : NULL;
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}

/* The edit box's text without surrounding spaces, into out. */
static void read_name(HWND dlg, wchar_t out[MAX_NAME + 1]) {
  wchar_t buf[MAX_NAME + 1];
  GetDlgItemTextW(dlg, IDC_PL_NAME, buf, MAX_NAME + 1);
  wchar_t *start = buf, *end = buf + wcslen(buf);
  while (*start && iswspace(*start)) start++;
  while (end > start && iswspace(end[-1])) end--;
  *end = L'\0';
  wcscpy(out, start);
}

static void apply_theme(HWND dlg) {
  theme_apply_window(dlg, FALSE);
  theme_apply_controls(dlg);
}

/* What both dialogs do alike: theme colours, and following theme changes. Returns TRUE
 * with *result set if it handled msg. */
static BOOL common_message(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR *result) {
  switch (msg) {
    case WM_CTLCOLORSTATIC:
      if (GetDlgCtrlID((HWND)lp) == IDC_PL_NOTE) {
        SetTextColor((HDC)wp, theme.text2);
        SetBkColor((HDC)wp, theme.bg);
        *result = (INT_PTR)theme.bg_brush;
        return TRUE;
      }
      /* fall through */
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
      *result = (INT_PTR)theme_ctlcolor(msg, (HDC)wp);
      return TRUE;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update(); /* the main window may not have heard yet */
        apply_theme(dlg);
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
  }
  return FALSE;
}

/* --- New and Rename --- */

/* OK is only for a name that's there and, renaming, different. */
static void update_ok(HWND dlg, const ask *a) {
  wchar_t name[MAX_NAME + 1];
  read_name(dlg, name);
  EnableWindow(GetDlgItem(dlg, IDOK), name[0] && (!a->name || wcscmp(name, a->name)));
}

static INT_PTR CALLBACK name_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  ask *a = (ask *)GetWindowLongPtrW(dlg, DWLP_USER);
  INT_PTR result;
  if (common_message(dlg, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_INITDIALOG: {
      a = (ask *)lp;
      SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)a);
      SetWindowTextW(dlg, a->caption);
      SetDlgItemTextW(dlg, IDC_PL_LABEL, mc_wstr(MC_STR_NAME));
      SetDlgItemTextW(dlg, IDOK, a->ok);
      SetDlgItemTextW(dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CANCEL));
      HWND edit = GetDlgItem(dlg, IDC_PL_NAME);
      SendMessageW(edit, EM_LIMITTEXT, MAX_NAME, 0);
      SetWindowTextW(edit, a->name ? a->name : L"");
      SendMessageW(edit, EM_SETSEL, 0, -1);
      update_ok(dlg, a);
      apply_theme(dlg);
      SetFocus(edit);
      return FALSE; /* focus is set */
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDC_PL_NAME:
          if (HIWORD(wp) == EN_CHANGE) update_ok(dlg, a);
          return TRUE;
        case IDOK:
          if (!IsWindowEnabled(GetDlgItem(dlg, IDOK))) return TRUE; /* Enter with nothing to do */
          read_name(dlg, a->entered);
          EndDialog(dlg, IDOK);
          return TRUE;
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;
  }
  return FALSE;
}

/* Asks for a name; returns it as malloc'd UTF-8, or NULL if the user cancelled. */
static char *ask_name(HWND owner, const wchar_t *caption, const wchar_t *ok, const wchar_t *current) {
  ask a = {.caption = caption, .ok = ok, .name = current};
  if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PLAYLIST_NAME), owner, name_proc,
                      (LPARAM)&a) != IDOK || !a.entered[0])
    return NULL;
  return utf8(a.entered);
}

BOOL playlist_create_dialog(HWND owner, mc_library *lib, char out_uid[MC_UID_STRLEN]) {
  if (!lib) return FALSE;
  char *name = ask_name(owner, mc_wstr(MC_STR_NEW_PLAYLIST), mc_wstr_mnemonic(MC_STR_CREATE), NULL);
  if (!name) return FALSE;
  mc_status st = mc_playlist_create(lib, name, out_uid);
  free(name);
  if (st != MC_OK)
    MessageBoxW(owner, mc_wstr(MC_STR_PLAYLIST_CREATE_FAILED), mc_wstr(MC_STR_NEW_PLAYLIST), MB_OK | MB_ICONERROR);
  return st == MC_OK;
}

BOOL playlist_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name) {
  if (!lib || !uid) return FALSE;
  char *entered = ask_name(owner, mc_wstr(MC_STR_PLAYLIST_RENAME_TITLE), mc_wstr_mnemonic(MC_STR_RENAME),
                           name ? name : L"");
  if (!entered) return FALSE;
  mc_status st = mc_playlist_rename(lib, uid, entered);
  free(entered);
  if (st != MC_OK)
    MessageBoxW(owner, mc_wstr(MC_STR_PLAYLIST_RENAME_FAILED), mc_wstr(MC_STR_PLAYLIST_RENAME_TITLE),
                MB_OK | MB_ICONERROR);
  return st == MC_OK;
}

/* --- Delete --- */

static INT_PTR CALLBACK delete_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  ask *a = (ask *)GetWindowLongPtrW(dlg, DWLP_USER);
  INT_PTR result;
  if (common_message(dlg, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_INITDIALOG: {
      a = (ask *)lp;
      SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)a);
      /* The heading in the dialog's font, a little larger and semibold. */
      LOGFONTW lf;
      HFONT font = (HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0);
      if (font && GetObjectW(font, sizeof lf, &lf)) {
        lf.lfHeight = MulDiv(lf.lfHeight, 4, 3);
        lf.lfWeight = FW_SEMIBOLD;
        a->heading_font = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(dlg, IDC_PL_HEADING, WM_SETFONT, (WPARAM)a->heading_font, FALSE);
      }
      SetWindowTextW(dlg, a->caption);
      wchar_t heading[MAX_NAME * 2 + 64];
      char *name = utf8(a->name ? a->name : L"");
      mc_wformat(heading, sizeof heading / sizeof heading[0], mc_str(MC_STR_PLAYLIST_DELETE_TITLE), MC_S(name));
      free(name);
      SetDlgItemTextW(dlg, IDC_PL_HEADING, heading);
      SetDlgItemTextW(dlg, IDC_PL_NOTE, a->note);
      SetDlgItemTextW(dlg, IDOK, mc_wstr_mnemonic(MC_STR_DELETE));
      SetDlgItemTextW(dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CANCEL));
      apply_theme(dlg);
      SetFocus(GetDlgItem(dlg, IDCANCEL));
      return FALSE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, LOWORD(wp));
        return TRUE;
      }
      break;
    case WM_DESTROY:
      if (a && a->heading_font) DeleteObject(a->heading_font);
      break;
  }
  return FALSE;
}

/* Asks whether to delete name, in a dialog titled caption with note under the question. */
static BOOL ask_delete(HWND owner, const wchar_t *caption, const wchar_t *note, const wchar_t *name) {
  ask a = {.caption = caption, .note = note, .name = name};
  return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_PLAYLIST_DELETE), owner, delete_proc,
                         (LPARAM)&a) == IDOK;
}

BOOL confirm_delete_dialog(HWND owner, const wchar_t *caption, const wchar_t *name, const wchar_t *note) {
  ask a = {.caption = caption, .note = note, .name = name};
  return DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_LIBRARY_DELETE), owner, delete_proc,
                         (LPARAM)&a) == IDOK;
}

BOOL playlist_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name) {
  if (!lib || !uid) return FALSE;
  if (!ask_delete(owner, mc_wstr(MC_STR_WINDOWS_PLAYLIST_DELETE_CAPTION), mc_wstr(MC_STR_PLAYLIST_DELETE_TEXT), name))
    return FALSE;
  mc_status st = mc_playlist_delete(lib, uid);
  if (st != MC_OK)
    MessageBoxW(owner, mc_wstr(MC_STR_PLAYLIST_DELETE_FAILED), mc_wstr(MC_STR_WINDOWS_PLAYLIST_DELETE_CAPTION),
                MB_OK | MB_ICONERROR);
  return st == MC_OK;
}

/* --- Album groups: the same dialogs --- */

BOOL album_group_create_dialog(HWND owner, mc_library *lib, char out_uid[MC_UID_STRLEN]) {
  if (!lib) return FALSE;
  char *name = ask_name(owner, mc_wstr(MC_STR_NEW_ALBUM_GROUP), mc_wstr_mnemonic(MC_STR_CREATE), NULL);
  if (!name) return FALSE;
  mc_status st = mc_album_group_create(lib, name, out_uid);
  free(name);
  if (st != MC_OK)
    MessageBoxW(owner, mc_wstr(MC_STR_ALBUM_GROUP_CREATE_FAILED), mc_wstr(MC_STR_NEW_ALBUM_GROUP), MB_OK | MB_ICONERROR);
  return st == MC_OK;
}

BOOL album_group_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name) {
  if (!lib || !uid) return FALSE;
  char *entered = ask_name(owner, mc_wstr(MC_STR_ALBUM_GROUP_RENAME_TITLE), mc_wstr_mnemonic(MC_STR_RENAME),
                           name ? name : L"");
  if (!entered) return FALSE;
  mc_status st = mc_album_group_rename(lib, uid, entered);
  free(entered);
  if (st != MC_OK)
    MessageBoxW(owner, mc_wstr(MC_STR_ALBUM_GROUP_RENAME_FAILED), mc_wstr(MC_STR_ALBUM_GROUP_RENAME_TITLE),
                MB_OK | MB_ICONERROR);
  return st == MC_OK;
}

BOOL album_group_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name) {
  if (!lib || !uid) return FALSE;
  const wchar_t *caption = mc_wstr(MC_STR_WINDOWS_ALBUM_GROUP_DELETE_CAPTION);
  if (!ask_delete(owner, caption, mc_wstr(MC_STR_ALBUM_GROUP_DELETE_TEXT), name)) return FALSE;
  mc_status st = mc_album_group_delete(lib, uid);
  if (st != MC_OK)
    MessageBoxW(owner, mc_wstr(MC_STR_ALBUM_GROUP_DELETE_FAILED), mc_wstr(MC_STR_WINDOWS_ALBUM_GROUP_DELETE_CAPTION),
                MB_OK | MB_ICONERROR);
  return st == MC_OK;
}
