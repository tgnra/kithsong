/* Win32 frontend. Layout:
 *
 *   +--------------------------------------------------------------+
 *   | |<  >  >| [] 0:00 ==o------------------- -3:12        _ [] X |  toolbar = title bar
 *   +-----------+.-----------------------------------+-------------+
 *   | Albums    |                                    | track list  |
 *   | Artists   |          album grid                | (shown when |
 *   | Playlists |                                    |  an album   |
 *   | DEVICES   |                                    |  is picked) |
 *   |           |                                    |             |
 *   | Import    |                                    |             |
 *   | Settings  |                                    |             |
 *   +-----------+------------------------------------+-------------+
 *
 * The sidebar picks the page beside it. Import and Settings replace the album grid and track
 * list with the import view (import_view.c) or the settings view (settings_view.c). Once an
 * import starts, the other pages are greyed out until the user clicks Done. Folders dropped
 * anywhere on the window start an import.
 *
 * The toolbar is the title bar: WM_NCCALCSIZE removes the standard one, and the frame is
 * extended into the client area (DwmExtendFrameIntoClientArea) so Windows still draws the
 * minimise, maximise and close buttons. On Windows 11 the toolbar and sidebar show Mica
 * through the extended frame. Everything drawn there needs real alpha, which GDI doesn't
 * give, so it's drawn with canvas.c.
 *
 * The album grid is album_grid.c (which also shows the playlists, on the Playlists page), the
 * sidebar sidebar.c, the track list track_list.c and the seekbar beside the playback buttons
 * now_playing.c. The track list's Add to Playlist menu is add_to_playlist.c, and the New,
 * Rename and Delete Playlist dialogs playlist_dialogs.c; the track list asks for playlist
 * changes and they're made here. The toolbar buttons are owner-drawn here.
 *
 * Sync (sync_bridge.c) runs while a library is open, and stops before it closes, so switching
 * library restarts it. Its events arrive as WM_APP_SYNC. The sidebar's DEVICES section lists
 * the group's devices and the nearby ones, refreshed shortly after sync events and every 10
 * seconds (a rename elsewhere fires no event). A paired device's page is device_view.c, the
 * pairing dialogs pair_dialog.c, and the Add Device dialog (the + on the DEVICES heading),
 * which pairs by address, add_device.c. The sidebar's footer shows the version and this
 * PC's address.
 *
 * Command line: musicapp.exe [--config-dir <dir> | --config-dir=<dir>] [--] [paths...]
 * --config-dir keeps settings.json and sync.db somewhere other than %LOCALAPPDATA%\musicapp,
 * so two instances can sync on one PC; the paths are imported.
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

#include "add_device.h"
#include "add_to_playlist.h"
#include "album_grid.h"
#include "build_info.h"
#include "canvas.h"
#include "device_view.h"
#include "import_view.h"
#include "musiccore/musiccore.h"
#include "now_playing.h"
#include "pair_dialog.h"
#include "pick_folder.h"
#include "playback.h"
#include "playlist_dialogs.h"
#include "resource.h"
#include "settings_view.h"
#include "sidebar.h"
#include "sync_bridge.h"
#include "theme.h"
#include "track_list.h"

enum {
  ID_PREV = 100,
  ID_PLAY,
  ID_NEXT, /* last toolbar button */
  ID_SIDEBAR,
  ID_GRID,
  ID_TRACKS,
  ID_NOW_PLAYING,
  NUM_BUTTONS = ID_NEXT - ID_PREV + 1
};

/* Layout at 96 DPI. */
enum {
  BAR_H = 48,
  SIDE_W = 220,
  SIDE_TOP = 6, /* above the sidebar's first row */
  TRACKS_W = 320,
  PAD = 8,
  ICON_BTN = 36,
  NOW_PLAYING_GAP = 16, /* either side of the seekbar strip */
  NOW_PLAYING_MAX = 640,
};

/* The main window's timers. */
enum {
  TIMER_REFRESH_DEVICES = 0x5D01, /* one-shot: a sync event asked for a refresh */
  TIMER_POLL_DEVICES,             /* repeating: renames elsewhere fire no event */
  REFRESH_DEVICES_MS = 200,
  POLL_DEVICES_MS = 10000,
  MAX_SYNCING = 16, /* devices marked as syncing in the sidebar at once */
  MAX_ADDRESSES = 8,
};

typedef struct app {
  HWND wnd;
  HWND buttons[NUM_BUTTONS];
  HWND tooltip;
  HWND sidebar;
  HWND grid;
  HWND tracks;
  HWND now_playing;
  HFONT font, icon_font;
  HWND hot_button; /* toolbar button under the mouse */
  canvas back;     /* the window's own background, around the children */
  canvas button;   /* scratch for drawing buttons */
  UINT dpi;
  BOOL show_tracks;
  int tracks_w; /* while shown */
  sidebar_page page;
  mc_settings *settings;
  mc_library *lib;
  char *config_dir;        /* UTF-8: settings.json and sync.db */
  BOOL refresh_pending;    /* TIMER_REFRESH_DEVICES is set */
  char syncing[MAX_SYNCING][MC_UID_STRLEN]; /* devices with a session in progress */
} app;

static app g_app;

static void change_library(void);
static void schedule_refresh(void);
static void rename_self(const char *name);

