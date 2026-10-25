#include "import_view.h"

#include <commctrl.h>
#include <shellapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pick_folder.h"
#include "resource.h"
#include "theme.h"

enum {
  ID_IMP_DROP = 200,
  ID_IMP_BROWSE,
  ID_IMP_STATUS,
  ID_IMP_PROGRESS,
  ID_IMP_LIST,
  ID_IMP_DONE,
  ID_IMP_BACK,
};

enum { GROUP_REVIEW = 1, GROUP_ACTIVE, GROUP_DONE, GROUP_SKIPPED, GROUP_FAILED };

enum { COL_STATUS, COL_ARTIST, COL_ALBUM, COL_FILES, COL_MATCH, COL_CONFIDENCE, NUM_COLS };

/* Sent to the review dialog when its item changed. */
#define WM_APP_REVIEW_REFRESH (WM_APP + 10)

typedef struct view {
  HWND parent;
  HWND back, drop, browse, status, progress, list, done;
  HFONT font;
  BOOL shown;
  mc_library *lib;

  mc_import *imp;
  WORD generation; /* tags events so ones from a closed session are ignored */
  BOOL idle;
  int scanned_files;
  int item_count;
  mc_import_state *states;

  HWND review; /* open review dialog, if any */
  int review_item;
  mc_import_item *review_snap;
  BOOL review_terms_set;
} view;

static view g_view;

/* --- Helpers --- */

static void set_text_utf8(HWND wnd, const char *utf8) {
  wchar_t buf[1024];
  if (!utf8 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, 1024)) buf[0] = L'\0';
  buf[1023] = L'\0';
  SetWindowTextW(wnd, buf);
}

/* UTF-8 text of a window, malloc'd. */
static char *get_text_utf8(HWND wnd) {
  wchar_t buf[512];
  GetWindowTextW(wnd, buf, 512);
  int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, NULL, 0, NULL, NULL);
  char *s = malloc((size_t)(n > 0 ? n : 1));
  if (s) {
    s[0] = '\0';
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, buf, -1, s, n, NULL, NULL);
  }
  return s;
}

static void lv_set(HWND lv, int row, int col, const wchar_t *text) {
  LVITEMW it = {.mask = LVIF_TEXT, .iItem = row, .iSubItem = col, .pszText = (wchar_t *)text};
  SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&it);
}

static void lv_set_utf8(HWND lv, int row, int col, const char *utf8) {
  wchar_t buf[512];
  if (!utf8 || !MultiByteToWideChar(CP_UTF8, 0, utf8, -1, buf, 512)) buf[0] = L'\0';
  buf[511] = L'\0';
  lv_set(lv, row, col, buf);
}

static int lv_append(HWND lv, LPARAM param, int group) {
  LVITEMW it = {.mask = LVIF_TEXT | LVIF_PARAM | (group ? LVIF_GROUPID : 0),
                .iItem = INT_MAX, .pszText = L"", .lParam = param, .iGroupId = group};
  return (int)SendMessageW(lv, LVM_INSERTITEMW, 0, (LPARAM)&it);
}

static void lv_add_column(HWND lv, int col, const wchar_t *title, int width, int fmt) {
  LVCOLUMNW c = {.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT, .fmt = fmt, .cx = width,
                 .pszText = (wchar_t *)title};
  SendMessageW(lv, LVM_INSERTCOLUMNW, (WPARAM)col, (LPARAM)&c);
}

static const char *file_name(const char *path) {
  const char *a = strrchr(path, '\\'), *b = strrchr(path, '/');
  const char *sep = a > b ? a : b;
  return sep ? sep + 1 : path;
}

static void format_length(int ms, wchar_t *out, size_t n) {
  if (ms <= 0) {
    out[0] = L'\0';
    return;
  }
  int s = (ms + 500) / 1000;
  swprintf(out, n, L"%d:%02d", s / 60, s % 60);
}

