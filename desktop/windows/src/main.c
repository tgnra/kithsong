/* Win32 frontend. Layout:
 *
 *   +--------------------------------------------------------------+
 *   | |<  >  >|                    [Import] [Settings]   _  []  X  |  toolbar = title bar
 *   +-----------+.-----------------------------------+-------------+
 *   | Albums    |                                    | track list  |
 *   | Artists   |          album grid                | (shown when |
 *   | Playlists |                                    |  an album   |
 *   | DEVICES   |                                    |  is picked) |
 *   +-----------+------------------------------------+-------------+
 *
 * Import replaces everything below the toolbar with the import view (import_view.c)
 * until the user clicks Done. Folders dropped anywhere on the window start an import.
 *
 * The toolbar is the title bar: WM_NCCALCSIZE removes the standard one, and the frame is
 * extended into the client area (DwmExtendFrameIntoClientArea) so Windows still draws the
 * minimise, maximise and close buttons. On Windows 11 the toolbar and sidebar show Mica
 * through the extended frame. Everything drawn there needs real alpha, which GDI doesn't
 * give, so it's drawn with canvas.c.
 *
 * The album grid is album_grid.c and the sidebar sidebar.c. The track list is a placeholder
 * for now. The toolbar buttons are owner-drawn here.
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "album_grid.h"
#include "canvas.h"
#include "import_view.h"
#include "musiccore/musiccore.h"
#include "pick_folder.h"
#include "sidebar.h"
#include "theme.h"

enum {
  ID_PREV = 100,
  ID_PLAY,
  ID_NEXT, /* last playback button */
  ID_IMPORT,
  ID_SETTINGS,
  ID_SIDEBAR,
  ID_GRID,
  ID_TRACKS,
  NUM_BUTTONS = ID_SETTINGS - ID_PREV + 1,
  NUM_PLAYBACK = ID_NEXT - ID_PREV + 1
};

/* Layout at 96 DPI. */
enum {
  BAR_H = 48,
  SIDE_W = 220,
  SIDE_TOP = 6, /* above the sidebar's first row */
  TRACKS_W = 320,
  PAD = 8,
  ICON_BTN = 36,
  BTN_W = 88,
  BTN_H = 32,
};

typedef struct app {
  HWND wnd;
  HWND buttons[NUM_BUTTONS];
  HWND tooltip;
  HWND sidebar;
  HWND grid;
  HWND tracks;
  HFONT font, icon_font;
  HWND hot_button; /* toolbar button under the mouse */
  canvas back;     /* the window's own background, around the children */
  canvas button;   /* scratch for drawing buttons */
  UINT dpi;
  BOOL show_tracks;
  BOOL importing; /* the import view is showing instead of the library */
  mc_settings *settings;
  mc_library *lib;
} app;

static app g_app;

static void change_library(void);

static int scale(int px) {
  return MulDiv(px, (int)g_app.dpi, USER_DEFAULT_SCREEN_DPI);
}

/* The resize border, which a maximised window also hangs off the screen by. */
static int frame_thickness(void) {
  return GetSystemMetricsForDpi(SM_CYFRAME, g_app.dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, g_app.dpi);
}

/* From the client area's right edge to the left of the minimise, maximise and close buttons. */
static int caption_buttons_width(void) {
  RECT b, wr, client;
  POINT origin = {0, 0};
  if (FAILED(DwmGetWindowAttribute(g_app.wnd, DWMWA_CAPTION_BUTTON_BOUNDS, &b, sizeof b)) || IsRectEmpty(&b))
    return scale(3 * 46);
  GetWindowRect(g_app.wnd, &wr);
  GetClientRect(g_app.wnd, &client);
  ClientToScreen(g_app.wnd, &origin);
  return (int)(origin.x - wr.left) + client.right - b.left; /* b is relative to the window */
}