static int scale(int px) {
  return MulDiv(px, (int)g_app.dpi, USER_DEFAULT_SCREEN_DPI);
}

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
  track_list_update(g_app.dpi);
  now_playing_update(g_app.dpi);
  import_view_set_font(font);
  settings_view_set_font(font, g_app.dpi);
  device_view_set_font(font, g_app.dpi);
  for (int i = 0; i < NUM_BUTTONS; i++) InvalidateRect(g_app.buttons[i], NULL, FALSE);

  if (g_app.font) DeleteObject(g_app.font);
  g_app.font = font;
}

/* Albums, Artists and Playlists all show the album grid: Playlists with the playlists in
 * it, the others the albums for now. */
static BOOL library_page(void) {
  return g_app.page != PAGE_IMPORT && g_app.page != PAGE_SETTINGS && g_app.page != PAGE_DEVICE;
}

static void layout(void) {
  RECT rc;
  GetClientRect(g_app.wnd, &rc);
  int w = rc.right, h = rc.bottom;
  int pad = scale(PAD), bar = scale(BAR_H), icon = scale(ICON_BTN);
  int side_w = scale(SIDE_W), side_top = scale(SIDE_TOP);
  /* The track list and its line take whole grid columns, so the albums keep their size as
   * it comes and goes. Too narrow for that, it falls back to its usual width. */
  int columns = album_grid_set_full_width(w - side_w, scale(TRACKS_W) + scale(1));
  g_app.tracks_w = columns ? columns - scale(1) : scale(TRACKS_W);
  int tracks_w = g_app.show_tracks ? g_app.tracks_w : 0;
  int grid_w = w - side_w - (tracks_w ? tracks_w + scale(1) : 0);

  HDWP dwp = BeginDeferWindowPos(NUM_BUTTONS + 4);
  int x = pad;
  for (int i = 0; i < NUM_BUTTONS; i++, x += icon + scale(2))
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, x, (bar - icon) / 2, icon, icon, SWP_NOZORDER);
  /* The seekbar takes the space between the playback and caption buttons, up to a point. */
  int right = w - caption_buttons_width();
  int np_left = x - scale(2) + scale(NOW_PLAYING_GAP), np_w = right - scale(NOW_PLAYING_GAP) - np_left;
  if (np_w > scale(NOW_PLAYING_MAX)) np_w = scale(NOW_PLAYING_MAX);
  dwp = DeferWindowPos(dwp, g_app.now_playing, NULL, np_left, 0, np_w > 0 ? np_w : 0, bar, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.sidebar, NULL, 0, bar + side_top, side_w, h - bar - side_top, SWP_NOZORDER);
  UINT library = library_page() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
  dwp = DeferWindowPos(dwp, g_app.grid, NULL, side_w, bar, grid_w, h - bar, SWP_NOZORDER | library);
  dwp = DeferWindowPos(dwp, g_app.tracks, NULL, w - tracks_w, bar, tracks_w, h - bar,
                       SWP_NOZORDER | (g_app.show_tracks && library_page() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  EndDeferWindowPos(dwp);

  /* The extended frame: under the toolbar, beside the sidebar, and down far enough for the
   * content area's rounded corner to show Mica. Anything else in that strip, like the track
   * list, must be drawn with alpha too. */
  MARGINS m = {theme.mica ? side_w : 0, 0, bar + (theme.mica ? scale(CONTENT_RADIUS) : 0), 0};
  DwmExtendFrameIntoClientArea(g_app.wnd, &m);
  InvalidateRect(g_app.wnd, NULL, TRUE);

  import_view_layout(side_w, bar, w - side_w, h - bar, g_app.dpi);
  settings_view_layout(side_w, bar, w - side_w, h - bar, g_app.dpi);
  device_view_layout(side_w, bar, w - side_w, h - bar, g_app.dpi);
}

/* The window's own areas, around its children: the toolbar and the sidebar column
 * (transparent over Mica, otherwise the frame colour) and the content background, which
 * shows behind the import and settings views. The album grid draws its own. */
static void paint_background(HDC dc, const RECT *area) {
  RECT client;
  GetClientRect(g_app.wnd, &client);
  canvas *c = &g_app.back;
  if (!canvas_reserve(c, client.right, client.bottom)) return;

  int w = client.right, h = client.bottom, bar = scale(BAR_H), side_w = scale(SIDE_W);
  int frame_alpha = theme.mica ? 0 : 255;
  RECT toolbar = {0, 0, w, bar}, side = {0, bar, side_w, h}, content = {side_w, bar, w, h}, r;
  if (IntersectRect(&r, &toolbar, area)) canvas_clear(c, &r, theme.frame, frame_alpha);
  if (IntersectRect(&r, &side, area)) canvas_clear(c, &r, theme.frame, frame_alpha);
  if (IntersectRect(&r, &content, area)) {
    /* The content area's border and rounded corner, as the album grid draws them. */
    int radius = scale(CONTENT_RADIUS);
    RECT border = {side_w, bar, w + radius, h + radius};
    canvas_clear(c, &r, theme.bg, 255);
    canvas_frame(c, &border, radius, 1, theme.line, 255);
    canvas_round_corners(c, &content, radius, CORNER_TL, theme.frame, frame_alpha);
  }
  if (g_app.show_tracks && library_page()) {
    RECT line = {w - g_app.tracks_w - scale(1), bar, w - g_app.tracks_w, h};
    if (IntersectRect(&r, &line, area)) canvas_clear(c, &r, theme.line, 255);
  }
  canvas_blit(c, dc, area->left, area->top, area);
}

/* For PAGE_DEVICE, the device selected in the sidebar (sidebar_select_device()) is shown. */
static void show_page(sidebar_page page) {
  g_app.page = page;
  if (page != PAGE_DEVICE) sidebar_select(page);
  if (library_page()) album_grid_show_playlists(page == PAGE_PLAYLISTS);
  if (page == PAGE_DEVICE) device_view_open(sync_bridge_get(), g_app.lib, sidebar_selected_device());
  import_view_show(page == PAGE_IMPORT);
  settings_view_show(page == PAGE_SETTINGS);
  device_view_show(page == PAGE_DEVICE);
  layout();
}

/* Previous and Next need a track to go from; Play needs one, or a shown album with something
 * to play. Disabled buttons are greyed out. */
static void update_buttons(void) {
  BOOL playable = playback_has_current();
  for (int i = 0, n = track_list_count(); !playable && i < n; i++) playable = track_list_item_at(i).path != NULL;
  EnableWindow(g_app.buttons[ID_PREV - ID_PREV], playback_has_current());
  EnableWindow(g_app.buttons[ID_PLAY - ID_PREV], playable);
  EnableWindow(g_app.buttons[ID_NEXT - ID_PREV], playback_has_next());
}

/* Shows the selected album's or playlist's tracks, or hides the track list when none is
 * selected. */
static void show_album(void) {
  char uid[MC_UID_STRLEN];
  const wchar_t *title = album_grid_selection(uid);
  track_list_show(g_app.lib, title ? uid : NULL, title, album_grid_showing_playlists());
  track_list_set_details(album_grid_selection_artist(), album_grid_selection_year(), album_grid_selection_cover());
  update_buttons();
  if (g_app.show_tracks != (title != NULL)) {
    g_app.show_tracks = title != NULL;
    layout();
  }
}

static void refresh_library(void) {
  album_grid_set_library(g_app.lib);
  show_album(); /* if the album went, the grid says so when it reloads */
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

/* The playback buttons are bare icons with a fill on hover. Drawn with alpha, as they sit
 * over Mica. */
static void draw_button(const DRAWITEMSTRUCT *dis) {
  static const wchar_t glyphs[NUM_BUTTONS] = {0xE892, 0xE768, 0xE893}; /* Previous, Play, Next */
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas *c = &g_app.button;
  if (!canvas_reserve(c, rc.right, rc.bottom)) return;
  canvas_clear(c, &rc, theme.frame, theme.mica ? 0 : 255);

  int index = (int)dis->CtlID - ID_PREV;
  BOOL pressed = (dis->itemState & ODS_SELECTED) != 0, disabled = (dis->itemState & ODS_DISABLED) != 0;
  control_state state = disabled ? STATE_NORMAL
                        : pressed ? STATE_PRESSED
                        : dis->hwndItem == g_app.hot_button ? STATE_HOT
                                                            : STATE_NORMAL;
  int radius = scale(6);
  COLORREF color = theme_draw_state(c, &rc, radius, state);
  if (disabled) color = theme.text3;
  wchar_t glyph = dis->CtlID == ID_PLAY && playback_is_playing() ? 0xE769 : glyphs[index]; /* Pause */
  canvas_text(c, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_app.icon_font,
              color);
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
  track_list_update(g_app.dpi);
  now_playing_update(g_app.dpi);
  album_grid_theme_changed();
  layout();
  RedrawWindow(g_app.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void create_controls(void) {
  /* The labels aren't shown; they name the buttons for tooltips and screen readers. */
  static const wchar_t *const labels[NUM_BUTTONS] = {L"Previous", L"Play", L"Next"};
  for (int i = 0; i < NUM_BUTTONS; i++) {
    g_app.buttons[i] = child(WC_BUTTONW, labels[i], BS_OWNERDRAW, ID_PREV + i);
    SetWindowSubclass(g_app.buttons[i], button_proc, 0, 0);
  }

  g_app.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_app.wnd, NULL,
                                  GetModuleHandleW(NULL), NULL);
  for (int i = 0; i < NUM_BUTTONS; i++) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_IDISHWND | TTF_SUBCLASS, .hwnd = g_app.wnd,
                      .uId = (UINT_PTR)g_app.buttons[i], .lpszText = (wchar_t *)labels[i]};
    SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  }

  g_app.sidebar = sidebar_create(g_app.wnd, ID_SIDEBAR);

  g_app.grid = album_grid_create(g_app.wnd, ID_GRID);
  g_app.tracks = track_list_create(g_app.wnd, ID_TRACKS);
  g_app.now_playing = now_playing_create(g_app.wnd, ID_NOW_PLAYING);
  refresh_library();

  import_view_create(g_app.wnd, g_app.lib);
  settings_view_create(g_app.wnd);
  settings_view_set_library_dir(mc_settings_library_dir(g_app.settings));
  settings_view_init_sync(g_app.settings, schedule_refresh);
  static const device_view_host host = {rename_self, schedule_refresh};
  device_view_create(g_app.wnd, &host);
}

/* Plays the shown album or playlist in order, from track start on. Tracks with no file on
 * this device are left out of the queue. */
static void play_album(int start) {
  int n = track_list_count(), count = 0, first = 0;
  playback_track *queue = n ? calloc((size_t)n, sizeof *queue) : NULL;
  if (!queue) return;
  for (int i = 0; i < n; i++) {
    track_list_item t = track_list_item_at(i);
    if (!t.path) continue;
    if (i <= start) first = count;
    queue[count++] = (playback_track){t.uid, t.path, t.title, t.artist, t.album, t.cover_dir, t.duration_ms};
  }
  if (count) playback_play(queue, count, first);
  free(queue);
}

/* The Play button turns into Pause while playing. */
static void playback_changed(void) {
  update_buttons();
  HWND play = g_app.buttons[ID_PLAY - ID_PREV];
  const wchar_t *label = playback_is_playing() ? L"Pause" : L"Play";
  SetWindowTextW(play, label); /* for screen readers */
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = g_app.wnd, .uId = (UINT_PTR)play, .lpszText = (wchar_t *)label};
  SendMessageW(g_app.tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
  InvalidateRect(play, NULL, FALSE);
}

/* The track list's requests to change a playlist (see track_list.h). */
static void on_track_request(int code) {
  const track_list_request *req = track_list_last_request();
  char uid[MC_UID_STRLEN];
  const wchar_t *name = album_grid_selection(uid); /* the shown playlist */
  BOOL changed = FALSE;
  switch (code) {
    case TLN_ADD_TO_PLAYLIST:
      changed = add_to_playlist(g_app.wnd, g_app.lib, req->track_uid, req->playlist_uid[0] ? req->playlist_uid : NULL);
      break;
    case TLN_REMOVE:
      changed = mc_playlist_remove(g_app.lib, req->entry_uid) == MC_OK;
      if (!changed) MessageBoxW(g_app.wnd, L"Couldn't remove the track.", L"Playlist", MB_OK | MB_ICONERROR);
      break;
    case TLN_MOVE:
      changed = mc_playlist_move(g_app.lib, req->entry_uid, req->after_uid[0] ? req->after_uid : NULL) == MC_OK;
      if (!changed) MessageBoxW(g_app.wnd, L"Couldn't move the track.", L"Playlist", MB_OK | MB_ICONERROR);
      break;
    case TLN_RENAME_PLAYLIST:
      if (name && !strcmp(uid, req->playlist_uid)) changed = playlist_rename_dialog(g_app.wnd, g_app.lib, uid, name);
      break;
    case TLN_DELETE_PLAYLIST:
      if (name && !strcmp(uid, req->playlist_uid)) changed = playlist_delete_dialog(g_app.wnd, g_app.lib, uid, name);
      break;
  }
  if (changed) {
    refresh_library();
    UpdateWindow(g_app.grid); /* reload the grid now: a deleted playlist's tracks go at once */
  }
}

/* --- Sync --- */

/* "address:port", with an IPv6 address in brackets. */
static void format_address(wchar_t *out, size_t cap, const char *host, unsigned port) {
  wchar_t *w = wide_from_utf8(host);
  BOOL v6 = strchr(host, ':') != NULL;
  swprintf(out, cap, v6 ? L"[%ls]:%u" : L"%ls:%u", w ? w : L"", port);
  free(w);
}

/* The sidebar's footer: the version, and where other devices reach this PC. */
static void update_footer(void) {
  wchar_t line1[160], line2[160];
  if (g_app.lib)
    swprintf(line1, 160, L"musicapp %hs (build %d) \x00B7 database v%d", mc_version(), MUSICAPP_BUILD_NUMBER,
             mc_library_schema_version(g_app.lib));
  else
    swprintf(line1, 160, L"musicapp %hs (build %d)", mc_version(), MUSICAPP_BUILD_NUMBER);

  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    swprintf(line2, 160, L"Sync off (%hs)", g_app.lib ? mc_status_str(sync_bridge_error()) : "no library");
  } else {
    char addrs[MAX_ADDRESSES][MC_SYNC_ADDR_STRLEN];
    unsigned port = mc_sync_port(sync);
    if (mc_sync_local_addresses(addrs, MAX_ADDRESSES) > 0) format_address(line2, 160, addrs[0], port);
    else swprintf(line2, 160, L"Port %u (no network)", port);
  }
  sidebar_set_footer(line1, line2);
}