static const wchar_t *state_label(mc_import_state s) {
  switch (s) {
    case MC_IMPORT_PENDING: return L"Waiting";
    case MC_IMPORT_MATCHING: return L"Matching\x2026";
    case MC_IMPORT_REVIEW: return L"Needs review";
    case MC_IMPORT_WRITING: return L"Writing\x2026";
    case MC_IMPORT_DONE: return L"Imported";
    case MC_IMPORT_SKIPPED: return L"Skipped";
    case MC_IMPORT_FAILED: return L"Failed";
  }
  return L"";
}

static int state_group(mc_import_state s) {
  switch (s) {
    case MC_IMPORT_REVIEW: return GROUP_REVIEW;
    case MC_IMPORT_DONE: return GROUP_DONE;
    case MC_IMPORT_SKIPPED: return GROUP_SKIPPED;
    case MC_IMPORT_FAILED: return GROUP_FAILED;
    default: return GROUP_ACTIVE;
  }
}

/* "Radiohead – OK Computer (1997)", or for a single "OutKast – Hey Ya! (2003)". */
static void match_summary(const mc_import_match *m, char *out, size_t n) {
  const char *artist = m->album_artist ? m->album_artist : "?";
  const char *title = m->is_single && m->track_count ? m->tracks[0].title : m->album;
  if (!title) title = "?";
  if (m->year) snprintf(out, n, "%s \xE2\x80\x93 %s (%d)", artist, title, m->year);
  else snprintf(out, n, "%s \xE2\x80\x93 %s", artist, title);
}

/* --- Drop zone --- */

static LRESULT CALLBACK drop_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(wnd, &ps);
    RECT rc;
    GetClientRect(wnd, &rc);
    FillRect(dc, &rc, theme.bg_brush);

    HPEN pen = CreatePen(PS_DOT, 1, theme.text3);
    HGDIOBJ old_pen = SelectObject(dc, pen);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, 16, 16);
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(pen);

    HGDIOBJ old_font = SelectObject(dc, g_view.font ? (HGDIOBJ)g_view.font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme.text2);
    DrawTextW(dc, L"Drop a folder of music here", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, old_font);
    EndPaint(wnd, &ps);
    return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* --- Session --- */

static void on_import_event(void *user, mc_import_event ev, int arg) {
  /* Import thread: hand the event to the UI thread. */
  WORD gen = (WORD)(uintptr_t)user;
  PostMessageW(g_view.parent, WM_APP_IMPORT, MAKEWPARAM((WORD)ev, gen), (LPARAM)arg);
}

static void close_session(void) {
  if (!g_view.imp) return;
  mc_import_free(g_view.imp); /* waits for the import thread */
  g_view.imp = NULL;
  free(g_view.states);
  g_view.states = NULL;
  g_view.item_count = 0;
  g_view.scanned_files = 0;
  SendMessageW(g_view.list, LVM_DELETEALLITEMS, 0, 0);
}

static void update_status(void) {
  wchar_t text[256];
  int counts[MC_IMPORT_FAILED + 1] = {0};
  for (int i = 0; i < g_view.item_count; i++) counts[g_view.states[i]]++;
  int settled = counts[MC_IMPORT_REVIEW] + counts[MC_IMPORT_DONE] + counts[MC_IMPORT_SKIPPED] +
                counts[MC_IMPORT_FAILED];

  if (!g_view.states) {
    swprintf(text, 256, L"Scanning\x2026 %d audio files found", g_view.scanned_files);
  } else if (g_view.item_count == 0) {
    swprintf(text, 256, L"No audio files found.");
  } else if (!g_view.idle) {
    swprintf(text, 256, L"Matching %d of %d\x2026   %d imported, %d need your attention",
             settled < g_view.item_count ? settled + 1 : settled, g_view.item_count,
             counts[MC_IMPORT_DONE], counts[MC_IMPORT_REVIEW]);
  } else if (counts[MC_IMPORT_REVIEW]) {
    swprintf(text, 256, L"%d imported. %d need your attention: click one to choose a match.",
             counts[MC_IMPORT_DONE], counts[MC_IMPORT_REVIEW]);
  } else {
    swprintf(text, 256, L"Finished: %d imported, %d skipped, %d failed.", counts[MC_IMPORT_DONE],
             counts[MC_IMPORT_SKIPPED], counts[MC_IMPORT_FAILED]);
  }
  SetWindowTextW(g_view.status, text);
  SendMessageW(g_view.progress, PBM_SETRANGE32, 0, g_view.item_count ? g_view.item_count : 1);
  SendMessageW(g_view.progress, PBM_SETPOS, (WPARAM)settled, 0);
}

