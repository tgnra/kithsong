/* Win32 frontend. Layout:
 *
 *   +--------------------------------------------------------------+
 *   | |<  >||  >|                              [Import] [Settings] |  toolbar
 *   +-----------+------------------------------------+-------------+
 *   | Albums    |                                    | track list  |
 *   | Artists   |          album grid                | (shown when |
 *   | Playlists |                                    |  an album   |
 *   | DEVICES   |                                    |  is picked) |
 *   +-----------+------------------------------------+-------------+
 *
 * Placeholder controls for now. The toolbar will become a custom title bar
 * (DwmExtendFrameIntoClientArea + WM_NCCALCSIZE) and the grid an owner-drawn,
 * virtualised view.
 */
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include <wchar.h>

#include "musiccore/musiccore.h"

enum {
  ID_PREV = 100,
  ID_PLAY,
  ID_NEXT,
  ID_IMPORT,
  ID_SETTINGS,
  ID_SIDEBAR,
  ID_GRID,
  ID_TRACKS,
  NUM_BUTTONS = ID_SETTINGS - ID_PREV + 1
};

typedef struct app {
  HWND wnd;
  HWND buttons[NUM_BUTTONS];
  HWND sidebar;
  HWND grid;
  HWND tracks;
  HFONT font;
  UINT dpi;
  BOOL show_tracks;
  mc_library *lib;
} app;

static app g_app;

static int scale(int px) {
  return MulDiv(px, (int)g_app.dpi, USER_DEFAULT_SCREEN_DPI);
}

static void update_font(void) {
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_app.dpi);
  HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

  HWND controls[NUM_BUTTONS + 3];
  int n = 0;
  for (int i = 0; i < NUM_BUTTONS; i++) controls[n++] = g_app.buttons[i];
  controls[n++] = g_app.sidebar;
  controls[n++] = g_app.grid;
  controls[n++] = g_app.tracks;
  for (int i = 0; i < n; i++) SendMessageW(controls[i], WM_SETFONT, (WPARAM)font, TRUE);

  if (g_app.font) DeleteObject(g_app.font);
  g_app.font = font;
}