/* Whether a sync session with uid is in progress, as far as the events have said. */
static int syncing_slot(const char *uid) {
  for (int i = 0; i < MAX_SYNCING; i++)
    if (g_app.syncing[i][0] && !strcmp(g_app.syncing[i], uid)) return i;
  return -1;
}

/* Marks uid as syncing or not; TRUE if that changed. */
static BOOL set_syncing(const char *uid, BOOL syncing) {
  int i = syncing_slot(uid);
  if (!syncing) {
    if (i >= 0) g_app.syncing[i][0] = '\0';
    return i >= 0;
  }
  if (i >= 0 || !uid[0]) return FALSE;
  for (i = 0; i < MAX_SYNCING; i++)
    if (!g_app.syncing[i][0]) {
      snprintf(g_app.syncing[i], MC_UID_STRLEN, "%s", uid);
      return TRUE;
    }
  return FALSE; /* too many at once: just not marked */
}

/* A group may rename this PC from another device, and this PC takes the name on; settings.json
 * keeps it for next time. A PC with no saved name goes by its host name, which isn't saved. */
static void save_own_name(mc_sync *sync) {
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  if (mc_sync_get_device_name(sync, name, sizeof name) != MC_OK || !name[0]) return;
  const char *saved = mc_settings_device_name(g_app.settings);
  if (saved && !strcmp(saved, name)) return;
  if (!saved) {
    wchar_t host[256];
    DWORD n = 256;
    char *h = GetComputerNameExW(ComputerNameDnsHostname, host, &n) ? utf8_from_wide(host) : NULL;
    BOOL is_host = h && !strcmp(h, name);
    free(h);
    if (is_host) return;
  }
  mc_settings_set_device_name(g_app.settings, name);
  mc_settings_save(g_app.settings); /* failing, it's saved on the next refresh */
}

