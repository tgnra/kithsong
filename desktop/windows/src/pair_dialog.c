#include "pair_dialog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "theme.h"
#include "wstr.h"

/* Wrong codes before the responder draws a new one (MC_PAIR_MAX_FAILURES in core's private
 * pair.h). A wrong-code result with this many tries left means a new code is showing. */
#define MAX_FAILURES 3

enum { ID_TEXT = 100, ID_ACTION };

/* Layout, in dialog units. */
enum { DLG_W = 220, DLG_H = 112, MARGIN = 7, BTN_W = 50, BTN_H = 14, TEXT_H = 18, CODE_Y = 29, STATUS_Y = 55 };
enum { ACTION_W = 96 }; /* the ended dialog's button left of OK */

/* What that button does. */
typedef enum ended_action {
  ACT_NONE,
  ACT_CHOOSE,      /* Choose What to Sync: the page of the device that joined */
  ACT_NEW_LIBRARY, /* Create a New Library, then offer to pair again */
  ACT_PAIR_AGAIN,  /* Pair Again, after that */
} ended_action;

/* settings.json's record that this PC showed which device is the archive. */
#define ARCHIVE_EXPLAINED_KEY "archive_explained"

/* Point sizes of the code: typed, and shown. */
enum { ENTER_PT = 16, SHOW_PT = 22 };

/* One open dialog. Freed with its window. */
typedef struct pair_dlg {
  HWND wnd;
  mc_sync *sync;
  BOOL initiator;
  BOOL checking; /* the code went to the other device; waiting to hear */
  BOOL ended;    /* shows how the pairing ended, with OK and maybe the action button */
  HFONT big_font;
  wchar_t peer[128];
  ended_action action;
  char target[MC_UID_STRLEN]; /* ACT_CHOOSE's device; the other device for the rest */
  char host[MC_SYNC_ADDR_STRLEN]; /* ACT_PAIR_AGAIN: where it was, "" if not known */
  uint16_t port;
} pair_dlg;

static struct {
  HWND owner;
  HWND focus;         /* the owner's focus before the first dialog opened */
  int open;           /* dialogs open; the owner is disabled while there are any */
  pair_dlg *enter;    /* the enter-code dialog (initiator), or NULL */
  pair_dlg *show;     /* the show-code dialog (responder), or NULL */
  BOOL enter_cancelled, show_cancelled; /* Cancel was pressed: the CANCELLED to come is ours */
  const pair_dialog_host *host;
} s;

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

static void put_string(tmpl *t, const wchar_t *str) {
  do put_word(t, *str); while (*str++);
}

static void align_dword(tmpl *t) {
  if (t->n % 2) put_word(t, 0);
}

/* atom is a predefined class: 0x0080 button, 0x0081 edit, 0x0082 static. */
static void put_item(tmpl *t, WORD atom, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
  align_dword(t);
  put_dword(t, 0); /* help id */
  put_dword(t, 0); /* extended style */
  put_dword(t, WS_CHILD | WS_VISIBLE | style);
  put_word(t, (WORD)x);
  put_word(t, (WORD)y);
  put_word(t, (WORD)w);
  put_word(t, (WORD)h);
  put_dword(t, (DWORD)id);
  put_word(t, 0xFFFF);
  put_word(t, atom);
  put_string(t, text);
  put_word(t, 0); /* no creation data */
}

/* Both dialogs: a line of text, the code, the status line, then Pair (or OK) and Cancel, and
 * the ended dialog's action button (hidden until then). The texts are set once it's
 * created. */
