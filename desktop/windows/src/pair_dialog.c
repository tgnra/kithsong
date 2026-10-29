#include "pair_dialog.h"

#include <commctrl.h>
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
  /* Merging libraries: confirming while this PC's answer is wanted (Pair, or the hidden OK,
   * reads Merge); proposing while the enter-code dialog shows the proposal in place of the
   * code. merge_text follows the status line (the proposal, a note, or that it will merge). */
  BOOL confirming, proposing;
  wchar_t title[256];        /* the enter-code dialog's own, while proposing */
  wchar_t status_base[1536]; /* the status line before merge_text */
  wchar_t merge_text[1536];
} pair_dlg;

/* The window the merge runs in, after pairing. */
typedef struct merge_wnd {
  HWND wnd;
  BOOL done;
} merge_wnd;

static struct {
  HWND owner;
  HWND focus;         /* the owner's focus before the first dialog opened */
  int open;           /* dialogs open; the owner is disabled while there are any */
  pair_dlg *enter;    /* the enter-code dialog (initiator), or NULL */
  pair_dlg *show;     /* the show-code dialog (responder), or NULL */
  BOOL enter_cancelled, show_cancelled; /* Cancel was pressed: the CANCELLED to come is ours */
  const pair_dialog_host *host;
  /* on_pair_merge comes just before on_pair_code on the device showing the code: kept for
   * the show-code dialog to open with. */
  BOOL has_pending_merge;
  sync_event pending_merge;
  merge_wnd *merge;   /* the merge's window, or NULL */
  BOOL merge_seen;    /* it opened for this merge's progress (and may have been closed) */
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

/* atom is a predefined class: 0x0080 button, 0x0081 edit, 0x0082 static; or 0 for the class
 * named cls. */
static void put_item_class(tmpl *t, WORD atom, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y,
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
    put_string(t, cls);
  }
  put_string(t, text);
  put_word(t, 0); /* no creation data */
}

static void put_item(tmpl *t, WORD atom, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
  put_item_class(t, atom, NULL, text, style, x, y, w, h, id);
}

/* The template's header, for a dialog of num_items items. */
static void put_header(tmpl *t, int num_items) {
  t->n = 0;
  put_word(t, 1);      /* version */
  put_word(t, 0xFFFF); /* DLGTEMPLATEEX */
  put_dword(t, 0);     /* help id */
  put_dword(t, 0);     /* extended style */
  put_dword(t, DS_SHELLFONT | DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU);
  put_word(t, (WORD)num_items);
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
}

/* Both dialogs: a line of text, the code, the status line, then Pair (or OK) and Cancel, and
 * the ended dialog's action button (hidden until then). The texts are set once it's
 * created. */