/* Rebuilds the sidebar's devices from sync, and everything that shows them. */
static void refresh_devices(void) {
  KillTimer(g_app.wnd, TIMER_REFRESH_DEVICES);
  g_app.refresh_pending = FALSE;

  mc_sync *sync = sync_bridge_get();
  mc_sync_device_list *paired = NULL;
  mc_sync_nearby_list *nearby = NULL;
  if (sync && mc_sync_list_devices(sync, &paired) != MC_OK) paired = NULL;
  if (sync && mc_sync_list_nearby(sync, &nearby) != MC_OK) nearby = NULL;
  int np = paired ? paired->count : 0, nn = nearby ? nearby->count : 0;
  sidebar_device *devs = np + nn ? calloc((size_t)(np + nn), sizeof *devs) : NULL;
  wchar_t **names = np + nn ? calloc((size_t)(np + nn), sizeof *names) : NULL;
  int npaired = 0, nnearby = 0;
  if (devs && names) {
    /* This PC first, then the others in the order listed (by name); removed ones left out. */
    for (int pass = 0; pass < 2; pass++)
      for (int i = 0; i < np; i++) {
        const mc_sync_device *d = &paired->items[i];
        if (d->removed || (pass == 0) != (d->is_self != 0)) continue;
        sidebar_device *s = &devs[npaired];
        snprintf(s->uid, sizeof s->uid, "%s", d->uid);
        s->name = names[npaired] = wide_from_utf8(d->name ? d->name : "");
        s->kind = (int)d->kind;
        s->flags = (d->is_self ? SB_DEV_SELF : 0) | (d->online || d->is_self ? SB_DEV_ONLINE : 0) |
                   (d->archive ? SB_DEV_ARCHIVE : 0) | (syncing_slot(d->uid) >= 0 ? SB_DEV_SYNCING : 0);
        npaired++;
      }
    for (int i = 0; i < nn; i++) {
      const mc_sync_nearby *d = &nearby->items[i];
      sidebar_device *s = &devs[npaired + nnearby];
      snprintf(s->uid, sizeof s->uid, "%s", d->uid);
      s->name = names[npaired + nnearby] = wide_from_utf8(d->name ? d->name : "");
      s->kind = (int)d->kind;
      s->flags = SB_DEV_ONLINE | (d->in_other_group ? SB_DEV_OTHER_GROUP : 0);
      nnearby++;
    }
  }
  sidebar_set_devices(devs, npaired, devs ? devs + npaired : NULL, nnearby);
  for (int i = 0; names && i < npaired + nnearby; i++) free(names[i]);
  free(names);
  free(devs);
  mc_sync_device_list_free(paired);
  mc_sync_nearby_list_free(nearby);

  /* The shown device may have left the group (the sidebar then selects Albums). */
  if (g_app.page == PAGE_DEVICE) {
    if (sidebar_selection() != PAGE_DEVICE) show_page(PAGE_ALBUMS);
    else device_view_refresh();
  }
  if (sync) save_own_name(sync);
  settings_view_update_sync();
  update_footer();
}