static void refresh_row(int idx) {
  mc_import_item *it;
  if (mc_import_item_get(g_view.imp, idx, &it) != MC_OK) return;
  g_view.states[idx] = it->state;

  HWND lv = g_view.list;
  LVITEMW li = {.mask = LVIF_GROUPID, .iItem = idx, .iGroupId = state_group(it->state)};
  SendMessageW(lv, LVM_SETITEMW, 0, (LPARAM)&li);
  lv_set(lv, idx, COL_STATUS, state_label(it->state));
  lv_set_utf8(lv, idx, COL_ARTIST, it->artist);
  lv_set_utf8(lv, idx, COL_ALBUM, it->album ? it->album : file_name(it->files[0].path));

  wchar_t num[32];
  swprintf(num, 32, it->kind == MC_IMPORT_SINGLE ? L"single" : L"%d", it->file_count);
  lv_set(lv, idx, COL_FILES, num);

  char match[512] = "";
  const mc_import_match *m = NULL;
  if (it->chosen >= 0 && it->chosen < it->match_count) m = &it->matches[it->chosen];
  else if (it->state != MC_IMPORT_DONE && it->match_count) m = &it->matches[0];
  if (it->error) snprintf(match, sizeof match, "%s", it->error);
  else if (it->state == MC_IMPORT_DONE && it->chosen < 0) snprintf(match, sizeof match, "Existing tags");
  else if (m) match_summary(m, match, sizeof match);
  lv_set_utf8(lv, idx, COL_MATCH, match);

  num[0] = L'\0';
  if (m && !(it->state == MC_IMPORT_DONE && it->chosen < 0))
    swprintf(num, 32, L"%.0f%%", m->confidence * 100);
  lv_set(lv, idx, COL_CONFIDENCE, num);

  mc_import_item_free(it);
}

BOOL import_view_start(const char *const *paths, int count) {
  if (!g_view.lib) {
    MessageBoxW(g_view.parent, L"The library could not be opened, so nothing can be imported.",
                L"Import", MB_OK | MB_ICONERROR);
    return FALSE;
  }
  if (g_view.imp && !g_view.idle) {
    MessageBoxW(g_view.parent, L"An import is still running. Wait for it to finish first.",
                L"Import", MB_OK | MB_ICONINFORMATION);
    return FALSE;
  }
  close_session();
  g_view.generation++;
  g_view.idle = FALSE;

  mc_import_options opts = {.threshold = 0, .fetch_art = 1};
  mc_status st = mc_import_start(g_view.lib, paths, count, &opts, on_import_event,
                                 (void *)(uintptr_t)g_view.generation, &g_view.imp);
  if (st != MC_OK) {
    wchar_t msg[128];
    swprintf(msg, 128, L"Couldn't start the import (%hs).", mc_status_str(st));
    MessageBoxW(g_view.parent, msg, L"Import", MB_OK | MB_ICONERROR);
    g_view.imp = NULL;
    return FALSE;
  }
  update_status();
  /* The layout switches from the drop zone to the list. */
  SendMessageW(g_view.parent, WM_SIZE, 0, 0);
  return TRUE;
}

void import_view_browse(void) {
  char *path = pick_folder(g_view.parent, L"Choose a Folder to Import", NULL);
  if (!path) return;
  const char *paths[] = {path};
  import_view_start(paths, 1);
  free(path);
}

BOOL import_view_drop(HDROP drop) {
  UINT n = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
  char **paths = calloc(n ? n : 1, sizeof *paths);
  UINT count = 0;
  for (UINT i = 0; paths && i < n; i++) {
    UINT len = DragQueryFileW(drop, i, NULL, 0);
    wchar_t *w = malloc((len + 1) * sizeof *w);
    if (!w) continue;
    DragQueryFileW(drop, i, w, len + 1);
    int u = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (u > 0 && (paths[count] = malloc((size_t)u))) {
      WideCharToMultiByte(CP_UTF8, 0, w, -1, paths[count], u, NULL, NULL);
      count++;
    }
    free(w);
  }
  DragFinish(drop);
  BOOL ok = count > 0 && import_view_start((const char *const *)paths, (int)count);
  for (UINT i = 0; i < count; i++) free(paths[i]);
  free(paths);
  return ok;
}