static void build(tmpl *t, BOOL enter) {
  enum { NUM_ITEMS = 6 };
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
  put_string(t, L"");
  put_word(t, 9);         /* point size */
  put_word(t, FW_NORMAL); /* weight */
  put_word(t, 0);         /* italic, charset */
  put_string(t, L"Segoe UI");

  int w = DLG_W - 2 * MARGIN;
  put_item(t, 0x0082, L"", SS_LEFT | SS_NOPREFIX, MARGIN, MARGIN, w, TEXT_H, ID_TEXT);
  if (enter) {
    put_item(t, 0x0081, L"", WS_BORDER | WS_TABSTOP | ES_NUMBER | ES_CENTER, MARGIN, CODE_Y, 80, 20, IDC_PAIR_CODE);
  } else {
    put_item(t, 0x0082, L"", SS_CENTER | SS_NOPREFIX, MARGIN, CODE_Y, w, 22, IDC_PAIR_CODE);
  }
  put_item(t, 0x0082, L"", SS_LEFT | SS_NOPREFIX, MARGIN, STATUS_Y, w, 28, IDC_PAIR_STATUS);
  int by = DLG_H - MARGIN - BTN_H;
  /* The show-code dialog hides Pair until it becomes OK. */
  put_item(t, 0x0080, mc_wstr(MC_STR_PAIR), BS_DEFPUSHBUTTON | WS_TABSTOP | WS_DISABLED,
           DLG_W - MARGIN - 2 * BTN_W - 4, by, BTN_W, BTN_H, IDOK);
  put_item(t, 0x0080, mc_wstr(MC_STR_CANCEL), BS_PUSHBUTTON | WS_TABSTOP, DLG_W - MARGIN - BTN_W, by, BTN_W, BTN_H,
           IDCANCEL);
  put_item(t, 0x0080, L"", BS_PUSHBUTTON | WS_TABSTOP, DLG_W - MARGIN - BTN_W - 4 - ACTION_W, by, ACTION_W, BTN_H,
           ID_ACTION);
}

/* --- Texts --- */

/* The other device's name, or a stand-in when it's not known. */
static const wchar_t *peer_name(const wchar_t *name, BOOL sentence_start) {
  if (name && *name) return name;
  return mc_wstr(sentence_start ? MC_STR_OTHER_DEVICE_START : MC_STR_SYNC_OTHER_DEVICE);
}

/* The string id with the other device's name (or a stand-in) as its {0}. */
static void peer_text(wchar_t *out, size_t n, enum mc_str_id id, const wchar_t *name, BOOL sentence_start) {
  char *peer = wstr_to_utf8(peer_name(name, sentence_start));
  mc_wformat(out, n, mc_str(id), MC_S(peer));
  free(peer);
}

/* prefix_id ("Wrong code entered. {0}") with how many tries are left as its {0}. */
static void tries_left(wchar_t *out, size_t n, enum mc_str_id prefix_id, int left) {
  char tries[64];
  mc_format(tries, sizeof tries, mc_str_plural(MC_STR_TRIES_LEFT, left), MC_D(left));
  mc_wformat(out, n, mc_str(prefix_id), MC_S(tries));
}

/* Takes the ** out of "the **archive** device": a static control can't show bold. */
static void drop_bold_marks(wchar_t *text) {
  wchar_t *to = text;
  for (const wchar_t *from = text; *from; from++) {
    if (from[0] == L'*' && from[1] == L'*') {
      from++;
      continue;
    }
    *to++ = *from;
  }
  *to = L'\0';
}

/* Once, on this PC after it joined a group: which device keeps the originals. Appends it
 * to out as a paragraph of its own; nothing if the archive isn't known or it was shown
 * before. */
static void append_archive_explainer(wchar_t *out, size_t n, const sync_event *ev) {
  if (!ev->joined || !ev->archive_name[0] || !s.host || !s.host->first_time) return;
  if (!s.host->first_time(ARCHIVE_EXPLAINED_KEY)) return;
  char *name = wstr_to_utf8(ev->archive_name);
  wchar_t text[640];
  if (ev->archives > 1)
    mc_wformat(text, 640, mc_str_plural(MC_STR_ARCHIVE_EXPLAINER_SEVERAL, ev->archives - 1), MC_S(name),
               MC_D(ev->archives - 1));
  else
    mc_wformat(text, 640, mc_str(MC_STR_ARCHIVE_EXPLAINER), MC_S(name));
  free(name);
  drop_bold_marks(text);
  size_t len = wcslen(out);
  if (len + 2 < n) swprintf(out + len, n - len, L"\n\n%ls", text);
}

/* Whether this PC could have joined: it's in no group, so a new, empty library lets it. */
static BOOL could_join(void) {
  mc_sync_lib_state state;
  mc_sync *sync = sync_bridge_get();
  return sync && s.host && s.host->new_library && mc_sync_library_state(sync, &state) == MC_OK &&
         state == MC_SYNC_LIBRARY_NO_GROUP;
}