/* Refreshes the devices shortly, once for a burst of sync events. */
static void schedule_refresh(void) {
  if (g_app.refresh_pending) return;
  g_app.refresh_pending = TRUE;
  SetTimer(g_app.wnd, TIMER_REFRESH_DEVICES, REFRESH_DEVICES_MS, NULL);
}

/* This PC renamed on its own device page: sync has the name; settings.json keeps it. */
static void rename_self(const char *name) {
  mc_settings_set_device_name(g_app.settings, name);
  if (mc_settings_save(g_app.settings) != MC_OK)
    MessageBoxW(g_app.wnd, L"Couldn't save the name; the old one will be used next time.", L"Settings",
                MB_OK | MB_ICONWARNING);
  schedule_refresh();
}

static void start_sync(void) {
  memset(g_app.syncing, 0, sizeof g_app.syncing);
  sync_bridge_start(g_app.wnd, g_app.lib, g_app.config_dir, mc_settings_device_name(g_app.settings));
  /* After a library switch on a device's page, it shows again on the new sync (or Albums, if
   * the refresh finds the device gone). */
  if (g_app.page == PAGE_DEVICE) device_view_open(sync_bridge_get(), g_app.lib, sidebar_selected_device());
  refresh_devices();
}

/* Before the library closes: sync reads it. Pairing dialogs go first, as they use sync. */
static void stop_sync(void) {
  pair_dialog_close_all();
  device_view_open(NULL, NULL, NULL);
  sync_bridge_stop();
  update_footer();
}

static void on_sync_event(WPARAM wp, LPARAM lp) {
  sync_event *ev = sync_bridge_receive(wp, lp);
  if (!ev) return;
  mc_sync *sync = sync_bridge_get();
  switch (ev->kind) {
    case SYNC_EV_PEER:
      schedule_refresh();
      device_view_on_sync_event(ev);
      break;
    case SYNC_EV_DONE:
      set_syncing(ev->uid, FALSE);
      schedule_refresh();
      device_view_on_sync_event(ev);
      break;
    case SYNC_EV_PAIR_CODE:
      pair_dialog_show_code(sync, ev);
      break;
    case SYNC_EV_PAIR_RESULT:
      if (pair_dialog_on_result(ev)) { /* paired: show the new device's page */
        refresh_devices();
        sidebar_select_device(ev->uid);
        if (sidebar_selected_device()) show_page(PAGE_DEVICE);
      }
      break;
    case SYNC_EV_PROGRESS:
      if (set_syncing(ev->uid, ev->phase != MC_SYNC_PHASE_DONE)) schedule_refresh();
      device_view_on_sync_event(ev);
      break;
  }
  free(ev);
}