BOOL import_view_confirm_close(void) {
  if (g_view.imp && !g_view.idle) {
    int answer = MessageBoxW(g_view.parent,
                             L"Stop the import? Anything already imported stays in your library.",
                             L"Import", MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) return FALSE;
  }
  close_session();
  return TRUE;
}

void import_view_cancel(void) {
  close_session();
}

/* --- Review dialog --- */

static int selected_match(HWND dlg) {
  return (int)SendDlgItemMessageW(dlg, IDC_REV_MATCHES, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

static void fill_tracks(HWND dlg, int match) {
  HWND lv = GetDlgItem(dlg, IDC_REV_TRACKS);
  SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
  const mc_import_item *it = g_view.review_snap;
  if (!it) return;

  wchar_t buf[64];
  int *used = calloc((size_t)it->file_count + 1, sizeof *used);
  if (match >= 0 && match < it->match_count) {
    const mc_import_match *m = &it->matches[match];
    int multi_disc = m->track_count && m->tracks[m->track_count - 1].disc > 1;
    for (int j = 0; j < m->track_count; j++) {
      const mc_import_match_track *t = &m->tracks[j];
      int row = lv_append(lv, j, 0);
      if (multi_disc) swprintf(buf, 64, L"%d-%d", t->disc, t->number);
      else swprintf(buf, 64, L"%d", t->number);
      lv_set(lv, row, 0, it->kind == MC_IMPORT_SINGLE ? L"" : buf);
      lv_set_utf8(lv, row, 1, t->title);
      format_length(t->length_ms, buf, 64);
      lv_set(lv, row, 2, buf);
      if (t->file >= 0 && t->file < it->file_count) {
        const mc_import_file *f = &it->files[t->file];
        if (used) used[t->file] = 1;
        lv_set_utf8(lv, row, 3, file_name(f->path));
        lv_set_utf8(lv, row, 4, f->title);
        format_length(f->duration_ms, buf, 64);
        lv_set(lv, row, 5, buf);
      } else {
        lv_set(lv, row, 3, L"\x2014 missing");
      }
    }
  }
  /* Files that weren't matched to any track: left untouched if this match is applied. */
  for (int i = 0; i < it->file_count; i++) {
    if (used && used[i]) continue;
    int row = lv_append(lv, -1, 0);
    lv_set(lv, row, 1, match >= 0 ? L"(not matched; left as is)" : L"");
    lv_set_utf8(lv, row, 3, file_name(it->files[i].path));
    lv_set_utf8(lv, row, 4, it->files[i].title);
    format_length(it->files[i].duration_ms, buf, 64);
    lv_set(lv, row, 5, buf);
  }
  free(used);
}

static void load_review(HWND dlg) {
  mc_import_item_free(g_view.review_snap);
  g_view.review_snap = NULL;
  if (mc_import_item_get(g_view.imp, g_view.review_item, &g_view.review_snap) != MC_OK) {
    EndDialog(dlg, IDCANCEL);
    return;
  }
  const mc_import_item *it = g_view.review_snap;
  BOOL busy = it->state == MC_IMPORT_MATCHING || it->state == MC_IMPORT_WRITING ||
              it->state == MC_IMPORT_PENDING;

  char header[1200];
  int n = snprintf(header, sizeof header, "%s: %s \xE2\x80\x93 %s",
                   it->kind == MC_IMPORT_SINGLE ? "Single" : "Album", it->artist ? it->artist : "?",
                   it->album ? it->album : file_name(it->files[0].path));
  if (it->kind == MC_IMPORT_ALBUM && n > 0 && (size_t)n < sizeof header)
    n += snprintf(header + n, sizeof header - (size_t)n, " (%d files)", it->file_count);
  if (n > 0 && (size_t)n < sizeof header)
    n += snprintf(header + n, sizeof header - (size_t)n, "\n%s", it->folder ? it->folder : "");
  if (busy && n > 0 && (size_t)n < sizeof header)
    snprintf(header + n, sizeof header - (size_t)n, "\nSearching MusicBrainz\xE2\x80\xA6");
  else if (it->error && n > 0 && (size_t)n < sizeof header)
    snprintf(header + n, sizeof header - (size_t)n, "\n%s", it->error);
  set_text_utf8(GetDlgItem(dlg, IDC_REV_HEADER), header);

  if (!g_view.review_terms_set) {
    set_text_utf8(GetDlgItem(dlg, IDC_REV_ARTIST), it->artist);
    set_text_utf8(GetDlgItem(dlg, IDC_REV_TITLE), it->album);
    SetDlgItemTextW(dlg, IDC_REV_TITLE_LABEL, it->kind == MC_IMPORT_SINGLE ? L"Title" : L"Album");
    g_view.review_terms_set = TRUE;
  }

  HWND lv = GetDlgItem(dlg, IDC_REV_MATCHES);
  SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
  wchar_t buf[64];
  for (int i = 0; i < it->match_count; i++) {
    const mc_import_match *m = &it->matches[i];
    int row = lv_append(lv, i, 0);
    swprintf(buf, 64, L"%.0f%%", m->confidence * 100);
    lv_set(lv, row, 0, buf);
    lv_set_utf8(lv, row, 1, m->album_artist);
    if (m->is_single) {
      char into[512];
      snprintf(into, sizeof into, "%s \xE2\x86\x92 %s", m->release ? m->release : "?", m->album);
      lv_set_utf8(lv, row, 2, into);
    } else {
      lv_set_utf8(lv, row, 2, m->album);
    }
    buf[0] = L'\0';
    if (m->year) swprintf(buf, 64, L"%d", m->year);
    lv_set(lv, row, 3, buf);
    swprintf(buf, 64, L"%d", m->track_count);
    lv_set(lv, row, 4, buf);
    lv_set_utf8(lv, row, 5, m->release_type);
    lv_set_utf8(lv, row, 6, m->format);
    lv_set_utf8(lv, row, 7, m->country);
    char label[256];
    snprintf(label, sizeof label, "%s%s%s", m->label ? m->label : "",
             m->label && m->catalog ? " \xC2\xB7 " : "", m->catalog ? m->catalog : "");
    lv_set_utf8(lv, row, 8, label);
  }
  int sel = it->chosen >= 0 && it->chosen < it->match_count ? it->chosen : it->match_count ? 0 : -1;
  if (sel >= 0) {
    LVITEMW li = {.stateMask = LVIS_SELECTED | LVIS_FOCUSED, .state = LVIS_SELECTED | LVIS_FOCUSED};
    SendMessageW(lv, LVM_SETITEMSTATE, (WPARAM)sel, (LPARAM)&li);
  }
  fill_tracks(dlg, sel);

  EnableWindow(GetDlgItem(dlg, IDOK), !busy && it->match_count > 0);
  EnableWindow(GetDlgItem(dlg, IDC_REV_ASIS), !busy);
  EnableWindow(GetDlgItem(dlg, IDC_REV_SKIP), !busy && it->state != MC_IMPORT_DONE);
  EnableWindow(GetDlgItem(dlg, IDC_REV_SEARCH), !busy);
}

static void init_review_columns(HWND dlg) {
  RECT rc;
  HWND lv = GetDlgItem(dlg, IDC_REV_MATCHES);
  SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  GetClientRect(lv, &rc);
  int w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
  static const struct { const wchar_t *title; int pct; int fmt; } matches[] = {
    {L"Match", 7, LVCFMT_RIGHT}, {L"Artist", 17, 0}, {L"Release", 27, 0},
    {L"Year", 6, LVCFMT_RIGHT}, {L"Tracks", 7, LVCFMT_RIGHT}, {L"Type", 8, 0},
    {L"Format", 10, 0}, {L"Country", 7, 0}, {L"Label", 11, 0}};
  for (int i = 0; i < 9; i++)
    lv_add_column(lv, i, matches[i].title, w * matches[i].pct / 100, matches[i].fmt);

  lv = GetDlgItem(dlg, IDC_REV_TRACKS);
  SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  GetClientRect(lv, &rc);
  w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
  static const struct { const wchar_t *title; int pct; int fmt; } tracks[] = {
    {L"#", 6, LVCFMT_RIGHT}, {L"Title", 30, 0}, {L"Length", 8, LVCFMT_RIGHT},
    {L"File", 22, 0}, {L"Current title", 26, 0}, {L"Length", 8, LVCFMT_RIGHT}};
  for (int i = 0; i < 6; i++)
    lv_add_column(lv, i, tracks[i].title, w * tracks[i].pct / 100, tracks[i].fmt);
}

static void review_done(HWND dlg, mc_status st) {
  if (st == MC_OK) {
    g_view.idle = FALSE; /* the import thread has work again */
    update_status();
    EndDialog(dlg, IDOK);
  } else {
    MessageBoxW(dlg, L"This item is busy. Try again in a moment.", L"Import", MB_OK | MB_ICONINFORMATION);
  }
}

static INT_PTR CALLBACK review_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG:
      g_view.review = dlg;
      g_view.review_item = (int)lp;
      g_view.review_terms_set = FALSE;
      init_review_columns(dlg);
      load_review(dlg);
      theme_apply_window(dlg, FALSE);
      theme_apply_controls(dlg);
      return TRUE;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
      return (INT_PTR)theme_ctlcolor(msg, (HDC)wp);
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
    case WM_APP_REVIEW_REFRESH:
      load_review(dlg);
      return TRUE;
    case WM_NOTIFY: {
      const NMHDR *nm = (const NMHDR *)lp;
      if (nm->idFrom != IDC_REV_MATCHES) break;
      if (nm->code == LVN_ITEMCHANGED) {
        const NMLISTVIEW *lv = (const NMLISTVIEW *)lp;
        if ((lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED))
          fill_tracks(dlg, lv->iItem);
      } else if (nm->code == NM_DBLCLK && selected_match(dlg) >= 0 &&
                 IsWindowEnabled(GetDlgItem(dlg, IDOK))) {
        review_done(dlg, mc_import_accept(g_view.imp, g_view.review_item, selected_match(dlg)));
      }
      break;
    }
    case WM_COMMAND:
      switch (LOWORD(wp)) {
        case IDOK:
          if (selected_match(dlg) >= 0)
            review_done(dlg, mc_import_accept(g_view.imp, g_view.review_item, selected_match(dlg)));
          return TRUE;
        case IDC_REV_ASIS:
          review_done(dlg, mc_import_accept_as_is(g_view.imp, g_view.review_item));
          return TRUE;
        case IDC_REV_SKIP:
          review_done(dlg, mc_import_skip(g_view.imp, g_view.review_item));
          return TRUE;
        case IDC_REV_SEARCH: {
          char *artist = get_text_utf8(GetDlgItem(dlg, IDC_REV_ARTIST));
          char *title = get_text_utf8(GetDlgItem(dlg, IDC_REV_TITLE));
          if (title && *title && mc_import_search(g_view.imp, g_view.review_item, artist, title) == MC_OK) {
            g_view.idle = FALSE;
            update_status();
          }
          free(artist);
          free(title);
          return TRUE; /* the dialog stays open and refreshes when results arrive */
        }
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;
    case WM_DESTROY:
      g_view.review = NULL;
      mc_import_item_free(g_view.review_snap);
      g_view.review_snap = NULL;
      break;
  }
  return FALSE;
}

static void open_review(int idx) {
  if (idx < 0 || idx >= g_view.item_count) return;
  mc_import_state s = g_view.states[idx];
  if (s == MC_IMPORT_PENDING || s == MC_IMPORT_MATCHING || s == MC_IMPORT_WRITING) return;
  DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_REVIEW), g_view.parent, review_proc,
                  (LPARAM)idx);
}

