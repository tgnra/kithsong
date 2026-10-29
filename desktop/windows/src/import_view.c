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
#include "wstr.h"

enum {
  ID_IMP_DROP = 200,
  ID_IMP_BROWSE,
  ID_IMP_STATUS,
  ID_IMP_PROGRESS,
  ID_IMP_LIST,
  ID_IMP_DONE,
  ID_IMP_BANNER,
  ID_IMP_SKIP_TAGGING,
};

enum { GROUP_REVIEW = 1, GROUP_ACTIVE, GROUP_DONE, GROUP_SKIPPED, GROUP_FAILED };

enum { COL_STATUS, COL_ARTIST, COL_ALBUM, COL_FILES, COL_MATCH, COL_CONFIDENCE, NUM_COLS };

/* Sent to the review dialog when its item changed. */
#define WM_APP_REVIEW_REFRESH (WM_APP + 10)
/* Sent to the review dialog to open the files menu of the missing track at list index wp. */
#define WM_APP_FILES_MENU (WM_APP + 11)

typedef struct view {
  HWND parent;
  HWND drop, browse, status, progress, list, done;
  HWND banner, skip_tagging; /* the offline banner, and its button (a child of it) */
  HFONT font;
  UINT dpi;
  BOOL shown;
  mc_library *lib;

  mc_import *imp;
  WORD generation; /* tags events so ones from a closed session are ignored */
  BOOL idle;
  int scanned_files;
  int item_count;
  mc_import_state *states;
  BOOL offline;      /* MusicBrainz can't be reached: show the banner */
  BOOL skipped_tags; /* the banner's button was used; it stays hidden from then on */
  BOOL long_paths_told; /* the session's long-path hint was shown */

  HWND review; /* open review dialog, if any */
  int review_item;
  mc_import_item *review_snap;
  BOOL review_terms_set;
  /* The review's track list, by row (each row's lParam is its index here). */
  struct review_row *rows;
  int row_count;
  int rows_match; /* the match the rows show; -1 if none */
  int rows_gen;   /* bumped whenever the rows are rebuilt, e.g. while a menu is open */
  BOOL can_assign; /* the rows have missing tracks, and files Use as Track can make them */
} view;

/* A row of the review's track list: a track of the match, with the file matched to it if
 * any, or a file no track has. A file is item from_item's file `file`: this item's own, or
 * one of a related item (mc_import_related) that Use as Track can bring in. */
typedef struct review_row {
  int track; /* index into the match's tracks, or -1 for a file row */
  int from_item;
  int file;
} review_row;

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
    case MC_IMPORT_PENDING: return mc_wstr(MC_STR_IMPORT_STATE_WAITING);
    case MC_IMPORT_MATCHING: return mc_wstr(MC_STR_IMPORT_STATE_MATCHING);
    case MC_IMPORT_REVIEW: return mc_wstr(MC_STR_IMPORT_NEEDS_DECISION);
    case MC_IMPORT_WRITING: return mc_wstr(MC_STR_IMPORT_STATE_WRITING);
    case MC_IMPORT_DONE: return mc_wstr(MC_STR_IMPORT_STATE_DONE);
    case MC_IMPORT_SKIPPED: return mc_wstr(MC_STR_IMPORT_STATE_SKIPPED);
    case MC_IMPORT_FAILED: return mc_wstr(MC_STR_IMPORT_STATE_FAILED);
  }
  return L"";
}