/* How a pairing ended, as on Android. */
static void ended_text(wchar_t *out, size_t n, const sync_event *ev, BOOL initiator, const wchar_t *name) {
  mc_pair_reason reason = ev->reason;
  switch (reason) {
    case MC_PAIR_PAIRED:
      /* Nothing syncs yet: the device that joined has its sync settings to choose. */
      peer_text(out, n, ev->joined ? MC_STR_WINDOWS_PC_PAIRED_TEXT : MC_STR_PAIRED_PEER_TEXT, name, !ev->joined);
      append_archive_explainer(out, n, ev);
      break;
    case MC_PAIR_WRONG_CODE:
      swprintf(out, n, L"%ls", mc_wstr(MC_STR_PAIR_WRONG_CODE_TOO_OFTEN));
      break;
    case MC_PAIR_IN_GROUP:
      /* Both are in groups, different ones (a device in no group just joins). */
      peer_text(out, n, MC_STR_WINDOWS_PC_PAIR_IN_GROUP, name, FALSE);
      break;
    case MC_PAIR_LIBRARY_MISMATCH:
      if (initiator) peer_text(out, n, MC_STR_WINDOWS_PC_PAIR_LIBRARY_MISMATCH, name, FALSE);
      else peer_text(out, n, MC_STR_WINDOWS_PC_PAIR_LIBRARY_MISMATCH_INCOMING, name, TRUE);
      if (could_join()) {
        size_t len = wcslen(out);
        if (len + 2 < n) swprintf(out + len, n - len, L"\n\n%ls", mc_wstr(MC_STR_PAIR_NEW_LIBRARY_HINT));
      }
      break;
    case MC_PAIR_ALREADY_PAIRED:
      peer_text(out, n, MC_STR_WINDOWS_PC_PAIR_ALREADY_PAIRED, name, FALSE);
      break;
    case MC_PAIR_BUSY:
      peer_text(out, n, MC_STR_PAIR_BUSY, name, TRUE);
      break;
    case MC_PAIR_CANCELLED:
      swprintf(out, n, L"%ls", mc_wstr(MC_STR_PAIR_CANCELLED));
      break;
    case MC_PAIR_FAILED:
    default:
      peer_text(out, n, MC_STR_PAIR_UNREACHABLE, name, FALSE);
      break;
  }
}

/* --- Owner --- */

/* The first dialog to open disables the owner. */
static void acquire_owner(void) {
  if (s.open++ > 0 || !s.owner) return;
  HWND focus = GetFocus();
  s.focus = focus && (focus == s.owner || IsChild(s.owner, focus)) ? focus : NULL;
  EnableWindow(s.owner, FALSE);
}

/* The last to close enables it again, and gives back its focus. */
static void release_owner(void) {
  if (s.open <= 0 || --s.open > 0 || !s.owner) return;
  EnableWindow(s.owner, TRUE);
  if (GetActiveWindow() == s.owner && s.focus && IsWindow(s.focus)) SetFocus(s.focus);
  s.focus = NULL;
}

static void close_dlg(pair_dlg *d) {
  if (!d) return;
  /* The owner must be enabled before the dialog goes, or Windows activates another app. */
  if (s.open == 1 && s.owner) EnableWindow(s.owner, TRUE);
  DestroyWindow(d->wnd); /* WM_NCDESTROY frees d */
}

/* --- The dialog --- */

/* Moves a control to a rect in dialog units. */
static void place(HWND dlg, int id, int x, int y, int w, int h) {
  RECT r = {x, y, x + w, y + h};
  MapDialogRect(dlg, &r);
  SetWindowPos(GetDlgItem(dlg, id), NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
               SWP_NOZORDER | SWP_NOACTIVATE);
}