static void layout(void) {
  RECT rc;
  GetClientRect(g_app.wnd, &rc);
  int w = rc.right, h = rc.bottom;
  int pad = scale(8), bar = scale(44), btn = scale(28);
  int side_w = scale(200), tracks_w = g_app.show_tracks ? scale(320) : 0;

  HDWP dwp = BeginDeferWindowPos(NUM_BUTTONS + 3);
  int x = pad;
  for (int i = 0; i < 3; i++, x += scale(40) + scale(4))
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, x, (bar - btn) / 2, scale(40), btn, SWP_NOZORDER);
  int right = w - pad;
  for (int i = NUM_BUTTONS - 1; i >= 3; i--) {
    right -= scale(88);
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, right, (bar - btn) / 2, scale(88), btn, SWP_NOZORDER);
    right -= scale(4);
  }
  dwp = DeferWindowPos(dwp, g_app.sidebar, NULL, 0, bar, side_w, h - bar, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.grid, NULL, side_w, bar, w - side_w - tracks_w, h - bar, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.tracks, NULL, w - tracks_w, bar, tracks_w, h - bar,
                       SWP_NOZORDER | (g_app.show_tracks ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  EndDeferWindowPos(dwp);
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_app.wnd,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

static void create_controls(void) {
  static const wchar_t *const labels[NUM_BUTTONS] = {
    L"⏮", L"⏯", L"⏭", L"Import", L"Settings"};
  for (int i = 0; i < NUM_BUTTONS; i++)
    g_app.buttons[i] = child(WC_BUTTONW, labels[i], BS_PUSHBUTTON, ID_PREV + i);

  g_app.sidebar = child(WC_LISTBOXW, NULL, LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, ID_SIDEBAR);
  static const wchar_t *const entries[] = {
    L"Albums", L"Artists", L"Playlists", L"", L"DEVICES", L"  No devices found"};
  for (size_t i = 0; i < sizeof entries / sizeof entries[0]; i++)
    SendMessageW(g_app.sidebar, LB_ADDSTRING, 0, (LPARAM)entries[i]);
  SendMessageW(g_app.sidebar, LB_SETCURSEL, 0, 0);

  wchar_t version[32], text[256];
  MultiByteToWideChar(CP_UTF8, 0, mc_version(), -1, version, 32);
  int64_t albums = 0;
  if (g_app.lib) mc_library_count(g_app.lib, MC_ENTITY_ALBUMS, &albums);
  if (!g_app.lib)
    swprintf(text, 256, L"Could not open the library.\n\nmusiccore %ls", version);
  else if (albums == 0)
    swprintf(text, 256, L"No albums yet. Use Import to add music.\n\nmusiccore %ls", version);
  else
    swprintf(text, 256, L"%lld albums\n\nmusiccore %ls", (long long)albums, version);
  g_app.grid = child(WC_STATICW, text, SS_CENTER | SS_CENTERIMAGE, ID_GRID);

  g_app.tracks = child(WC_LISTBOXW, NULL, LBS_NOINTEGRALHEIGHT, ID_TRACKS);
  ShowWindow(g_app.tracks, SW_HIDE);
}

static void on_command(int id) {
  switch (id) {
    case ID_IMPORT:
      MessageBoxW(g_app.wnd, L"The import UI is not implemented yet.", L"Import", MB_OK);
      break;
    case ID_SETTINGS:
      MessageBoxW(g_app.wnd, L"Settings are not implemented yet.", L"Settings", MB_OK);
      break;
    default:
      break;
  }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE:
      g_app.wnd = wnd;
      g_app.dpi = GetDpiForWindow(wnd);
      create_controls();
      update_font();
      return 0;
    case WM_SIZE:
      layout();
      return 0;
    case WM_DPICHANGED: {
      g_app.dpi = HIWORD(wp);
      update_font();
      const RECT *r = (const RECT *)lp;
      SetWindowPos(wnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      MINMAXINFO *mmi = (MINMAXINFO *)lp;
      mmi->ptMinTrackSize.x = scale(640);
      mmi->ptMinTrackSize.y = scale(400);
      return 0;
    }
    case WM_COMMAND:
      on_command(LOWORD(wp));
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(wnd, msg, wp, lp);
  }
}

/* %LOCALAPPDATA%\musicapp\library.db, as UTF-8 for musiccore. */
static int library_path(char *out, int out_len) {
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH - 32) return 0;
  wcscat(dir, L"\\musicapp");
  CreateDirectoryW(dir, NULL);
  wcscat(dir, L"\\library.db");
  return WideCharToMultiByte(CP_UTF8, 0, dir, -1, out, out_len, NULL, NULL) > 0;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;

  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES};
  InitCommonControlsEx(&icc);

  mc_init();
  char path[MAX_PATH * 4];
  if (library_path(path, (int)sizeof path)) {
    mc_status st = mc_library_open(path, &g_app.lib);
    if (st != MC_OK) g_app.lib = NULL;
  }

  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .lpfnWndProc = wnd_proc,
    .hInstance = inst,
    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
    .hbrBackground = (HBRUSH)(COLOR_WINDOW + 1),
    .lpszClassName = L"musicapp",
  };
  RegisterClassExW(&wc);

  HWND wnd = CreateWindowExW(0, L"musicapp", L"musicapp", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                             CW_USEDEFAULT, 1200, 800, NULL, NULL, inst, NULL);
  if (!wnd) return 1;
  ShowWindow(wnd, show);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  mc_library_close(g_app.lib);
  mc_shutdown();
  if (g_app.font) DeleteObject(g_app.font);
  return (int)msg.wParam;
}