/* The status column: what writing the item did about duplicates, else its state. */
static const wchar_t *item_label(const mc_import_item *it) {
  if (it->state == MC_IMPORT_DONE || it->state == MC_IMPORT_SKIPPED) {
    switch (it->outcome) {
      case MC_IMPORT_OUTCOME_REPLACED: return mc_wstr(MC_STR_IMPORT_STATE_REPLACED);
      case MC_IMPORT_OUTCOME_ADDED: return mc_wstr(MC_STR_IMPORT_STATE_ADDED_MISSING);
      case MC_IMPORT_OUTCOME_DUPLICATE: return mc_wstr(MC_STR_IMPORT_STATE_DUPLICATE);
      case MC_IMPORT_OUTCOME_MOVED: return mc_wstr(MC_STR_IMPORT_STATE_MOVED);
      case MC_IMPORT_OUTCOME_NONE: break;
    }
  }
  return state_label(it->state);
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
  const char *artist = m->album_artist ? m->album_artist : mc_str(MC_STR_UNKNOWN_VALUE);
  const char *title = m->is_single && m->track_count ? m->tracks[0].title : m->album;
  if (!title) title = mc_str(MC_STR_UNKNOWN_VALUE);
  if (m->year) mc_format(out, n, mc_str(MC_STR_IMPORT_MATCH_YEAR), MC_S(artist), MC_S(title), MC_D(m->year));
  else mc_format(out, n, mc_str(MC_STR_TRACK_BYLINE), MC_S(artist), MC_S(title));
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
    DrawTextW(dc, mc_wstr(MC_STR_WINDOWS_IMPORT_DROP_HERE), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
    EndPaint(wnd, &ps);
    return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* --- Offline banner --- */

/* c over the content background at alpha 0..255. */
static COLORREF tint(COLORREF c, int alpha) {
  COLORREF b = theme.bg;
#define MIX(f) ((f(c) * alpha + f(b) * (255 - alpha)) / 255)
  return RGB(MIX(GetRValue), MIX(GetGValue), MIX(GetBValue));
#undef MIX
}

static LRESULT CALLBACK banner_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      RECT rc;
      GetClientRect(wnd, &rc);
      FillRect(dc, &rc, theme.bg_brush);
      int r = MulDiv(8, (int)g_view.dpi, USER_DEFAULT_SCREEN_DPI);
      HBRUSH fill = CreateSolidBrush(tint(theme.accent, theme.dark ? 60 : 36));
      HPEN pen = CreatePen(PS_SOLID, 1, tint(theme.accent, 110));
      HGDIOBJ old_brush = SelectObject(dc, fill), old_pen = SelectObject(dc, pen);
      RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, r, r);
      SelectObject(dc, old_brush);
      SelectObject(dc, old_pen);
      DeleteObject(fill);
      DeleteObject(pen);

      /* Info icon, then the text up to the button. */
      int pad = MulDiv(12, (int)g_view.dpi, USER_DEFAULT_SCREEN_DPI);
      int icon = MulDiv(16, (int)g_view.dpi, USER_DEFAULT_SCREEN_DPI);
      HFONT icon_font = CreateFontW(-icon, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                    CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, theme.text);
      HGDIOBJ old_font = SelectObject(dc, icon_font);
      RECT ir = {rc.left + pad, rc.top, rc.left + pad + icon, rc.bottom};
      DrawTextW(dc, L"\xE946", 1, &ir, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      SelectObject(dc, g_view.font ? (HGDIOBJ)g_view.font : GetStockObject(DEFAULT_GUI_FONT));
      RECT btn = {0};
      if (IsWindowVisible(g_view.skip_tagging)) {
        GetWindowRect(g_view.skip_tagging, &btn);
        MapWindowPoints(NULL, wnd, (POINT *)&btn, 2);
      }
      RECT tr = {ir.right + pad / 2 + pad / 4, rc.top, btn.left ? btn.left - pad : rc.right - pad, rc.bottom};
      DrawTextW(dc, mc_wstr(MC_STR_IMPORT_OFFLINE), -1, &tr,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      SelectObject(dc, old_font);
      DeleteObject(icon_font);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_COMMAND: /* the button: the main window routes it back to import_view_on_command */
      return SendMessageW(g_view.parent, msg, wp, lp);
    case WM_CTLCOLORBTN: return (LRESULT)theme_ctlcolor(msg, (HDC)wp);
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static void skip_tagging(void) {
  if (!g_view.imp || mc_import_skip_tagging(g_view.imp) != MC_OK) return;
  g_view.skipped_tags = TRUE;
  SendMessageW(g_view.parent, WM_APP_IMPORT_SESSION, TRUE, 0); /* relayout without the banner */
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
  g_view.offline = FALSE;
  g_view.skipped_tags = FALSE;
  g_view.long_paths_told = FALSE;
  SendMessageW(g_view.list, LVM_DELETEALLITEMS, 0, 0);
  SendMessageW(g_view.parent, WM_APP_IMPORT_SESSION, FALSE, 0);
}

/* "12 imported" and the like: the plural id with its count, into buf. */
static const char *count_text(char *buf, size_t n, enum mc_str_id id, int count) {
  mc_format(buf, n, mc_str_plural(id, count), MC_D(count));
  return buf;
}

static void update_status(void) {
  wchar_t text[512];
  int counts[MC_IMPORT_FAILED + 1] = {0};
  for (int i = 0; i < g_view.item_count; i++) counts[g_view.states[i]]++;
  int settled = counts[MC_IMPORT_REVIEW] + counts[MC_IMPORT_DONE] + counts[MC_IMPORT_SKIPPED] +
                counts[MC_IMPORT_FAILED];

  char done[64], review[64], skipped[64], failed[64];
  count_text(done, sizeof done, MC_STR_IMPORT_IMPORTED, counts[MC_IMPORT_DONE]);
  count_text(review, sizeof review, MC_STR_IMPORT_NEED_DECISION, counts[MC_IMPORT_REVIEW]);
  if (!g_view.states) {
    mc_wformat(text, 512, mc_str_plural(MC_STR_IMPORT_SCANNING_FILES, g_view.scanned_files),
               MC_D(g_view.scanned_files));
  } else if (g_view.item_count == 0) {
    wstr_from_utf8(text, 512, mc_str(MC_STR_IMPORT_NO_MUSIC));
  } else if (!g_view.idle) {
    /* "Importing 3 of 12   2 imported, 1 needs a decision" */
    char progress[128];
    mc_format(progress, sizeof progress, mc_str(MC_STR_IMPORT_MATCHING),
              MC_D(settled < g_view.item_count ? settled + 1 : settled), MC_D(g_view.item_count));
    mc_wformat(text, 512, "{0}   {1}{2}{3}", MC_S(progress), MC_S(done), MC_S(mc_str(MC_STR_IMPORT_LIST_SEPARATOR)),
               MC_S(review));
  } else if (counts[MC_IMPORT_REVIEW]) {
    mc_wformat(text, 512, mc_str(MC_STR_IMPORT_STATUS_REVIEW), MC_S(done), MC_S(review));
  } else {
    count_text(skipped, sizeof skipped, MC_STR_IMPORT_SKIPPED, counts[MC_IMPORT_SKIPPED]);
    count_text(failed, sizeof failed, MC_STR_IMPORT_FAILED, counts[MC_IMPORT_FAILED]);
    mc_wformat(text, 512, mc_str(MC_STR_IMPORT_STATUS_FINISHED), MC_S(done), MC_S(skipped), MC_S(failed));
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
  lv_set(lv, idx, COL_STATUS, item_label(it));
  lv_set_utf8(lv, idx, COL_ARTIST, it->artist);
  lv_set_utf8(lv, idx, COL_ALBUM, it->album ? it->album : it->file_count ? file_name(it->files[0].path) : "");

  wchar_t num[64];
  if (it->kind == MC_IMPORT_SINGLE) wstr_from_utf8(num, 64, mc_str(MC_STR_IMPORT_SINGLE));
  else swprintf(num, 64, L"%d", it->file_count);
  lv_set(lv, idx, COL_FILES, num);

  char match[512] = "";
  const mc_import_match *m = NULL;
  if (it->chosen >= 0 && it->chosen < it->match_count) m = &it->matches[it->chosen];
  else if (it->state != MC_IMPORT_DONE && it->match_count) m = &it->matches[0];
  if (it->outcome == MC_IMPORT_OUTCOME_MOVED) m = NULL; /* none of it is imported with a match */
  if (it->error) snprintf(match, sizeof match, "%s", it->error);
  else if (it->state == MC_IMPORT_DONE && it->chosen < 0)
    snprintf(match, sizeof match, "%s", mc_str(MC_STR_IMPORT_EXISTING_TAGS));
  else if (m) match_summary(m, match, sizeof match);
  lv_set_utf8(lv, idx, COL_MATCH, match);

  num[0] = L'\0';
  if (m && !(it->state == MC_IMPORT_DONE && it->chosen < 0))
    swprintf(num, 64, L"%.0f%%", m->confidence * 100);
  lv_set(lv, idx, COL_CONFIDENCE, num);

  mc_import_item_free(it);
}

BOOL import_view_start(const char *const *paths, int count) {
  if (!g_view.lib) {
    MessageBoxW(g_view.parent, mc_wstr(MC_STR_IMPORT_NO_LIBRARY_TEXT), mc_wstr(MC_STR_IMPORT_MUSIC),
                MB_OK | MB_ICONERROR);
    return FALSE;
  }
  if (g_view.imp && !g_view.idle) {
    wchar_t text[512];
    swprintf(text, 512, L"%ls\n\n%ls", mc_wstr(MC_STR_IMPORT_RUNNING_TITLE), mc_wstr(MC_STR_IMPORT_RUNNING_TEXT));
    MessageBoxW(g_view.parent, text, mc_wstr(MC_STR_IMPORT_MUSIC), MB_OK | MB_ICONINFORMATION);
    return FALSE;
  }
  close_session();
  g_view.generation++;
  g_view.idle = FALSE;

  mc_import_options opts = {.threshold = 0, .fetch_art = 1};
  mc_status st = mc_import_start(g_view.lib, paths, count, &opts, on_import_event,
                                 (void *)(uintptr_t)g_view.generation, &g_view.imp);
  if (st != MC_OK) {
    wchar_t msg[256];
    mc_wformat(msg, 256, mc_str(MC_STR_IMPORT_START_FAILED), MC_S(mc_status_str(st)));
    MessageBoxW(g_view.parent, msg, mc_wstr(MC_STR_IMPORT_MUSIC), MB_OK | MB_ICONERROR);
    g_view.imp = NULL;
    return FALSE;
  }
  update_status();
  SendMessageW(g_view.parent, WM_APP_IMPORT_SESSION, TRUE, 0); /* and the layout switches to the list */
  return TRUE;
}

void import_view_browse(void) {
  int n = 0;
  char **paths = pick_folders(g_view.parent, mc_wstr(MC_STR_IMPORT_CHOOSE_FOLDERS_TITLE), &n);
  if (!paths) return;
  import_view_start((const char *const *)paths, n);
  for (int i = 0; i < n; i++) free(paths[i]);
  free(paths);
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
    wchar_t text[512];
    swprintf(text, 512, L"%ls %ls", mc_wstr(MC_STR_IMPORT_STOP_TITLE), mc_wstr(MC_STR_IMPORT_STOP_TEXT));
    int answer = MessageBoxW(g_view.parent, text, mc_wstr(MC_STR_IMPORT_MUSIC), MB_YESNO | MB_ICONQUESTION);
    if (answer != IDYES) return FALSE;
  }
  close_session();
  return TRUE;
}

/* --- Review dialog --- */

static int selected_match(HWND dlg) {
  return (int)SendDlgItemMessageW(dlg, IDC_REV_MATCHES, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

/* A track's number as the list shows it: "3", or "2-3" on a multi-disc release. */
static void track_number(const mc_import_match *m, const mc_import_match_track *t, wchar_t *out, size_t n) {
  if (m->track_count && m->tracks[m->track_count - 1].disc > 1) swprintf(out, n, L"%d-%d", t->disc, t->number);
  else swprintf(out, n, L"%d", t->number);
}

/* Records what a track list row is; returns its lParam. */
static LPARAM add_row(int track, int from_item, int file) {
  review_row *grown = realloc(g_view.rows, (size_t)(g_view.row_count + 1) * sizeof *grown);
  if (!grown) return -1;
  g_view.rows = grown;
  g_view.rows[g_view.row_count] = (review_row){track, from_item, file};
  return g_view.row_count++;
}

static void file_columns(HWND lv, int row, const mc_import_file *f) {
  wchar_t buf[64];
  lv_set_utf8(lv, row, 3, file_name(f->path));
  lv_set_utf8(lv, row, 4, f->title);
  format_length(f->duration_ms, buf, 64);
  lv_set(lv, row, 5, buf);
}

/* Files of other items from the same folder (mc_import_related), which Use as Track can make
 * tracks of this match. Returns how many were listed. */
static int fill_related(HWND lv) {
  int related[64];
  int n = mc_import_related(g_view.imp, g_view.review_item, related, 64), listed = 0;
  for (int k = 0; k < n && k < 64; k++) {
    int idx = related[k];
    mc_import_state s = idx < g_view.item_count ? g_view.states[idx] : MC_IMPORT_PENDING;
    if (s == MC_IMPORT_PENDING || s == MC_IMPORT_MATCHING || s == MC_IMPORT_WRITING) continue;
    mc_import_item *other;
    if (mc_import_item_get(g_view.imp, idx, &other) != MC_OK) continue;
    char label[512];
    const char *name = other->album ? other->album : mc_str(MC_STR_UNKNOWN_VALUE);
    mc_format(label, sizeof label, mc_str(MC_STR_IMPORT_OTHER_ITEM_FILE), MC_S(name));
    for (int i = 0; i < other->file_count; i++) {
      int row = lv_append(lv, add_row(-1, idx, i), 0);
      lv_set_utf8(lv, row, 1, label);
      file_columns(lv, row, &other->files[i]);
      listed++;
    }
    mc_import_item_free(other);
  }
  return listed;
}

/* The track list's row (lParam) at list index i, or NULL. */
static const review_row *row_at(HWND lv, int i) {
  LVITEMW li = {.mask = LVIF_PARAM, .iItem = i};
  if (!SendMessageW(lv, LVM_GETITEMW, 0, (LPARAM)&li) || li.lParam < 0 || li.lParam >= g_view.row_count)
    return NULL;
  return &g_view.rows[li.lParam];
}

static void fill_tracks(HWND dlg, int match) {
  HWND lv = GetDlgItem(dlg, IDC_REV_TRACKS);
  SendMessageW(lv, LVM_DELETEALLITEMS, 0, 0);
  g_view.row_count = 0;
  g_view.rows_match = -1;
  g_view.rows_gen++;
  g_view.can_assign = FALSE;
  SetDlgItemTextW(dlg, IDC_REV_TRACKS_HINT, L"");
  const mc_import_item *it = g_view.review_snap;
  if (!it) return;

  wchar_t buf[64];
  int *used = calloc((size_t)it->file_count + 1, sizeof *used);
  int missing = 0, loose = 0;
  if (match >= 0 && match < it->match_count) {
    const mc_import_match *m = &it->matches[match];
    g_view.rows_match = match;
    for (int j = 0; j < m->track_count; j++) {
      const mc_import_match_track *t = &m->tracks[j];
      int row = lv_append(lv, add_row(j, -1, -1), 0);
      track_number(m, t, buf, 64);
      lv_set(lv, row, 0, it->kind == MC_IMPORT_SINGLE ? L"" : buf);
      lv_set_utf8(lv, row, 1, t->title);
      format_length(t->length_ms, buf, 64);
      lv_set(lv, row, 2, buf);
      if (t->file >= 0 && t->file < it->file_count) {
        if (used) used[t->file] = 1;
        file_columns(lv, row, &it->files[t->file]);
      } else {
        lv_set(lv, row, 3, mc_wstr(MC_STR_IMPORT_TRACK_MISSING));
        missing++;
      }
    }
  }
  /* Files that weren't matched to any track: left untouched if this match is applied. */
  for (int i = 0; i < it->file_count; i++) {
    if (used && used[i]) continue;
    int row = lv_append(lv, add_row(-1, g_view.review_item, i), 0);
    lv_set(lv, row, 1, match >= 0 ? mc_wstr(MC_STR_IMPORT_NOT_MATCHED) : L"");
    file_columns(lv, row, &it->files[i]);
    loose++;
  }
  free(used);
  /* With tracks missing, files from the same folder that ended up elsewhere may be them. */
  if (missing && it->kind == MC_IMPORT_ALBUM) loose += fill_related(lv);
  if (!missing || !loose || it->kind != MC_IMPORT_ALBUM) return;

  /* The missing tracks' File cells read as a dropdown: clicking the row lists the files. */
  g_view.can_assign = TRUE;
  SetDlgItemTextW(dlg, IDC_REV_TRACKS_HINT, mc_wstr(MC_STR_IMPORT_USE_AS_TRACK_HINT));
  wchar_t choose[128];
  swprintf(choose, 128, L"%ls \x25BE", mc_wstr(MC_STR_IMPORT_CHOOSE_FILE));
  const mc_import_match *m = &it->matches[match];
  int count = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0);
  for (int i = 0; i < count; i++) {
    const review_row *r = row_at(lv, i);
    if (r && r->track >= 0 && r->track < m->track_count && m->tracks[r->track].file < 0) lv_set(lv, i, 3, choose);
  }
}

/* The row at list index i if it's a missing track that Use as Track can fill, else NULL. */
static const review_row *missing_row(HWND lv, int i) {
  const mc_import_item *it = g_view.review_snap;
  if (!g_view.can_assign || !it || g_view.rows_match < 0 || g_view.rows_match >= it->match_count) return NULL;
  const review_row *r = i >= 0 ? row_at(lv, i) : NULL;
  const mc_import_match *m = &it->matches[g_view.rows_match];
  return r && r->track >= 0 && r->track < m->track_count && m->tracks[r->track].file < 0 ? r : NULL;
}

static BOOL review_busy(void) {
  const mc_import_item *it = g_view.review_snap;
  return !it || it->state == MC_IMPORT_PENDING || it->state == MC_IMPORT_MATCHING || it->state == MC_IMPORT_WRITING;
}

/* "Use as track 3", for track `track` of the match the rows show, into text. */
static void use_as_track_text(int track, wchar_t *text, size_t n) {
  const mc_import_match *m = &g_view.review_snap->matches[g_view.rows_match];
  wchar_t number[32];
  track_number(m, &m->tracks[track], number, 32);
  char num8[64];
  WideCharToMultiByte(CP_UTF8, 0, number, -1, num8, sizeof num8, NULL, NULL);
  mc_wformat(text, n, mc_str(MC_STR_IMPORT_USE_AS_TRACK), MC_S(num8));
}

/* Makes the file row's file the missing track row's track. */
static void assign(HWND dlg, const review_row *track, const review_row *file) {
  if (mc_import_assign(g_view.imp, g_view.review_item, g_view.rows_match, track->track, file->from_item,
                       file->file) == MC_OK) {
    g_view.idle = FALSE; /* the import thread writes the item again; the dialog stays open */
    update_status();
  } else {
    MessageBoxW(dlg, mc_wstr(MC_STR_IMPORT_ITEM_BUSY), mc_wstr(MC_STR_IMPORT_MUSIC), MB_OK | MB_ICONINFORMATION);
  }
}

/* A list cell's text into out, with '&' doubled for a menu item. */
static void menu_cell(HWND lv, int row, int col, wchar_t *out, size_t n) {
  wchar_t text[MAX_PATH];
  LVITEMW li = {.iSubItem = col, .pszText = text, .cchTextMax = MAX_PATH};
  text[0] = L'\0';
  SendMessageW(lv, LVM_GETITEMTEXTW, (WPARAM)row, (LPARAM)&li);
  size_t o = 0;
  for (const wchar_t *p = text; *p && o + 2 < n; p++) {
    if (*p == L'&') out[o++] = L'&';
    out[o++] = *p;
  }
  out[o] = L'\0';
}

/* The menu of files to use as the missing track at list index `index`: this item's files no
 * track took, then each related item's under its name. pt is in screen coordinates, or
 * (-1, -1) to drop it down from the track's File cell (a click, Enter, the Menu key). */
static void files_menu(HWND dlg, int index, POINT pt) {
  HWND lv = GetDlgItem(dlg, IDC_REV_TRACKS);
  const review_row *track = missing_row(lv, index);
  if (!track) return;
  int gen = g_view.rows_gen;
  UINT item_flags = MF_STRING | (review_busy() ? MF_GRAYED : 0);

  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  /* No "Use as track N" heading: the arrow keys would stop on it, as on any disabled item.
   * The menu drops down from the track's own row. */
  wchar_t text[MAX_PATH * 2 + 64], cell[MAX_PATH * 2];
  int count = (int)SendMessageW(lv, LVM_GETITEMCOUNT, 0, 0), from = g_view.review_item;
  for (int i = 0; i < count; i++) {
    const review_row *r = row_at(lv, i);
    if (!r || r->track >= 0) continue;
    if (r->from_item != from) { /* a related item's files, under "Grouped separately: <album>" */
      from = r->from_item;
      if (GetMenuItemCount(menu) > 0) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
      menu_cell(lv, i, 1, cell, sizeof cell / sizeof *cell);
      AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, cell);
    }
    menu_cell(lv, i, 3, cell, sizeof cell / sizeof *cell);
    wchar_t len[32];
    LVITEMW li = {.iSubItem = 5, .pszText = len, .cchTextMax = 32};
    len[0] = L'\0';
    SendMessageW(lv, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)&li);
    if (len[0]) swprintf(text, sizeof text / sizeof *text, L"%ls\t%ls", cell, len);
    else swprintf(text, sizeof text / sizeof *text, L"%ls", cell);
    AppendMenuW(menu, item_flags, (UINT_PTR)i + 1, text);
  }

  TPMPARAMS tpm = {.cbSize = sizeof tpm};
  if (pt.x == -1 && pt.y == -1) {
    RECT rc = {.top = 3, .left = LVIR_BOUNDS};
    if (!SendMessageW(lv, LVM_GETSUBITEMRECT, (WPARAM)index, (LPARAM)&rc)) GetClientRect(lv, &rc);
    MapWindowPoints(lv, NULL, (POINT *)&rc, 2);
    tpm.rcExclude = rc; /* so it opens above the cell when there's no room below */
    pt.x = rc.left;
    pt.y = rc.bottom;
  }
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_VERTICAL |
               (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN);
  int cmd = (int)TrackPopupMenuEx(menu, flags, pt.x, pt.y, dlg, &tpm);
  DestroyMenu(menu);
  /* The menu's loop may have let a refresh rebuild the rows; then the choice means nothing. */
  if (cmd <= 0 || gen != g_view.rows_gen || review_busy()) return;
  const review_row *file = row_at(lv, cmd - 1);
  track = missing_row(lv, index);
  if (track && file && file->track < 0) assign(dlg, track, file);
}

/* The track list's menu. On a missing track alone, the files it can take (files_menu); on a
 * missing track selected with one file (Ctrl+click), Use as Track N. pt is in screen
 * coordinates, or (-1, -1) from the keyboard. */
static void tracks_menu(HWND dlg, POINT pt) {
  const mc_import_item *it = g_view.review_snap;
  int match = g_view.rows_match;
  if (!it || match < 0 || match >= it->match_count || it->kind != MC_IMPORT_ALBUM) return;
  const mc_import_match *m = &it->matches[match];
  HWND lv = GetDlgItem(dlg, IDC_REV_TRACKS);

  const review_row *track = NULL, *file = NULL;
  int selected = 0, tracks = 0, files = 0, track_index = -1;
  for (int i = -1; (i = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)i, LVNI_SELECTED)) >= 0;) {
    const review_row *r = row_at(lv, i);
    selected++;
    if (!r) continue;
    if (r->track >= 0) {
      tracks++;
      if (r->track < m->track_count && m->tracks[r->track].file < 0) {
        track = r;
        track_index = i;
      }
    } else {
      files++;
      file = r;
    }
  }
  if (!track || tracks != 1) return; /* only for a missing track */
  if (selected == 1) {
    files_menu(dlg, track_index, pt);
    return;
  }

  BOOL ok = !review_busy() && selected == 2 && files == 1;
  wchar_t text[128];
  use_as_track_text(track->track, text, 128);

  if (pt.x == -1 && pt.y == -1) {
    RECT rc = {.left = LVIR_LABEL};
    int focus = (int)SendMessageW(lv, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
    if (focus < 0 || !SendMessageW(lv, LVM_GETITEMRECT, (WPARAM)focus, (LPARAM)&rc)) GetClientRect(lv, &rc);
    pt.x = rc.left;
    pt.y = rc.bottom;
    ClientToScreen(lv, &pt);
  }
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING | (ok ? 0 : MF_GRAYED), 1, text);
  int gen = g_view.rows_gen;
  review_row t = *track, f = file ? *file : (review_row){-1, -1, -1};
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
  int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, dlg, NULL);
  DestroyMenu(menu);
  /* As in files_menu: only if the rows weren't rebuilt meanwhile. */
  if (cmd == 1 && ok && gen == g_view.rows_gen && !review_busy()) assign(dlg, &t, &f);
}

