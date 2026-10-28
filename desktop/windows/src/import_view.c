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
  lv_set_utf8(lv, idx, COL_ALBUM, it->album ? it->album : file_name(it->files[0].path));

  wchar_t num[64];
  if (it->kind == MC_IMPORT_SINGLE) wstr_from_utf8(num, 64, mc_str(MC_STR_IMPORT_SINGLE));
  else swprintf(num, 64, L"%d", it->file_count);
  lv_set(lv, idx, COL_FILES, num);

  char match[512] = "";
  const mc_import_match *m = NULL;
  if (it->chosen >= 0 && it->chosen < it->match_count) m = &it->matches[it->chosen];
  else if (it->state != MC_IMPORT_DONE && it->match_count) m = &it->matches[0];
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
        lv_set(lv, row, 3, mc_wstr(MC_STR_IMPORT_TRACK_MISSING));
      }
    }
  }
  /* Files that weren't matched to any track: left untouched if this match is applied. */
  for (int i = 0; i < it->file_count; i++) {
    if (used && used[i]) continue;
    int row = lv_append(lv, -1, 0);
    lv_set(lv, row, 1, match >= 0 ? mc_wstr(MC_STR_IMPORT_NOT_MATCHED) : L"");
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
  const char *artist = it->artist ? it->artist : mc_str(MC_STR_UNKNOWN_VALUE);
  const char *album = it->album ? it->album : file_name(it->files[0].path);
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
    {MC_STR_WINDOWS_COL_MATCH, 7, LVCFMT_RIGHT}, {MC_STR_ARTIST, 17, 0}, {MC_STR_WINDOWS_COL_RELEASE, 27, 0},
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
    case MC_IMPORT_EV_OFFLINE:
      if (g_view.offline != (arg != 0)) {
        g_view.offline = arg != 0;
        SendMessageW(g_view.parent, WM_APP_IMPORT_SESSION, TRUE, 0); /* relayout: banner in or out */
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
    MC_STR_WINDOWS_COL_FILES, MC_STR_WINDOWS_COL_MATCH_RELEASE, MC_STR_WINDOWS_COL_CONFIDENCE};
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