static void build(tmpl *t, BOOL enter) {
  put_header(t, 6);
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

/* Appends text to out as a paragraph of its own (or as all of it, if out is empty). */
static void append_para(wchar_t *out, size_t n, const wchar_t *text) {
  size_t len = wcslen(out);
  if (!*text || len + 2 >= n) return;
  swprintf(out + len, n - len, len ? L"\n\n%ls" : L"%ls", text);
}

/* After pairing by merging: whose library was kept, and the Last.fm account signed out. */
static void append_merged(wchar_t *out, size_t n, const sync_event *ev, const wchar_t *name) {
  if (!ev->merged) return;
  wchar_t text[512];
  peer_text(text, 512, ev->kept_here ? MC_STR_PAIRED_MERGED_KEPT_TEXT : MC_STR_PAIRED_MERGED_TEXT, name, TRUE);
  append_para(out, n, text);
  if (ev->scrobble_user[0]) {
    char *peer = wstr_to_utf8(peer_name(name, FALSE));
    mc_wformat(text, 512, mc_str(ev->kept_here ? MC_STR_PAIRED_MERGED_SCROBBLE_THERE : MC_STR_PAIRED_MERGED_SCROBBLE_HERE),
               MC_S(ev->scrobble_user), MC_S(peer));
    free(peer);
    append_para(out, n, text);
  }
}

/* How a pairing ended, as on Android. */
static void ended_text(wchar_t *out, size_t n, const sync_event *ev, BOOL initiator, const wchar_t *name) {
  mc_pair_reason reason = ev->reason;
  switch (reason) {
    case MC_PAIR_PAIRED:
      /* Nothing syncs yet: the device that joined has its sync settings to choose. */
      peer_text(out, n, ev->joined ? MC_STR_WINDOWS_PC_PAIRED_TEXT : MC_STR_PAIRED_PEER_TEXT, name, !ev->joined);
      append_merged(out, n, ev, name);
      append_archive_explainer(out, n, ev);
      break;
    case MC_PAIR_MERGE_DECLINED:
      swprintf(out, n, L"%ls", mc_wstr(MC_STR_PAIR_MERGE_DECLINED));
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
      {
        /* This PC merges libraries, so the other device can't (yet). */
        wchar_t hint[512];
        peer_text(hint, 512, MC_STR_PAIR_MERGE_UPDATE_HINT, name, FALSE);
        append_para(out, n, hint);
      }
      if (could_join()) append_para(out, n, mc_wstr(MC_STR_PAIR_NEW_LIBRARY_HINT));
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

static void relayout(pair_dlg *d);

/* The status line: text, then merge_text as a paragraph of its own (alone while proposing).
 * The dialog grows or shrinks to fit. */
static void set_status(pair_dlg *d, const wchar_t *text) {
  if (text != d->status_base) {
    wcsncpy(d->status_base, text ? text : L"", sizeof d->status_base / sizeof d->status_base[0] - 1);
    d->status_base[sizeof d->status_base / sizeof d->status_base[0] - 1] = L'\0';
  }
  wchar_t shown[3200] = L"";
  if (!d->proposing) append_para(shown, 3200, d->status_base);
  append_para(shown, 3200, d->merge_text);
  SetDlgItemTextW(d->wnd, IDC_PAIR_STATUS, shown);
  relayout(d);
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
  if (d->ended) return;
  if (d->confirming) EnableWindow(GetDlgItem(d->wnd, IDOK), d->proposing || !d->initiator);
  else if (d->initiator) EnableWindow(GetDlgItem(d->wnd, IDOK), !d->checking && code_text(d->wnd, code));
}

/* The enter-code dialog, ready for (another) code. */
static void ready_for_code(pair_dlg *d) {
  d->checking = FALSE;
  HWND edit = GetDlgItem(d->wnd, IDC_PAIR_CODE);
  EnableWindow(edit, TRUE);
  SetWindowTextW(edit, L"");
  update_pair(d);
  if (GetActiveWindow() == d->wnd && !d->proposing) SetFocus(edit);
}

/* Moves a control to y (pixels, in the dialog's client area), keeping its x. */
static void move_to_y(HWND dlg, int id, int y) {
  HWND item = GetDlgItem(dlg, id);
  RECT r;
  if (!item || !GetWindowRect(item, &r)) return;
  MapWindowPoints(NULL, dlg, (POINT *)&r, 2);
  SetWindowPos(item, NULL, r.left, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The height text needs in the static control ctl at width w, in pixels. */
static int text_height(HWND ctl, int w) {
  int len = GetWindowTextLengthW(ctl);
  wchar_t *text = malloc(((size_t)len + 1) * sizeof *text);
  if (!text) return 0;
  GetWindowTextW(ctl, text, len + 1);
  RECT need = {0, 0, w, 0};
  HDC dc = GetDC(ctl);
  HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0));
  DrawTextW(dc, text, -1, &need, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
  SelectObject(dc, old);
  ReleaseDC(ctl, dc);
  free(text);
  return need.bottom;
}

/* Lays out dlg's status line from top to at least bottom (dialog units), taller if its text
 * needs it, the buttons under it, and the dialog's height to fit, growing or shrinking back
 * and keeping its middle. */
static void layout_status(HWND dlg, int top, int bottom) {
  int by_dlu = DLG_H - MARGIN - BTN_H;
  RECT st = {MARGIN, top, DLG_W - MARGIN, bottom};
  RECT gap = {0, 0, MARGIN, MARGIN}, btn = {0, by_dlu, BTN_W, by_dlu + BTN_H};
  MapDialogRect(dlg, &st);
  MapDialogRect(dlg, &gap);
  MapDialogRect(dlg, &btn);
  HWND status = GetDlgItem(dlg, IDC_PAIR_STATUS);
  int need = text_height(status, st.right - st.left);
  if (st.top + need > st.bottom) st.bottom = st.top + need;
  SetWindowPos(status, NULL, st.left, st.top, st.right - st.left, st.bottom - st.top, SWP_NOZORDER | SWP_NOACTIVATE);
  int by = max(btn.top, st.bottom + gap.bottom);
  move_to_y(dlg, IDOK, by);
  move_to_y(dlg, IDCANCEL, by);
  move_to_y(dlg, ID_ACTION, by);
  int want = by + (btn.bottom - btn.top) + gap.bottom;
  RECT client, w;
  GetClientRect(dlg, &client);
  GetWindowRect(dlg, &w);
  int dy = want - client.bottom;
  if (dy)
    SetWindowPos(dlg, NULL, w.left, w.top - dy / 2, w.right - w.left, w.bottom - w.top + dy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  /* Now, parent first: a dialog doesn't clip its children, so an erase of the area its
   * controls moved into that came after their own paint would wipe them out. */
  RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW | RDW_ERASENOW);
}

/* The status line starts under the code, or at the top once the text and code are hidden
 * (ended, proposing), and is at least as tall as the template has it. */
static void relayout(pair_dlg *d) {
  BOOL top = d->ended || d->proposing;
  layout_status(d->wnd, top ? MARGIN : STATUS_Y, top ? DLG_H - 2 * MARGIN - BTN_H : STATUS_Y + 28);
}

/* Turns the dialog into a message: title, text, and OK (labelled ok_id) in Cancel's place,
 * with the action button left of it unless action is ACT_NONE. */
static void show_message(pair_dlg *d, const wchar_t *title, const wchar_t *text, ended_action action,
                         enum mc_str_id action_id, enum mc_str_id ok_id) {
  d->ended = TRUE;
  d->checking = FALSE;
  d->confirming = d->proposing = FALSE;
  d->merge_text[0] = L'\0';
  d->action = action;
  SetWindowTextW(d->wnd, title);
  ShowWindow(GetDlgItem(d->wnd, ID_TEXT), SW_HIDE);
  ShowWindow(GetDlgItem(d->wnd, IDC_PAIR_CODE), SW_HIDE);
  ShowWindow(GetDlgItem(d->wnd, IDCANCEL), SW_HIDE);
  HWND ok = GetDlgItem(d->wnd, IDOK), act = GetDlgItem(d->wnd, ID_ACTION);
  place(d->wnd, IDOK, DLG_W - MARGIN - BTN_W, DLG_H - MARGIN - BTN_H, BTN_W, BTN_H);
  SetWindowTextW(ok, mc_wstr_mnemonic(ok_id));
  EnableWindow(ok, TRUE);
  ShowWindow(ok, SW_SHOW);
  if (action != ACT_NONE) {
    SetWindowTextW(act, mc_wstr_mnemonic(action_id));
    ShowWindow(act, SW_SHOW);
  }
  set_status(d, text); /* and lays it out */
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
  wchar_t text[2048];
  ended_text(text, 2048, ev, d->initiator, name);
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

/* --- Merging libraries --- */

/* "1,204 tracks"-style count (the tracks string) as UTF-8. */
static void track_count(char *out, size_t n, int64_t count) {
  mc_format(out, n, mc_str_plural(MC_STR_TRACKS, (long)count), MC_D(count));
}

/* What on_pair_merge proposes: on the PC whose library is merged, which library is kept and
 * what goes with this one; on the one kept, a note. */
static void merge_proposal(wchar_t *out, size_t n, const sync_event *ev, const wchar_t *name) {
  char *peer = wstr_to_utf8(peer_name(name, FALSE));
  char *peer_start = wstr_to_utf8(peer_name(name, TRUE));
  char here[64], there[64];
  track_count(here, sizeof here, ev->tracks_here);
  track_count(there, sizeof there, ev->tracks_there);
  wchar_t para[768];
  out[0] = L'\0';
  if (!ev->needs_confirm) {
    mc_wformat(out, n, mc_str(MC_STR_PAIR_MERGE_KEPT_TEXT), MC_S(peer), MC_S(here), MC_S(there));
  } else {
    mc_wformat(out, n, mc_str(MC_STR_PAIR_MERGE_TEXT), MC_S(peer_start), MC_S(there), MC_S(here));
    append_para(out, n, mc_wstr(MC_STR_PAIR_MERGE_BACKUP));
    if (ev->scrobble_sign_out && ev->scrobble_user[0]) {
      mc_wformat(para, 768, mc_str(MC_STR_PAIR_MERGE_SCROBBLE), MC_S(ev->scrobble_user), MC_S(peer));
      append_para(out, n, para);
    }
    if (ev->forget_count > 0) {
      mc_wformat(para, 768, mc_str_plural(MC_STR_PAIR_MERGE_FORGET, ev->forget_count), MC_D(ev->forget_count),
                 MC_S(ev->forget_names));
      append_para(out, n, para);
    }
  }
  free(peer);
  free(peer_start);
}

/* Shows on_pair_merge in d. Wanting an answer, the enter-code dialog puts the proposal in
 * place of the code, with Pair as Merge; the show-code dialog shows it under the code, with
 * Merge (its hidden OK) beside Cancel. */
static void show_merge(pair_dlg *d, const sync_event *ev) {
  if (ev->name[0]) wcsncpy(d->peer, ev->name, sizeof d->peer / sizeof d->peer[0] - 1);
  merge_proposal(d->merge_text, sizeof d->merge_text / sizeof d->merge_text[0], ev, d->peer);
  d->confirming = ev->needs_confirm;
  HWND ok = GetDlgItem(d->wnd, IDOK);
  if (d->confirming) {
    SetWindowTextW(ok, mc_wstr_mnemonic(MC_STR_PAIR_MERGE_BUTTON));
    if (d->initiator) {
      d->proposing = TRUE;
      GetWindowTextW(d->wnd, d->title, sizeof d->title / sizeof d->title[0]);
      SetWindowTextW(d->wnd, mc_wstr(MC_STR_PAIR_MERGE_TITLE));
      ShowWindow(GetDlgItem(d->wnd, ID_TEXT), SW_HIDE);
      ShowWindow(GetDlgItem(d->wnd, IDC_PAIR_CODE), SW_HIDE);
    }
    ShowWindow(ok, SW_SHOW);
    update_pair(d);
    SendMessageW(d->wnd, DM_SETDEFID, IDOK, 0);
    if (GetActiveWindow() == d->wnd) SendMessageW(d->wnd, WM_NEXTDLGCTL, (WPARAM)ok, TRUE);
  }
  set_status(d, d->status_base);
}

/* Merge pressed: this PC's library goes into the other's. The enter-code dialog goes back to
 * the code (still waiting for one, or for the other device if it was sent already). */
static void answer_merge(pair_dlg *d) {
  if (!d->confirming || !d->sync) return;
  if (mc_sync_pair_merge_confirm(d->sync, 1) != MC_OK) return; /* over: its result follows */
  d->confirming = FALSE;
  swprintf(d->merge_text, sizeof d->merge_text / sizeof d->merge_text[0], L"%ls", mc_wstr(MC_STR_PAIR_MERGE_CONFIRMED));
  HWND ok = GetDlgItem(d->wnd, IDOK);
  if (d->initiator) {
    d->proposing = FALSE;
    SetWindowTextW(d->wnd, d->title);
    SetWindowTextW(ok, mc_wstr_mnemonic(MC_STR_PAIR));
    ShowWindow(GetDlgItem(d->wnd, ID_TEXT), SW_SHOW);
    ShowWindow(GetDlgItem(d->wnd, IDC_PAIR_CODE), SW_SHOW);
    update_pair(d);
    HWND next = d->checking ? GetDlgItem(d->wnd, IDCANCEL) : GetDlgItem(d->wnd, IDC_PAIR_CODE);
    SendMessageW(d->wnd, WM_NEXTDLGCTL, (WPARAM)next, TRUE);
  } else {
    ShowWindow(ok, SW_HIDE);
    SendMessageW(d->wnd, WM_NEXTDLGCTL, (WPARAM)GetDlgItem(d->wnd, IDCANCEL), TRUE);
  }
  set_status(d, d->status_base);
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
          if (d->confirming) { /* Merge */
            answer_merge(d);
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
            /* Cancelling here: the CANCELLED (or MERGE_DECLINED) result it brings needs no
             * telling. */
            if (d->initiator) s.enter_cancelled = TRUE;
            else s.show_cancelled = TRUE;
            /* While the merge is asked, Cancel declines it: that ends the pairing on the
             * initiator, and on the device showing the code when the other enters it. */
            if (d->sync && (!d->confirming || mc_sync_pair_merge_confirm(d->sync, 0) != MC_OK))
              mc_sync_pair_cancel(d->sync);
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

/* --- The merge's window ---
 *
 *   +- Merging libraries ---------------- X -+      +- Libraries merged -------------- X -+
 *   | The music from your old library is     |      | Tracks: 120 added, 4 improved, 30   |
 *   | coming into this one. You can keep ... |  ->  | already in the library.             |
 *   | [##########---------]                  |      | 3 playlists, ... carried over.      |
 *   | Albums: 12 of 40                       |      | The old library is saved as C:\...  |
 *   |                              [Close]   |      |                             [Close] |
 *   +----------------------------------------+      +-------------------------------------+
 *
 * Modeless, and the main window stays enabled: the merge runs on sync's threads. Close only
 * hides it from view; the summary opens it again. */

enum { ID_MERGE_PROGRESS = 102, MERGE_BAR_Y = 29, MERGE_STATUS_Y = 43 };

static void merge_layout(merge_wnd *m) {
  layout_status(m->wnd, m->done ? MARGIN : MERGE_STATUS_Y, DLG_H - 2 * MARGIN - BTN_H);
}

static INT_PTR CALLBACK merge_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  merge_wnd *m = (merge_wnd *)GetWindowLongPtrW(dlg, DWLP_USER);
  switch (msg) {
    case WM_INITDIALOG:
      m = (merge_wnd *)lp;
      m->wnd = dlg;
      SetWindowLongPtrW(dlg, DWLP_USER, lp);
      SendDlgItemMessageW(dlg, ID_MERGE_PROGRESS, PBM_SETRANGE32, 0, 1000);
      apply_theme(dlg);
      return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      HBRUSH brush = theme_ctlcolor(msg, (HDC)wp);
      if (msg == WM_CTLCOLORSTATIC && (HWND)lp == GetDlgItem(dlg, IDC_PAIR_STATUS) && m && !m->done)
        SetTextColor((HDC)wp, theme.text2);
      return (INT_PTR)brush;
    }
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update();
        apply_theme(dlg);
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
        DestroyWindow(dlg);
        return TRUE;
      }
      break;
    case WM_NCDESTROY:
      if (s.merge == m) s.merge = NULL;
      free(m);
      SetWindowLongPtrW(dlg, DWLP_USER, 0);
      break;
  }
  return FALSE;
}

/* The merge's window, opened if it isn't: intro text, progress bar, status line, Close. */
static merge_wnd *merge_window(void) {
  if (s.merge) return s.merge;
  merge_wnd *m = calloc(1, sizeof *m);
  tmpl *t = malloc(sizeof *t);
  if (!m || !t) {
    free(m);
    free(t);
    return NULL;
  }
  put_header(t, 4);
  int w = DLG_W - 2 * MARGIN;
  put_item(t, 0x0082, mc_wstr(MC_STR_MERGE_TEXT), SS_LEFT | SS_NOPREFIX, MARGIN, MARGIN, w, TEXT_H, ID_TEXT);
  put_item_class(t, 0, PROGRESS_CLASSW, L"", 0, MARGIN, MERGE_BAR_Y, w, 9, ID_MERGE_PROGRESS);
  /* Edit-control wrapping: the backup's path has no spaces to break at. */
  put_item(t, 0x0082, L"", SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL, MARGIN, MERGE_STATUS_Y, w, 20, IDC_PAIR_STATUS);
  put_item(t, 0x0080, mc_wstr_mnemonic(MC_STR_CLOSE), BS_DEFPUSHBUTTON | WS_TABSTOP, DLG_W - MARGIN - BTN_W,
           DLG_H - MARGIN - BTN_H, BTN_W, BTN_H, IDOK);
  HWND wnd = CreateDialogIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, s.owner, merge_proc,
                                        (LPARAM)m);
  free(t);
  if (!wnd) {
    free(m);
    return NULL;
  }
  s.merge = m;
  SetWindowTextW(wnd, mc_wstr(MC_STR_MERGE_TITLE));
  merge_layout(m);
  ShowWindow(wnd, SW_SHOWNOACTIVATE);
  return m;
}

/* Appends item to a list being built in out (UTF-8), with the list separator. */
static void list_add(char *out, size_t n, const char *item) {
  size_t len = strlen(out);
  if (len) snprintf(out + len, n - len, "%s%s", mc_str(MC_STR_IMPORT_LIST_SEPARATOR), item);
  else snprintf(out, n, "%s", item);
}

static void list_add_count(char *out, size_t n, enum mc_str_id id, int count) {
  if (count <= 0) return;
  char item[128];
  mc_format(item, sizeof item, mc_str_plural(id, count), MC_D(count));
  list_add(out, n, item);
}

/* What the merge did, in short paragraphs. */
static void merge_summary(wchar_t *out, size_t n, const sync_event *ev) {
  const mc_merge_result *r = &ev->merge;
  wchar_t para[1280];
  out[0] = L'\0';
  if (ev->status != MC_OK) {
    mc_wformat(para, 1280, mc_str(MC_STR_MERGE_FAILED), MC_S(mc_status_str(ev->status)));
    append_para(out, n, para);
  }
  char tracks[512] = "";
  list_add_count(tracks, sizeof tracks, MC_STR_MERGE_TRACKS_ADDED, r->tracks_added);
  list_add_count(tracks, sizeof tracks, MC_STR_MERGE_TRACKS_BETTER, r->tracks_better);
  list_add_count(tracks, sizeof tracks, MC_STR_MERGE_TRACKS_MATCHED, r->tracks_attached + r->tracks_mapped);
  list_add_count(tracks, sizeof tracks, MC_STR_WINDOWS_MERGE_TRACKS_RECYCLED, r->tracks_skipped);
  list_add_count(tracks, sizeof tracks, MC_STR_MERGE_TRACKS_LEFT_OUT, r->tracks_left_out);
  list_add_count(tracks, sizeof tracks, MC_STR_MERGE_TRACKS_FAILED, r->tracks_failed);
  if (tracks[0]) mc_wformat(para, 1280, mc_str(MC_STR_MERGE_TRACKS), MC_S(tracks));
  else swprintf(para, 1280, L"%ls", mc_wstr(MC_STR_MERGE_TRACKS_NONE));
  append_para(out, n, para);
  if (r->playlists > 0) {
    mc_wformat(para, 1280, mc_str_plural(MC_STR_MERGE_PLAYLISTS, r->playlists), MC_D(r->playlists));
    if (r->playlists_renamed > 0) {
      size_t len = wcslen(para);
      para[len++] = L' ';
      mc_wformat(para + len, 1280 - len, mc_str_plural(MC_STR_MERGE_PLAYLISTS_RENAMED, r->playlists_renamed),
                 MC_D(r->playlists_renamed));
    }
    append_para(out, n, para);
  }
  if (ev->scrobble_user[0]) {
    mc_wformat(para, 1280, mc_str(MC_STR_MERGE_SCROBBLE_SIGNED_OUT), MC_S(ev->scrobble_user));
    if (r->scrobbles_dropped > 0) {
      size_t len = wcslen(para);
      para[len++] = L' ';
      mc_wformat(para + len, 1280 - len, mc_str_plural(MC_STR_MERGE_SCROBBLES_DROPPED, r->scrobbles_dropped),
                 MC_D(r->scrobbles_dropped));
    }
    append_para(out, n, para);
  }
  if (ev->backup_path[0]) {
    mc_wformat(para, 1280, mc_str(MC_STR_MERGE_BACKUP), MC_S(ev->backup_path));
    append_para(out, n, para);
  }
}

void pair_dialog_on_merge_event(const sync_event *ev) {
  if (ev->kind == SYNC_EV_MERGE_PROGRESS) {
    merge_wnd *m = s.merge;
    if (!m) {
      /* Closed while it runs: it stays closed until the summary. Opened the first time. */
      if (s.merge_seen) return;
      s.merge_seen = TRUE;
      if (!(m = merge_window())) return;
    }
    if (m->done) return;
    wchar_t text[128];
    mc_wformat(text, 128, mc_str(ev->merge_phase == MC_MERGE_PHASE_TRACKS ? MC_STR_MERGE_PROGRESS_TRACKS
                                                                          : MC_STR_MERGE_PROGRESS_LISTS),
               MC_D(ev->merge_done), MC_D(ev->merge_total));
    SetDlgItemTextW(m->wnd, IDC_PAIR_STATUS, text);
    /* Albums take most of the time: the bar gives them nine tenths. */
    int part = ev->merge_total > 0 ? MulDiv(min(ev->merge_done, ev->merge_total), 1000, ev->merge_total) : 0;
    int pos = ev->merge_phase == MC_MERGE_PHASE_TRACKS ? part * 9 / 10 : 900 + part / 10;
    SendDlgItemMessageW(m->wnd, ID_MERGE_PROGRESS, PBM_SETPOS, (WPARAM)pos, 0);
    return;
  }
  if (ev->kind != SYNC_EV_MERGE_DONE) return;
  s.merge_seen = FALSE;
  merge_wnd *m = merge_window();
  if (!m) return;
  m->done = TRUE;
  SetWindowTextW(m->wnd, mc_wstr(ev->status == MC_OK ? MC_STR_MERGE_DONE_TITLE : MC_STR_MERGE_FAILED_TITLE));
  ShowWindow(GetDlgItem(m->wnd, ID_TEXT), SW_HIDE);
  ShowWindow(GetDlgItem(m->wnd, ID_MERGE_PROGRESS), SW_HIDE);
  wchar_t text[3200];
  merge_summary(text, 3200, ev);
  SetDlgItemTextW(m->wnd, IDC_PAIR_STATUS, text);
  merge_layout(m);
  ShowWindow(m->wnd, SW_SHOW);
  if (s.owner) {
    FLASHWINFO fi = {sizeof fi, s.owner, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
    FlashWindowEx(&fi);
  }
}

/* --- API --- */

void pair_dialog_on_merge(const sync_event *ev) {
  pair_dlg *d = ev->initiator ? s.enter : s.show;
  if (d && !d->ended) {
    show_merge(d, ev);
  } else if (!ev->initiator) {
    /* The code is on its way. */
    s.pending_merge = *ev;
    s.has_pending_merge = TRUE;
  }
  /* An initiator with no dialog cancelled already. */
}

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
  if (s.has_pending_merge) { /* asked just before the code */
    s.has_pending_merge = FALSE;
    show_merge(d, &s.pending_merge);
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
  if (ev->reason != MC_PAIR_WRONG_CODE) s.has_pending_merge = FALSE; /* over */
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
  if (s.merge && IsDialogMessageW(s.merge->wnd, msg)) return TRUE;
  return FALSE;
}

void pair_dialog_close_all(void) {
  close_dlg(s.enter);
  close_dlg(s.show);
  if (s.merge) DestroyWindow(s.merge->wnd); /* frees it */
  s.enter_cancelled = s.show_cancelled = FALSE;
  s.has_pending_merge = s.merge_seen = FALSE;
}