/* The track list: Enter on a missing track opens its files menu instead of pressing the
 * dialog's default button. */
static LRESULT CALLBACK tracks_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_GETDLGCODE: {
      const MSG *m = (const MSG *)lp;
      LRESULT code = DefSubclassProc(wnd, msg, wp, lp);
      if (m && (m->message == WM_KEYDOWN || m->message == WM_CHAR) && m->wParam == VK_RETURN &&
          missing_row(wnd, (int)SendMessageW(wnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED)))
        code |= DLGC_WANTMESSAGE;
      return code;
    }
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        int focus = (int)SendMessageW(wnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED);
        if (missing_row(wnd, focus)) {
          /* Posted, so the key's WM_CHAR, already queued, is handled first: inside the
           * menu's loop it would close the menu again. */
          PostMessageW(GetParent(wnd), WM_APP_FILES_MENU, (WPARAM)focus, 0);
          return 0;
        }
      }
      break;
    case WM_CHAR:
      if (wp == L'\r' && missing_row(wnd, (int)SendMessageW(wnd, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_FOCUSED)))
        return 0; /* the Enter above, without a beep */
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, tracks_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

static void load_review(HWND dlg) {
  mc_import_item_free(g_view.review_snap);
  g_view.review_snap = NULL;
  if (mc_import_item_get(g_view.imp, g_view.review_item, &g_view.review_snap) != MC_OK ||
      g_view.review_snap->file_count == 0) { /* its files went to another item */
    EndDialog(dlg, IDCANCEL);
    return;
  }
  const mc_import_item *it = g_view.review_snap;
  BOOL busy = it->state == MC_IMPORT_MATCHING || it->state == MC_IMPORT_WRITING ||
              it->state == MC_IMPORT_PENDING;

  char header[1200];
  const char *artist = it->artist ? it->artist : mc_str(MC_STR_UNKNOWN_VALUE);
  const char *album = it->album ? it->album : it->file_count ? file_name(it->files[0].path) : "";
  size_t n = it->kind == MC_IMPORT_SINGLE
               ? mc_format(header, sizeof header, mc_str(MC_STR_IMPORT_REVIEW_SINGLE), MC_S(artist), MC_S(album))
               : mc_format(header, sizeof header, mc_str_plural(MC_STR_IMPORT_REVIEW_ALBUM, it->file_count),
                           MC_S(artist), MC_S(album), MC_D(it->file_count));
  if (n < sizeof header)
    n += (size_t)snprintf(header + n, sizeof header - n, "\n%s", it->folder ? it->folder : "");
  if (busy && n < sizeof header)
    snprintf(header + n, sizeof header - n, "\n%s", mc_str(MC_STR_IMPORT_SEARCHING));
  else if (it->error && n < sizeof header)
    snprintf(header + n, sizeof header - n, "\n%s", it->error);
  set_text_utf8(GetDlgItem(dlg, IDC_REV_HEADER), header);

  if (!g_view.review_terms_set) {
    set_text_utf8(GetDlgItem(dlg, IDC_REV_ARTIST), it->artist);
    set_text_utf8(GetDlgItem(dlg, IDC_REV_TITLE), it->album);
    SetDlgItemTextW(dlg, IDC_REV_TITLE_LABEL,
                    mc_wstr_mnemonic(it->kind == MC_IMPORT_SINGLE ? MC_STR_TITLE : MC_STR_ALBUM));
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
      mc_format(into, sizeof into, mc_str(MC_STR_IMPORT_MATCH_INTO),
                MC_S(m->release ? m->release : mc_str(MC_STR_UNKNOWN_VALUE)), MC_S(m->album));
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
  /* Only albums have the clues; once it has run, a manual search brings it back. */
  ShowWindow(GetDlgItem(dlg, IDC_REV_WIDER), it->kind == MC_IMPORT_ALBUM ? SW_SHOW : SW_HIDE);
  EnableWindow(GetDlgItem(dlg, IDC_REV_WIDER), !busy && !it->searched_wider && !g_view.offline);
}

/* The dialog's fixed text; the template has none (app.rc). */
static void init_review_text(HWND dlg) {
  SetWindowTextW(dlg, mc_wstr(MC_STR_IMPORT_REVIEW_TITLE));
  SetDlgItemTextW(dlg, IDC_REV_MATCHES_LABEL, mc_wstr_mnemonic(MC_STR_IMPORT_MATCHES));
  SetDlgItemTextW(dlg, IDC_REV_TRACKS_LABEL, mc_wstr_mnemonic(MC_STR_TRACKS_TITLE));
  wchar_t label[128];
  SetDlgItemTextW(dlg, IDC_REV_SEARCH_LABEL, wstr_field_label(label, 128, MC_STR_IMPORT_SEARCH_AGAIN));
  SetDlgItemTextW(dlg, IDC_REV_ARTIST_LABEL, mc_wstr_mnemonic(MC_STR_ARTIST));
  SetDlgItemTextW(dlg, IDC_REV_TITLE_LABEL, mc_wstr_mnemonic(MC_STR_ALBUM));
  SetDlgItemTextW(dlg, IDC_REV_SEARCH, mc_wstr_mnemonic(MC_STR_SEARCH));
  SetDlgItemTextW(dlg, IDC_REV_WIDER, mc_wstr_mnemonic(MC_STR_IMPORT_SEARCH_WIDER));
  SetDlgItemTextW(dlg, IDC_REV_ASIS, mc_wstr_mnemonic(MC_STR_IMPORT_AS_TAGGED));
  SetDlgItemTextW(dlg, IDC_REV_SKIP, mc_wstr_mnemonic(MC_STR_IMPORT_SKIP));
  SetDlgItemTextW(dlg, IDOK, mc_wstr_mnemonic(MC_STR_IMPORT_USE_MATCH));
  SetDlgItemTextW(dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CANCEL));
}