static void set_big_font(pair_dlg *d) {
  UINT dpi = GetDpiForWindow(d->wnd);
  HFONT font = CreateFontW(-MulDiv(d->initiator ? ENTER_PT : SHOW_PT, (int)dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH, L"Segoe UI");
  if (!font) return;
  SendDlgItemMessageW(d->wnd, IDC_PAIR_CODE, WM_SETFONT, (WPARAM)font, TRUE);
  if (d->big_font) DeleteObject(d->big_font);
  d->big_font = font;
}

static void set_status(pair_dlg *d, const wchar_t *text) {
  SetDlgItemTextW(d->wnd, IDC_PAIR_STATUS, text);
}

/* The digits typed, if there are all six. */
static BOOL code_text(HWND dlg, char *code) {
  wchar_t buf[16];
  int n = GetDlgItemTextW(dlg, IDC_PAIR_CODE, buf, 16);
  if (n != MC_PAIR_CODE_LEN) return FALSE;
  for (int i = 0; i < n; i++) {
    if (buf[i] < L'0' || buf[i] > L'9') return FALSE; /* pasted past ES_NUMBER */
    code[i] = (char)buf[i];
  }
  code[n] = '\0';
  return TRUE;
}

static void update_pair(pair_dlg *d) {
  char code[MC_PAIR_CODE_LEN + 1];
  if (!d->ended) EnableWindow(GetDlgItem(d->wnd, IDOK), !d->checking && code_text(d->wnd, code));
}

/* The enter-code dialog, ready for (another) code. */
static void ready_for_code(pair_dlg *d) {
  d->checking = FALSE;
  HWND edit = GetDlgItem(d->wnd, IDC_PAIR_CODE);
  EnableWindow(edit, TRUE);
  SetWindowTextW(edit, L"");
  update_pair(d);
  if (GetActiveWindow() == d->wnd) SetFocus(edit);
}

/* Moves a control down by dy pixels. */
static void move_down(HWND dlg, int id, int dy) {
  HWND item = GetDlgItem(dlg, id);
  RECT r;
  GetWindowRect(item, &r);
  MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
  SetWindowPos(item, NULL, r.left, r.top + dy, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Makes the dialog taller if the status text needs more room than it has. */
static void fit_status(pair_dlg *d) {
  HWND status = GetDlgItem(d->wnd, IDC_PAIR_STATUS);
  int len = GetWindowTextLengthW(status);
  wchar_t *text = malloc(((size_t)len + 1) * sizeof *text);
  if (!text) return;
  GetWindowTextW(status, text, len + 1);
  RECT have;
  GetClientRect(status, &have);
  RECT need = {0, 0, have.right, 0};
  HDC dc = GetDC(status);
  HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(status, WM_GETFONT, 0, 0));
  DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
  SelectObject(dc, old);
  ReleaseDC(status, dc);
  free(text);
  int dy = need.bottom - have.bottom;
  if (dy <= 0) return;
  RECT w;
  GetWindowRect(d->wnd, &w);
  SetWindowPos(d->wnd, NULL, w.left, w.top - dy / 2, w.right - w.left, w.bottom - w.top + dy,
               SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(status, NULL, 0, 0, have.right, have.bottom + dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  move_down(d->wnd, IDOK, dy);
  move_down(d->wnd, ID_ACTION, dy);
}

/* Turns the dialog into a message: title, text, and OK (labelled ok_id) in Cancel's place,
 * with the action button left of it unless action is ACT_NONE. */
static void show_message(pair_dlg *d, const wchar_t *title, const wchar_t *text, ended_action action,
                         enum mc_str_id action_id, enum mc_str_id ok_id) {
  d->ended = TRUE;
  d->checking = FALSE;
  d->action = action;
  SetWindowTextW(d->wnd, title);
  ShowWindow(GetDlgItem(d->wnd, ID_TEXT), SW_HIDE);
  ShowWindow(GetDlgItem(d->wnd, IDC_PAIR_CODE), SW_HIDE);
  ShowWindow(GetDlgItem(d->wnd, IDCANCEL), SW_HIDE);
  place(d->wnd, IDC_PAIR_STATUS, MARGIN, MARGIN, DLG_W - 2 * MARGIN, DLG_H - 3 * MARGIN - BTN_H);
  set_status(d, text);
  HWND ok = GetDlgItem(d->wnd, IDOK), act = GetDlgItem(d->wnd, ID_ACTION);
  place(d->wnd, IDOK, DLG_W - MARGIN - BTN_W, DLG_H - MARGIN - BTN_H, BTN_W, BTN_H);
  SetWindowTextW(ok, mc_wstr_mnemonic(ok_id));
  EnableWindow(ok, TRUE);
  ShowWindow(ok, SW_SHOW);
  if (action != ACT_NONE) {
    SetWindowTextW(act, mc_wstr_mnemonic(action_id));
    ShowWindow(act, SW_SHOW);
  }
  fit_status(d);
  /* Enter takes the next step, except Create a New Library, which is a bigger one. */
  HWND def = action == ACT_NONE || action == ACT_NEW_LIBRARY ? ok : act;
  SendMessageW(d->wnd, DM_SETDEFID, (WPARAM)GetDlgCtrlID(def), 0);
  if (GetActiveWindow() == d->wnd) SendMessageW(d->wnd, WM_NEXTDLGCTL, (WPARAM)def, TRUE);
}

/* Shows how the pairing ended, with OK in Cancel's place, and a button for the next step:
 * Choose What to Sync after pairing, Create a New Library when this PC has music of its own
 * but could otherwise join. */
static void show_ended(pair_dlg *d, const sync_event *ev) {
  const wchar_t *name = ev->name[0] ? ev->name : d->peer;
  if (ev->name[0]) wcsncpy(d->peer, ev->name, sizeof d->peer / sizeof d->peer[0] - 1);
  wchar_t text[1024];
  ended_text(text, 1024, ev, d->initiator, name);
  ended_action action = ACT_NONE;
  enum mc_str_id action_id = MC_STR_OK;
  if (ev->reason == MC_PAIR_PAIRED && s.host && s.host->show_device) {
    mc_sync *sync = sync_bridge_get();
    const char *uid = ev->joined ? (sync ? mc_sync_device_uid(sync) : "") : ev->uid;
    if (uid && *uid) {
      snprintf(d->target, sizeof d->target, "%s", uid);
      action = ACT_CHOOSE;
      action_id = MC_STR_CHOOSE_WHAT_TO_SYNC;
    }
  } else if (ev->reason == MC_PAIR_LIBRARY_MISMATCH && could_join()) {
    if (ev->uid[0]) snprintf(d->target, sizeof d->target, "%s", ev->uid); /* else as set_target said */
    action = ACT_NEW_LIBRARY;
    action_id = MC_STR_PAIR_CREATE_LIBRARY;
  }
  show_message(d, mc_wstr(ev->reason == MC_PAIR_PAIRED ? MC_STR_PAIRED : MC_STR_PAIR_FAILED), text, action,
               action_id, MC_STR_OK);
}

static void apply_theme(HWND dlg) {
  theme_apply_window(dlg, FALSE);
  theme_apply_controls(dlg);
}

static void run_action(pair_dlg *d);

/* Posted to itself after a DPI change, once the dialog manager has rescaled the controls. */
#define WM_APP_REFONT (WM_APP + 1)

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  pair_dlg *d = (pair_dlg *)GetWindowLongPtrW(dlg, DWLP_USER);
  switch (msg) {
    case WM_INITDIALOG:
      d = (pair_dlg *)lp;
      d->wnd = dlg;
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      set_big_font(d);
      ShowWindow(GetDlgItem(dlg, ID_ACTION), SW_HIDE);
      if (d->initiator) {
        SendDlgItemMessageW(dlg, IDC_PAIR_CODE, EM_LIMITTEXT, MC_PAIR_CODE_LEN, 0);
      } else {
        ShowWindow(GetDlgItem(dlg, IDOK), SW_HIDE);
      }
      apply_theme(dlg);
      return TRUE; /* the first tab stop gets the focus: the code, or Cancel */
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
      HBRUSH brush = theme_ctlcolor(msg, (HDC)wp);
      if (msg == WM_CTLCOLORSTATIC && (HWND)lp == GetDlgItem(dlg, IDC_PAIR_STATUS) && d && !d->ended) {
        SetTextColor((HDC)wp, theme.text2);
      }
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
    case WM_DPICHANGED:
      PostMessageW(dlg, WM_APP_REFONT, 0, 0);
      break;
    case WM_APP_REFONT:
      if (d) set_big_font(d);
      return TRUE;
    case WM_COMMAND:
      if (!d) break;
      switch (LOWORD(wp)) {
        case IDC_PAIR_CODE:
          if (HIWORD(wp) == EN_CHANGE) update_pair(d);
          return TRUE;
        case IDOK: {
          if (d->ended) {
            close_dlg(d);
            return TRUE;
          }
          /* Enter reaches here even while Pair is disabled or hidden. */
          char code[MC_PAIR_CODE_LEN + 1];
          if (!d->initiator || d->checking || !code_text(dlg, code)) return TRUE;
          mc_status st = mc_sync_pair_enter(d->sync, code);
          if (st == MC_OK) {
            d->checking = TRUE;
            EnableWindow(GetDlgItem(dlg, IDC_PAIR_CODE), FALSE);
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            SendMessageW(dlg, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(dlg, IDCANCEL), TRUE);
            set_status(d, mc_wstr(MC_STR_PAIRING_PROGRESS));
          } else {
            wchar_t text[160];
            mc_wformat(text, 160, mc_str(MC_STR_PAIR_SEND_FAILED), MC_S(mc_status_str(st)));
            set_status(d, text);
          }
          return TRUE;
        }
        case ID_ACTION:
          if (d->ended && d->action != ACT_NONE) run_action(d);
          return TRUE;
        case IDCANCEL:
          if (!d->ended) {
            /* Cancelling here: the CANCELLED result it brings needs no telling. */
            if (d->initiator) s.enter_cancelled = TRUE;
            else s.show_cancelled = TRUE;
            if (d->sync) mc_sync_pair_cancel(d->sync);
          }
          close_dlg(d);
          return TRUE;
      }
      break;
    case WM_NCDESTROY:
      if (!d) break;
      if (s.enter == d) s.enter = NULL;
      if (s.show == d) s.show = NULL;
      if (d->big_font) DeleteObject(d->big_font);
      free(d);
      SetWindowLongPtrW(dlg, DWLP_USER, 0);
      release_owner();
      break;
  }
  return FALSE;
}

/* Creates a dialog in place of *slot (which goes once the new one is up, so the owner stays
 * disabled throughout). Returns it, or NULL. */
static pair_dlg *open_dlg(pair_dlg **slot, mc_sync *sync, BOOL initiator, const wchar_t *peer) {
  pair_dlg *d = calloc(1, sizeof *d);
  tmpl *t = malloc(sizeof *t);
  if (!d || !t) {
    free(d);
    free(t);
    return NULL;
  }
  d->sync = sync;
  d->initiator = initiator;
  if (peer) wcsncpy(d->peer, peer, sizeof d->peer / sizeof d->peer[0] - 1);
  build(t, initiator);
  acquire_owner();
  HWND wnd = CreateDialogIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, s.owner, dialog_proc,
                                        (LPARAM)d);
  free(t);
  if (!wnd) {
    free(d);
    release_owner();
    return NULL;
  }
  pair_dlg *old = *slot;
  *slot = d;
  if (old) close_dlg(old);
  /* Both open: step this one aside so the other shows. */
  pair_dlg *other = initiator ? s.show : s.enter;
  if (other) {
    RECT r;
    GetWindowRect(wnd, &r);
    int step = MulDiv(32, (int)GetDpiForWindow(wnd), USER_DEFAULT_SCREEN_DPI);
    SetWindowPos(wnd, NULL, r.left + step, r.top + step, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  return d;
}

/* After Create a New Library: offers to pair with the same device again. */
static void offer_pair_again(const char *uid, const wchar_t *peer, const char *host, uint16_t port) {
  if (!*uid && !*host) return; /* nothing to ask */
  wchar_t shown[128];
  if (*peer) wcsncpy(shown, peer, 127);
  else wstr_from_utf8(shown, 128, host); /* refused before it said its name: its address */
  shown[127] = L'\0';
  peer = shown;
  pair_dlg *d = open_dlg(&s.enter, NULL, TRUE, peer);
  if (!d) return;
  snprintf(d->target, sizeof d->target, "%s", uid);
  snprintf(d->host, sizeof d->host, "%s", host);
  d->port = port;
  wchar_t title[256], text[512];
  peer_text(title, 256, MC_STR_PAIR_WITH, peer, FALSE);
  peer_text(text, 512, MC_STR_PAIR_AGAIN_TEXT, peer, FALSE);
  show_message(d, title, text, ACT_PAIR_AGAIN, MC_STR_PAIR_AGAIN, MC_STR_CANCEL);
  ShowWindow(d->wnd, SW_SHOW);
}

/* The ended dialog's action button. The dialog closes first, so what follows (a page, a
 * folder picker, another pairing) has the main window back. */
static void run_action(pair_dlg *d) {
  ended_action action = d->action;
  char uid[MC_UID_STRLEN], host[MC_SYNC_ADDR_STRLEN];
  wchar_t peer[128];
  snprintf(uid, sizeof uid, "%s", d->target);
  snprintf(host, sizeof host, "%s", d->host);
  uint16_t port = d->port;
  wcscpy(peer, d->peer);
  const pair_dialog_host *h = s.host;
  if (action == ACT_NEW_LIBRARY && *uid) {
    /* Where it is now, in case discovery hasn't seen it again by the time the new library's
     * sync asks. */
    mc_sync_nearby_list *nearby = NULL;
    mc_sync *sync = sync_bridge_get();
    if (sync && mc_sync_list_nearby(sync, &nearby) == MC_OK) {
      for (int i = 0; i < nearby->count; i++)
        if (!strcmp(nearby->items[i].uid, uid) && nearby->items[i].address) {
          snprintf(host, sizeof host, "%s", nearby->items[i].address);
          port = nearby->items[i].port;
        }
      mc_sync_nearby_list_free(nearby);
    }
  }
  close_dlg(d); /* frees d */
  if (!h) return;
  switch (action) {
    case ACT_CHOOSE:
      if (h->show_device) h->show_device(uid);
      break;
    case ACT_NEW_LIBRARY:
      if (h->new_library && h->new_library()) offer_pair_again(uid, peer, host, port);
      break;
    case ACT_PAIR_AGAIN:
      if (h->pair_again) h->pair_again(uid, peer, *host ? host : NULL, port);
      break;
    case ACT_NONE:
      break;
  }
}

/* --- API --- */

void pair_dialog_init(HWND owner, const pair_dialog_host *host) {
  s.owner = owner;
  s.host = host;
}

void pair_dialog_set_target(const char *uid, const char *host, uint16_t port) {
  pair_dlg *d = s.enter;
  if (!d || d->ended) return;
  snprintf(d->target, sizeof d->target, "%s", uid ? uid : "");
  snprintf(d->host, sizeof d->host, "%s", host ? host : "");
  d->port = port;
}

void pair_dialog_begin_enter(mc_sync *sync, const wchar_t *peer, const wchar_t *address) {
  pair_dlg *d = open_dlg(&s.enter, sync, TRUE, peer);
  if (!d) return;
  s.enter_cancelled = FALSE;
  wchar_t text[256];
  if (peer && *peer) {
    peer_text(text, 256, MC_STR_PAIR_WITH, peer, FALSE);
    SetWindowTextW(d->wnd, text);
  } else {
    SetWindowTextW(d->wnd, mc_wstr(MC_STR_PAIR_BY_ADDRESS));
  }
  if (!(peer && *peer) && address && *address) { /* the address on a line of its own */
    char *a = wstr_to_utf8(address);
    mc_wformat(text, 256, mc_str(MC_STR_PAIR_ENTER_CODE_AT), MC_S(a));
    free(a);
  } else {
    peer_text(text, 256, MC_STR_PAIR_ENTER_CODE, peer, FALSE);
  }
  SetDlgItemTextW(d->wnd, ID_TEXT, text);
  ShowWindow(d->wnd, SW_SHOW);
}

/* "123456" as "123 456". */
static void spaced_code(wchar_t *out, const char *code) {
  int n = 0;
  for (int i = 0; code[i] && i < MC_PAIR_CODE_LEN; i++) {
    if (i == MC_PAIR_CODE_LEN / 2) out[n++] = L' ';
    out[n++] = (wchar_t)(unsigned char)code[i];
  }
  out[n] = L'\0';
}

void pair_dialog_show_code(mc_sync *sync, const sync_event *ev) {
  wchar_t code[MC_PAIR_CODE_LEN + 2];
  spaced_code(code, ev->code);
  pair_dlg *d = s.show;
  if (d && !d->ended) {
    /* A new code after too many wrong ones. */
    SetDlgItemTextW(d->wnd, IDC_PAIR_CODE, code);
    set_status(d, mc_wstr(MC_STR_PAIR_NEW_CODE));
  } else {
    if (!(d = open_dlg(&s.show, sync, FALSE, ev->name))) return;
    s.show_cancelled = FALSE;
    wchar_t text[256];
    SetWindowTextW(d->wnd, mc_wstr(MC_STR_PAIR_REQUEST));
    peer_text(text, 256, MC_STR_PAIR_SHOW_CODE, ev->name, FALSE);
    SetDlgItemTextW(d->wnd, ID_TEXT, text);
    SetDlgItemTextW(d->wnd, IDC_PAIR_CODE, code);
    ShowWindow(d->wnd, SW_SHOW);
  }
  if (s.owner) {
    FLASHWINFO fi = {sizeof fi, s.owner, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
  }
}

static void initiator_result(const sync_event *ev) {
  pair_dlg *d = s.enter;
  if (!d) {
    /* Closed by Cancel: only a pairing that made it anyway is worth telling. */
    if (ev->reason == MC_PAIR_WRONG_CODE || (s.enter_cancelled && ev->reason != MC_PAIR_PAIRED)) {
      if (ev->reason != MC_PAIR_WRONG_CODE) s.enter_cancelled = FALSE;
      return;
    }
    if (!(d = open_dlg(&s.enter, NULL, TRUE, ev->name))) return;
    s.enter_cancelled = FALSE;
    show_ended(d, ev);
    ShowWindow(d->wnd, SW_SHOW);
    return;
  }
  if (d->ended) return;
  if (ev->reason == MC_PAIR_WRONG_CODE) {
    /* The pairing waits for another code. */
    wchar_t text[256];
    if (ev->attempts_left >= MAX_FAILURES) {
      peer_text(text, 256, MC_STR_PAIR_PEER_NEW_CODE, ev->name[0] ? ev->name : d->peer, TRUE);
    } else {
      tries_left(text, 256, MC_STR_PAIR_WRONG_CODE, ev->attempts_left);
    }
    set_status(d, text);
    ready_for_code(d);
    return;
  }
  show_ended(d, ev);
}

static void responder_result(const sync_event *ev) {
  pair_dlg *d = s.show;
  if (ev->reason == MC_PAIR_CANCELLED && (s.show_cancelled || !d || d->ended)) {
    s.show_cancelled = FALSE; /* ours, or nothing showing to cancel */
    return;
  }
  if (ev->reason == MC_PAIR_WRONG_CODE) {
    if (!d || d->ended) return;
    wchar_t text[256];
    if (ev->attempts_left >= MAX_FAILURES) {
      /* The new code is already showing. */
      set_status(d, mc_wstr(MC_STR_PAIR_NEW_CODE));
    } else {
      tries_left(text, 256, MC_STR_PAIR_WRONG_CODE, ev->attempts_left);
      set_status(d, text);
    }
    return;
  }
  if (!d || d->ended) {
    /* Refused before a code was shown: say why on a dialog of its own. */
    if (s.show_cancelled && ev->reason != MC_PAIR_PAIRED) {
      s.show_cancelled = FALSE;
      return;
    }
    if (!(d = open_dlg(&s.show, NULL, FALSE, ev->name))) return;
    show_ended(d, ev);
    ShowWindow(d->wnd, SW_SHOW);
    if (s.owner) {
      FLASHWINFO fi = {sizeof fi, s.owner, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
      FlashWindowEx(&fi);
    }
  } else {
    show_ended(d, ev);
  }
  s.show_cancelled = FALSE;
}

BOOL pair_dialog_on_result(const sync_event *ev) {
  if (!ev) return FALSE;
  if (ev->initiator) initiator_result(ev);
  else responder_result(ev);
  return ev->reason == MC_PAIR_PAIRED;
}

BOOL pair_dialog_translate(MSG *msg) {
  if (s.enter && IsDialogMessageW(s.enter->wnd, msg)) return TRUE;
  if (s.show && IsDialogMessageW(s.show->wnd, msg)) return TRUE;
  return FALSE;
}

void pair_dialog_close_all(void) {
  close_dlg(s.enter);
  close_dlg(s.show);
  s.enter_cancelled = s.show_cancelled = FALSE;
}
