#include "move_library.h"

#include <commctrl.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "pick_folder.h"
#include "theme.h"
#include "wstr.h"

/* To the progress window. */
enum {
  WM_MOVE_PROGRESS = WM_APP + 1, /* the worker has news */
  WM_MOVE_DONE,                  /* the worker has finished */
};

enum { ID_HEADING = 100, ID_BAR, ID_STATUS, ID_FILE, ID_NOTE };

/* Layout at 96 DPI. */
enum { WIN_W = 520, PAD = 24, GAP = 8, BAR_H = 6, BAR_RANGE = 1000 };

static struct {
  HWND main, wnd;
  HWND heading, bar, status, file, note;
  HFONT font, heading_font;
  UINT dpi;
  const move_library_host *host;
  mc_settings *settings;
  char *src, *dst; /* UTF-8 */
  HANDLE thread;
  BOOL lock_ready;
  int delay_ms; /* debug builds: KITHSONG_TEST_MOVE_DELAY_MS */

  /* Shared with the worker, under lock. */
  CRITICAL_SECTION lock;
  mc_library_move_progress p;
  char current[1024]; /* p.current, copied */
  BOOL posted;        /* a WM_MOVE_PROGRESS is on its way */
  mc_status st;
  mc_library_move_result result;
} g_move;

static int scale(int px) {
  return MulDiv(px, (int)g_move.dpi, USER_DEFAULT_SCREEN_DPI);
}

/* "12.5 MB", as the device page writes sizes. */
static wchar_t *fmt_size(int64_t bytes, wchar_t *out, size_t cap) {
  static const enum mc_str_id units[] = {MC_STR_COUNT, MC_STR_SIZE_KB, MC_STR_SIZE_MB, MC_STR_SIZE_GB,
                                         MC_STR_SIZE_TB};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 4) v /= 1024.0, u++;
  if (u == 0) {
    mc_wformat(out, cap, mc_str_plural(MC_STR_SIZE_BYTES, (long)bytes), MC_D(bytes));
  } else {
    char num[32];
    snprintf(num, sizeof num, v < 100.0 ? "%.1f" : "%.0f", v);
    mc_wformat(out, cap, mc_str(units[u]), MC_S(num));
  }
  return out;
}

static char *utf8_size(int64_t bytes) {
  wchar_t w[64];
  return wstr_to_utf8(fmt_size(bytes, w, 64));
}

/* --- Choosing the folder --- */

/* The folder that holds dir, for the picker to start in; malloc'd, or NULL. */
static char *parent_of(const char *dir) {
  char *p = dir ? _strdup(dir) : NULL;
  char *cut = p ? p + strlen(p) : NULL;
  while (cut && cut > p && (cut[-1] == '\\' || cut[-1] == '/')) *--cut = '\0';
  while (cut && cut > p && cut[-1] != '\\' && cut[-1] != '/') cut--;
  if (cut && cut > p + 1 && cut[-2] != ':') cut--; /* drop the separator, but not C:'s */
  if (cut && cut > p) *cut = '\0';
  return p;
}

/* Why dst won't take the library, as a message; NULL if it will. */
static wchar_t *problem_text(mc_status st, const mc_library_move_info *info, const char *dst, wchar_t *out,
                             size_t cap) {
  if (st != MC_OK) return mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_CHECK_FAILED), MC_S(mc_status_str(st)));
  switch (info->problem) {
    case MC_MOVE_OK: return NULL;
    case MC_MOVE_SAME_FOLDER: return mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_SAME_FOLDER), MC_S(dst));
    case MC_MOVE_INSIDE_LIBRARY: return mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_INSIDE), MC_S(dst));
    case MC_MOVE_CONTAINS_LIBRARY: return mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_CONTAINS), MC_S(dst));
    case MC_MOVE_NOT_EMPTY: return mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_NOT_EMPTY), MC_S(dst));
    case MC_MOVE_NOT_WRITABLE: return mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_NOT_WRITABLE), MC_S(dst));
    case MC_MOVE_NO_SPACE: {
      char *needed = utf8_size(info->needed_bytes), *avail = utf8_size(info->free_bytes);
      mc_wformat(out, cap, mc_str(MC_STR_LIBRARY_MOVE_NO_SPACE), MC_S(needed), MC_S(avail));
      free(needed);
      free(avail);
      return out;
    }
  }
  return NULL;
}