static void sync_off_message(void) {
  wchar_t text[256];
  swprintf(text, 256, L"Sync is off, so this PC can't pair with other devices (%hs).",
           mc_status_str(sync_bridge_error()));
  MessageBoxW(g_app.wnd, text, L"Pair Device", MB_OK | MB_ICONINFORMATION);
}

/* After mc_sync_pair_request() or _address(): opens the enter-code dialog, or says why
 * pairing couldn't start. name names the other device, or is NULL. */
static void pair_started(mc_status st, const wchar_t *name) {
  wchar_t text[512];
  switch (st) {
    case MC_OK:
      pair_dialog_begin_enter(sync_bridge_get(), name);
      return;
    case MC_ERR_LOCKED:
      swprintf(text, 512, L"Already pairing with a device. Finish or cancel that first.");
      break;
    case MC_ERR_NOT_FOUND:
      swprintf(text, 512, L"%ls is no longer on the network.", name && *name ? name : L"The device");
      break;
    default:
      swprintf(text, 512, L"Couldn't start pairing (%hs).", mc_status_str(st));
      break;
  }
  MessageBoxW(g_app.wnd, text, L"Pair Device", MB_OK | MB_ICONINFORMATION);
}

/* A nearby device was clicked in the sidebar. */
static void pair_nearby(void) {
  const char *clicked = sidebar_clicked_nearby();
  mc_sync *sync = sync_bridge_get();
  if (!clicked) return;
  if (!sync) {
    sync_off_message();
    return;
  }
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", clicked); /* the sidebar's copy goes when it refreshes */
  wchar_t *name = NULL;
  BOOL other_group = FALSE;
  mc_sync_nearby_list *nearby = NULL;
  if (mc_sync_list_nearby(sync, &nearby) == MC_OK) {
    for (int i = 0; i < nearby->count; i++)
      if (!strcmp(nearby->items[i].uid, uid)) {
        name = wide_from_utf8(nearby->items[i].name ? nearby->items[i].name : "");
        other_group = nearby->items[i].in_other_group;
        break;
      }
    mc_sync_nearby_list_free(nearby);
  }
  if (other_group) {
    wchar_t text[512];
    swprintf(text, 512, L"%ls already syncs with other devices. To pair, it must leave its group first.",
             name && *name ? name : L"This device");
    MessageBoxW(g_app.wnd, text, L"Pair Device", MB_OK | MB_ICONINFORMATION);
  } else {
    pair_started(mc_sync_pair_request(sync, uid), name);
  }
  free(name);
}

/* The Add Device dialog closed with Connect. */
static void on_add_device_connect(void *user, const char *host, uint16_t port) {
  (void)user;
  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    sync_off_message();
    return;
  }
  wchar_t peer[300];
  format_address(peer, 300, host, port ? port : MC_SYNC_DEFAULT_PORT);
  pair_started(mc_sync_pair_request_address(sync, host, port), peer);
}