static void update_font(void) {
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_app.dpi);
  HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

  if (g_app.icon_font) DeleteObject(g_app.icon_font);
  g_app.icon_font = CreateFontW(-scale(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH, theme_icon_face());
  sidebar_update(g_app.dpi);

  SendMessageW(g_app.grid, WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(g_app.tracks, WM_SETFONT, (WPARAM)font, TRUE);
  import_view_set_font(font);
  for (int i = 0; i < NUM_BUTTONS; i++) InvalidateRect(g_app.buttons[i], NULL, FALSE);

  if (g_app.font) DeleteObject(g_app.font);
  g_app.font = font;
}

static void layout(void) {
  RECT rc;
  GetClientRect(g_app.wnd, &rc);
  int w = rc.right, h = rc.bottom;
  int pad = scale(PAD), bar = scale(BAR_H), icon = scale(ICON_BTN), btn_w = scale(BTN_W), btn_h = scale(BTN_H);
  int side_w = scale(SIDE_W), side_top = scale(SIDE_TOP), tracks_w = g_app.show_tracks ? scale(TRACKS_W) : 0;
  int grid_w = w - side_w - (tracks_w ? tracks_w + scale(1) : 0);

  HDWP dwp = BeginDeferWindowPos(NUM_BUTTONS + 3);
  int x = pad;
  for (int i = 0; i < NUM_PLAYBACK; i++, x += icon + scale(2))
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, x, (bar - icon) / 2, icon, icon, SWP_NOZORDER);
  int right = w - caption_buttons_width() - pad;
  for (int i = NUM_BUTTONS - 1; i >= NUM_PLAYBACK; i--) {
    right -= btn_w;
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, right, (bar - btn_h) / 2, btn_w, btn_h, SWP_NOZORDER);
    right -= pad;
  }
  UINT library = g_app.importing ? SWP_HIDEWINDOW : SWP_SHOWWINDOW;
  dwp = DeferWindowPos(dwp, g_app.sidebar, NULL, 0, bar + side_top, side_w, h - bar - side_top,
                       SWP_NOZORDER | library);
  dwp = DeferWindowPos(dwp, g_app.grid, NULL, side_w, bar, grid_w, h - bar, SWP_NOZORDER | library);
  dwp = DeferWindowPos(dwp, g_app.tracks, NULL, w - tracks_w, bar, tracks_w, h - bar,
                       SWP_NOZORDER | (g_app.show_tracks && !g_app.importing ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  EndDeferWindowPos(dwp);

  /* The extended frame: under the toolbar, beside the sidebar, and down far enough for the
   * album grid's rounded corner to show Mica (anything else in that strip, like the track
   * list when it's real, must be drawn with alpha too). */
  BOOL mica = theme.mica && !g_app.importing;
  MARGINS m = {mica ? side_w : 0, 0, bar + (mica ? scale(CONTENT_RADIUS) : 0), 0};
  DwmExtendFrameIntoClientArea(g_app.wnd, &m);
  InvalidateRect(g_app.wnd, NULL, TRUE);

  import_view_layout(0, bar, w, h - bar, g_app.dpi);
}

/* The window's own areas, around its children: the toolbar and the sidebar column
 * (transparent over Mica, otherwise the frame colour) and the content background. */
static void paint_background(HDC dc, const RECT *area) {
  RECT client;
  GetClientRect(g_app.wnd, &client);
  canvas *c = &g_app.back;
  if (!canvas_reserve(c, client.right, client.bottom)) return;

  int w = client.right, h = client.bottom, bar = scale(BAR_H), side_w = g_app.importing ? 0 : scale(SIDE_W);
  int frame_alpha = theme.mica ? 0 : 255;
  RECT toolbar = {0, 0, w, bar}, side = {0, bar, side_w, h}, content = {side_w, bar, w, h}, r;
  if (IntersectRect(&r, &toolbar, area)) canvas_clear(c, &r, theme.frame, frame_alpha);
  if (IntersectRect(&r, &side, area)) canvas_clear(c, &r, theme.frame, frame_alpha);
  if (IntersectRect(&r, &content, area)) canvas_clear(c, &r, theme.bg, 255);
  RECT line = {0, 0, 0, 0};
  if (g_app.importing) SetRect(&line, 0, bar, w, bar + scale(1));
  else if (g_app.show_tracks) SetRect(&line, w - scale(TRACKS_W) - scale(1), bar, w - scale(TRACKS_W), h);
  if (IntersectRect(&r, &line, area)) canvas_clear(c, &r, theme.line, 255);
  canvas_blit(c, dc, area->left, area->top, area);
}

static void show_import(BOOL show) {
  g_app.importing = show;
  import_view_show(show);
  layout();
}

static void refresh_library(void) {
  album_grid_set_library(g_app.lib);
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, g_app.wnd,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

/* Tracks the mouse for the toolbar buttons' hover highlight. */
static LRESULT CALLBACK button_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_MOUSEMOVE:
      if (g_app.hot_button != wnd) {
        if (g_app.hot_button) InvalidateRect(g_app.hot_button, NULL, FALSE);
        g_app.hot_button = wnd;
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(wnd, NULL, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      if (g_app.hot_button == wnd) g_app.hot_button = NULL;
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, button_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* Windows 11's standard button: a light fill (faint white in dark mode) with a hairline
 * border. Returns the text colour. */
static COLORREF draw_standard_button(canvas *c, const RECT *rc, int radius, control_state state) {
  if (theme.high_contrast) {
    canvas_frame(c, rc, radius, 1, theme.text, 255);
    return theme_draw_state(c, rc, radius, state);
  }
  static const int light[] = {[STATE_NORMAL] = 179, [STATE_HOT] = 128, [STATE_PRESSED] = 77, [STATE_SELECTED] = 179};
  static const int dark[] = {[STATE_NORMAL] = 15, [STATE_HOT] = 21, [STATE_PRESSED] = 8, [STATE_SELECTED] = 15};
  canvas_fill(c, rc, radius, RGB(255, 255, 255), (theme.dark ? dark : light)[state]);
  canvas_frame(c, rc, radius, 1, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), theme.dark ? 18 : 24);
  return state == STATE_PRESSED ? theme.text2 : theme.text;
}

/* Playback buttons are bare icons with a fill on hover; Import and Settings are standard
 * buttons. Drawn with alpha, as they sit over Mica. */
static void draw_button(const DRAWITEMSTRUCT *dis) {
  static const wchar_t glyphs[NUM_PLAYBACK] = {0xE892, 0xE768, 0xE893}; /* Previous, Play, Next */
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas *c = &g_app.button;
  if (!canvas_reserve(c, rc.right, rc.bottom)) return;
  canvas_clear(c, &rc, theme.frame, theme.mica ? 0 : 255);

  int index = (int)dis->CtlID - ID_PREV;
  BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
  control_state state = pressed ? STATE_PRESSED : dis->hwndItem == g_app.hot_button ? STATE_HOT : STATE_NORMAL;
  int radius = scale(index < NUM_PLAYBACK ? 6 : 4);
  UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
  if (index < NUM_PLAYBACK) {
    COLORREF color = theme_draw_state(c, &rc, radius, state);
    canvas_text(c, &glyphs[index], 1, &rc, format, g_app.icon_font, color);
  } else {
    COLORREF color = draw_standard_button(c, &rc, radius, state);
    wchar_t text[64];
    int n = GetWindowTextW(dis->hwndItem, text, 64);
    canvas_text(c, text, n, &rc, format, g_app.font, color);
  }
  if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT))
    canvas_frame(c, &rc, radius, scale(2), theme.text, 255);
  canvas_blit(c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
}

/* After creating the controls, and whenever the Windows theme changes. */
static void apply_theme(void) {
  theme_apply_window(g_app.wnd, TRUE);
  theme_apply_controls(g_app.wnd);
  theme_apply_control(g_app.tooltip);
  sidebar_update(g_app.dpi);
  album_grid_theme_changed();
  layout();
  RedrawWindow(g_app.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void create_controls(void) {
  /* The playback labels aren't shown; they name the buttons for tooltips and screen readers. */
  static const wchar_t *const labels[NUM_BUTTONS] = {
    L"Previous", L"Play", L"Next", L"Import", L"Settings"};
  for (int i = 0; i < NUM_BUTTONS; i++) {
    g_app.buttons[i] = child(WC_BUTTONW, labels[i], BS_OWNERDRAW, ID_PREV + i);
    SetWindowSubclass(g_app.buttons[i], button_proc, 0, 0);
  }

  g_app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_app.wnd, NULL,
                                  GetModuleHandleW(NULL), NULL);
  for (int i = 0; i < NUM_PLAYBACK; i++) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_IDISHWND | TTF_SUBCLASS, .hwnd = g_app.wnd,
                      .uId = (UINT_PTR)g_app.buttons[i], .lpszText = (wchar_t *)labels[i]};
    SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  }

  g_app.sidebar = sidebar_create(g_app.wnd, ID_SIDEBAR);

  g_app.grid = album_grid_create(g_app.wnd, ID_GRID);
  refresh_library();

  g_app.tracks = child(WC_LISTBOXW, NULL, LBS_NOINTEGRALHEIGHT, ID_TRACKS);
  ShowWindow(g_app.tracks, SW_HIDE);

  import_view_create(g_app.wnd, g_app.lib);
}

static void on_command(int id) {
  switch (id) {
    case ID_IMPORT:
      show_import(TRUE);
      break;
    case ID_SETTINGS:
      change_library();
      break;
    default:
      import_view_on_command(id);
      break;
  }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT result;
  if (DwmDefWindowProc(wnd, msg, wp, lp, &result)) return result; /* the caption buttons, snap layouts */

  switch (msg) {
    case WM_CREATE:
      g_app.wnd = wnd;
      g_app.dpi = GetDpiForWindow(wnd);
      theme_apply_window(wnd, TRUE);
      create_controls();
      update_font();
      apply_theme();
      /* No focus rectangles until the keyboard is used (dialogs do this for themselves). */
      SendMessageW(wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
      /* Recalculates the frame, which drops the title bar (WM_NCCALCSIZE). */
      SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
    case WM_NCCALCSIZE:
      if (wp) {
        /* Keep the standard side and bottom borders, but no title bar: the client area goes
         * up to the top. A maximised window hangs off the screen by its border, so it's
         * inset by that much. */
        NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
        LONG top = p->rgrc[0].top;
        DefWindowProcW(wnd, msg, wp, lp);
        p->rgrc[0].top = top + (IsZoomed(wnd) ? frame_thickness() : 0);
        return 0;
      }
      break;
    case WM_NCHITTEST: {
      LRESULT hit = DefWindowProcW(wnd, msg, wp, lp);
      if (hit != HTCLIENT) return hit;
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(wnd, &pt);
      if (!IsZoomed(wnd) && pt.y < frame_thickness()) return HTTOP; /* the top border is in the client area */
      return pt.y < scale(BAR_H) ? HTCAPTION : HTCLIENT;           /* toolbar buttons are child windows */
    }
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
    case WM_ERASEBKGND: { /* also how themed child controls draw their parent's background */
      RECT area;
      if (GetClipBox((HDC)wp, &area) != ERROR) paint_background((HDC)wp, &area);
      return 1;
    }
    case WM_PRINTCLIENT: {
      RECT area;
      GetClientRect(wnd, &area);
      paint_background((HDC)wp, &area);
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint_background(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_DRAWITEM: {
      const DRAWITEMSTRUCT *dis = (const DRAWITEMSTRUCT *)lp;
      if (dis->CtlID == ID_SIDEBAR) sidebar_draw(dis);
      else if (dis->CtlID >= ID_PREV && dis->CtlID < ID_PREV + NUM_BUTTONS) draw_button(dis);
      return TRUE;
    }
    case WM_CTLCOLORLISTBOX: {
      HBRUSH brush = sidebar_ctlcolor((HWND)lp);
      return (LRESULT)(brush ? brush : theme_ctlcolor(msg, (HDC)wp));
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
      return (LRESULT)theme_ctlcolor(msg, (HDC)wp);
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update();
        apply_theme();
      }
      return DefWindowProcW(wnd, msg, wp, lp);
    case WM_COMMAND:
      on_command(LOWORD(wp));
      return 0;
    case WM_NOTIFY:
      import_view_on_notify((const NMHDR *)lp);
      return 0;
    case WM_DROPFILES:
      if (!g_app.importing) show_import(TRUE);
      import_view_drop((HDROP)wp);
      return 0;
    case WM_APP_IMPORT:
      import_view_on_event(wp, lp);
      return 0;
    case WM_APP_LIBRARY_CHANGED:
      refresh_library();
      return 0;
    case WM_APP_IMPORT_CLOSE:
      if (wp == IMPORT_CLOSE_CANCEL) import_view_cancel();
      if (wp == IMPORT_CLOSE_CANCEL || import_view_confirm_close()) {
        show_import(FALSE);
        refresh_library();
      }
      return 0;
    case WM_CLOSE:
      if (import_view_confirm_close()) DestroyWindow(wnd);
      return 0;
    case WM_DESTROY:
      import_view_destroy();
      canvas_free(&g_app.back);
      canvas_free(&g_app.button);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(wnd, msg, wp, lp);
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* Folders or files on the command line (including ones dropped onto the exe or a shortcut)
 * are imported straight away. */
static void import_args(void) {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!argv) return;
  char **paths = argc > 1 ? calloc((size_t)argc, sizeof *paths) : NULL;
  int n = 0;
  for (int i = 1; paths && i < argc; i++) {
    int len = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, NULL, 0, NULL, NULL);
    if (len > 0 && (paths[n] = malloc((size_t)len)))
      WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, paths[n++], len, NULL, NULL);
  }
  LocalFree(argv);
  if (n > 0) {
    show_import(TRUE);
    import_view_start((const char *const *)paths, n);
  }
  for (int i = 0; i < n; i++) free(paths[i]);
  free(paths);
}

/* --- Library location --- */

static char *utf8_from_wide(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = n > 0 ? malloc((size_t)n) : NULL;
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
}

static wchar_t *wide_from_utf8(const char *s) {
  int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
  return w;
}

/* %LOCALAPPDATA%\musicapp: where settings.json lives. */
static char *settings_dir(void) {
  wchar_t dir[MAX_PATH];
  DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
  if (n == 0 || n >= MAX_PATH - 16) return NULL;
  wcscat(dir, L"\\musicapp");
  return utf8_from_wide(dir);
}

/* The user's Music folder: the suggested library location. */
static char *music_folder(void) {
  wchar_t *path = NULL;
  char *out = NULL;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Music, 0, NULL, &path))) out = utf8_from_wide(path);
  CoTaskMemFree(path);
  return out;
}

