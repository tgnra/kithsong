#include "pair_dialog.h"

#include <stdlib.h>
#include <wchar.h>

#include "theme.h"
#include "wstr.h"

/* Wrong codes before the responder draws a new one (MC_PAIR_MAX_FAILURES in core's private
 * pair.h). A wrong-code result with this many tries left means a new code is showing. */
#define MAX_FAILURES 3

enum { ID_TEXT = 100 };

/* Layout, in dialog units. */
enum { DLG_W = 220, DLG_H = 112, MARGIN = 7, BTN_W = 50, BTN_H = 14, TEXT_H = 18, CODE_Y = 29, STATUS_Y = 55 };

/* Point sizes of the code: typed, and shown. */
enum { ENTER_PT = 16, SHOW_PT = 22 };

/* One open dialog. Freed with its window. */
typedef struct pair_dlg {
  HWND wnd;
  mc_sync *sync;
  BOOL initiator;
  BOOL checking; /* the code went to the other device; waiting to hear */
  BOOL ended;    /* shows how the pairing ended, with only OK */
  HFONT big_font;
  wchar_t peer[128];
} pair_dlg;

static struct {
  HWND owner;
  HWND focus;         /* the owner's focus before the first dialog opened */
  int open;           /* dialogs open; the owner is disabled while there are any */
  pair_dlg *enter;    /* the enter-code dialog (initiator), or NULL */
  pair_dlg *show;     /* the show-code dialog (responder), or NULL */
  BOOL enter_cancelled, show_cancelled; /* Cancel was pressed: the CANCELLED to come is ours */
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

/* Both dialogs: a line of text, the code, the status line, then Pair (or OK) and Cancel. The
 * texts are set once it's created. */
static void build(tmpl *t, BOOL enter) {
  enum { NUM_ITEMS = 5 };
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

/* prefix_id ("Wrong code. {0}") with how many tries are left as its {0}. */
static void tries_left(wchar_t *out, size_t n, enum mc_str_id prefix_id, int left) {
  char tries[64];
  mc_format(tries, sizeof tries, mc_str_plural(MC_STR_TRIES_LEFT, left), MC_D(left));
  mc_wformat(out, n, mc_str(prefix_id), MC_S(tries));
}

/* How a pairing ended, as on Android. */
static void ended_text(wchar_t *out, size_t n, mc_pair_reason reason, BOOL initiator, const wchar_t *name) {
  switch (reason) {
    case MC_PAIR_PAIRED:
      peer_text(out, n, MC_STR_WINDOWS_PC_PAIRED_TEXT, name, FALSE);
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

/* Shows how the pairing ended, with one OK button in Cancel's place. */
static void show_ended(pair_dlg *d, mc_pair_reason reason, const wchar_t *name) {
  wchar_t text[512];
  ended_text(text, 512, reason, d->initiator, name && *name ? name : d->peer);
  d->ended = TRUE;
  d->checking = FALSE;
  SetWindowTextW(d->wnd, mc_wstr(reason == MC_PAIR_PAIRED ? MC_STR_PAIRED : MC_STR_PAIR_FAILED));
  ShowWindow(GetDlgItem(d->wnd, ID_TEXT), SW_HIDE);
  ShowWindow(GetDlgItem(d->wnd, IDC_PAIR_CODE), SW_HIDE);
  ShowWindow(GetDlgItem(d->wnd, IDCANCEL), SW_HIDE);
  place(d->wnd, IDC_PAIR_STATUS, MARGIN, MARGIN, DLG_W - 2 * MARGIN, DLG_H - 3 * MARGIN - BTN_H);
  set_status(d, text);
  HWND ok = GetDlgItem(d->wnd, IDOK);
  place(d->wnd, IDOK, DLG_W - MARGIN - BTN_W, DLG_H - MARGIN - BTN_H, BTN_W, BTN_H);
  SetWindowTextW(ok, mc_wstr(MC_STR_OK));
  EnableWindow(ok, TRUE);
  ShowWindow(ok, SW_SHOW);
  SendMessageW(d->wnd, DM_SETDEFID, IDOK, 0);
  if (GetActiveWindow() == d->wnd) SendMessageW(d->wnd, WM_NEXTDLGCTL, (WPARAM)ok, TRUE);
}

static void apply_theme(HWND dlg) {
  theme_apply_window(dlg, FALSE);
  theme_apply_controls(dlg);
}

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

/* --- API --- */

void pair_dialog_init(HWND owner) {
  s.owner = owner;
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
    show_ended(d, ev->reason, ev->name);
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
  show_ended(d, ev->reason, ev->name);
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
      tries_left(text, 256, MC_STR_PAIR_WRONG_CODE_ENTERED, ev->attempts_left);
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
    show_ended(d, ev->reason, ev->name);
    ShowWindow(d->wnd, SW_SHOW);
    if (s.owner) {
      FLASHWINFO fi = {sizeof fi, s.owner, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
      FlashWindowEx(&fi);
    }
  } else {
    show_ended(d, ev->reason, ev->name);
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