/* Debug builds: KITHSONG_TEST_MOVE_TO in place of the picker. */
static BOOL test_hook(char **dst) {
#ifdef NDEBUG
  (void)dst;
  return FALSE;
#else
  wchar_t w[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"KITHSONG_TEST_MOVE_TO", w, MAX_PATH);
  if (!n || n >= MAX_PATH) return FALSE;
  *dst = wstr_to_utf8(w);
  n = GetEnvironmentVariableW(L"KITHSONG_TEST_MOVE_DELAY_MS", w, MAX_PATH);
  g_move.delay_ms = n && n < MAX_PATH ? _wtoi(w) : 0;
  return TRUE;
#endif
}

/* Asks until the user picks a folder that will do, or cancels. Returns it malloc'd. */
static char *choose(HWND owner, const char *src) {
  char *initial = parent_of(src);
  for (;;) {
    char *dst = NULL;
    BOOL hook = test_hook(&dst);
    if (!hook) dst = pick_folder(owner, mc_wstr(MC_STR_LIBRARY_MOVE_CHOOSE), initial);
    if (!dst) break;

    /* Listing a big library takes a moment. */
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
    mc_library_move_info info;
    mc_status st = mc_library_move_check(src, dst, &info);
    SetCursor(old);
    wchar_t text[1024];
    if (!problem_text(st, &info, dst, text, 1024)) {
      free(initial);
      return dst;
    }
    MessageBoxW(owner, text, mc_wstr(MC_STR_LIBRARY_MOVE_CANT_TITLE), MB_OK | MB_ICONWARNING);
    free(dst);
    if (hook || st != MC_OK) break; /* the hook's folder won't change; nor will an unreadable library */
  }
  free(initial);
  return NULL;
}

/* --- The worker --- */

/* On the worker thread: keeps the latest progress, and wakes the window if it isn't
 * already on its way. */
static void on_progress(void *user, const mc_library_move_progress *p) {
  (void)user;
  EnterCriticalSection(&g_move.lock);
  g_move.p = *p;
  snprintf(g_move.current, sizeof g_move.current, "%s", p->current ? p->current : "");
  BOOL post = !g_move.posted;
  g_move.posted = TRUE;
  LeaveCriticalSection(&g_move.lock);
  if (post) PostMessageW(g_move.wnd, WM_MOVE_PROGRESS, 0, 0);
  if (g_move.delay_ms > 0 && p->current) Sleep((DWORD)g_move.delay_ms);
}

static unsigned __stdcall worker(void *arg) {
  (void)arg;
  mc_library_move_result result;
  mc_status st = mc_library_move(g_move.src, g_move.dst, g_move.settings, on_progress, NULL, &result);
  EnterCriticalSection(&g_move.lock);
  g_move.st = st;
  g_move.result = result;
  LeaveCriticalSection(&g_move.lock);
  PostMessageW(g_move.wnd, WM_MOVE_DONE, 0, 0);
  return 0;
}

/* --- The progress window --- */

static void show_progress(void) {
  EnterCriticalSection(&g_move.lock);
  mc_library_move_progress p = g_move.p;
  wchar_t file[1024];
  wstr_from_utf8(file, 1024, g_move.current);
  g_move.posted = FALSE;
  LeaveCriticalSection(&g_move.lock);

  wchar_t status[512];
  switch (p.phase) {
    case MC_MOVE_PHASE_COPYING: {
      wchar_t done[64], total[64];
      char *d = wstr_to_utf8(fmt_size(p.bytes_done, done, 64)), *t = wstr_to_utf8(fmt_size(p.bytes_total, total, 64));
      mc_wformat(status, 512, mc_str_plural(MC_STR_LIBRARY_MOVE_PROGRESS, p.files_total), MC_D(p.files_done),
                 MC_D(p.files_total), MC_S(d), MC_S(t));
      free(d);
      free(t);
      break;
    }
    case MC_MOVE_PHASE_CHECKING: wcscpy(status, mc_wstr(MC_STR_LIBRARY_MOVE_CHECKING)); break;
    case MC_MOVE_PHASE_REMOVING: wcscpy(status, mc_wstr(MC_STR_LIBRARY_MOVE_REMOVING)); break;
    case MC_MOVE_PHASE_UNDOING: wcscpy(status, mc_wstr(MC_STR_LIBRARY_MOVE_UNDOING)); break;
  }
  SetWindowTextW(g_move.status, status);
  SetWindowTextW(g_move.file, p.phase == MC_MOVE_PHASE_COPYING ? file : L"");
  int pos = p.bytes_total > 0 ? (int)(p.bytes_done * BAR_RANGE / p.bytes_total)
                              : p.files_total > 0 ? p.files_done * BAR_RANGE / p.files_total : 0;
  if (p.phase != MC_MOVE_PHASE_COPYING) pos = BAR_RANGE;
  SendMessageW(g_move.bar, PBM_SETPOS, (WPARAM)pos, 0);
}