/* Sync clients copy files while they're being written, which can corrupt the SQLite
 * database, and Windows often redirects Music into OneDrive. */
static BOOL in_onedrive(const char *dir) {
  static const wchar_t *const vars[] = {L"OneDrive", L"OneDriveConsumer", L"OneDriveCommercial"};
  wchar_t *wdir = wide_from_utf8(dir);
  BOOL inside = FALSE;
  for (size_t i = 0; wdir && !inside && i < sizeof vars / sizeof vars[0]; i++) {
    wchar_t root[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(vars[i], root, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) continue;
    inside = CompareStringOrdinal(wdir, (int)min(n, wcslen(wdir)), root, (int)n, TRUE) == CSTR_EQUAL &&
             (wdir[n] == L'\0' || wdir[n] == L'\\');
  }
  free(wdir);
  return inside;
}

/* Asks where the library should live. On first run current is NULL and the Music folder
 * is offered; otherwise the folder picker opens straight away. Returns a malloc'd UTF-8
 * path, or NULL if the user gave up. */
static char *choose_library_dir(HWND owner, const char *current) {
  char *suggested = current ? NULL : music_folder();
  for (;;) {
    char *dir = NULL;
    if (!current && suggested) {
      wchar_t *w = wide_from_utf8(suggested);
      wchar_t text[1024];
      swprintf(text, 1024,
               L"musicapp keeps your music in one library folder. Imported music is copied there, "
               L"organised by artist and album, along with the library database.\n\n"
               L"Use %ls?\n\nChoose No to pick a different folder.",
               w ? w : L"");
      free(w);
      int answer = MessageBoxW(owner, text, L"Welcome to musicapp", MB_YESNOCANCEL | MB_ICONQUESTION);
      if (answer == IDCANCEL) break;
      if (answer == IDYES && (dir = malloc(strlen(suggested) + 1))) strcpy(dir, suggested);
    }
    if (!dir) dir = pick_folder(owner, L"Choose Your Music Library Folder", current ? current : suggested);
    if (!dir) {
      if (current) break; /* changing location: cancel means keep the current one */
      continue;
    }
    if (in_onedrive(dir)) {
      int answer = MessageBoxW(
        owner,
        L"This folder is synced by OneDrive. Sync can damage the library database while musicapp "
        L"is writing to it.\n\nUse it anyway?",
        L"Library Folder", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
      if (answer != IDYES) {
        free(dir);
        continue;
      }
    }
    free(suggested);
    return dir;
  }
  free(suggested);
  return NULL;
}

static BOOL open_library(const char *dir) {
  mc_library *lib = NULL;
  mc_status st = mc_library_open(dir, &lib);
  if (st != MC_OK) {
    wchar_t *w = wide_from_utf8(dir);
    wchar_t text[1024];
    swprintf(text, 1024, L"Couldn't open the library in %ls (%hs).", w ? w : L"?", mc_status_str(st));
    free(w);
    MessageBoxW(g_app.wnd, text, L"musicapp", MB_OK | MB_ICONERROR);
    return FALSE;
  }
  mc_library_close(g_app.lib);
  g_app.lib = lib;
  return TRUE;
}

/* Settings button: show where the library is and offer to switch to another folder. */
static void change_library(void) {
  const char *current = mc_settings_library_dir(g_app.settings);
  wchar_t *w = wide_from_utf8(current ? current : "");
  wchar_t text[1200];
  swprintf(text, 1200,
           L"Library folder:\n%ls\n\nUse a different folder? Music you've imported stays where it "
           L"is; musicapp switches to the library in the new folder, or starts an empty one there.",
           w ? w : L"");
  free(w);
  if (MessageBoxW(g_app.wnd, text, L"Settings", MB_YESNO | MB_ICONINFORMATION) != IDYES) return;
  if (!import_view_confirm_close()) return;

  char *dir = choose_library_dir(g_app.wnd, current);
  if (dir && open_library(dir)) {
    mc_settings_set_library_dir(g_app.settings, dir);
    if (mc_settings_save(g_app.settings) != MC_OK)
      MessageBoxW(g_app.wnd, L"Couldn't save the setting; the old folder will be used next time.",
                  L"Settings", MB_OK | MB_ICONWARNING);
    import_view_set_library(g_app.lib);
    refresh_library();
  }
  free(dir);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;

  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                            ICC_PROGRESS_CLASS | ICC_BAR_CLASSES /* tooltips */};
  InitCommonControlsEx(&icc);
  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); /* folder picker */

  mc_init();
  theme_update();

  /* Settings say where the library folder is; the first run asks. */
  char *settings = settings_dir();
  if (!settings || mc_settings_load(settings, &g_app.settings) != MC_OK) {
    MessageBoxW(NULL, L"Couldn't find the folder for musicapp's settings (%LOCALAPPDATA%).",
                L"musicapp", MB_OK | MB_ICONERROR);
    return 1;
  }
  free(settings);
  if (!mc_settings_library_dir(g_app.settings)) {
    char *dir = choose_library_dir(NULL, NULL);
    if (!dir) return 0;
    mc_settings_set_library_dir(g_app.settings, dir);
    free(dir);
    if (mc_settings_save(g_app.settings) != MC_OK)
      MessageBoxW(NULL, L"Couldn't save your settings; you'll be asked again next time.",
                  L"musicapp", MB_OK | MB_ICONWARNING);
  }
  open_library(mc_settings_library_dir(g_app.settings)); /* on failure the window says so */

  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .lpfnWndProc = wnd_proc,
    .hInstance = inst,
    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
    .lpszClassName = L"musicapp", /* no background brush: WM_ERASEBKGND paints */
  };
  RegisterClassExW(&wc);

  HWND wnd = CreateWindowExW(0, L"musicapp", L"musicapp", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, NULL, NULL, inst, NULL);
  if (!wnd) return 1;
  DragAcceptFiles(wnd, TRUE);
  ShowWindow(wnd, show);
  import_args();

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  mc_library_close(g_app.lib);
  mc_settings_free(g_app.settings);
  mc_shutdown();
  CoUninitialize();
  if (g_app.font) DeleteObject(g_app.font);
  if (g_app.icon_font) DeleteObject(g_app.icon_font);
  return (int)msg.wParam;
}