static void on_command(int id, int code) {
  switch (id) {
    case ID_SIDEBAR:
      if (code == LBN_SELCHANGE) show_page(sidebar_selection());
      else if (code == SBN_ADD_DEVICE) add_device_show(g_app.wnd, on_add_device_connect, NULL);
      else if (code == SBN_PAIR_DEVICE) pair_nearby();
      break;
    case ID_GRID:
      if (code == AGN_SELCHANGE) show_album();
      break;
    case ID_PREV:
      playback_previous();
      break;
    case ID_PLAY:
      if (!playback_toggle()) play_album(0); /* nothing queued yet: the album that's shown */
      break;
    case ID_NEXT:
      playback_next();
      break;
    case ID_TRACKS:
      if (code == TLN_PLAY) play_album(track_list_play_index());
      else if (code == TLN_CLOSE) { /* unselecting the album hides the list */
        album_grid_unselect();
        SetFocus(g_app.grid);
      } else on_track_request(code);
      break;
    case ID_NOW_PLAYING:
      if (code == NPN_SEEK) playback_seek(now_playing_position());
      break;
    default:
      if (!import_view_on_command(id) && !settings_view_on_command(id, code)) device_view_on_command(id, code);
      break;
  }
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT result;
  if (DwmDefWindowProc(wnd, msg, wp, lp, &result)) return result; /* the caption buttons, snap layouts */
  if (playback_window_message(msg, wp, lp, &result)) return result;   /* media keys, taskbar buttons */

  switch (msg) {
    case WM_CREATE:
      g_app.wnd = wnd;
      g_app.dpi = GetDpiForWindow(wnd);
      playback_init(wnd, playback_changed);
      theme_apply_window(wnd, TRUE);
      create_controls();
      update_font();
      apply_theme();
      /* No focus rectangles until the keyboard is used (dialogs do this for themselves). */
      SendMessageW(wnd, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS), 0);
      /* Recalculates the frame, which drops the title bar (WM_NCCALCSIZE). */
      SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      SetTimer(wnd, TIMER_POLL_DEVICES, POLL_DEVICES_MS, NULL);
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
      else device_view_draw_item(dis);
      return TRUE;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
      HBRUSH brush = device_view_ctlcolor(msg, (HWND)lp, (HDC)wp);
      if (!brush && msg == WM_CTLCOLORLISTBOX) brush = sidebar_ctlcolor((HWND)lp);
      if (!brush && msg == WM_CTLCOLORSTATIC) brush = settings_view_ctlcolor((HWND)lp, (HDC)wp);
      return (LRESULT)(brush ? brush : theme_ctlcolor(msg, (HDC)wp));
    }
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
      on_command(LOWORD(wp), HIWORD(wp));
      return 0;
    case WM_NOTIFY:
      if (device_view_on_notify((const NMHDR *)lp, &result)) return result;
      import_view_on_notify((const NMHDR *)lp);
      return 0;
    case WM_TIMER:
      if (wp == TIMER_REFRESH_DEVICES || wp == TIMER_POLL_DEVICES) {
        refresh_devices();
        return 0;
      }
      break;
    case WM_APP_SYNC:
      on_sync_event(wp, lp);
      return 0;
    case WM_DROPFILES:
      if (g_app.page != PAGE_IMPORT) show_page(PAGE_IMPORT);
      import_view_drop((HDROP)wp);
      return 0;
    case WM_APP_IMPORT:
      import_view_on_event(wp, lp);
      return 0;
    case WM_APP_LIBRARY_CHANGED:
      refresh_library();
      return 0;
    case WM_APP_IMPORT_CLOSE:
      if (import_view_confirm_close()) {
        show_page(PAGE_ALBUMS);
        refresh_library();
      }
      return 0;
    case WM_APP_IMPORT_SESSION:
      sidebar_set_locked((BOOL)wp);
      layout(); /* the import view switches between the drop zone and the list */
      return 0;
    case WM_APP_OPEN_LIBRARY:
      change_library();
      return 0;
    case WM_CLOSE:
      if (import_view_confirm_close()) DestroyWindow(wnd);
      return 0;
    case WM_DESTROY:
      KillTimer(wnd, TIMER_REFRESH_DEVICES);
      KillTimer(wnd, TIMER_POLL_DEVICES);
      stop_sync(); /* before the library closes, after the loop */
      playback_free();
      import_view_destroy();
      settings_view_destroy();
      device_view_destroy();
      canvas_free(&g_app.back);
      canvas_free(&g_app.button);
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(wnd, msg, wp, lp);
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* --- Command line --- */

typedef struct app_args {
  wchar_t **argv;              /* CommandLineToArgvW's, which the others point into */
  const wchar_t *config_dir;   /* --config-dir, or NULL */
  const wchar_t **paths;       /* to import */
  int npaths;
} app_args;

static const wchar_t usage[] =
  L"musicapp.exe [--config-dir <folder>] [--] [folders or files to import...]\n\n"
  L"--config-dir <folder>\n"
  L"    Keeps settings.json and sync.db in <folder> instead of %LOCALAPPDATA%\\musicapp, "
  L"so two copies of musicapp can run, and sync, on one PC.\n\n"
  L"Folders and files given are imported.";

/* Fills args from the command line. Returns -1 to carry on, or the exit code (after showing
 * the usage). Free with free_args(). */
static int parse_args(app_args *args) {
  memset(args, 0, sizeof *args);
  int argc = 0;
  args->argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (!args->argv) return -1;
  args->paths = argc > 1 ? calloc((size_t)argc, sizeof *args->paths) : NULL;
  BOOL options = TRUE;
  for (int i = 1; i < argc; i++) {
    const wchar_t *a = args->argv[i];
    if (options && a[0] == L'-' && a[1] == L'-') {
      if (!a[2]) {
        options = FALSE; /* "--": the rest are paths */
        continue;
      }
      if (!wcscmp(a, L"--help")) {
        MessageBoxW(NULL, usage, L"musicapp", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if (!wcscmp(a, L"--config-dir") && i + 1 < argc && args->argv[i + 1][0]) {
        args->config_dir = args->argv[++i];
        continue;
      }
      if (!wcsncmp(a, L"--config-dir=", 13) && a[13]) {
        args->config_dir = a + 13;
        continue;
      }
      MessageBoxW(NULL, usage, L"musicapp", MB_OK | MB_ICONWARNING); /* unknown, or no folder */
      return 2;
    }
    if (options && !wcscmp(a, L"/?")) {
      MessageBoxW(NULL, usage, L"musicapp", MB_OK | MB_ICONINFORMATION);
      return 0;
    }
    if (args->paths) args->paths[args->npaths++] = a;
  }
  return -1;
}

static void free_args(app_args *args) {
  free(args->paths);
  if (args->argv) LocalFree(args->argv);
  memset(args, 0, sizeof *args);
}

/* Where settings.json and sync.db live: --config-dir made absolute, or
 * %LOCALAPPDATA%\musicapp. Created if missing. Returns UTF-8, or NULL (*shown says which
 * folder it was, for the error). */
static char *config_dir(const app_args *args, wchar_t *shown, DWORD cap) {
  wchar_t dir[MAX_PATH];
  shown[0] = L'\0';
  if (args->config_dir) {
    DWORD n = GetFullPathNameW(args->config_dir, MAX_PATH, dir, NULL);
    lstrcpynW(shown, n && n < MAX_PATH ? dir : args->config_dir, (int)cap);
    if (n == 0 || n >= MAX_PATH) return NULL;
  } else {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) {
      lstrcpynW(shown, L"%LOCALAPPDATA%\\musicapp", (int)cap);
      return NULL;
    }
    wcscat(dir, L"\\musicapp");
    lstrcpynW(shown, dir, (int)cap);
  }
  int err = SHCreateDirectoryExW(NULL, dir, NULL);
  if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) return NULL;
  return utf8_from_wide(dir);
}