static void init_review_columns(HWND dlg) {
  RECT rc;
  HWND lv = GetDlgItem(dlg, IDC_REV_MATCHES);
  SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  GetClientRect(lv, &rc);
  int w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
  static const struct { enum mc_str_id title; int pct; int fmt; } matches[] = {
    {MC_STR_COL_CONFIDENCE, 10, LVCFMT_RIGHT}, {MC_STR_ARTIST, 17, 0}, {MC_STR_WINDOWS_COL_RELEASE, 24, 0},
    {MC_STR_WINDOWS_COL_YEAR, 6, LVCFMT_RIGHT}, {MC_STR_TRACKS_TITLE, 7, LVCFMT_RIGHT},
    {MC_STR_WINDOWS_COL_TYPE, 8, 0}, {MC_STR_WINDOWS_COL_FORMAT, 10, 0}, {MC_STR_WINDOWS_COL_COUNTRY, 7, 0},
    {MC_STR_WINDOWS_COL_LABEL, 11, 0}};
  for (int i = 0; i < 9; i++)
    lv_add_column(lv, i, mc_wstr(matches[i].title), w * matches[i].pct / 100, matches[i].fmt);

  lv = GetDlgItem(dlg, IDC_REV_TRACKS);
  SendMessageW(lv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  GetClientRect(lv, &rc);
  w = rc.right - GetSystemMetrics(SM_CXVSCROLL);
  static const struct { enum mc_str_id title; int pct; int fmt; } tracks[] = {
    {MC_STR_WINDOWS_COL_NUMBER, 6, LVCFMT_RIGHT}, {MC_STR_TITLE, 30, 0},
    {MC_STR_WINDOWS_COL_LENGTH, 8, LVCFMT_RIGHT}, {MC_STR_WINDOWS_COL_FILE, 22, 0},
    {MC_STR_WINDOWS_COL_CURRENT_TITLE, 26, 0}, {MC_STR_WINDOWS_COL_LENGTH, 8, LVCFMT_RIGHT}};
  for (int i = 0; i < 6; i++)
    lv_add_column(lv, i, mc_wstr(tracks[i].title), w * tracks[i].pct / 100, tracks[i].fmt);
}

static void review_done(HWND dlg, mc_status st) {
  if (st == MC_OK) {
    g_view.idle = FALSE; /* the import thread has work again */
    update_status();
    EndDialog(dlg, IDOK);
  } else {
    MessageBoxW(dlg, mc_wstr(MC_STR_IMPORT_ITEM_BUSY), mc_wstr(MC_STR_IMPORT_MUSIC), MB_OK | MB_ICONINFORMATION);
  }
}

static INT_PTR CALLBACK review_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG:
      g_view.review = dlg;
      g_view.review_item = (int)lp;
      g_view.review_terms_set = FALSE;
      init_review_text(dlg);
      init_review_columns(dlg);
      SetWindowSubclass(GetDlgItem(dlg, IDC_REV_TRACKS), tracks_proc, 0, 0);
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
    case WM_APP_FILES_MENU:
      files_menu(dlg, (int)wp, (POINT){-1, -1});
      return TRUE;
    case WM_CONTEXTMENU:
      if ((HWND)wp == GetDlgItem(dlg, IDC_REV_TRACKS)) {
        tracks_menu(dlg, (POINT){(short)LOWORD(lp), (short)HIWORD(lp)});
        return TRUE;
      }
      break;
    case WM_NOTIFY: {
      const NMHDR *nm = (const NMHDR *)lp;
      if (nm->idFrom == IDC_REV_TRACKS && nm->code == NM_CLICK) {
        /* A plain click on a missing track drops its files menu down; Ctrl or Shift selects. */
        const NMITEMACTIVATE *ia = (const NMITEMACTIVATE *)lp;
        if (!(ia->uKeyFlags & (LVKF_CONTROL | LVKF_SHIFT)) && missing_row(nm->hwndFrom, ia->iItem))
          files_menu(dlg, ia->iItem, (POINT){-1, -1});
        break;
      }
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
        case IDC_REV_WIDER:
          if (mc_import_search_wider(g_view.imp, g_view.review_item) == MC_OK) {
            g_view.idle = FALSE;
            update_status();
          }
          return TRUE; /* like Search: the dialog refreshes when results arrive */
        case IDCANCEL:
          EndDialog(dlg, IDCANCEL);
          return TRUE;
      }
      break;
    case WM_DESTROY:
      g_view.review = NULL;
      mc_import_item_free(g_view.review_snap);
      g_view.review_snap = NULL;
      free(g_view.rows);
      g_view.rows = NULL;
      g_view.row_count = 0;
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

/* Whether Windows' long paths are on: LongPathsEnabled, which the "Enable Win32 long paths"
 * policy sets too. With the manifest's longPathAware, every Win32 call then takes them. */
static BOOL long_paths_enabled(void) {
  DWORD on = 0, size = sizeof on;
  return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem", L"LongPathsEnabled",
                      RRF_RT_REG_DWORD, NULL, &on, &size) == ERROR_SUCCESS &&
         on == 1;
}