/* --- Events --- */

void import_view_on_event(WPARAM wp, LPARAM lp) {
  if (!g_view.imp || HIWORD(wp) != g_view.generation) return; /* from a closed session */
  int arg = (int)lp;
  switch ((mc_import_event)LOWORD(wp)) {
    case MC_IMPORT_EV_SCANNING:
      g_view.scanned_files = arg;
      break;
    case MC_IMPORT_EV_SCANNED:
      g_view.item_count = mc_import_item_count(g_view.imp);
      g_view.states = calloc((size_t)g_view.item_count + 1, sizeof *g_view.states);
      if (!g_view.states) g_view.item_count = 0;
      SendMessageW(g_view.list, WM_SETREDRAW, FALSE, 0);
      for (int i = 0; i < g_view.item_count; i++) {
        lv_append(g_view.list, i, GROUP_ACTIVE);
        refresh_row(i);
      }
      SendMessageW(g_view.list, WM_SETREDRAW, TRUE, 0);
      break;
    case MC_IMPORT_EV_ITEM_CHANGED:
      if (arg < 0 || arg >= g_view.item_count) break;
      refresh_row(arg);
      if (g_view.review && g_view.review_item == arg) SendMessageW(g_view.review, WM_APP_REVIEW_REFRESH, 0, 0);
      if (g_view.states[arg] == MC_IMPORT_DONE) PostMessageW(g_view.parent, WM_APP_LIBRARY_CHANGED, 0, 0);
      break;
    case MC_IMPORT_EV_IDLE:
      g_view.idle = TRUE;
      break;
  }
  update_status();
}