/* The height wnd's text needs at width w. */
static int text_height(HWND wnd, HFONT font, int w) {
  wchar_t buf[1024];
  int n = GetWindowTextW(wnd, buf, 1024);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, font);
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, n ? buf : L" ", n ? n : 1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return rc.bottom;
}

/* Sizes the window to its content and centres it where the main window was. */
static void layout(void) {
  int w = scale(WIN_W), pad = scale(PAD), gap = scale(GAP), text_w = w - 2 * pad;
  int line = text_height(g_move.status, g_move.font, text_w);
  int y = pad, hh = text_height(g_move.heading, g_move.heading_font, text_w);
  MoveWindow(g_move.heading, pad, y, text_w, hh, FALSE);
  y += hh + 2 * gap;
  MoveWindow(g_move.bar, pad, y, text_w, scale(BAR_H), FALSE);
  y += scale(BAR_H) + 2 * gap;
  MoveWindow(g_move.status, pad, y, text_w, line, FALSE);
  y += line + gap / 2;
  MoveWindow(g_move.file, pad, y, text_w, line, FALSE);
  y += line + 2 * gap;
  int nh = text_height(g_move.note, g_move.font, text_w);
  MoveWindow(g_move.note, pad, y, text_w, nh, FALSE);
  y += nh + pad;

  RECT rc = {0, 0, w, y};
  DWORD style = (DWORD)GetWindowLongPtrW(g_move.wnd, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(g_move.wnd, GWL_EXSTYLE);
  AdjustWindowRectExForDpi(&rc, style, FALSE, ex, g_move.dpi);
  RECT owner;
  GetWindowRect(g_move.main, &owner);
  int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
  int x = (owner.left + owner.right - ww) / 2, top = (owner.top + owner.bottom - wh) / 2;
  SetWindowPos(g_move.wnd, NULL, x, top, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void make_fonts(void) {
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_move.dpi);
  if (g_move.font) DeleteObject(g_move.font);
  if (g_move.heading_font) DeleteObject(g_move.heading_font);
  g_move.font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 14, 9); /* a subtitle, as Settings' headings */
  lf.lfWeight = FW_SEMIBOLD;
  g_move.heading_font = CreateFontIndirectW(&lf);
  SendMessageW(g_move.heading, WM_SETFONT, (WPARAM)g_move.heading_font, FALSE);
  HWND body[] = {g_move.status, g_move.file, g_move.note};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)g_move.font, FALSE);
}

static void finish(void);

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_MOVE_PROGRESS:
      show_progress();
      return 0;
    case WM_MOVE_DONE:
      finish();
      return 0;
    case WM_CLOSE: /* Alt+F4: not until it's done */
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_CLOSE) return 0;
      break;
    case WM_QUERYENDSESSION: /* signing out or shutting down would leave a half-moved library */
      return FALSE;
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(wnd, &rc);
      FillRect((HDC)wp, &rc, theme.bg_brush);
      return 1;
    }
    case WM_CTLCOLORSTATIC: {
      HBRUSH brush = theme_ctlcolor(msg, (HDC)wp);
      if ((HWND)lp == g_move.file || (HWND)lp == g_move.note) SetTextColor((HDC)wp, theme.text2);
      return (LRESULT)(brush ? brush : theme.bg_brush);
    }
    case WM_DPICHANGED:
      g_move.dpi = HIWORD(wp);
      make_fonts();
      layout();
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_move.wnd, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

/* The progress window: a caption with no buttons, in the taskbar while the main window is
 * hidden. */