/* Once a session, when files were left out for their long paths (see "Long paths" in
 * tagger.h) and long paths are off: how to turn them on. */
static void tell_long_paths(void) {
  if (g_view.long_paths_told) return;
  const char *why = mc_str(MC_STR_IMPORT_ERROR_LONG_PATH);
  int n = 0;
  for (int i = 0; i < g_view.item_count; i++) {
    mc_import_item *it;
    if (g_view.states[i] != MC_IMPORT_FAILED || mc_import_item_get(g_view.imp, i, &it) != MC_OK) continue;
    n += it->error && strcmp(it->error, why) == 0;
    mc_import_item_free(it);
  }
  g_view.long_paths_told = TRUE; /* also when there are none: they're all found by now */
  if (!n || long_paths_enabled()) return;
  wchar_t text[1024];
  mc_wformat(text, 1024, mc_str_plural(MC_STR_WINDOWS_IMPORT_LONG_PATHS, n), MC_D(n));
  MessageBoxW(g_view.parent, text, mc_wstr(MC_STR_IMPORT_MUSIC), MB_OK | MB_ICONWARNING);
}

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
      update_status();
      tell_long_paths(); /* modal: after the status says the import is done */
      break;
    case MC_IMPORT_EV_OFFLINE:
      if (g_view.offline != (arg != 0)) {
        g_view.offline = arg != 0;
        SendMessageW(g_view.parent, WM_APP_IMPORT_SESSION, TRUE, 0); /* relayout: banner in or out */
        if (g_view.review) SendMessageW(g_view.review, WM_APP_REVIEW_REFRESH, 0, 0); /* Search More Widely */
      }
      break;
  }
  update_status();
}