BOOL import_view_on_command(int id) {
  switch (id) {
    case ID_IMP_BROWSE: import_view_browse(); return TRUE;
    case ID_IMP_DONE: PostMessageW(g_view.parent, WM_APP_IMPORT_CLOSE, 0, 0); return TRUE;
    case ID_IMP_BACK: PostMessageW(g_view.parent, WM_APP_IMPORT_CLOSE, IMPORT_CLOSE_CANCEL, 0); return TRUE;
    default: return FALSE;
  }
}

BOOL import_view_on_notify(const NMHDR *nm) {
  if (nm->hwndFrom != g_view.list) return FALSE;
  if (nm->code == NM_CLICK || nm->code == NM_RETURN) {
    int row = nm->code == NM_CLICK ? ((const NMITEMACTIVATE *)nm)->iItem
                                   : (int)SendMessageW(g_view.list, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
    open_review(row);
    return TRUE;
  }
  return FALSE;
}

/* --- Window plumbing --- */

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, g_view.parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void add_group(int id, const wchar_t *header) {
  LVGROUP g = {.cbSize = sizeof g, .mask = LVGF_HEADER | LVGF_GROUPID | LVGF_STATE,
               .pszHeader = (wchar_t *)header, .iGroupId = id,
               .stateMask = LVGS_COLLAPSIBLE, .state = LVGS_COLLAPSIBLE};
  SendMessageW(g_view.list, LVM_INSERTGROUP, (WPARAM)-1, (LPARAM)&g);
}

void import_view_create(HWND parent, mc_library *lib) {
  g_view.parent = parent;
  g_view.lib = lib;

  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = drop_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"musicapp_drop"};
  RegisterClassExW(&wc);

  g_view.back = child(WC_BUTTONW, L"\x2190 Back", BS_PUSHBUTTON, ID_IMP_BACK);
  g_view.drop = child(L"musicapp_drop", NULL, 0, ID_IMP_DROP);
  g_view.browse = child(WC_BUTTONW, L"Choose Folder\x2026", BS_PUSHBUTTON, ID_IMP_BROWSE);
  g_view.status = child(WC_STATICW, NULL, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_IMP_STATUS);
  g_view.progress = child(PROGRESS_CLASSW, NULL, 0, ID_IMP_PROGRESS);
  g_view.done = child(WC_BUTTONW, L"Done", BS_PUSHBUTTON, ID_IMP_DONE);
  g_view.list = child(WC_LISTVIEWW, NULL, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                      ID_IMP_LIST);
  SendMessageW(g_view.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
               LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

  static const wchar_t *const titles[NUM_COLS] = {L"Status", L"Artist", L"Album / Title",
                                                  L"Files", L"Match", L"Confidence"};
  for (int i = 0; i < NUM_COLS; i++)
    lv_add_column(g_view.list, i, titles[i], 100, i == COL_FILES || i == COL_CONFIDENCE ? LVCFMT_RIGHT : 0);

  SendMessageW(g_view.list, LVM_ENABLEGROUPVIEW, TRUE, 0);
  add_group(GROUP_REVIEW, L"Needs your attention");
  add_group(GROUP_ACTIVE, L"In progress");
  add_group(GROUP_DONE, L"Imported");
  add_group(GROUP_SKIPPED, L"Skipped");
  add_group(GROUP_FAILED, L"Failed");
}

void import_view_destroy(void) {
  close_session();
}

void import_view_set_library(mc_library *lib) {
  close_session();
  g_view.lib = lib;
}

void import_view_set_font(HFONT font) {
  g_view.font = font;
  HWND controls[] = {g_view.back, g_view.browse, g_view.status, g_view.done, g_view.list};
  for (size_t i = 0; i < sizeof controls / sizeof controls[0]; i++)
    SendMessageW(controls[i], WM_SETFONT, (WPARAM)font, TRUE);
  InvalidateRect(g_view.drop, NULL, TRUE);
}

void import_view_show(BOOL show) {
  g_view.shown = show;
  if (!show) {
    HWND all[] = {g_view.back, g_view.drop, g_view.browse, g_view.status, g_view.progress,
                  g_view.done, g_view.list};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(all[i], SW_HIDE);
  }
}

void import_view_layout(int x, int y, int w, int h, UINT dpi) {
  if (!g_view.shown) return;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int pad = S(16);
  BOOL session = g_view.imp != NULL;

  /* Back (drop zone only) returns to the library; Done does that once an import started. */
  int btn_w = S(88), btn_h = S(28);
  ShowWindow(g_view.back, session ? SW_HIDE : SW_SHOW);
  MoveWindow(g_view.back, x + pad, y + pad, btn_w, btn_h, TRUE);

  ShowWindow(g_view.drop, session ? SW_HIDE : SW_SHOW);
  ShowWindow(g_view.browse, session ? SW_HIDE : SW_SHOW);
  ShowWindow(g_view.status, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.progress, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.done, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.list, session ? SW_SHOW : SW_HIDE);

  if (!session) {
    int dz_w = min(S(520), w - 2 * pad), dz_h = S(200), browse_w = S(150), browse_h = S(30);
    int top = y + pad + btn_h + pad; /* below the Back button */
    int dz_x = x + (w - dz_w) / 2, dz_y = max(top, y + (h - dz_h - browse_h - S(16)) / 2);
    MoveWindow(g_view.drop, dz_x, dz_y, dz_w, dz_h, TRUE);
    MoveWindow(g_view.browse, x + (w - browse_w) / 2, dz_y + dz_h + S(16), browse_w, browse_h, TRUE);
    return;
  }

  MoveWindow(g_view.done, x + w - pad - btn_w, y + pad, btn_w, btn_h, TRUE);
  MoveWindow(g_view.status, x + pad, y + pad + (btn_h - S(18)) / 2, w - 3 * pad - btn_w, S(18), TRUE);
  MoveWindow(g_view.progress, x + pad, y + pad + btn_h + S(8), w - 2 * pad, S(6), TRUE);
  int list_y = y + pad + btn_h + S(22);
  MoveWindow(g_view.list, x + pad, list_y, w - 2 * pad, y + h - pad - list_y, TRUE);

  /* Fixed columns, with Match taking whatever is left. */
  static const int widths[NUM_COLS] = {110, 170, 220, 60, 0, 90};
  int lw = w - 2 * pad - GetSystemMetrics(SM_CXVSCROLL) - S(4), used = 0;
  for (int i = 0; i < NUM_COLS; i++)
    if (i != COL_MATCH) {
      SendMessageW(g_view.list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(widths[i]));
      used += S(widths[i]);
    }
  SendMessageW(g_view.list, LVM_SETCOLUMNWIDTH, COL_MATCH, max(S(120), lw - used));
#undef S
}