/* Folders or files on the command line (including ones dropped onto the exe or a shortcut)
 * are imported straight away. */
static void import_args(const app_args *args) {
  char **paths = args->npaths ? calloc((size_t)args->npaths, sizeof *paths) : NULL;
  int n = 0;
  for (int i = 0; paths && i < args->npaths; i++)
    if ((paths[n] = utf8_from_wide(args->paths[i]))) n++;
  if (n > 0) {
    show_page(PAGE_IMPORT);
    import_view_start((const char *const *)paths, n);
  }
  for (int i = 0; i < n; i++) free(paths[i]);
  free(paths);
}

/* --- Library location --- */

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

/* The library in dir, or NULL after saying why it couldn't be opened. */
static mc_library *open_library(const char *dir) {
  mc_library *lib = NULL;
  mc_status st = mc_library_open(dir, &lib);
  if (st != MC_OK) {
    wchar_t *w = wide_from_utf8(dir);
    wchar_t text[1024];
    swprintf(text, 1024, L"Couldn't open the library in %ls (%hs).", w ? w : L"?", mc_status_str(st));
    free(w);
    MessageBoxW(g_app.wnd, text, L"musicapp", MB_OK | MB_ICONERROR);
    return NULL;
  }
  return lib;
}

/* Settings' Open Another Library: switch to the library in another folder. The new one opens
 * first, so if it can't, nothing changes; sync then moves from the old one to it. */
static void change_library(void) {
  const char *current = mc_settings_library_dir(g_app.settings);
  if (!import_view_confirm_close()) return;

  char *dir = choose_library_dir(g_app.wnd, current);
  mc_library *lib = dir ? open_library(dir) : NULL;
  if (lib) {
    stop_sync();
    mc_library_close(g_app.lib);
    g_app.lib = lib;
    mc_settings_set_library_dir(g_app.settings, dir);
    if (mc_settings_save(g_app.settings) != MC_OK)
      MessageBoxW(g_app.wnd, L"Couldn't save the setting; the old folder will be used next time.",
                  L"Settings", MB_OK | MB_ICONWARNING);
    import_view_set_library(g_app.lib);
    settings_view_set_library_dir(dir);
    refresh_library();
    start_sync();
  }
  free(dir);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;

  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                            ICC_PROGRESS_CLASS | ICC_BAR_CLASSES /* tooltips */};
  InitCommonControlsEx(&icc);
  app_args args;
  int exit_code = parse_args(&args);
  if (exit_code >= 0) {
    free_args(&args);
    return exit_code;
  }

  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); /* folder picker */

  mc_init();
  theme_update();

  /* Settings say where the library folder is; the first run asks. */
  wchar_t shown_dir[MAX_PATH];
  g_app.config_dir = config_dir(&args, shown_dir, MAX_PATH);
  if (!g_app.config_dir || mc_settings_load(g_app.config_dir, &g_app.settings) != MC_OK) {
    wchar_t text[MAX_PATH + 128];
    swprintf(text, MAX_PATH + 128, L"Couldn't load musicapp's settings from %ls.", shown_dir);
    MessageBoxW(NULL, text, L"musicapp", MB_OK | MB_ICONERROR);
    free(g_app.config_dir);
    free_args(&args);
    return 1;
  }
  if (!mc_settings_library_dir(g_app.settings)) {
    char *dir = choose_library_dir(NULL, NULL);
    if (!dir) return 0;
    mc_settings_set_library_dir(g_app.settings, dir);
    free(dir);
    if (mc_settings_save(g_app.settings) != MC_OK)
      MessageBoxW(NULL, L"Couldn't save your settings; you'll be asked again next time.",
                  L"musicapp", MB_OK | MB_ICONWARNING);
  }
  g_app.lib = open_library(mc_settings_library_dir(g_app.settings)); /* on failure the window says so */

  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .lpfnWndProc = wnd_proc,
    .hInstance = inst,
    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
    .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)),
    .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                          GetSystemMetrics(SM_CYSMICON), 0),
    .lpszClassName = L"musicapp", /* no background brush: WM_ERASEBKGND paints */
  };
  RegisterClassExW(&wc);

  /* A config folder of its own is shown in the title, to tell instances apart. */
  wchar_t title[MAX_PATH + 32] = L"musicapp";
  if (args.config_dir) swprintf(title, MAX_PATH + 32, L"musicapp \x2014 %ls", shown_dir);
  HWND wnd = CreateWindowExW(0, L"musicapp", title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, NULL, NULL, inst, NULL);
  if (!wnd) return 1;
  DragAcceptFiles(wnd, TRUE);
  ShowWindow(wnd, show);
  pair_dialog_init(wnd);
  start_sync();
  import_args(&args);
  free_args(&args);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (pair_dialog_translate(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  mc_library_close(g_app.lib); /* sync stopped in WM_DESTROY */
  mc_settings_free(g_app.settings);
  free(g_app.config_dir);
  mc_shutdown();
  CoUninitialize();
  if (g_app.font) DeleteObject(g_app.font);
  if (g_app.icon_font) DeleteObject(g_app.icon_font);
  return (int)msg.wParam;
}