BOOL import_view_on_command(int id) {
  switch (id) {
    case ID_IMP_BROWSE: import_view_browse(); return TRUE;
    case ID_IMP_DONE: PostMessageW(g_view.parent, WM_APP_IMPORT_CLOSE, 0, 0); return TRUE;
    case ID_IMP_SKIP_TAGGING: skip_tagging(); return TRUE;
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
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_drop"};
  RegisterClassExW(&wc);
  WNDCLASSEXW bc = {.cbSize = sizeof bc, .lpfnWndProc = banner_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_banner"};
  RegisterClassExW(&bc);

  g_view.drop = child(L"kithsong_drop", NULL, 0, ID_IMP_DROP);
  g_view.browse = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_IMPORT_CHOOSE_FOLDERS), BS_PUSHBUTTON, ID_IMP_BROWSE);
  g_view.status = child(WC_STATICW, NULL, SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, ID_IMP_STATUS);
  g_view.progress = child(PROGRESS_CLASSW, NULL, 0, ID_IMP_PROGRESS);
  g_view.done = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_DONE), BS_PUSHBUTTON, ID_IMP_DONE);
  g_view.banner = child(L"kithsong_banner", NULL, WS_CLIPCHILDREN, ID_IMP_BANNER);
  g_view.skip_tagging = CreateWindowExW(0, WC_BUTTONW, mc_wstr_mnemonic(MC_STR_IMPORT_SKIP_TAGGING),
                                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0, g_view.banner,
                                        (HMENU)(INT_PTR)ID_IMP_SKIP_TAGGING, GetModuleHandleW(NULL), NULL);
  g_view.list = child(WC_LISTVIEWW, NULL, LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_BORDER,
                      ID_IMP_LIST);
  SendMessageW(g_view.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
               LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

  static const enum mc_str_id titles[NUM_COLS] = {
    MC_STR_WINDOWS_COL_STATUS, MC_STR_ARTIST, MC_STR_WINDOWS_COL_ALBUM_TITLE,
    MC_STR_WINDOWS_COL_FILES, MC_STR_WINDOWS_COL_MATCH_RELEASE, MC_STR_COL_CONFIDENCE};
  for (int i = 0; i < NUM_COLS; i++)
    lv_add_column(g_view.list, i, mc_wstr(titles[i]), 100,
                  i == COL_FILES || i == COL_CONFIDENCE ? LVCFMT_RIGHT : 0);

  SendMessageW(g_view.list, LVM_ENABLEGROUPVIEW, TRUE, 0);
  add_group(GROUP_REVIEW, mc_wstr(MC_STR_IMPORT_NEEDS_DECISION));
  add_group(GROUP_ACTIVE, mc_wstr(MC_STR_IMPORT_GROUP_ACTIVE));
  add_group(GROUP_DONE, mc_wstr(MC_STR_IMPORT_STATE_DONE));
  add_group(GROUP_SKIPPED, mc_wstr(MC_STR_IMPORT_STATE_SKIPPED));
  add_group(GROUP_FAILED, mc_wstr(MC_STR_IMPORT_STATE_FAILED));
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
  HWND controls[] = {g_view.browse, g_view.status, g_view.done, g_view.list, g_view.skip_tagging};
  for (size_t i = 0; i < sizeof controls / sizeof controls[0]; i++)
    SendMessageW(controls[i], WM_SETFONT, (WPARAM)font, TRUE);
  InvalidateRect(g_view.drop, NULL, TRUE);
}