static BOOL create_window(void) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = wnd_proc, .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .hIcon = (HICON)GetClassLongPtrW(g_move.main, GCLP_HICON),
                    .hIconSm = (HICON)GetClassLongPtrW(g_move.main, GCLP_HICONSM),
                    .lpszClassName = L"kithsong_move"};
  RegisterClassExW(&wc);
  g_move.wnd = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME, L"kithsong_move",
                               mc_wstr(MC_STR_LIBRARY_MOVE_TITLE), WS_POPUP | WS_CAPTION | WS_CLIPCHILDREN, 0, 0,
                               0, 0, NULL, NULL, GetModuleHandleW(NULL), NULL);
  if (!g_move.wnd) return FALSE;
  g_move.dpi = GetDpiForWindow(g_move.wnd);
  theme_apply_window(g_move.wnd, FALSE);

  wchar_t heading[1024];
  mc_wformat(heading, 1024, mc_str(MC_STR_LIBRARY_MOVE_TO), MC_S(g_move.dst));
  DWORD label = SS_LEFT | SS_NOPREFIX;
  g_move.heading = child(WC_STATICW, heading, label, ID_HEADING);
  g_move.bar = child(PROGRESS_CLASSW, NULL, PBS_SMOOTH, ID_BAR);
  SendMessageW(g_move.bar, PBM_SETRANGE32, 0, BAR_RANGE);
  g_move.status = child(WC_STATICW, L" ", label | SS_ENDELLIPSIS, ID_STATUS);
  g_move.file = child(WC_STATICW, L" ", label | SS_PATHELLIPSIS, ID_FILE);
  g_move.note = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_MOVE_WAIT), label, ID_NOTE);
  theme_apply_controls(g_move.wnd);
  make_fonts();
  layout();
  show_progress();
  ShowWindow(g_move.wnd, SW_SHOW);
  SetForegroundWindow(g_move.wnd);
  return TRUE;
}

/* --- Starting and finishing --- */

void move_library_start(HWND main, mc_settings *settings, const move_library_host *host) {
  if (g_move.wnd || !settings || !mc_settings_library_dir(settings)) return;
  const char *src = mc_settings_library_dir(settings);
  char *dst = choose(main, src);
  if (!dst) return;

  if (!g_move.lock_ready) InitializeCriticalSection(&g_move.lock);
  g_move.lock_ready = TRUE;
  g_move.main = main;
  g_move.host = host;
  g_move.settings = settings;
  g_move.src = _strdup(src); /* settings will change under it */
  g_move.dst = dst;
  memset(&g_move.p, 0, sizeof g_move.p);
  g_move.current[0] = '\0';
  g_move.posted = FALSE;

  host->close_library();
  ShowWindow(main, SW_HIDE);
  if (create_window())
    g_move.thread = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);
  if (!g_move.thread) { /* nothing has moved: back to how it was */
    if (g_move.wnd) DestroyWindow(g_move.wnd);
    g_move.wnd = NULL;
    host->reopen_library(g_move.src);
    free(g_move.src);
    free(g_move.dst);
    g_move.src = g_move.dst = NULL;
  }
}

BOOL move_library_running(void) {
  return g_move.wnd != NULL;
}

/* The worker is done: the library opens where it is now, and the main window comes back. */
static void finish(void) {
  WaitForSingleObject(g_move.thread, INFINITE);
  CloseHandle(g_move.thread);
  g_move.thread = NULL;
  mc_status st = g_move.st;
  mc_library_move_result result = g_move.result;

  DestroyWindow(g_move.wnd);
  g_move.wnd = NULL;
  if (g_move.font) DeleteObject(g_move.font);
  if (g_move.heading_font) DeleteObject(g_move.heading_font);
  g_move.font = g_move.heading_font = NULL;

  g_move.host->reopen_library(st == MC_OK ? g_move.dst : g_move.src);

  wchar_t text[2048];
  if (st != MC_OK && result.failed_path)
    mc_wformat(text, 2048, mc_str(MC_STR_LIBRARY_MOVE_FAILED_FILE), MC_S(result.failed_path),
               MC_S(mc_status_str(st)), MC_S(g_move.src));
  else if (st != MC_OK)
    mc_wformat(text, 2048, mc_str(MC_STR_LIBRARY_MOVE_FAILED), MC_S(mc_status_str(st)), MC_S(g_move.src));
  else if (result.left_behind)
    mc_wformat(text, 2048, mc_str_plural(MC_STR_LIBRARY_MOVE_LEFT_BEHIND, result.left_behind),
               MC_D(result.left_behind), MC_S(g_move.src));
  if (st != MC_OK || result.left_behind)
    MessageBoxW(g_move.main, text, mc_wstr(MC_STR_LIBRARY_MOVE_TITLE),
                MB_OK | (st != MC_OK ? MB_ICONERROR : MB_ICONWARNING));

  mc_library_move_result_free(&result);
  free(g_move.src);
  free(g_move.dst);
  g_move.src = g_move.dst = NULL;
}