void import_view_show(BOOL show) {
  g_view.shown = show;
  if (!show) {
    HWND all[] = {g_view.drop, g_view.browse, g_view.status, g_view.progress,
                  g_view.done, g_view.list, g_view.banner};
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(all[i], SW_HIDE);
  }
}

void import_view_layout(int x, int y, int w, int h, UINT dpi) {
  if (!g_view.shown) return;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int pad = S(16);
  BOOL session = g_view.imp != NULL;
  BOOL banner = session && g_view.offline && !g_view.skipped_tags;
  g_view.dpi = dpi;

  int btn_w = S(88), btn_h = S(28);

  ShowWindow(g_view.drop, session ? SW_HIDE : SW_SHOW);
  ShowWindow(g_view.browse, session ? SW_HIDE : SW_SHOW);
  ShowWindow(g_view.status, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.progress, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.done, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.list, session ? SW_SHOW : SW_HIDE);
  ShowWindow(g_view.banner, banner ? SW_SHOW : SW_HIDE);

  if (!session) {
    int dz_w = min(S(520), w - 2 * pad), dz_h = S(200), browse_w = S(150), browse_h = S(30);
    int dz_x = x + (w - dz_w) / 2, dz_y = max(y + pad, y + (h - dz_h - browse_h - S(16)) / 2);
    MoveWindow(g_view.drop, dz_x, dz_y, dz_w, dz_h, TRUE);
    MoveWindow(g_view.browse, x + (w - browse_w) / 2, dz_y + dz_h + S(16), browse_w, browse_h, TRUE);
    return;
  }

  MoveWindow(g_view.done, x + w - pad - btn_w, y + pad, btn_w, btn_h, TRUE);
  MoveWindow(g_view.status, x + pad, y + pad + (btn_h - S(18)) / 2, w - 3 * pad - btn_w, S(18), TRUE);
  MoveWindow(g_view.progress, x + pad, y + pad + btn_h + S(8), w - 2 * pad, S(6), TRUE);
  int list_y = y + pad + btn_h + S(22);
  if (banner) {
    /* Offline: a banner across the top of the list, its button at the right. */
    int banner_h = S(44), skip_w = S(180);
    MoveWindow(g_view.banner, x + pad, list_y, w - 2 * pad, banner_h, TRUE);
    MoveWindow(g_view.skip_tagging, w - 2 * pad - S(8) - skip_w, (banner_h - btn_h) / 2, skip_w, btn_h, TRUE);
    InvalidateRect(g_view.banner, NULL, TRUE);
    list_y += banner_h + S(10);
  }
  MoveWindow(g_view.list, x + pad, list_y, w - 2 * pad, y + h - pad - list_y, TRUE);

  /* Fixed columns, with Match taking whatever is left. Status fits "Replaced with higher
   * quality". */
  static const int widths[NUM_COLS] = {180, 150, 210, 60, 0, 90};
  int lw = w - 2 * pad - GetSystemMetrics(SM_CXVSCROLL) - S(4), used = 0;
  for (int i = 0; i < NUM_COLS; i++)
    if (i != COL_MATCH) {
      SendMessageW(g_view.list, LVM_SETCOLUMNWIDTH, (WPARAM)i, S(widths[i]));
      used += S(widths[i]);
    }
  SendMessageW(g_view.list, LVM_SETCOLUMNWIDTH, COL_MATCH, max(S(120), lw - used));
#undef S
}
