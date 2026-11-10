/* Win32 frontend. Layout:
 *
 *   +--------------------------------------------------------------+
 *   | [K] |<  >  >| [] 0:00 ==o----------- -3:12 [S] [V]  _ [] X |  toolbar = title bar
 *   +-----------+.-----------------------------------+-------------+
 *   | Home      |                                    | track list  |
 *   | Search    |                                    | (shown when |
 *   |-----------|                                    |  an album   |
 *   | Albums    |                                    |  is picked) |
 *   | Artists   |          album grid                |             |
 *   | Playlists |                                    |             |
 *   | Album grp |                                    |             |
 *   | RECENTS   |                                    |             |
 *   | DEVICES   |                                    |             |
 *   |-----------|                                    |             |
 *   | Import    |                                    |             |
 *   | Settings  |                                    |             |
 *   +-----------+------------------------------------+-------------+
 *
 * The sidebar picks the page beside it. Its RECENTS are the albums and playlists last
 * played and the artists and album groups last opened, here or on another device (they sync
 * with the library); clicking one shows it on its page, and its right-click menu removes it
 * or clears them all. Home is rows of covers to scroll through (home_view.c, with what's in
 * them from core's home_picks.c). Artists lists the artists (artist_view.c); an artist's page is that view's
 * header, with the track list beneath showing the artist's tracks, and Back returns to the
 * list. Search,
 * Import and Settings replace the album grid and track list with the search page
 * (search_view.c), the import view (import_view.c) or the settings view (settings_view.c);
 * a search result opens on its page. Once an
 * import starts, the other pages are greyed out until the user clicks Done. Folders dropped
 * anywhere on the window start an import. Keyboard shortcuts (shortcut(), listed in
 * README.md) work from anywhere in the window, but leave text boxes their own keys.
 *
 * The toolbar is the title bar: WM_NCCALCSIZE removes the standard one, and the frame is
 * extended into the client area (DwmExtendFrameIntoClientArea) so Windows still draws the
 * minimise, maximise and close buttons. [K] is the Kithsong logo (paint_logo()), drawn on
 * the toolbar itself rather than as a control, so it's part of the title bar: it drags the
 * window and isn't a UI Automation element. On Windows 11 the toolbar and sidebar show Mica
 * through the extended frame. Everything drawn there needs real alpha, which GDI doesn't
 * give, so it's drawn with canvas.c.
 *
 * The album grid is album_grid.c (which also shows the playlists, on the Playlists page, and
 * the album groups, on the Album Groups page, making their changes itself), the
 * sidebar sidebar.c, the track list track_list.c and the seekbar beside the playback buttons
 * now_playing.c. A track's menu is track_menu.c, for the track list and, on its cover and
 * title, the playing track. Its Add to Playlist submenu is add_to_playlist.c, and the New,
 * Rename and Delete Playlist dialogs playlist_dialogs.c; the track list asks for playlist
 * changes (a smart playlist's too: its rule's editor is smart_editor.c) and they're made
 * here. The toolbar buttons, the shuffle toggle after the seekbar ([S]), the cast button
 * beside it while there's a Google Cast device to cast to (or a cast under way), whose
 * drop-down is cast_popup.c, the queue button after that, whose drop-down is queue_popup.c,
 * and the speaker next to it ([V]) are owner-drawn here; shuffle is kept in the settings, as
 * is the volume, which the speaker's drop-down (volume_popup.c) and the keyboard change.
 * While casting, the speaker and the volume keys are the device's volume (playback.h), and
 * the computer's own is left as it was.
 *
 * Sync (sync_bridge.c) runs while a library is open, and stops before it closes, so switching
 * library restarts it. Its events arrive as WM_APP_SYNC. While tracks wait for a device this PC
 * can't reach, a strip above the album grid says so (waiting_banner.c). The sidebar's DEVICES section lists
 * the group's devices and the nearby ones, refreshed shortly after sync events and every 10
 * seconds (a rename elsewhere fires no event). A paired device's page is device_view.c, the
 * pairing dialogs pair_dialog.c, and the Add Device dialog (the + on the DEVICES heading),
 * which pairs by address, add_device.c. The sidebar's footer shows the version and this
 * PC's address.
 *
 * Settings' Move Library is move_library.c: the main window hides while the library moves,
 * and comes back with the library open in its new folder (or its old one, if the move
 * failed).
 *
 * Command line: kithsong.exe [--config-dir <dir> | --config-dir=<dir>] [--demo-library]
 * [--window-size WxH] [--page <page>] [--demo-playing <spec>] [--] [paths...]
 * --config-dir keeps settings.json and sync.db somewhere other than %LOCALAPPDATA%\kithsong,
 * so two instances can sync on one PC; the paths are imported. --demo-library opens the demo
 * library (kithcore/demo.h) in <config dir>\demo-library, making it first, and refuses a
 * config folder whose settings name another library. --window-size sets the window's size as
 * it shows (the frame DWM draws, so a capture of it is that size), --page what it opens on:
 * home, albums, artists, album:<title>, artist:<name> or search:<text>, and with the demo
 * library import, import-review or sync, which show a made-up import or sync part-way through
 * (kithcore/demo.h). --demo-playing <album>#<track>@<m:ss> shows that track paused there. These
 * are for screenshots (scripts/store-screenshots), and the demo states run only with them. Run from a checkout, the app
 * has folders of its own (kithcore/source_run.h). Launching it again with the
 * same config folder brings the open window forward and hands it the paths instead, as
 * GApplication does on Linux (claim_instance). --add-firewall-rule or
 * --add-firewall-rule-public alone is firewall.c's elevated helper, which adds the Windows
 * Firewall rule sync needs and exits.
 */
#define _CRT_RAND_S /* rand_s(), for Shuffle Albums */
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

#include "kithcore/analysis.h"

#include "add_device.h"
#include "add_to_playlist.h"
#include "album_grid.h"
#include "artist_view.h"
#include "build_info.h"
#include "canvas.h"
#include "cast_popup.h"
#include "delete_dialog.h"
#include "prompt_dialog.h"
#include "remove_here.h"
#include "device_view.h"
#include "edit_dialog.h"
#include "firewall.h"
#include "first_run.h"
#include "home_view.h"
#include "import_view.h"
#include "move_library.h"
#include "kithcore/kithcore.h"
#include "kithcore/plugins.h"
#include "kithcore/source_run.h"
#include "now_playing.h"
#include "pair_dialog.h"
#include "pick_folder.h"
#include "playback.h"
#include "playlist_dialogs.h"
#include "resource.h"
#include "search_view.h"
#include "settings_view.h"
#include "analysis_dialog.h"
#include "sidebar.h"
#include "smart_editor.h"
#include "stats_dialog.h"
#include "sync_bridge.h"
#include "tethered_bridge.h"
#include "theme.h"
#include "track_list.h"
#include "track_menu.h"
#include "queue_popup.h"
#include "volume_popup.h"
#include "scroller.h"
#include "testing_view.h"
#include "waiting_banner.h"
#include "wstr.h"

enum {
  ID_PREV = 100,
  ID_PLAY,
  ID_NEXT, /* last toolbar button */
  ID_SIDEBAR,
  ID_GRID,
  ID_TRACKS,
  ID_NOW_PLAYING,
  ID_SEARCH,
  ID_ARTISTS,
  ID_SHUFFLE, /* after the seekbar */
  ID_CAST,    /* after shuffle, while shown: opens cast_popup.c */
  ID_QUEUE,   /* after shuffle and cast: opens queue_popup.c */
  ID_VOLUME,  /* after the queue: opens volume_popup.c */
  ID_WAITING, /* the waiting banner, above the album grid */
  ID_HOME,
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
  LOGO = ICON_BTN, /* the logo's square, as tall as the playback buttons */
  ICON_GAP = 2,         /* between neighbouring icon buttons */
  NOW_PLAYING_GAP = 16, /* between the playback buttons and the seekbar strip */
  SHUFFLE_GAP = 6,      /* between the seekbar strip and the shuffle button */
  NOW_PLAYING_MAX = 640 + 2 * (ICON_GAP + ICON_BTN), /* the strip, shuffle, the queue and the speaker; the cast
                                                       * button, when it shows, takes from the strip */
};

/* The main window's timers. */
enum {
  TIMER_REFRESH_DEVICES = 0x5D01, /* one-shot: a sync event asked for a refresh */
  TIMER_PERF_RUN = 0x5D0F,        /* one-shot: KITHSONG_PERF_RUN's scroll test starts */
  TIMER_POLL_DEVICES,             /* repeating: renames elsewhere fire no event */
  TIMER_REFRESH_LIBRARY,          /* one-shot: sync brought library rows or files */
  TIMER_SAVE_VOLUME,              /* one-shot: the volume changed, so a held key saves once */
  TIMER_DEBUG_PAIR,               /* debug builds: KITHSONG_DEBUG_PAIR, below */
  TIMER_ANALYSIS,                 /* repeating: what the audio analysis is up to */
  TIMER_CAST_PULSE,               /* repeating, while connecting: the cast button pulses */
  TIMER_START_PAGE,               /* repeating until it's open: --page's page */
  TIMER_DEMO_REVIEW,              /* one-shot: --page import-review's review dialog */
  REFRESH_DEVICES_MS = 200,
  START_PAGE_MS = 250,
  START_PAGE_TRIES = 40, /* ten seconds for the grid to have the album */
  SAVE_VOLUME_MS = 1000,
  VOLUME_STEP = 5, /* per press of a volume shortcut */
  SEEK_STEP_MS = 5000,
  REFRESH_LIBRARY_MS = 500,
  POLL_DEVICES_MS = 10000,
  CAST_PULSE_MS = 500,
  MAX_SYNCING = 16, /* devices marked as syncing in the sidebar at once */
  MAX_ADDRESSES = 8,
  MAX_RECENTS = 3, /* in the sidebar */
};

typedef struct app {
  HWND wnd;
  HWND buttons[NUM_BUTTONS];
  HWND shuffle; /* a toggle, drawn like the playback buttons */
  HWND cast;    /* opens the Cast drop-down, drawn like them too; shown while cast_shown */
  HWND queue;   /* opens the play queue, drawn like them too */
  HWND volume;  /* the speaker, drawn like them too */
  HWND tooltip;
  HWND sidebar;
  HWND grid;
  HWND waiting; /* above the grid while tracks wait for a device (waiting_banner.c) */
  HWND tracks;
  HWND now_playing;
  HWND search;
  HWND artists;
  HWND home;
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
  BOOL from_source;        /* running from a checkout (kithcore/source_run.h) */
  char *art_dir;           /* UTF-8: playlists' pictures, in config_dir */
  BOOL refresh_pending;    /* TIMER_REFRESH_DEVICES is set */
  BOOL library_pending;    /* TIMER_REFRESH_LIBRARY is set */
  BOOL volume_pending;     /* TIMER_SAVE_VOLUME is set */
  BOOL cast_shown;         /* the cast button: there are devices, a cast, or its drop-down is open */
  BOOL cast_pulse;         /* the connecting cast button's dim half of the pulse */
  BOOL locked;             /* an import session has the other pages greyed out */
  char syncing[MAX_SYNCING][MC_UID_STRLEN]; /* devices with a session in progress */
} app;

static app g_app;

static BOOL change_library(BOOL new_empty);
static void move_library(void);
static void schedule_refresh(void);
static void plugins_changed(void);
static void update_tethered_scrobbler(void);
static void rename_self(const char *name);
static void import_forwarded(wchar_t *list);
static BOOL open_start_page(const char *page);

/* --window-size, or 0: the frame as it shows, in pixels. */
static int g_window_w, g_window_h;
/* --page's page, kept for TIMER_START_PAGE: UTF-8, or NULL. */
static char *g_start_page;
static int g_start_page_tries;
/* --demo-playing's value, kept until the start page is open: UTF-8, or NULL. */
static char *g_demo_playing;

/* The window property holding its config folder's instance key (see claim_instance). */
#define INSTANCE_PROP L"Kithsong.Instance"
#define INSTANCE_PATHS 0x4B535041     /* WM_COPYDATA's dwData: paths, NUL-separated */
#define WM_APP_FORWARDED (WM_APP + 9) /* lParam: those paths, malloc'd and ending in "\0\0" */
#define WM_APP_SCROLL_TEST (WM_APP + 10) /* a scroll test's next frame */

static ULONG_PTR g_instance_key;

static int scale(int px) {
  return MulDiv(px, (int)g_app.dpi, USER_DEFAULT_SCREEN_DPI);
}

static char *utf8_from_wide(const wchar_t *w) {
  int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
  char *s = n > 0 ? malloc((size_t)n) : NULL;
  if (s) WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
  return s;
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
  waiting_banner_set_font(font, g_app.dpi);
  track_list_update(g_app.dpi);
  now_playing_update(g_app.dpi);
  search_view_update(g_app.dpi, font);
  artist_view_update(g_app.dpi, font);
  home_view_update(g_app.dpi, font);
  import_view_set_font(font);
  settings_view_set_font(font, g_app.dpi);
  device_view_set_font(font, g_app.dpi);
  for (int i = 0; i < NUM_BUTTONS; i++) InvalidateRect(g_app.buttons[i], NULL, FALSE);
  InvalidateRect(g_app.shuffle, NULL, FALSE);
  InvalidateRect(g_app.volume, NULL, FALSE);
  InvalidateRect(g_app.queue, NULL, FALSE);
  InvalidateRect(g_app.cast, NULL, FALSE);
  volume_popup_close(); /* it opens again at the new size */
  queue_popup_close();
  cast_popup_close();

  if (g_app.font) DeleteObject(g_app.font);
  g_app.font = font;
}

/* Albums, Playlists and Album Groups show the album grid: Playlists with the playlists in it,
 * Album Groups with the groups. */
static BOOL library_page(void) {
  return g_app.page == PAGE_ALBUMS || g_app.page == PAGE_PLAYLISTS || g_app.page == PAGE_ALBUM_GROUPS;
}

/* An artist's page is open on the Artists page: the track list is beneath its header. */
static BOOL artist_page(void) {
  return g_app.page == PAGE_ARTISTS && artist_view_artist();
}

/* Where the logo goes: at the start of the toolbar, before the playback buttons and level
 * with them. */
static RECT logo_rect(void) {
  int size = scale(LOGO), pad = scale(PAD), top = (scale(BAR_H) - size) / 2;
  RECT r = {pad, top, pad + size, top + size};
  return r;
}

/* The app icon (IDI_APP) as a size x size premultiplied, top-down image, for the logo: kept
 * until the size changes (the DPI does). LoadIconWithScaleDown() takes the .ico's nearest
 * bigger size and scales it down well. NULL if it can't be loaded. */
static const uint32_t *logo_image(int size) {
  static uint32_t *image;
  static int image_size;
  if (image && image_size == size) return image;
  free(image);
  image = NULL;
  image_size = size;
  HICON icon;
  if (FAILED(LoadIconWithScaleDown(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APP), size, size, &icon))) return NULL;
  ICONINFO info;
  if (GetIconInfo(icon, &info)) {
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = size, .biHeight = -size,
                                   .biPlanes = 1, .biBitCount = 32, .biCompression = BI_RGB}};
    image = malloc((size_t)size * (size_t)size * sizeof *image);
    HDC dc = GetDC(NULL);
    if (image && !GetDIBits(dc, info.hbmColor, 0, (UINT)size, image, &bi, DIB_RGB_COLORS)) {
      free(image);
      image = NULL;
    }
    ReleaseDC(NULL, dc);
    for (int i = 0; image && i < size * size; i++) { /* the icon's alpha is straight */
      uint32_t p = image[i], a = p >> 24;
      image[i] = a << 24 | ((p >> 16 & 255) * a / 255) << 16 | ((p >> 8 & 255) * a / 255) << 8 | (p & 255) * a / 255;
    }
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
  }
  DestroyIcon(icon);
  return image;
}

/* The Kithsong logo, in logo_rect(): the app icon at the DPI's size. */
static void paint_logo(canvas *c) {
  RECT r = logo_rect();
  int size = r.right - r.left;
  canvas_draw_image(c, &r, logo_image(size), size, size);
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

  HDWP dwp = BeginDeferWindowPos(NUM_BUTTONS + 13);
  int x = logo_rect().right + pad;
  for (int i = 0; i < NUM_BUTTONS; i++, x += icon + scale(ICON_GAP))
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, x, (bar - icon) / 2, icon, icon, SWP_NOZORDER);
  /* The seekbar, with the shuffle, cast (while shown), queue and speaker buttons after it, takes
   * the space between the playback buttons and the caption buttons, up to a point, centred in
   * the window where that leaves it clear of both. The cast button's room comes from the strip. */
  int min_left = x - scale(ICON_GAP) + scale(NOW_PLAYING_GAP);
  int max_right = w - caption_buttons_width() - pad;
  int np_w = max_right - min_left;
  if (np_w > scale(NOW_PLAYING_MAX)) np_w = scale(NOW_PLAYING_MAX);
  int np_left = (w - np_w) / 2;
  if (np_left + np_w > max_right) np_left = max_right - np_w;
  if (np_left < min_left) np_left = min_left;
  int after = g_app.cast_shown ? 4 : 3; /* buttons after the strip */
  int strip_w = max(0, np_w - after * icon - scale(SHUFFLE_GAP) - (after - 1) * scale(ICON_GAP));
  int button_x = np_left + strip_w + scale(SHUFFLE_GAP), button_y = (bar - icon) / 2;
  dwp = DeferWindowPos(dwp, g_app.now_playing, NULL, np_left, 0, strip_w, bar, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.shuffle, NULL, button_x, button_y, icon, icon, SWP_NOZORDER);
  button_x += icon + scale(ICON_GAP);
  dwp = DeferWindowPos(dwp, g_app.cast, NULL, button_x, button_y, icon, icon,
                       SWP_NOZORDER | (g_app.cast_shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  if (g_app.cast_shown) button_x += icon + scale(ICON_GAP);
  dwp = DeferWindowPos(dwp, g_app.queue, NULL, button_x, button_y, icon, icon, SWP_NOZORDER);
  button_x += icon + scale(ICON_GAP);
  dwp = DeferWindowPos(dwp, g_app.volume, NULL, button_x, button_y, icon, icon, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.sidebar, NULL, 0, bar + side_top, side_w, h - bar - side_top, SWP_NOZORDER);
  UINT library = library_page() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
  /* Tracks waiting for a device: a strip over the grid, beside the track list. */
  int waiting_h = library_page() && waiting_banner_shown() ? min(waiting_banner_height(), h - bar) : 0;
  dwp = DeferWindowPos(dwp, g_app.waiting, NULL, side_w, bar, grid_w, waiting_h,
                       SWP_NOZORDER | (waiting_h ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  album_grid_set_below_banner(waiting_h > 0);
  dwp = DeferWindowPos(dwp, g_app.grid, NULL, side_w, bar + waiting_h, grid_w, h - bar - waiting_h,
                       SWP_NOZORDER | library);
  int artist_h = artist_page() ? min(artist_view_header_height(), h - bar) : h - bar;
  dwp = DeferWindowPos(dwp, g_app.artists, NULL, side_w, bar, w - side_w, artist_h,
                       SWP_NOZORDER | (g_app.page == PAGE_ARTISTS ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  if (artist_page())
    dwp = DeferWindowPos(dwp, g_app.tracks, NULL, side_w, bar + artist_h, w - side_w, h - bar - artist_h,
                         SWP_NOZORDER | SWP_SHOWWINDOW);
  else
    dwp = DeferWindowPos(dwp, g_app.tracks, NULL, w - tracks_w, bar, tracks_w, h - bar,
                         SWP_NOZORDER | (g_app.show_tracks && library_page() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  dwp = DeferWindowPos(dwp, g_app.search, NULL, side_w, bar, w - side_w, h - bar,
                       SWP_NOZORDER | (g_app.page == PAGE_SEARCH ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  dwp = DeferWindowPos(dwp, g_app.home, NULL, side_w, bar, w - side_w, h - bar,
                       SWP_NOZORDER | (g_app.page == PAGE_HOME ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
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
  if (IntersectRect(&r, &toolbar, area)) {
    canvas_clear(c, &r, theme.frame, frame_alpha);
    RECT logo = logo_rect();
    if (IntersectRect(&r, &logo, area)) paint_logo(c);
  }
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

static void show_album(void);
static void show_about(void);
static void analysis_update_settings(void);

/* What the profiler calls a page. */
static const char *page_name(sidebar_page page) {
  switch (page) {
    case PAGE_ALBUMS: return "albums";
    case PAGE_ARTISTS: return "artists";
    case PAGE_PLAYLISTS: return "playlists";
    case PAGE_ALBUM_GROUPS: return "album_groups";
    case PAGE_SEARCH: return "search";
    case PAGE_SETTINGS: return "settings";
    default: return "other";
  }
}

/* A view switch under way ends once what it changed is painted, which is made to happen now:
 * only while profiling. */
static void perf_view_shown(void) {
  if (!mc_perf_view_pending()) return;
  RedrawWindow(g_app.wnd, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
  mc_perf_view_end();
}

/* The page of the device selected in the sidebar: a group member's, or a tethered device's. */
static void open_device_page(void) {
  const char *uid = sidebar_selected_device();
  if (uid && tethered_bridge_has(uid)) device_view_open_tethered(tethered_bridge_get(), g_app.lib, uid);
  else device_view_open(sync_bridge_get(), g_app.lib, uid);
}

/* For PAGE_DEVICE, the device selected in the sidebar (sidebar_select_device()) is shown. */
static void show_page(sidebar_page page) {
  mc_perf_view_begin(page_name(page));
  g_app.page = page;
  if (page != PAGE_DEVICE) sidebar_select(page);
  if (library_page())
    album_grid_show(page == PAGE_PLAYLISTS ? AGV_PLAYLISTS : page == PAGE_ALBUM_GROUPS ? AGV_GROUPS : AGV_ALBUMS);
  if (library_page() || page == PAGE_ARTISTS) show_album(); /* the track list is the page's */
  if (page == PAGE_DEVICE) open_device_page();
  if (page == PAGE_HOME) home_view_refresh(); /* plays since it last showed */
  import_view_show(page == PAGE_IMPORT);
  settings_view_show(page == PAGE_SETTINGS);
  analysis_update_settings();
  device_view_show(page == PAGE_DEVICE);
  layout();
  perf_view_shown();
}

/* Previous needs a track to go from, and Next one or the queue; Play needs one, the queue, or a
 * shown album with something to play. Disabled buttons are greyed out. */
static void update_buttons(void) {
  BOOL playable = playback_has_current() || playback_up_next_count() > 0;
  for (int i = 0, n = track_list_count(); !playable && i < n; i++) playable = track_list_item_at(i).path != NULL;
  EnableWindow(g_app.buttons[ID_PREV - ID_PREV], playback_has_current());
  EnableWindow(g_app.buttons[ID_PLAY - ID_PREV], playable);
  EnableWindow(g_app.buttons[ID_NEXT - ID_PREV], playback_has_next());
}

/* Shows the selected album's or playlist's tracks, or hides the track list when none is
 * selected. On the Artists page, the open artist's tracks instead. */
static void show_album(void) {
  if (g_app.page == PAGE_ARTISTS) {
    track_list_show_artist(g_app.lib, artist_view_artist());
    update_buttons();
    return;
  }
  char uid[MC_UID_STRLEN];
  const wchar_t *title = album_grid_selection(uid);
  BOOL random;
  if (title && album_grid_selection_smart(&random)) track_list_show_smart(g_app.lib, uid, title, random);
  else track_list_show(g_app.lib, title ? uid : NULL, title, album_grid_showing_playlists());
  track_list_set_details(album_grid_selection_artist(), album_grid_selection_year(), album_grid_selection_mix(),
                         album_grid_selection_cover());
  update_buttons();
  if (g_app.show_tracks != (title != NULL)) {
    g_app.show_tracks = title != NULL;
    layout();
  }
}

/* The sidebar's recents: the albums, playlists and stations last played and the artists and
 * album groups last opened, on any device, smart playlists and groups included. */
static void refresh_recents(void) {
  mc_recent_list *list = NULL;
  if (!g_app.lib || mc_library_list_recents(g_app.lib, 0, MC_LIST_PLAYABLE | MC_LIST_STATIONS, &list) != MC_OK)
    list = NULL;
  sidebar_recent items[MAX_RECENTS];
  wchar_t *names[MAX_RECENTS], *details[MAX_RECENTS]; /* an album's artist, what a station's like */
  int n = 0;
  for (int i = 0; list && i < list->count && n < MAX_RECENTS; i++) {
    const mc_recent_info *r = &list->items[i];
    BOOL album = r->kind == MC_RECENT_ALBUM, station = r->kind == MC_RECENT_STATION;
    names[n] = wstr_dup_from_utf8(r->name ? r->name : "");
    details[n] = album     ? wstr_dup_from_utf8(r->artist ? r->artist : mc_str(MC_STR_UNKNOWN_ARTIST))
                 : station ? wstr_dup_from_utf8(r->sub ? r->sub : mc_str(MC_STR_HOME_STATION_SUB))
                           : NULL;
    const wchar_t *kind = mc_wstr(r->kind == MC_RECENT_ARTIST     ? MC_STR_ARTIST
                                  : r->kind == MC_RECENT_PLAYLIST ? (r->is_smart ? MC_STR_SMART_PLAYLIST : MC_STR_PLAYLIST)
                                  : r->is_smart                   ? MC_STR_SMART_ALBUM_GROUP
                                                                  : MC_STR_ALBUM_GROUP);
    items[n] = (sidebar_recent){.kind = r->kind, .smart = r->is_smart, .name = names[n], .detail = details[n] ? details[n] : kind};
    snprintf(items[n].uid, sizeof items[n].uid, "%s", r->uid);
    n++;
  }
  mc_recent_list_free(list);
  sidebar_set_recents(items, n);
  home_view_refresh();
  for (int i = 0; i < n; i++) {
    free(names[i]);
    free(details[i]);
  }
}

/* An artist's page has opened: it goes first in the recents, and its tracks in the list. */
static void artist_opened(void) {
  if (g_app.lib && mc_library_record_opened(g_app.lib, MC_RECENT_ARTIST, artist_view_artist()) == MC_OK)
    refresh_recents();
  show_album();
  layout();
}

/* The Artists page, with the page of the artist with uid open if it's there. */
static void open_artist(const char *uid) {
  show_page(PAGE_ARTISTS);
  if (artist_view_open(uid)) artist_opened();
}

/* The Albums page with the album with uid selected, and in the track list its track
 * track_uid if that isn't NULL, as clicking an album on an artist's page or Go to Album on a
 * track's menu opens it. */
static void open_album(const char *uid, const char *track_uid) {
  show_page(PAGE_ALBUMS);
  /* The track list comes in now, narrowing the grid, which drops a scroll under way;
   * selecting again scrolls the album into view at the grid's new width. */
  if (!album_grid_select(uid)) return;
  show_album();
  album_grid_select(uid);
  if (track_uid) {
    track_list_select(track_uid);
    SetFocus(g_app.tracks);
  }
}

/* Item uid, of kind: its page, with it selected. */
static void open_item(mc_recent_kind kind, const char *uid) {
  if (kind == MC_RECENT_ARTIST) {
    open_artist(uid);
    return;
  }
  if (kind == MC_RECENT_ALBUM_GROUP) {
    show_page(PAGE_ALBUM_GROUPS);
    album_grid_open_group(uid);
    return;
  }
  show_page(kind == MC_RECENT_PLAYLIST ? PAGE_PLAYLISTS : PAGE_ALBUMS);
  album_grid_select(uid);
}

/* A recent was clicked in the sidebar. A station has no page, so it plays. */
static void open_recent(void) {
  mc_recent_kind kind;
  const char *clicked = sidebar_clicked_recent(&kind);
  if (!clicked) return;
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", clicked); /* the sidebar's copy goes when the recents refresh */
  if (kind == MC_RECENT_STATION) playback_play_radio(uid);
  else open_item(kind, uid);
}

/* A tile was clicked on the Home page. */
static void open_home_tile(void) {
  mc_recent_kind kind;
  const char *clicked = home_view_clicked(&kind);
  if (!clicked) return;
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", clicked);
  if (kind == MC_RECENT_ALBUM) open_album(uid, NULL);
  else open_item(kind, uid);
}

/* Remove from Recents, of clicked, or with all Clear Recents, on a recent's menu in the sidebar
 * or on the Home page. */
static void remove_recents(const char *clicked, BOOL all) {
  if (!g_app.lib || (!all && !clicked)) return;
  mc_status st = all ? mc_library_clear_recents(g_app.lib) : mc_library_remove_recent(g_app.lib, clicked);
  if (st == MC_OK || st == MC_ERR_NOT_FOUND) refresh_recents(); /* gone already: sync took it */
}

/* After the library changed: an import, a playlist edit, or rows from sync, which may have
 * removed the shown album or a recent, or brought recents from other devices. */
static void refresh_library(void) {
  album_grid_set_library(g_app.lib);
  artist_view_set_library(g_app.lib); /* if the open artist went, it goes back (ARN_BACK) */
  home_view_set_library(g_app.lib);
  show_album(); /* if the album went, the grid says so when it reloads */
  refresh_recents();
  if (g_app.lib) mc_library_search_prepare(g_app.lib); /* the index is ready before the user types */
  search_view_set_library(g_app.lib);
  stats_dialog_refresh();
}

/* Edit... on an album's or artist's recent in the sidebar. */
static void edit_recent(void) {
  mc_recent_kind kind = MC_RECENT_ALBUM;
  const char *clicked = sidebar_clicked_recent(&kind);
  if (!g_app.lib || !clicked) return;
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", clicked); /* the sidebar's copy goes when the recents refresh */
  BOOL changed = kind == MC_RECENT_ARTIST ? edit_dialog_artist(g_app.wnd, g_app.lib, uid)
                                          : edit_dialog_album(g_app.wnd, g_app.lib, uid);
  if (changed) refresh_library();
}

/* A search result was opened: an artist, album, playlist or album group on its page, a track
 * on its album with the track selected. */
static void open_search_result(void) {
  search_view_hit hit = *search_view_opened();
  if (hit.kind == MC_SEARCH_ARTIST) {
    open_artist(hit.uid);
    return;
  }
  if (hit.kind == MC_SEARCH_ALBUM_GROUP) {
    show_page(PAGE_ALBUM_GROUPS);
    album_grid_open_group(hit.uid);
    return;
  }
  if (hit.kind == MC_SEARCH_PLAYLIST) {
    show_page(PAGE_PLAYLISTS);
    album_grid_select(hit.uid);
    return;
  }
  show_page(PAGE_ALBUMS);
  if (!album_grid_select(hit.kind == MC_SEARCH_TRACK ? hit.album_uid : hit.uid)) return;
  if (hit.kind == MC_SEARCH_TRACK) {
    show_album(); /* now, rather than when the grid's notification arrives */
    track_list_select(hit.uid);
    SetFocus(g_app.tracks);
  }
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

/* The playback buttons, and the shuffle and speaker buttons, are bare icons with a fill on
 * hover. Drawn with alpha, as they sit over Mica. Shuffle, while on, is in the accent colour
 * with a dot under it, as Windows' Media Player shows its toggles, and so is the cast button
 * while casting; while connecting, it pulses between the accent colour and dim. The speaker
 * shows the volume (volume_popup_glyph()); it, the queue and cast have the selected fill
 * while their drop-downs are open. */
static void draw_button(const DRAWITEMSTRUCT *dis) {
  static const wchar_t glyphs[NUM_BUTTONS] = {0xE892, 0xE768, 0xE893}; /* Previous, Play, Next */
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas *c = &g_app.button;
  if (!canvas_reserve(c, rc.right, rc.bottom)) return;
  canvas_clear(c, &rc, theme.frame, theme.mica ? 0 : 255);

  BOOL shuffle = dis->CtlID == ID_SHUFFLE, cast = dis->CtlID == ID_CAST;
  BOOL connecting = cast && playback_cast_status()->state == MC_CAST_CONNECTING;
  BOOL on = (shuffle && playback_shuffle()) || (cast && playback_casting()) || (connecting && !g_app.cast_pulse);
  BOOL volume = dis->CtlID == ID_VOLUME, queue = dis->CtlID == ID_QUEUE;
  BOOL pressed = (dis->itemState & ODS_SELECTED) != 0, disabled = (dis->itemState & ODS_DISABLED) != 0;
  BOOL open = (volume && volume_popup_is_open()) || (queue && queue_popup_is_open()) || (cast && cast_popup_is_open());
  control_state state = disabled ? STATE_NORMAL
                        : pressed ? STATE_PRESSED
                        : open ? STATE_SELECTED
                        : dis->hwndItem == g_app.hot_button ? STATE_HOT
                                                            : STATE_NORMAL;
  int radius = scale(6);
  COLORREF color = theme_draw_state(c, &rc, radius, state);
  if (disabled) color = theme.text3;
  else if (on && state != STATE_PRESSED) color = theme.accent;
  else if (connecting && state == STATE_NORMAL) color = theme.text3; /* the pulse's dim half */
  wchar_t glyph = shuffle ? 0xE8B1                                         /* Shuffle */
                  : cast ? 0xEC15 /* MiracastLogoSmall: a screen with waves, for Cast */
                  : queue ? 0xE8FD /* BulletedList */
                  : volume ? volume_popup_glyph()
                  : dis->CtlID == ID_PLAY && playback_is_playing() ? 0xE769 /* Pause */
                                                                   : glyphs[dis->CtlID - ID_PREV];
  canvas_text(c, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_app.icon_font,
              color);
  if (on && !connecting) {
    int dot = scale(4);
    RECT r = {(rc.right - dot) / 2, rc.bottom - scale(6) - dot / 2, (rc.right - dot) / 2 + dot, 0};
    r.bottom = r.top + dot;
    canvas_fill(c, &r, dot / 2, color, 255);
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
  track_list_update(g_app.dpi);
  now_playing_update(g_app.dpi);
  search_view_update(g_app.dpi, g_app.font);
  artist_view_update(g_app.dpi, g_app.font);
  home_view_update(g_app.dpi, g_app.font);
  album_grid_theme_changed();
  waiting_banner_theme_changed();
  device_view_theme_changed();
  volume_popup_close(); /* it opens again in the new colours */
  queue_popup_close();
  cast_popup_close();
  layout();
  RedrawWindow(g_app.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void set_button_label(HWND button, enum mc_str_id label, enum mc_str_id shortcut);
static void open_stress(const char *dir);
static void back_to_library(void);
static void scroll_test_start(void);
static void scroll_test_frame(void);

static mc_library *current_library(void) {
  return g_app.lib;
}

static void create_controls(void) {
  /* The labels aren't shown; they name the buttons for screen readers, and with their
   * shortcuts are the tooltips (set_button_label()). */
  const wchar_t *const labels[NUM_BUTTONS] = {mc_wstr(MC_STR_PREVIOUS_TRACK), mc_wstr(MC_STR_PLAY),
                                              mc_wstr(MC_STR_NEXT_TRACK)};
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
  /* Its name and tooltip say whether it's on (see playback_changed()), as an owner-drawn
   * button can't be a checkbox. */
  g_app.shuffle = child(WC_BUTTONW, mc_wstr(MC_STR_SHUFFLE_OFF), BS_OWNERDRAW, ID_SHUFFLE);
  SetWindowSubclass(g_app.shuffle, button_proc, 0, 0);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_IDISHWND | TTF_SUBCLASS, .hwnd = g_app.wnd,
                    .uId = (UINT_PTR)g_app.shuffle, .lpszText = (wchar_t *)mc_wstr(MC_STR_SHUFFLE_OFF)};
  SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  set_button_label(g_app.buttons[ID_PREV - ID_PREV], MC_STR_PREVIOUS_TRACK, MC_STR_WINDOWS_SHORTCUT_PREVIOUS);
  set_button_label(g_app.buttons[ID_NEXT - ID_PREV], MC_STR_NEXT_TRACK, MC_STR_WINDOWS_SHORTCUT_NEXT);
  set_button_label(g_app.buttons[ID_PLAY - ID_PREV], MC_STR_PLAY, MC_STR_WINDOWS_SHORTCUT_PLAY_PAUSE);
  set_button_label(g_app.shuffle, MC_STR_SHUFFLE_OFF, MC_STR_WINDOWS_SHORTCUT_SHUFFLE);
  /* Hidden until there's a device (layout()). Its name and tooltip say whether it's casting,
   * and to what (update_cast_button()). */
  g_app.cast = child(WC_BUTTONW, mc_wstr(MC_STR_CAST), BS_OWNERDRAW, ID_CAST);
  ShowWindow(g_app.cast, SW_HIDE);
  SetWindowSubclass(g_app.cast, button_proc, 0, 0);
  ti.uId = (UINT_PTR)g_app.cast;
  ti.lpszText = (wchar_t *)mc_wstr(MC_STR_CAST);
  SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  g_app.queue = child(WC_BUTTONW, mc_wstr(MC_STR_QUEUE), BS_OWNERDRAW, ID_QUEUE);
  SetWindowSubclass(g_app.queue, button_proc, 0, 0);
  ti.uId = (UINT_PTR)g_app.queue;
  ti.lpszText = (wchar_t *)mc_wstr(MC_STR_QUEUE);
  SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  /* Its name and tooltip stay "Volume"; the drop-down says the level. */
  g_app.volume = child(WC_BUTTONW, mc_wstr(MC_STR_VOLUME), BS_OWNERDRAW, ID_VOLUME);
  SetWindowSubclass(g_app.volume, button_proc, 0, 0);
  ti.uId = (UINT_PTR)g_app.volume;
  ti.lpszText = (wchar_t *)mc_wstr(MC_STR_VOLUME);
  SendMessageW(g_app.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

  g_app.sidebar = sidebar_create(g_app.wnd, ID_SIDEBAR);
  sidebar_set_library_source(current_library);

  g_app.grid = album_grid_create(g_app.wnd, ID_GRID);
  album_grid_set_playlist_art_dir(g_app.art_dir);
  g_app.waiting = waiting_banner_create(g_app.wnd, ID_WAITING);
  g_app.tracks = track_list_create(g_app.wnd, ID_TRACKS);
  g_app.now_playing = now_playing_create(g_app.wnd, ID_NOW_PLAYING);
  g_app.search = search_view_create(g_app.wnd, ID_SEARCH);
  g_app.artists = artist_view_create(g_app.wnd, ID_ARTISTS);
  g_app.home = home_view_create(g_app.wnd, ID_HOME);
  home_view_set_playlist_art_dir(g_app.art_dir);
  refresh_library();

  import_view_create(g_app.wnd, g_app.lib);
  settings_view_create(g_app.wnd);
  settings_view_set_source_run(g_app.from_source ? g_app.config_dir : NULL);
  settings_view_set_library_dir(mc_settings_library_dir(g_app.settings));
  settings_view_init_sync(g_app.settings, schedule_refresh);
  settings_view_init_plugins(plugins_changed, update_tethered_scrobbler);
  static const testing_host testing = {open_stress, back_to_library, scroll_test_start, current_library};
  testing_view_init(g_app.settings, g_app.config_dir, &testing);
  static const device_view_host host = {rename_self, schedule_refresh};
  device_view_create(g_app.wnd, &host);
}

/* Plays n tracks, item_at's, in order from track start on; or with start -1 from the first
 * track, which shuffled is any. Tracks with no file on this device are left out of the queue.
 * Playing them puts the shown album, playlist or artist first in the recents, as on
 * Android. */
static void play_items(int n, track_list_item (*item_at)(int), int start) {
  int count = 0, first = start < 0 ? -1 : 0;
  playback_track *queue = n ? calloc((size_t)n, sizeof *queue) : NULL;
  if (!queue) return;
  for (int i = 0; i < n; i++) {
    track_list_item t = item_at(i);
    if (!t.path) continue;
    if (start >= 0 && i <= start) first = count;
    queue[count++] = (playback_track){t.uid, t.path, t.title, t.artist, t.album, t.cover_dir, t.duration_ms};
  }
  char uid[MC_UID_STRLEN] = "";
  mc_recent_kind kind = album_grid_showing_playlists() ? MC_RECENT_PLAYLIST : MC_RECENT_ALBUM;
  if (artist_page()) {
    kind = MC_RECENT_ARTIST;
    snprintf(uid, sizeof uid, "%s", artist_view_artist());
  } else if (!album_grid_selection(uid)) {
    uid[0] = '\0';
  }
  mc_play_context context = !uid[0]                      ? MC_PLAY_CONTEXT_UNKNOWN
                            : kind == MC_RECENT_ARTIST   ? MC_PLAY_CONTEXT_ARTIST
                            : kind == MC_RECENT_PLAYLIST ? MC_PLAY_CONTEXT_PLAYLIST
                                                         : MC_PLAY_CONTEXT_ALBUM;
  if (count) playback_play(queue, count, first, context, uid);
  free(queue);

  if (count && g_app.lib && uid[0] && mc_library_record_opened(g_app.lib, kind, uid) == MC_OK) refresh_recents();
}

static BOOL play_group_from(const char *album, int start);

/* The shown album, playlist or artist's tracks (see play_items()). An album on an album group's
 * page plays as part of the group, on into its following albums (play_group_from()), but for
 * playing it shuffled: that's the album's own Shuffle, and the group has Shuffle Tracks. */
static void play_album(int start) {
  char album[MC_UID_STRLEN];
  if (g_app.page == PAGE_ALBUM_GROUPS && album_grid_group() && album_grid_selection(album) &&
      (start >= 0 || !playback_shuffle()) && play_group_from(album, start))
    return;
  play_items(track_list_count(), track_list_item_at, start);
}

/* A button's name, for screen readers, and its tooltip, which adds the button's shortcut:
 * "Next track (Ctrl+Right)". */
static void set_button_label(HWND button, enum mc_str_id label, enum mc_str_id shortcut) {
  SetWindowTextW(button, mc_wstr(label));
  wchar_t tip[128];
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = g_app.wnd, .uId = (UINT_PTR)button,
                    .lpszText = wstr_tooltip(tip, 128, label, shortcut)};
  SendMessageW(g_app.tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
  InvalidateRect(button, NULL, FALSE);
}

/* The Play button turns into Pause while playing; Shuffle shows whether it's on. */
static void playback_changed(void) {
  update_buttons();
  set_button_label(g_app.buttons[ID_PLAY - ID_PREV], playback_is_playing() ? MC_STR_PAUSE : MC_STR_PLAY,
                   MC_STR_WINDOWS_SHORTCUT_PLAY_PAUSE);
  set_button_label(g_app.shuffle, playback_shuffle() ? MC_STR_SHUFFLE_ON : MC_STR_SHUFFLE_OFF,
                   MC_STR_WINDOWS_SHORTCUT_SHUFFLE);
}

/* Shuffle on or off, kept in the settings for next time. */
static void set_shuffle(BOOL on) {
  if (!on == !playback_shuffle()) return;
  playback_set_shuffle(on);
  mc_settings_set_shuffle(g_app.settings, on);
  mc_settings_save(g_app.settings); /* failing, it's saved with the next setting */
}

/* Ctrl+S and the media flyout's shuffle button: says so in the now-playing strip for a
 * moment, as the button may not be in sight. */
static void set_shuffle_flash(BOOL on) {
  set_shuffle(on);
  now_playing_flash(mc_wstr(playback_shuffle() ? MC_STR_SHUFFLE_ON : MC_STR_SHUFFLE_OFF));
}

/* The speaker and its drop-down show the volume and muting. */
static void volume_changed(void) {
  InvalidateRect(g_app.volume, NULL, FALSE);
  volume_popup_update();
}

/* Volume 0 to 100, kept in the settings for next time. Changing it unmutes. While casting it's
 * the device's (playback.h), which isn't kept: the computer's own stays as it was. */
static void change_volume(int volume) {
  playback_set_muted(FALSE);
  playback_set_volume(volume);
  volume_changed();
  if (playback_casting()) return;
  mc_settings_set_volume(g_app.settings, playback_volume());
  g_app.volume_pending = TRUE;
  SetTimer(g_app.wnd, TIMER_SAVE_VOLUME, SAVE_VOLUME_MS, NULL);
}

/* From the keyboard: says so in the now-playing strip for a moment, as the speaker only
 * shows the level roughly. */
static void set_volume(int volume) {
  change_volume(volume);
  wchar_t text[64];
  mc_wformat(text, 64, mc_str(MC_STR_VOLUME_LEVEL), MC_D(playback_volume()));
  now_playing_flash(text);
}

/* The speaker's drop-down, which shows the change itself. */
static void set_muted(BOOL muted) {
  playback_set_muted(muted);
  volume_changed();
}

static const volume_popup_host volume_host = {change_volume, set_muted};

/* --- Google Cast --- */

static void cast_popup_closed(void);
static const cast_popup_host cast_host = {change_volume, cast_popup_closed};

/* The cast button shows while there's a device to cast to, a cast under way, or its drop-down
 * open (which may be saying it's still looking); the strip makes room for it, and takes the
 * room back. Its name and tooltip say what it's doing: "Cast", "Connecting to Living Room TV…",
 * "Casting to Living Room TV". */
static void update_cast_button(void) {
  const mc_cast_status *st = playback_cast_status();
  mc_cast_device_list *list = playback_cast_devices();
  BOOL show = (list && list->count > 0) || st->state != MC_CAST_IDLE || cast_popup_is_open();
  mc_cast_device_list_free(list);

  wchar_t label[256];
  if (st->state == MC_CAST_IDLE) {
    lstrcpynW(label, mc_wstr(MC_STR_CAST), 256);
  } else {
    const char *name = st->device_name[0] ? st->device_name : mc_str(MC_STR_CAST_DEVICE_UNNAMED);
    mc_wformat(label, 256, mc_str(st->state == MC_CAST_CASTING ? MC_STR_CAST_CASTING_TO : MC_STR_CAST_CONNECTING_TO),
               MC_S(name));
  }
  SetWindowTextW(g_app.cast, label);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = g_app.wnd, .uId = (UINT_PTR)g_app.cast, .lpszText = label};
  SendMessageW(g_app.tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

  if (st->state == MC_CAST_CONNECTING) {
    SetTimer(g_app.wnd, TIMER_CAST_PULSE, CAST_PULSE_MS, NULL); /* restarts it, which is fine */
  } else {
    KillTimer(g_app.wnd, TIMER_CAST_PULSE);
    g_app.cast_pulse = FALSE;
  }
  if (show != g_app.cast_shown) {
    if (!show && GetFocus() == g_app.cast) SetFocus(g_app.queue); /* the next button along */
    g_app.cast_shown = show;
    layout();
  }
  InvalidateRect(g_app.cast, NULL, FALSE);
}

/* A cast ended that the user should hear of: "Couldn't connect to Living Room TV." in the
 * now-playing strip, as other passing troubles are. */
static void say_cast_ended(mc_cast_end ended, const char *name) {
  wchar_t text[256];
  mc_wformat(text, 256, mc_str(ended == MC_CAST_END_LOST ? MC_STR_CAST_LOST : MC_STR_CAST_FAILED),
             MC_S(name[0] ? name : mc_str(MC_STR_CAST_DEVICE_UNNAMED)));
  now_playing_notice(text);
}

/* WM_APP_CAST: the devices, the cast's state or the device's volume changed. Read here, on the
 * UI thread, as the cast's callback mustn't call it back. */
static void on_cast_event(void) {
  mc_cast_end ended = MC_CAST_END_NONE;
  char name[MC_CAST_NAME_LEN];
  if (playback_cast_update(&ended, name)) say_cast_ended(ended, name);
  update_cast_button();
  volume_changed(); /* the device's volume while casting, else the computer's again */
  cast_popup_update();
}

static void cast_popup_closed(void) {
  update_cast_button(); /* it may go, with nothing to cast to */
}

static void save_volume(void) {
  KillTimer(g_app.wnd, TIMER_SAVE_VOLUME);
  if (g_app.volume_pending) mc_settings_save(g_app.settings); /* failing, it's saved with the next setting */
  g_app.volume_pending = FALSE;
}

static void toggle_muted(void) {
  set_muted(!playback_muted());
  wchar_t text[64];
  if (playback_muted()) lstrcpynW(text, mc_wstr(MC_STR_VOLUME_MUTED), 64);
  else mc_wformat(text, 64, mc_str(MC_STR_VOLUME_LEVEL), MC_D(playback_volume()));
  now_playing_flash(text);
}

/* A track played from the track list. From an album (on Albums or in a group; not a
 * playlist or an artist's tracks), shuffle goes off first so the album plays in order, unless
 * mc_settings_album_in_order() says not to. */
static void play_track(int index) {
  album_grid_view shown = album_grid_shown();
  char uid[MC_UID_STRLEN];
  if (!artist_page() && (shown == AGV_ALBUMS || shown == AGV_GROUP) && album_grid_selection(uid) &&
      mc_settings_album_in_order(g_app.settings))
    set_shuffle(FALSE);
  play_album(index);
}

/* The track list's Play and Shuffle buttons: the whole album, playlist or artist's tracks,
 * in order from the first track, or shuffled. */
static void play_all(BOOL shuffled) {
  set_shuffle(shuffled);
  play_album(shuffled ? -1 : 0);
}

/* The shown group's albums' tracks with a file here, one album after another: the albums in
 * the group's order, or if order isn't NULL, album order[i] in place i. NULL if there's none
 * to list. Free with mc_track_list_free(). */
static mc_track_list *group_tracks(const int *order) {
  int albums = album_grid_count();
  const char **uids = albums ? malloc((size_t)albums * sizeof *uids) : NULL;
  if (!uids || !g_app.lib) {
    free(uids);
    return NULL;
  }
  for (int i = 0; i < albums; i++) uids[i] = album_grid_uid_at(order ? order[i] : i);
  mc_track_list *list = NULL;
  if (mc_library_list_albums_tracks(g_app.lib, uids, albums, &list) != MC_OK) list = NULL;
  free(uids);
  return list;
}

/* Plays list, the shown group's (group_tracks()), from item start; or with start -1 from the
 * first, which shuffled is any. Playing it puts the group first in the recents, as playing an
 * album does. */
static void play_group_tracks(const mc_track_list *list, int start) {
  int n = list->count;
  playback_track *queue = calloc((size_t)n, sizeof *queue);
  wchar_t **text = calloc((size_t)n * 4, sizeof *text); /* the queue's strings, freed once it has copied them */
  if (queue && text) {
    for (int i = 0; i < n; i++) {
      const mc_track_info *t = &list->items[i];
      wchar_t **mine = text + 4 * i;
      mine[0] = wstr_dup_from_utf8(t->title && *t->title ? t->title : mc_str(MC_STR_UNTITLED));
      mine[1] = t->artist ? wstr_dup_from_utf8(t->artist) : NULL;
      mine[2] = t->album ? wstr_dup_from_utf8(t->album) : NULL;
      mine[3] = t->cover_dir ? wstr_dup_from_utf8(t->cover_dir) : NULL;
      queue[i] = (playback_track){t->uid, t->path, mine[0], mine[1], mine[2], mine[3], t->duration_ms};
    }
    playback_play(queue, n, start, MC_PLAY_CONTEXT_ALBUM_GROUP, album_grid_group());
    for (int i = 0; i < 4 * n; i++) free(text[i]);
  }
  free(text);
  free(queue);
  const char *group = album_grid_group();
  if (group && mc_library_record_opened(g_app.lib, MC_RECENT_ALBUM_GROUP, group) == MC_OK) refresh_recents();
}

/* A group's Play, Shuffle Albums and Shuffle Tracks (album_grid.h): its albums' tracks with a
 * file here, each album's in its own order, the albums in the group's order or a random one,
 * or every track shuffled. */
static void play_group(void) {
  album_grid_play how = album_grid_play_how();
  int albums = album_grid_count();
  int *order = albums ? malloc((size_t)albums * sizeof *order) : NULL;
  if (!order) return;
  for (int i = 0; i < albums; i++) order[i] = i;
  for (int i = albums - 1; how == AG_PLAY_SHUFFLE_ALBUMS && i > 0; i--) { /* Fisher-Yates */
    unsigned r = 0;
    rand_s(&r);
    int j = (int)(r % (unsigned)(i + 1)), t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  mc_track_list *list = group_tracks(order);
  if (list && list->count) {
    set_shuffle(how == AG_PLAY_SHUFFLE_TRACKS);
    play_group_tracks(list, how == AG_PLAY_SHUFFLE_TRACKS ? -1 : 0);
  }
  mc_track_list_free(list);
  free(order);
}

/* An album shown in the track list, on its group's page: the whole group as the queue, from
 * the album's track start (the list's index), or with start -1 its first, so previous and
 * next see the albums either side. A track with no file here starts from the one before it,
 * as play_items() does. FALSE, with nothing played, if none of the album is in the queue. */
static BOOL play_group_from(const char *album, int start) {
  const char *track = NULL;
  for (int i = start; i >= 0 && !track; i--) {
    track_list_item t = track_list_item_at(i);
    if (t.path) track = t.uid;
  }
  mc_track_list *list = group_tracks(NULL);
  int first = -1;
  for (int i = 0; list && i < list->count && first < 0; i++)
    if (!strcmp(list->items[i].album_uid, album) && (!track || !strcmp(list->items[i].uid, track))) first = i;
  if (first >= 0) play_group_tracks(list, first);
  mc_track_list_free(list);
  return first >= 0;
}

/* The track list's requests to change a playlist (see track_list.h). */
static void on_track_request(int code) {
  const track_list_request *req = track_list_last_request();
  char uid[MC_UID_STRLEN];
  const wchar_t *name = album_grid_selection(uid); /* the shown playlist */
  BOOL changed = FALSE;
  switch (code) {
    case TLN_ADD_TO_PLAYLIST:
      changed = add_to_playlist(g_app.wnd, g_app.lib, req->track_uid, req->here,
                                req->playlist_uid[0] ? req->playlist_uid : NULL);
      break;
    case TLN_REMOVE:
      changed = mc_playlist_remove(g_app.lib, req->entry_uid) == MC_OK;
      if (!changed)
        message_dialog(g_app.wnd, mc_wstr(MC_STR_PLAYLIST), mc_wstr(MC_STR_PLAYLIST_REMOVE_FAILED), MB_ICONERROR);
      break;
    case TLN_MOVE:
      changed = mc_playlist_move(g_app.lib, req->entry_uid, req->after_uid[0] ? req->after_uid : NULL) == MC_OK;
      if (!changed)
        message_dialog(g_app.wnd, mc_wstr(MC_STR_PLAYLIST), mc_wstr(MC_STR_PLAYLIST_MOVE_FAILED), MB_ICONERROR);
      break;
    case TLN_RENAME_PLAYLIST:
      if (!name || strcmp(uid, req->playlist_uid)) break;
      if (album_grid_selection_smart(NULL)) changed = smart_rename_dialog(g_app.wnd, g_app.lib, uid, name, MC_SMART_TRACKS);
      else changed = playlist_rename_dialog(g_app.wnd, g_app.lib, uid, name);
      break;
    case TLN_DELETE_PLAYLIST:
      if (!name || strcmp(uid, req->playlist_uid)) break;
      if (album_grid_selection_smart(NULL)) changed = smart_delete_dialog(g_app.wnd, g_app.lib, uid, name, MC_SMART_TRACKS);
      else changed = playlist_delete_dialog(g_app.wnd, g_app.lib, uid, name);
      break;
    case TLN_EDIT_SMART: /* a smart playlist's rule */
      if (name && !strcmp(uid, req->playlist_uid) && album_grid_selection_smart(NULL))
        changed = smart_edit_dialog(g_app.wnd, g_app.lib, uid);
      break;
    case TLN_DELETE_TRACK:
      for (int i = 0, n = track_list_count(); i < n; i++) {
        track_list_item it = track_list_item_at(i);
        if (strcmp(it.uid, req->track_uid)) continue;
        changed = delete_dialog(g_app.wnd, g_app.lib, MC_DELETE_TRACKS, req->track_uid, it.title);
        break;
      }
      break;
    case TLN_EDIT_TRACK: {
      /* Copied: the dialog's message loop runs while the list may ask for other things. */
      char track[MC_UID_STRLEN];
      snprintf(track, sizeof track, "%s", req->track_uid);
      changed = edit_dialog_track(g_app.wnd, g_app.lib, track);
      break;
    }
    case TLN_RESHUFFLE: /* a smart playlist's new order */
      if (!name || strcmp(uid, req->playlist_uid)) break;
      changed = mc_smart_reshuffle(g_app.lib, uid) == MC_OK;
      if (!changed)
        message_dialog(g_app.wnd, mc_wstr(MC_STR_SMART_PLAYLIST), mc_wstr(MC_STR_RESHUFFLE_FAILED), MB_ICONERROR);
      break;
  }
  /* A change to the shown playlist's tracks puts it first in the recents, as adding to one
   * does (add_to_playlist()). */
  if (changed && (code == TLN_REMOVE || code == TLN_MOVE) && name)
    mc_library_record_opened(g_app.lib, MC_RECENT_PLAYLIST, uid);
  if (changed) {
    refresh_library();
    UpdateWindow(g_app.grid); /* reload the grid now: a deleted playlist's tracks go at once */
  }
}

/* The track with uid, or NULL if the library hasn't got it. Free with mc_track_list_free(). */
static mc_track_list *find_track(const char *uid) {
  mc_track_list *list = NULL;
  if (!g_app.lib || mc_library_list_track(g_app.lib, uid, &list) != MC_OK) return NULL;
  if (list->count) return list;
  mc_track_list_free(list);
  return NULL;
}

/* The playing track, if it can have a menu: not during an import session, which has the
 * library, or if it left the library while it played. Else NULL, and the title bar keeps
 * its own menu. Free with mc_track_list_free(). */
static mc_track_list *now_playing_menu_track(void) {
  return g_app.locked || !playback_current_uid()[0] ? NULL : find_track(playback_current_uid());
}

/* Whether a right-click at lp (screen coordinates) is for the playing track's menu. */
static BOOL now_playing_menu_at(LPARAM lp) {
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  if (!now_playing_on_track(pt)) return FALSE;
  mc_track_list *list = now_playing_menu_track();
  mc_track_list_free(list);
  return list != NULL;
}

/* The menu for the playing track, at pt (screen coordinates). It has no Move Up, Move Down or
 * Remove from Playlist: those are for a place in a list.
 *
 * The track may change or go while the menu's open, so what's picked applies to the track
 * it was opened on, and to nothing if that left the library. */
static void now_playing_menu(POINT pt) {
  mc_track_list *list = now_playing_menu_track();
  if (!list) return;
  const mc_track_info *t = &list->items[0];
  track_menu_options options = {.lib = g_app.lib, .track_uid = t->uid,
                                .go_to_album = track_menu_album_has_page(g_app.lib, t->album_uid),
                                .go_to_artist = t->artist_uid[0] != '\0'};
  char playlist[MC_UID_STRLEN];
  track_menu_pick pick = track_menu_show(g_app.wnd, pt, &options, playlist);

  mc_track_list *still = pick != TRACK_MENU_NOTHING ? find_track(t->uid) : NULL;
  BOOL changed = FALSE;
  if (still) {
    switch (pick) {
      case TRACK_MENU_ADD_TO_PLAYLIST:
      case TRACK_MENU_NEW_PLAYLIST:
        changed = add_to_playlist(g_app.wnd, g_app.lib, t->uid, t->path != NULL,
                                  pick == TRACK_MENU_ADD_TO_PLAYLIST ? playlist : NULL);
        break;
      case TRACK_MENU_START_RADIO:
        playback_play_radio(t->uid);
        break;
      case TRACK_MENU_GO_TO_ALBUM:
        open_album(t->album_uid, t->uid);
        break;
      case TRACK_MENU_GO_TO_ARTIST:
        open_artist(t->artist_uid);
        break;
      case TRACK_MENU_EDIT:
        changed = edit_dialog_track(g_app.wnd, g_app.lib, t->uid);
        break;
      case TRACK_MENU_DELETE: {
        wchar_t *title = wstr_dup_from_utf8(t->title && *t->title ? t->title : mc_str(MC_STR_UNTITLED));
        if (title) changed = delete_dialog(g_app.wnd, g_app.lib, MC_DELETE_TRACKS, t->uid, title);
        free(title);
        break;
      }
      default: /* the others are for a place in a list, and aren't offered */
        break;
    }
  }
  mc_track_list_free(still);
  mc_track_list_free(list);
  if (changed) {
    refresh_library();
    UpdateWindow(g_app.grid);
  }
}

/* An artist's page's requests for a single (see artist_view.h), as the track list's for a
 * track. */
static void on_single_request(int code) {
  const track_list_request *req = artist_view_last_request();
  BOOL changed = FALSE;
  if (code == ARN_ADD_TO_PLAYLIST) {
    changed = add_to_playlist(g_app.wnd, g_app.lib, req->track_uid, req->here,
                              req->playlist_uid[0] ? req->playlist_uid : NULL);
  } else if (code == ARN_DELETE_TRACK) {
    for (int i = 0, n = artist_view_single_count(); i < n; i++) {
      track_list_item it = artist_view_single_at(i);
      if (strcmp(it.uid, req->track_uid)) continue;
      /* Copies: the dialog's message loop repaints the page, which may reload it. */
      char uid[MC_UID_STRLEN];
      snprintf(uid, sizeof uid, "%s", req->track_uid);
      wchar_t *title = _wcsdup(it.title);
      if (title) changed = delete_dialog(g_app.wnd, g_app.lib, MC_DELETE_TRACKS, uid, title);
      free(title);
      break;
    }
  } else if (code == ARN_EDIT_TRACK) {
    char uid[MC_UID_STRLEN]; /* copied, as above */
    snprintf(uid, sizeof uid, "%s", req->track_uid);
    changed = edit_dialog_track(g_app.wnd, g_app.lib, uid);
  }
  if (changed) refresh_library();
}

/* --- Sync --- */

/* "address:port", with an IPv6 address in brackets. */
static void format_address(wchar_t *out, size_t cap, const char *host, unsigned port) {
  wchar_t *w = wstr_dup_from_utf8(host);
  BOOL v6 = strchr(host, ':') != NULL;
  swprintf(out, cap, v6 ? L"[%ls]:%u" : L"%ls:%u", w ? w : L"", port);
  free(w);
}

/* For the About box: the library database's version, then where other devices reach this
 * PC (every address), or why sync is off; one per line. */
static void about_details(wchar_t *out, size_t cap) {
  wchar_t line[160];
  out[0] = 0;
  if (g_app.lib) {
    mc_wformat(line, 160, mc_str(MC_STR_WINDOWS_ABOUT_DB_VERSION), MC_D(mc_library_schema_version(g_app.lib)));
    wcsncat_s(out, cap, line, _TRUNCATE);
    wcsncat_s(out, cap, L"\n", _TRUNCATE);
  }

  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    mc_wformat(line, 160, mc_str(MC_STR_WINDOWS_ABOUT_SYNC_OFF),
               MC_S(g_app.lib ? mc_status_str(sync_bridge_error()) : mc_str(MC_STR_WINDOWS_NO_LIBRARY)));
    wcsncat_s(out, cap, line, _TRUNCATE);
    return;
  }
  char addrs[MAX_ADDRESSES][MC_SYNC_ADDR_STRLEN];
  unsigned port = mc_sync_port(sync);
  int n = mc_sync_local_addresses(addrs, MAX_ADDRESSES);
  if (n <= 0) {
    mc_wformat(line, 160, mc_str(MC_STR_WINDOWS_PORT_NO_NETWORK), MC_D(port));
    wcsncat_s(out, cap, line, _TRUNCATE);
    return;
  }
  wcsncat_s(out, cap, mc_wstr(MC_STR_WINDOWS_ABOUT_REACHED_AT), _TRUNCATE);
  for (int i = 0; i < n; i++) {
    format_address(line, 160, addrs[i], port);
    wcsncat_s(out, cap, L"\n", _TRUNCATE);
    wcsncat_s(out, cap, line, _TRUNCATE);
  }
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
static void schedule_library_refresh(void);

/* A pairing that merged libraries wasn't finished (the app closed, or this PC left the group,
 * before Sync now), so the core put everything back as it was: the now-playing strip says so
 * once, and the library views show the old library again. */
static void say_restored(mc_sync *sync) {
  char other[MC_SYNC_DEVICE_NAME_MAX + 1];
  if (!sync || !mc_sync_take_merge_restored(sync, other, sizeof other)) return;
  wchar_t text[192];
  if (other[0])
    mc_wformat(text, 192, mc_str(MC_STR_MERGE_RESTORED), MC_S(other));
  else
    lstrcpynW(text, mc_wstr(MC_STR_MERGE_RESTORED_UNNAMED), 192);
  now_playing_notice(text);
  schedule_library_refresh();
}

static void refresh_devices(void) {
  KillTimer(g_app.wnd, TIMER_REFRESH_DEVICES);
  g_app.refresh_pending = FALSE;

  mc_sync *sync = sync_bridge_get();
  say_restored(sync);
  mc_sync_device_list *paired = NULL;
  mc_sync_nearby_list *nearby = NULL;
  if (sync && mc_sync_list_devices(sync, &paired) != MC_OK) paired = NULL;
  if (sync && mc_sync_list_nearby(sync, &nearby) != MC_OK) nearby = NULL;
  /* Tethered devices (an iPod, say) go after the group's, in or out of a group. */
  mc_tethered_device_list *tethered = NULL;
  if (tethered_bridge_get() && mc_tethered_list(tethered_bridge_get(), &tethered) != MC_OK) tethered = NULL;
  int np = paired ? paired->count : 0, nn = nearby ? nearby->count : 0, nt = tethered ? tethered->count : 0;
  sidebar_device *devs = np + nn + nt ? calloc((size_t)(np + nn + nt), sizeof *devs) : NULL;
  wchar_t **names = np + nn + nt ? calloc((size_t)(np + nn + nt), sizeof *names) : NULL;
  int npaired = 0, nnearby = 0;
  if (devs && names) {
    /* This PC first, then the online devices, then the offline ones, each in the order
     * listed (by name); removed ones left out. */
    for (int pass = 0; pass < 3; pass++)
      for (int i = 0; i < np; i++) {
        const mc_sync_device *d = &paired->items[i];
        int in_pass = d->is_self ? 0 : d->online ? 1 : 2;
        if (d->removed || in_pass != pass) continue;
        sidebar_device *s = &devs[npaired];
        snprintf(s->uid, sizeof s->uid, "%s", d->uid);
        s->name = names[npaired] = wstr_dup_from_utf8(d->name ? d->name : "");
        s->kind = (int)d->kind;
        s->flags = (d->is_self ? SB_DEV_SELF : 0) | (d->online || d->is_self ? SB_DEV_ONLINE : 0) |
                   (d->archive ? SB_DEV_ARCHIVE : 0) | (syncing_slot(d->uid) >= 0 ? SB_DEV_SYNCING : 0);
        npaired++;
      }
    for (int i = 0; i < nt; i++) { /* plugged in first, then by name, as listed */
      const mc_tethered_device *d = &tethered->items[i];
      sidebar_device *s = &devs[npaired];
      snprintf(s->uid, sizeof s->uid, "%s", d->uid);
      s->name = names[npaired] = wstr_dup_from_utf8(d->name && *d->name ? d->name : d->kind ? d->kind : "");
      s->kind = SB_KIND_TETHERED;
      s->flags = SB_DEV_TETHERED | (d->connected ? SB_DEV_ONLINE : 0) | (d->syncing ? SB_DEV_SYNCING : 0);
      npaired++;
    }
    for (int i = 0; i < nn; i++) {
      const mc_sync_nearby *d = &nearby->items[i];
      sidebar_device *s = &devs[npaired + nnearby];
      snprintf(s->uid, sizeof s->uid, "%s", d->uid);
      s->name = names[npaired + nnearby] = wstr_dup_from_utf8(d->name ? d->name : "");
      s->kind = (int)d->kind;
      s->flags = SB_DEV_ONLINE | (d->in_other_group ? SB_DEV_OTHER_GROUP : 0);
      nnearby++;
    }
  }
  sidebar_set_devices(devs, npaired, devs ? devs + npaired : NULL, nnearby);
  /* For the empty pages: pairing while this PC isn't in a group, the sync profile once it is. */
  int members = 0;
  for (int i = 0; i < np; i++) members += !paired->items[i].removed;
  album_grid_set_sync_state(!sync ? AG_SYNC_OFF : members ? AG_SYNC_GROUPED : AG_SYNC_ALONE);
  for (int i = 0; names && i < npaired + nnearby; i++) free(names[i]);
  free(names);
  free(devs);
  mc_sync_device_list_free(paired);
  mc_sync_nearby_list_free(nearby);
  mc_tethered_device_list_free(tethered);

  /* The shown device may have left the group (the sidebar then selects Albums). */
  if (g_app.page == PAGE_DEVICE) {
    if (sidebar_selection() != PAGE_DEVICE) show_page(PAGE_ALBUMS);
    else device_view_refresh();
  }
  if (sync) save_own_name(sync);
  settings_view_update_sync();
}

/* Refreshes the devices shortly, once for a burst of sync events. */
static void schedule_refresh(void) {
  if (g_app.refresh_pending) return;
  g_app.refresh_pending = TRUE;
  SetTimer(g_app.wnd, TIMER_REFRESH_DEVICES, REFRESH_DEVICES_MS, NULL);
}

/* Reloads the library views shortly, once for a burst of rows and files from sync, as an
 * import does after each album. */
static void schedule_library_refresh(void) {
  if (g_app.library_pending) return;
  g_app.library_pending = TRUE;
  SetTimer(g_app.wnd, TIMER_REFRESH_LIBRARY, REFRESH_LIBRARY_MS, NULL);
}

/* This PC renamed on its own device page: sync has the name; settings.json keeps it. */
static void rename_self(const char *name) {
  mc_settings_set_device_name(g_app.settings, name);
  if (mc_settings_save(g_app.settings) != MC_OK)
    message_dialog(g_app.wnd, mc_wstr(MC_STR_SETTINGS), mc_wstr(MC_STR_NAME_SAVE_FAILED), MB_ICONWARNING);
  schedule_refresh();
}

/* Deleted tracks whose files are back from the Recycle Bin come back into the library
 * (kithcore/delete.h). Cheap: run at startup, on switching libraries and whenever the
 * window is activated. The now-playing strip says how many tracks came back. */
static void restore_deleted(void) {
  int restored = 0, tracks = 0;
  if (!g_app.lib || !g_app.config_dir || mc_delete_restore(g_app.lib, g_app.config_dir, &restored, &tracks) != MC_OK ||
      !restored)
    return;
  refresh_library();
  if (tracks) {
    wchar_t text[64];
    mc_wformat(text, 64, mc_str_plural(MC_STR_DELETE_RESTORED, tracks), MC_D(tracks));
    now_playing_flash(text);
  }
}

/* Just before a delete trashes files: the playing file, and the next one the player may have
 * opened already, can't go while they're open, so playback stops if any queued track goes. */
static void before_delete(const mc_delete_plan *plan) {
  for (int i = 0, n = playback_queue_count(); i < n; i++)
    if (mc_delete_plan_has(plan, playback_queue_uid(i))) {
      playback_clear();
      return;
    }
}

/* The same before Remove from this PC: an open file can't be removed either. */
static void before_remove_here(const mc_here_plan *plan) {
  for (int i = 0, n = playback_queue_count(); i < n; i++)
    if (mc_here_plan_has(plan, playback_queue_uid(i))) {
      playback_clear();
      return;
    }
}

/* The library just opened, for the report and the way back from a stress test library. */
static void library_opened(void) {
  BOOL stress = mc_library_is_stress(g_app.lib);
  /* A way back left from before is no use in a real library. */
  if (!stress && mc_settings_string(g_app.settings, MC_STRESS_RETURN_SETTING)) {
    mc_settings_set_string(g_app.settings, MC_STRESS_RETURN_SETTING, NULL);
    mc_settings_save(g_app.settings);
  }
  if (waiting_banner_set_stress(stress)) layout();
  if (stress) settings_view_open_testing(TRUE); /* the way back's in it */
  testing_view_update();
  if (!mc_perf_enabled() || !g_app.lib) return;
  static const struct { const char *key; mc_entity entity; } counts[] = {
    {"library_tracks", MC_ENTITY_TRACKS}, {"library_albums", MC_ENTITY_ALBUMS},
    {"library_artists", MC_ENTITY_ARTISTS}, {"library_playlists", MC_ENTITY_PLAYLISTS}};
  for (size_t i = 0; i < sizeof counts / sizeof *counts; i++) {
    int64_t n = 0;
    if (mc_library_count(g_app.lib, counts[i].entity, &n) == MC_OK) mc_perf_note_int(counts[i].key, n);
  }
  mc_perf_note("library_stress", stress ? "yes" : "no");
}
/* The audio analysis of the library that's open; it comes and goes with sync. A small batch
 * runs flat out with nothing shown. A large one opens analysis_dialog.h: Background carries on
 * paced, with a row in the sidebar, and is remembered until the work is done, so a restart
 * doesn't ask again. Cancel stops it, over restarts too, until Settings' Resume analysis. */
#define SETTING_ANALYSIS_BACKGROUND "analysis_background"
#define SETTING_ANALYSIS_STOPPED "analysis_stopped"
enum { ANALYSIS_POLL_MS = 500 };

static mc_analysis_scanner *g_analysis;
static BOOL g_analysis_remembered; /* SETTING_ANALYSIS_BACKGROUND is set */
static BOOL g_analysis_active;     /* it was at the last look */
static int g_analysis_left = -1;   /* the tracks waiting, for Settings: -1 to count them */

static void analysis_remember(BOOL background) {
  g_analysis_remembered = background;
  mc_settings_set_bool(g_app.settings, SETTING_ANALYSIS_BACKGROUND, background);
  mc_settings_save(g_app.settings); /* failing, it asks again at the next start */
}

/* Settings' section, while it shows: the scanner's own count if it's going, or a query, which is
 * only worth making when that can have changed. */
static void analysis_update_settings(void) {
  if (g_app.page != PAGE_SETTINGS) {
    g_analysis_left = -1;
    return;
  }
  mc_analysis_status st;
  mc_analysis_scanner_status(g_analysis, &st);
  if (g_analysis_active && !st.active) g_analysis_left = -1;
  g_analysis_active = st.active;
  if (st.active) {
    g_analysis_left = st.total - st.done;
  } else if (g_analysis_left < 0) {
    int64_t ms;
    if (!g_app.lib || mc_analysis_pending(g_app.lib, &g_analysis_left, &ms) != MC_OK) g_analysis_left = 0;
  }
  settings_view_update_analysis(mc_settings_bool(g_app.settings, SETTING_ANALYSIS_STOPPED, 0), g_analysis_left);
}

static void on_analysis_background(void) {
  mc_analysis_scanner_set_paced(g_analysis, 1);
  analysis_remember(TRUE);
  sidebar_set_analysing(TRUE);
}

static void on_analysis_cancel(void) {
  mc_analysis_scanner_stop(g_analysis);
  g_analysis = NULL;
  analysis_remember(FALSE);
  mc_settings_set_bool(g_app.settings, SETTING_ANALYSIS_STOPPED, TRUE);
  mc_settings_save(g_app.settings); /* failing, it asks again at the next start */
  sidebar_set_analysing(FALSE);
  analysis_update_settings();
}

/* Settings' Resume analysis: in the background, as there's been an answer to what the dialog
 * would ask. */
static void analysis_resume(void) {
  mc_settings_set_bool(g_app.settings, SETTING_ANALYSIS_STOPPED, FALSE);
  analysis_remember(TRUE);
  if (g_app.lib && !g_analysis) mc_analysis_scanner_start_resumed(mc_library_dir(g_app.lib), &g_analysis);
  analysis_update_settings();
}

static const analysis_dialog_host analysis_host = {on_analysis_background, on_analysis_cancel};

static void analysis_tick(void) {
  mc_analysis_status st;
  mc_analysis_scanner_status(g_analysis, &st);
  if (g_analysis_remembered && g_analysis && !st.paced) analysis_remember(FALSE); /* it finished */
  /* After a track is done: a pass of files that are all missing is over in a moment. */
  if (st.active && st.large && !st.paced && st.done > 0) analysis_dialog_show(g_app.wnd, &analysis_host);
  if (st.active) analysis_dialog_update(st.done, st.total);
  else analysis_dialog_close();
  sidebar_set_analysing(st.active && st.paced);
  analysis_update_settings();
}

/* The sidebar row: the dialog again, for its progress or its Cancel. */
static void analysis_present(void) {
  mc_analysis_status st;
  mc_analysis_scanner_status(g_analysis, &st);
  if (!st.active) return;
  analysis_dialog_show(g_app.wnd, &analysis_host);
  analysis_dialog_update(st.done, st.total);
}

/* Plays taken in from tethered devices are scrobbled while this PC scrobbles. */
static void update_tethered_scrobbler(void) {
  tethered_bridge_set_scrobbler(mc_settings_scrobble(g_app.settings) ? playback_scrobbler() : NULL);
}

/* Plugins were turned on or off in Settings: a device target may know a volume now. */
static void plugins_changed(void) {
  tethered_bridge_scan();
  refresh_devices();
}

static void on_tethered_event(WPARAM wp, LPARAM lp) {
  tethered_event *ev = tethered_bridge_receive(wp, lp);
  if (!ev) return;
  switch (ev->kind) {
    case TETHERED_EV_DEVICES:
      schedule_refresh();
      break;
    case TETHERED_EV_PROGRESS:
      if (ev->phase == MC_TETHERED_PHASE_READING) schedule_refresh(); /* the sidebar's badge */
      device_view_on_tethered_event(ev);
      break;
    case TETHERED_EV_DONE:
      device_view_on_tethered_event(ev);
      /* Plays and On-The-Go playlists came into the library, and the sidebar's badge goes. */
      if (ev->result.plays_imported > 0 || ev->result.playlists_imported > 0) schedule_library_refresh();
      schedule_refresh();
      break;
  }
  free(ev);
}

static void start_sync(void) {
  restore_deleted(); /* the library just opened */
  library_opened();
  memset(g_app.syncing, 0, sizeof g_app.syncing);
  sync_bridge_start(g_app.wnd, g_app.lib, g_app.config_dir, mc_settings_device_name(g_app.settings));
  playback_set_library(g_app.lib, g_app.config_dir);
  tethered_bridge_start(g_app.wnd, g_app.lib, g_app.config_dir); /* after playback: its scrobbler */
  update_tethered_scrobbler();
  /* After a library switch on a device's page, it shows again on the new sync (or Albums, if
   * the refresh finds the device gone). */
  if (g_app.page == PAGE_DEVICE) open_device_page();
  refresh_devices();
  if (waiting_banner_update(sync_bridge_get())) layout(); /* nothing yet: it comes with SYNC_EV_WAITING */
  g_analysis_left = -1;
  if (g_app.lib && !g_analysis && !mc_settings_bool(g_app.settings, SETTING_ANALYSIS_STOPPED, 0)) {
    g_analysis_remembered = mc_settings_bool(g_app.settings, SETTING_ANALYSIS_BACKGROUND, 0);
    mc_analysis_scanner_start_sized(mc_library_dir(g_app.lib), g_analysis_remembered, &g_analysis);
  }
  SetTimer(g_app.wnd, TIMER_ANALYSIS, ANALYSIS_POLL_MS, NULL);
}

/* Before the library closes: sync reads it. Pairing dialogs go first, as they use sync. */
static void stop_sync(void) {
  KillTimer(g_app.wnd, TIMER_ANALYSIS);
  analysis_dialog_close();
  sidebar_set_analysing(FALSE);
  mc_analysis_scanner_stop(g_analysis); /* it reads the files, which may be about to move */
  g_analysis = NULL;
  pair_dialog_close_all();
  device_view_open(NULL, NULL, NULL);
  tethered_bridge_stop(); /* before playback's scrobbler goes */
  sync_bridge_stop();
  playback_set_library(NULL, NULL);
  if (waiting_banner_update(NULL)) layout();
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
      /* Reconcile may have removed files after the last progress. */
      if (ev->library_synced) schedule_library_refresh();
      device_view_on_sync_event(ev);
      break;
    case SYNC_EV_PAIR_CODE:
      pair_dialog_show_code(sync, ev);
      break;
    case SYNC_EV_PAIR_RESULT:
      /* Paired: the dialog's Choose What to Sync opens the page of the device that joined. */
      if (pair_dialog_on_result(ev)) refresh_devices();
      break;
    case SYNC_EV_PROGRESS:
      if (set_syncing(ev->uid, ev->phase != MC_SYNC_PHASE_DONE)) schedule_refresh();
      if (ev->library_changed) schedule_library_refresh();
      device_view_on_sync_event(ev);
      break;
    case SYNC_EV_PAIR_MERGE:
      pair_dialog_on_merge(ev);
      break;
    case SYNC_EV_MERGE_PROGRESS:
      /* Albums come into the library as it goes. */
      pair_dialog_on_merge_event(ev);
      schedule_library_refresh();
      break;
    case SYNC_EV_MERGE_DONE:
      pair_dialog_on_merge_event(ev);
      schedule_library_refresh();
      schedule_refresh(); /* this PC's page: no merge waiting any more */
      break;
    case SYNC_EV_WAITING:
      if (waiting_banner_update(sync)) layout();
      break;
  }
  free(ev);
}

static void sync_off_message(void) {
  wchar_t text[256];
  mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_PAIR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
  message_dialog(g_app.wnd, mc_wstr(MC_STR_PAIR_DEVICE), text, MB_ICONINFORMATION);
}

/* After mc_sync_pair_request() or _address(): opens the enter-code dialog, or says why
 * pairing couldn't start. name names the other device, or is NULL when it's known only by
 * its address. */
static void pair_started(mc_status st, const wchar_t *name, const wchar_t *address) {
  wchar_t text[512];
  switch (st) {
    case MC_OK:
      pair_dialog_begin_enter(sync_bridge_get(), name, address);
      return;
    case MC_ERR_LOCKED:
      swprintf(text, 512, L"%ls", mc_wstr(MC_STR_PAIR_ALREADY_PAIRING));
      break;
    case MC_ERR_NOT_FOUND:
      if (name && *name) address = name;
      if (address) {
        char *a = wstr_to_utf8(address);
        mc_wformat(text, 512, mc_str(MC_STR_PAIR_GONE), MC_S(a));
        free(a);
      } else {
        swprintf(text, 512, L"%ls", mc_wstr(MC_STR_PAIR_GONE_UNNAMED));
      }
      break;
    default:
      mc_wformat(text, 512, mc_str(MC_STR_PAIR_START_FAILED), MC_S(mc_status_str(st)));
      break;
  }
  message_dialog(g_app.wnd, mc_wstr(MC_STR_PAIR_DEVICE), text, MB_ICONINFORMATION);
}

/* Asks nearby device clicked, in the sidebar, to pair. Even one in another group: pairing
 * then says why it can't. */
static void pair_nearby_uid(const char *clicked) {
  mc_sync *sync = sync_bridge_get();
  if (!clicked) return;
  if (!sync) {
    sync_off_message();
    return;
  }
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", clicked); /* the sidebar's copy goes when it refreshes */
  wchar_t *name = NULL;
  mc_sync_nearby_list *nearby = NULL;
  if (mc_sync_list_nearby(sync, &nearby) == MC_OK) {
    for (int i = 0; i < nearby->count; i++)
      if (!strcmp(nearby->items[i].uid, uid)) {
        name = wstr_dup_from_utf8(nearby->items[i].name ? nearby->items[i].name : "");
        break;
      }
    mc_sync_nearby_list_free(nearby);
  }
  mc_status st = mc_sync_pair_request(sync, uid);
  pair_started(st, name, NULL);
  if (st == MC_OK) pair_dialog_set_target(uid, NULL, 0);
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
  wchar_t address[300];
  format_address(address, 300, host, port ? port : MC_SYNC_DEFAULT_PORT);
  mc_status st = mc_sync_pair_request_address(sync, host, port);
  pair_started(st, NULL, address);
  if (st == MC_OK) pair_dialog_set_target(NULL, host, port);
}

#ifndef NDEBUG
/* Debug builds, for test scripts: KITHSONG_DEBUG_PAIR=<IPv4 or host>:<port> asks that device
 * to pair, as Add Device by Address does, shortly after startup. Two scratch instances can
 * then pair over localhost with no clicks. */
static void debug_pair(void) {
  const wchar_t *value = _wgetenv(L"KITHSONG_DEBUG_PAIR");
  char *spec = value ? utf8_from_wide(value) : NULL;
  char *colon = spec ? strrchr(spec, ':') : NULL;
  if (colon) {
    *colon = '\0';
    on_add_device_connect(NULL, spec, (uint16_t)atoi(colon + 1));
  }
  free(spec);
}
#endif

/* The empty Albums and Playlists pages' Choose What to Sync: this PC's own page, whose
 * What to Sync picks what it holds. */
static void show_own_page(void) {
  mc_sync *sync = sync_bridge_get();
  if (!sync) return;
  sidebar_select_device(mc_sync_device_uid(sync));
  if (!sidebar_selected_device()) return;
  show_page(PAGE_DEVICE);
  device_view_focus_what_to_sync();
}


/* --- The pairing dialogs' side --- */

/* Choose What to Sync: the device's page (this PC's own, when it joined). */
static void pair_show_device(const char *uid) {
  refresh_devices();
  sidebar_select_device(uid);
  if (!sidebar_selected_device()) return;
  show_page(PAGE_DEVICE);
  device_view_focus_what_to_sync();
}

/* Create a New Library, from a pairing refused because this PC has music of its own. */
static BOOL pair_new_library(void) {
  return change_library(TRUE);
}

/* Pair Again, once the new library is open: by uid if discovery has seen the device again,
 * else at the address it had. */
static void pair_again(const char *uid, const wchar_t *name, const char *host, uint16_t port) {
  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    sync_off_message();
    return;
  }
  mc_status st = uid && *uid ? mc_sync_pair_request(sync, uid) : MC_ERR_NOT_FOUND;
  if (st == MC_ERR_NOT_FOUND && host && *host) {
    wchar_t address[300];
    format_address(address, 300, host, port ? port : MC_SYNC_DEFAULT_PORT);
    st = mc_sync_pair_request_address(sync, host, port);
    pair_started(st, name, address);
  } else {
    pair_started(st, name, NULL);
  }
  if (st == MC_OK) pair_dialog_set_target(uid, host, port);
}

/* One-time explanations: TRUE the first time key is asked about, which settings.json then
 * records. Not saved, it shows again next run. */
static BOOL pair_first_time(const char *key) {
  if (mc_settings_bool(g_app.settings, key, 0)) return FALSE;
  mc_settings_set_bool(g_app.settings, key, 1);
  mc_settings_save(g_app.settings);
  return TRUE;
}

static const pair_dialog_host k_pair_host = {pair_show_device, pair_new_library, pair_again, pair_first_time};

static void on_command(int id, int code) {
  switch (id) {
    case ID_SIDEBAR:
      if (code == LBN_SELCHANGE) {
        show_page(sidebar_selection());
        /* Clicked rather than reached with the arrow keys, which stay in the sidebar. */
        if (g_app.page == PAGE_SEARCH && GetKeyState(VK_LBUTTON) < 0) search_view_focus();
      } else if (code == SBN_ENTER_PAGE) {
        if (g_app.page == PAGE_SEARCH) search_view_focus();
        else if (library_page()) SetFocus(g_app.grid);
        else if (g_app.page == PAGE_ARTISTS) SetFocus(g_app.artists);
        else if (g_app.page == PAGE_SETTINGS) settings_view_focus();
        else if (g_app.page == PAGE_HOME) SetFocus(g_app.home);
      } else if (code == SBN_ADD_DEVICE) add_device_show(g_app.wnd, on_add_device_connect, NULL);
      else if (code == SBN_PAIR_DEVICE) pair_nearby_uid(sidebar_clicked_nearby());
      else if (code == SBN_OPEN_RECENT) open_recent();
      else if (code == SBN_ABOUT) show_about();
      else if (code == SBN_STATS && g_app.lib) stats_dialog_show(g_app.wnd, g_app.lib);
      else if (code == SBN_ANALYSIS) analysis_present();
      else if (code == SBN_REMOVE_RECENT) remove_recents(sidebar_clicked_recent(NULL), FALSE);
      else if (code == SBN_CLEAR_RECENTS) remove_recents(NULL, TRUE);
      else if (code == SBN_EDIT_RECENT) edit_recent();
      else if (code == SBN_NEW_SMART_PLAYLIST || code == SBN_NEW_SMART_GROUP) {
        show_page(code == SBN_NEW_SMART_PLAYLIST ? PAGE_PLAYLISTS : PAGE_ALBUM_GROUPS);
        album_grid_new_smart();
      }
      break;
    case ID_HOME:
      if (code == HVN_OPEN) open_home_tile();
      else if (code == HVN_IMPORT) show_page(PAGE_IMPORT);
      else if (code == HVN_LIBRARY_CHANGED) refresh_library();
      else if (code == HVN_CLEAR_RECENTS) remove_recents(NULL, TRUE);
      else if (code == HVN_REMOVE_RECENT) {
        mc_recent_kind kind;
        remove_recents(home_view_clicked(&kind), FALSE);
      }
      break;
    case ID_GRID:
      if (code == AGN_SELCHANGE) {
        mc_perf_view_begin("album_tracks");
        show_album();
        perf_view_shown();
      }
      else if (code == AGN_LIBRARY_CHANGED) refresh_library();
      else if (code == AGN_IMPORT) show_page(PAGE_IMPORT);
      else if (code == AGN_SYNC_SETTINGS) show_own_page();
      else if (code == AGN_PLAY_GROUP) play_group();
      else if (code == AGN_OPEN_GROUP && g_app.lib && album_grid_group() &&
               mc_library_record_opened(g_app.lib, MC_RECENT_ALBUM_GROUP, album_grid_group()) == MC_OK)
        refresh_recents();
      break;
    case ID_WAITING:
      if (code == WAITING_BANNER_REMOVE) {
        if (delete_dialog_lost(g_app.wnd, g_app.lib, sync_bridge_get())) refresh_library();
      } else {
        back_to_library(); /* the banner says a stress test library is open */
      }
      break;
    case ID_ARTISTS:
      if (code == ARN_OPEN) {
        artist_opened();
      } else if (code == ARN_LIBRARY_CHANGED) {
        refresh_library();
      } else if (code == ARN_BACK) {
        show_album(); /* empties the track list */
        layout();
      } else if (code == ARN_RESIZED) {
        layout(); /* the track list goes beneath the header */
      } else if (code == ARN_PLAY_SINGLE) {
        play_items(artist_view_single_count(), artist_view_single_at, artist_view_play_index());
      } else if (code == ARN_ADD_TO_PLAYLIST || code == ARN_DELETE_TRACK || code == ARN_EDIT_TRACK) {
        on_single_request(code);
      } else if (code == ARN_OPEN_ALBUM) {
        char uid[MC_UID_STRLEN];
        snprintf(uid, sizeof uid, "%s", artist_view_opened_album());
        open_album(uid, NULL);
      } else if (code == ARN_GO_TO_ARTIST) {
        char uid[MC_UID_STRLEN];
        snprintf(uid, sizeof uid, "%s", artist_view_last_request()->artist_uid);
        open_artist(uid);
      }
      break;
    case ID_PREV:
      playback_previous();
      break;
    case ID_PLAY:
      if (!playback_toggle()) play_album(-1); /* nothing queued yet: the album that's shown */
      break;
    case ID_NEXT:
      playback_next();
      break;
    case ID_SHUFFLE:
      set_shuffle(!playback_shuffle());
      break;
    case ID_CAST:
      cast_popup_toggle(g_app.cast, g_app.dpi, &cast_host);
      break;
    case ID_QUEUE:
      queue_popup_toggle(g_app.queue, g_app.dpi);
      break;
    case ID_VOLUME:
      volume_popup_toggle(g_app.volume, g_app.dpi, &volume_host);
      break;
    case ID_TRACKS:
      if (code == TLN_PLAY) play_track(track_list_play_index());
      else if (code == TLN_PLAY_ALL || code == TLN_SHUFFLE_ALL) play_all(code == TLN_SHUFFLE_ALL);
      else if (code == TLN_CLOSE && artist_page()) { /* Escape on an artist's page: back to the list */
        artist_view_back();
        show_album();
        layout();
        SetFocus(g_app.artists);
      } else if (code == TLN_CLOSE) { /* unselecting the album hides the list */
        album_grid_unselect();
        SetFocus(g_app.grid);
      } else if (code == TLN_GO_TO_ALBUM) {
        char uid[MC_UID_STRLEN], track[MC_UID_STRLEN];
        snprintf(uid, sizeof uid, "%s", track_list_last_request()->album_uid);
        snprintf(track, sizeof track, "%s", track_list_last_request()->track_uid);
        open_album(uid, track);
      } else if (code == TLN_GO_TO_ARTIST) {
        char uid[MC_UID_STRLEN];
        snprintf(uid, sizeof uid, "%s", track_list_last_request()->artist_uid);
        open_artist(uid);
      } else on_track_request(code);
      break;
    case ID_SEARCH:
      if (code == SVN_OPEN) open_search_result();
      else if (code == SVN_LIBRARY_CHANGED) refresh_library();
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
      g_app.page = PAGE_ALBUMS; /* the sidebar starts there */
      playback_init(wnd, playback_changed);
      playback_on_recents_changed(refresh_recents);
      playback_on_shuffle_requested(set_shuffle_flash);
      playback_on_up_next_changed(queue_popup_update);
      theme_apply_window(wnd, TRUE);
      create_controls();
      playback_set_shuffle(mc_settings_shuffle(g_app.settings)); /* as it was left */
      playback_set_volume(mc_settings_volume(g_app.settings));
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
    /* The strip lets right-clicks on the cover and title through, so they reach us as title
     * bar clicks. DefWindowProc would keep the button's release for itself: left out of it,
     * the release comes here too, and the menu opens on it as a track row's does. */
    case WM_NCRBUTTONDOWN:
      if (wp == HTCAPTION && now_playing_menu_at(lp)) return 0;
      break;
    case WM_NCRBUTTONUP:
      if (wp == HTCAPTION && now_playing_menu_at(lp)) {
        now_playing_menu((POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
      }
      break;
    case WM_CONTEXTMENU: /* from the strip: the edge of the bar overlaps the title, or the keyboard (lp -1) */
      if ((HWND)wp != g_app.now_playing) break;
      if (lp == -1) {
        now_playing_menu(now_playing_menu_point());
        return 0;
      }
      if (now_playing_menu_at(lp)) {
        now_playing_menu((POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)});
        return 0;
      }
      break;
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
      mmi->ptMinTrackSize.x = scale(1152);
      mmi->ptMinTrackSize.y = scale(864);
      if (g_window_w) { /* --window-size: as asked, whatever the screen (a CI runner's is small) */
        if (g_window_w < mmi->ptMinTrackSize.x) mmi->ptMinTrackSize.x = g_window_w;
        if (g_window_h < mmi->ptMinTrackSize.y) mmi->ptMinTrackSize.y = g_window_h;
        if (mmi->ptMaxTrackSize.x < g_window_w + 64) mmi->ptMaxTrackSize.x = g_window_w + 64;
        if (mmi->ptMaxTrackSize.y < g_window_h + 64) mmi->ptMaxTrackSize.y = g_window_h + 64;
      }
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
      if ((dis->CtlID >= ID_PREV && dis->CtlID < ID_PREV + NUM_BUTTONS) || dis->CtlID == ID_SHUFFLE ||
          dis->CtlID == ID_CAST || dis->CtlID == ID_QUEUE || dis->CtlID == ID_VOLUME)
        draw_button(dis);
      else device_view_draw_item(dis);
      return TRUE;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT: {
      HBRUSH brush = device_view_ctlcolor(msg, (HWND)lp, (HDC)wp);
      if (!brush && (msg == WM_CTLCOLORSTATIC || msg == WM_CTLCOLORBTN))
        brush = settings_view_ctlcolor((HWND)lp, (HDC)wp);
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
    case WM_ACTIVATEAPP: /* back from Explorer, maybe from restoring files from the Recycle Bin */
      if (wp) {
        restore_deleted();
        firewall_recheck(); /* maybe from Windows' firewall or network settings */
      }
      break;
    case WM_APPCOMMAND: /* the mouse's back button, over the track list on an artist's page */
      if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_BACKWARD && artist_page()) {
        on_command(ID_TRACKS, TLN_CLOSE);
        return TRUE;
      }
      /* While casting, the keyboard's volume keys are the device's, as on Android, while the
       * window has the focus; otherwise they're Windows' own. */
      if (playback_casting()) {
        switch (GET_APPCOMMAND_LPARAM(lp)) {
          case APPCOMMAND_VOLUME_UP: set_volume(playback_volume() + VOLUME_STEP); return TRUE;
          case APPCOMMAND_VOLUME_DOWN: set_volume(playback_volume() - VOLUME_STEP); return TRUE;
          case APPCOMMAND_VOLUME_MUTE: toggle_muted(); return TRUE;
        }
      }
      break;
    case WM_NOTIFY:
      if (device_view_on_notify((const NMHDR *)lp, &result)) return result;
      import_view_on_notify((const NMHDR *)lp);
      return 0;
    case WM_TIMER:
      if (wp == TIMER_REFRESH_DEVICES || wp == TIMER_POLL_DEVICES) {
        refresh_devices();
        return 0;
      }
      if (wp == TIMER_ANALYSIS) {
        analysis_tick();
        return 0;
      }
      if (wp == TIMER_REFRESH_LIBRARY) {
        KillTimer(wnd, TIMER_REFRESH_LIBRARY);
        g_app.library_pending = FALSE;
        refresh_library();
        return 0;
      }
      if (wp == TIMER_PERF_RUN) {
        KillTimer(wnd, TIMER_PERF_RUN);
        scroll_test_start();
        return 0;
      }
      if (wp == TIMER_START_PAGE) {
        if (!g_start_page || open_start_page(g_start_page) || ++g_start_page_tries >= START_PAGE_TRIES) {
          KillTimer(wnd, TIMER_START_PAGE);
          free(g_start_page);
          g_start_page = NULL;
          /* --demo-playing: over whatever the page put in the track list. */
          char uid[MC_UID_STRLEN];
          int position = 0;
          if (g_demo_playing && g_app.lib && mc_demo_playing_track(g_app.lib, g_demo_playing, uid, &position) == MC_OK)
            playback_demo_show(uid, position);
          free(g_demo_playing);
          g_demo_playing = NULL;
        }
        return 0;
      }
      if (wp == TIMER_DEMO_REVIEW) {
        KillTimer(wnd, TIMER_DEMO_REVIEW);
        import_view_open_review(MC_DEMO_IMPORT_REVIEW);
        return 0;
      }
      if (wp == TIMER_SAVE_VOLUME) {
        save_volume();
        return 0;
      }
      if (wp == TIMER_CAST_PULSE) {
        g_app.cast_pulse = !g_app.cast_pulse;
        InvalidateRect(g_app.cast, NULL, FALSE);
        return 0;
      }
#ifndef NDEBUG
      if (wp == TIMER_DEBUG_PAIR) {
        KillTimer(wnd, TIMER_DEBUG_PAIR);
        debug_pair();
        return 0;
      }
#endif
      break;
    case WM_APP_TETHERED:
      on_tethered_event(wp, lp);
      return 0;
    case WM_DEVICECHANGE:
      tethered_bridge_device_change(wp, lp);
      return TRUE;
    case WM_APP_SYNC:
      on_sync_event(wp, lp);
      return 0;
    case WM_APP_CAST: /* wParam: the mc_cast_event, which doesn't matter: everything is read again */
      on_cast_event();
      return 0;
    case WM_COPYDATA: {
      /* Another launch's paths. Imported once its SendMessage has returned, as the import
       * may show a message box. */
      const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
      if (cds->dwData != INSTANCE_PATHS || !cds->cbData || cds->cbData % sizeof(wchar_t)) break;
      wchar_t *list = malloc(cds->cbData + 2 * sizeof *list);
      if (!list) return TRUE;
      memcpy(list, cds->lpData, cds->cbData);
      list[cds->cbData / sizeof *list] = list[cds->cbData / sizeof *list + 1] = L'\0';
      if (!PostMessageW(wnd, WM_APP_FORWARDED, 0, (LPARAM)list)) free(list);
      return TRUE;
    }
    case WM_APP_FORWARDED:
      import_forwarded((wchar_t *)lp);
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
      g_app.locked = (BOOL)wp;
      sidebar_set_locked((BOOL)wp);
      layout(); /* the import view switches between the drop zone and the list */
      return 0;
    case WM_APP_SCROLL_TEST:
      scroll_test_frame();
      return 0;
    case WM_APP_OPEN_LIBRARY:
      change_library(FALSE);
      return 0;
    case WM_APP_RESUME_ANALYSIS:
      analysis_resume();
      return 0;
    case WM_APP_MOVE_LIBRARY:
      move_library();
      return 0;
    case WM_APP_RESET: { /* Settings' Reset Kithsong…, confirmed: the next start clears the folder */
      mc_status st = mc_settings_request_reset(g_app.config_dir);
      if (st != MC_OK) {
        wchar_t text[MAX_PATH + 512];
        mc_wformat(text, MAX_PATH + 512, mc_str(MC_STR_RESET_APP_FAILED), MC_S(g_app.config_dir),
                   MC_S(mc_status_str(st)));
        message_dialog(wnd, mc_wstr(MC_STR_SETTINGS_RESET), text, MB_ICONERROR);
        return 0;
      }
      PostMessageW(wnd, WM_CLOSE, 0, 0);
      return 0;
    }
    case WM_ENDSESSION:
      if (wp) playback_end_session();
      return 0;
    case WM_CLOSE:
      if (import_view_confirm_close()) DestroyWindow(wnd);
      return 0;
    case WM_DESTROY:
      RemovePropW(wnd, INSTANCE_PROP);
      KillTimer(wnd, TIMER_REFRESH_DEVICES);
      KillTimer(wnd, TIMER_POLL_DEVICES);
      KillTimer(wnd, TIMER_REFRESH_LIBRARY);
      KillTimer(wnd, TIMER_CAST_PULSE);
      save_volume();
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
  BOOL demo_library;           /* --demo-library */
  int window_w, window_h;      /* --window-size, or 0 */
  const wchar_t *page;         /* --page, or NULL */
  const wchar_t *demo_playing; /* --demo-playing, or NULL */
} app_args;

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
        MessageBoxW(NULL, mc_wstr(MC_STR_WINDOWS_USAGE), mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONINFORMATION);
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
      if (!wcscmp(a, L"--demo-library")) {
        args->demo_library = TRUE;
        continue;
      }
      const wchar_t *size = !wcscmp(a, L"--window-size") && i + 1 < argc ? args->argv[++i]
                            : !wcsncmp(a, L"--window-size=", 14)        ? a + 14
                                                                         : NULL;
      if (size) {
        int w = 0, h = 0;
        wchar_t end = 0;
        if (swscanf(size, L"%dx%d%lc", &w, &h, &end) == 2 && w >= 320 && h >= 240 && w <= 16384 && h <= 16384) {
          args->window_w = w;
          args->window_h = h;
          continue;
        }
      }
      const wchar_t *page = !wcscmp(a, L"--page") && i + 1 < argc ? args->argv[++i]
                            : !wcsncmp(a, L"--page=", 7)         ? a + 7
                                                                 : NULL;
      if (page && *page) {
        args->page = page;
        continue;
      }
      const wchar_t *playing = !wcscmp(a, L"--demo-playing") && i + 1 < argc ? args->argv[++i]
                               : !wcsncmp(a, L"--demo-playing=", 15)        ? a + 15
                                                                             : NULL;
      if (playing && *playing) {
        args->demo_playing = playing;
        continue;
      }
      /* unknown, or no folder */
      MessageBoxW(NULL, mc_wstr(MC_STR_WINDOWS_USAGE), mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONWARNING);
      return 2;
    }
    if (options && !wcscmp(a, L"/?")) {
      MessageBoxW(NULL, mc_wstr(MC_STR_WINDOWS_USAGE), mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONINFORMATION);
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

/* --page's page, once the library's open: home, albums, artists, album:<title>,
 * artist:<name> or search:<text>. FALSE to try again shortly, as the album grid may not have
 * the album yet; TRUE once it's open, or can't be (an unknown page, a title not there). */
static BOOL open_start_page(const char *page) {
  if (!g_app.lib) return TRUE;
  if (!strcmp(page, "import") || !strcmp(page, "import-review")) {
    /* The demo import: its items come with its first events. */
    if (g_app.page != PAGE_IMPORT) {
      show_page(PAGE_IMPORT);
      import_view_start_demo();
    }
    if (!strcmp(page, "import")) return TRUE;
    if (!import_view_item_shown(MC_DEMO_IMPORT_REVIEW)) return FALSE;
    /* Modal, so not from this timer, which would fire again inside it. */
    SetTimer(g_app.wnd, TIMER_DEMO_REVIEW, 500, NULL);
  } else if (!strcmp(page, "sync")) {
    /* The demo group's phone (use_demo_library made the group), once sync is up. */
    mc_sync *sync = sync_bridge_get();
    if (!sync) return FALSE;
    refresh_devices();
    sidebar_select_device(MC_DEMO_PHONE_UID);
    if (!sidebar_selected_device()) return FALSE;
    show_page(PAGE_DEVICE);
    mc_demo_sync_show(sync);
  } else if (!strcmp(page, "home")) show_page(PAGE_HOME);
  else if (!strcmp(page, "albums")) show_page(PAGE_ALBUMS);
  else if (!strcmp(page, "artists")) show_page(PAGE_ARTISTS);
  else if (!strncmp(page, "search:", 7)) {
    show_page(PAGE_SEARCH);
    wchar_t *text = wstr_dup_from_utf8(page + 7);
    if (text) search_view_set_text(text);
    free(text);
  } else if (!strncmp(page, "album:", 6)) {
    char uid[MC_UID_STRLEN] = "";
    mc_album_list *list = NULL;
    if (mc_library_list_albums(g_app.lib, &list) == MC_OK)
      for (int i = 0; i < list->count && !uid[0]; i++)
        if (list->items[i].title && !strcmp(list->items[i].title, page + 6)) snprintf(uid, sizeof uid, "%s", list->items[i].uid);
    mc_album_list_free(list);
    if (!uid[0]) return TRUE;
    show_page(PAGE_ALBUMS);
    if (!album_grid_select(uid)) return FALSE;
    show_album(); /* as open_album() does */
    album_grid_select(uid);
  } else if (!strncmp(page, "artist:", 7)) {
    char uid[MC_UID_STRLEN] = "";
    mc_artist_list *list = NULL;
    if (mc_library_list_artists(g_app.lib, &list) == MC_OK)
      for (int i = 0; i < list->count && !uid[0]; i++)
        if (list->items[i].name && !strcmp(list->items[i].name, page + 7)) snprintf(uid, sizeof uid, "%s", list->items[i].uid);
    mc_artist_list_free(list);
    if (!uid[0]) return TRUE;
    show_page(PAGE_ARTISTS);
    if (!artist_view_open(uid)) return FALSE;
    artist_opened();
  }
  return TRUE;
}

/* --demo-library: the demo library in <config dir>\demo-library, made if it isn't there, as
 * the settings' library. FALSE (having said why) if they name another library already, so a
 * real config folder never loses its library to it, or if it can't be made. */
static BOOL use_demo_library(const wchar_t *page) {
  size_t n = strlen(g_app.config_dir) + sizeof "\\" MC_DEMO_DIR_NAME;
  char *dir = malloc(n);
  if (!dir) return FALSE;
  snprintf(dir, n, "%s\\%s", g_app.config_dir, MC_DEMO_DIR_NAME);
  const char *current = mc_settings_library_dir(g_app.settings);
  wchar_t text[2 * MAX_PATH + 256];
  BOOL ok = FALSE;
  if (current && _stricmp(current, dir)) {
    mc_wformat(text, 2 * MAX_PATH + 256, mc_str(MC_STR_DEMO_LIBRARY_OTHER), MC_S(current));
    message_dialog(NULL, mc_wstr(MC_STR_APP_NAME), text, MB_ICONERROR);
  } else {
    mc_status st = mc_demo_is_complete(dir) ? MC_OK : mc_demo_create(dir, NULL);
    if (st != MC_OK) {
      mc_wformat(text, 2 * MAX_PATH + 256, mc_str(MC_STR_DEMO_LIBRARY_FAILED), MC_S(dir), MC_S(mc_status_str(st)));
      message_dialog(NULL, mc_wstr(MC_STR_APP_NAME), text, MB_ICONERROR);
    } else {
      ok = TRUE;
      firewall_demo_clear();
      if (!current && mc_settings_set_library_dir(g_app.settings, dir) == MC_OK) mc_settings_save(g_app.settings);
      /* --page sync: the demo group, before sync opens sync.db. Its page says if it failed. */
      if (page && !wcscmp(page, L"sync")) mc_demo_sync_group(dir, g_app.config_dir, mc_settings_device_name(g_app.settings));
    }
  }
  free(dir);
  return ok;
}

/* --window-size: the window resized so the frame DWM draws, without the invisible resize
 * borders around it, is w x h. After it's shown, when DWM knows those borders. */
static void set_window_size(HWND wnd, int w, int h) {
  RECT outer, frame;
  int dx = 0, dy = 0;
  GetWindowRect(wnd, &outer);
  if (SUCCEEDED(DwmGetWindowAttribute(wnd, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof frame))) {
    dx = (outer.right - outer.left) - (frame.right - frame.left);
    dy = (outer.bottom - outer.top) - (frame.bottom - frame.top);
  }
  SetWindowPos(wnd, NULL, 0, 0, w + dx, h + dy, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* Where settings.json and sync.db live: --config-dir made absolute, or
 * %LOCALAPPDATA%\kithsong, or %LOCALAPPDATA%\kithsong-debug when running from a checkout.
 * Created if missing. Returns UTF-8, or NULL (*shown says which folder it was, for the
 * error). */
static char *config_dir(const app_args *args, BOOL from_source, wchar_t *shown, DWORD cap) {
  wchar_t dir[MAX_PATH];
  shown[0] = L'\0';
  if (args->config_dir) {
    DWORD n = GetFullPathNameW(args->config_dir, MAX_PATH, dir, NULL);
    lstrcpynW(shown, n && n < MAX_PATH ? dir : args->config_dir, (int)cap);
    if (n == 0 || n >= MAX_PATH) return NULL;
  } else {
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH - 16) {
      lstrcpynW(shown, from_source ? L"%LOCALAPPDATA%\\" MC_SOURCE_SETTINGS_DIR : L"%LOCALAPPDATA%\\kithsong", (int)cap);
      return NULL;
    }
    wchar_t old[MAX_PATH];
    wcscpy(old, dir);
    wcscat(old, L"\\musicapp");
    wcscat(dir, from_source ? L"\\" MC_SOURCE_SETTINGS_DIR : L"\\kithsong");
    lstrcpynW(shown, dir, (int)cap);
    if (!from_source) { /* the old folder is the installed app's to take */
      char *old8 = utf8_from_wide(old), *dir8 = utf8_from_wide(dir);
      mc_settings_move_old_dir(old8, dir8);
      free(old8);
      free(dir8);
    }
  }
  int err = SHCreateDirectoryExW(NULL, dir, NULL);
  if (err != ERROR_SUCCESS && err != ERROR_ALREADY_EXISTS && err != ERROR_FILE_EXISTS) return NULL;
  return utf8_from_wide(dir);
}

static void import_paths(const wchar_t *const *wide, int count) {
  char **paths = count ? calloc((size_t)count, sizeof *paths) : NULL;
  int n = 0;
  for (int i = 0; paths && i < count; i++)
    if ((paths[n] = utf8_from_wide(wide[i]))) n++;
  if (n > 0) {
    show_page(PAGE_IMPORT);
    import_view_start((const char *const *)paths, n);
  }
  for (int i = 0; i < n; i++) free(paths[i]);
  free(paths);
}

/* Folders or files on the command line (including ones dropped onto the exe or a shortcut)
 * are imported straight away. */
static void import_args(const app_args *args) {
  import_paths(args->paths, args->npaths);
}

/* Paths another launch handed over (forward_instance), imported as the command line's are.
 * Dropped while a dialog is open or the window is hidden (moving the library), as a drop
 * onto the window would be. Frees list. */
static void import_forwarded(wchar_t *list) {
  int count = 0;
  for (const wchar_t *p = list; *p; p += wcslen(p) + 1) count++;
  const wchar_t **paths = calloc((size_t)count + 1, sizeof *paths);
  if (paths && IsWindowEnabled(g_app.wnd) && IsWindowVisible(g_app.wnd)) {
    int i = 0;
    for (const wchar_t *p = list; *p; p += wcslen(p) + 1) paths[i++] = p;
    import_paths(paths, count);
  }
  free(paths);
  free(list);
}

/* --- One instance per config folder --- */

/* Its key: FNV-1a of the folder, ASCII lower-cased, with / as \ and no trailing separator. */
static ULONG_PTR instance_key(const char *dir) {
  size_t n = strlen(dir);
  while (n > 3 && (dir[n - 1] == '\\' || dir[n - 1] == '/')) n--;
  unsigned long long h = 14695981039346656037ull;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)dir[i];
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c == '/') c = '\\';
    h = (h ^ c) * 1099511628211ull;
  }
  return (ULONG_PTR)h;
}

typedef struct instance_find {
  ULONG_PTR key;
  HWND found;
} instance_find;

static BOOL CALLBACK find_instance(HWND wnd, LPARAM lp) {
  instance_find *f = (instance_find *)lp;
  wchar_t cls[16];
  if (GetClassNameW(wnd, cls, 16) && !wcscmp(cls, L"kithsong") && (ULONG_PTR)GetPropW(wnd, INSTANCE_PROP) == f->key) {
    f->found = wnd;
    return FALSE;
  }
  return TRUE;
}

/* Brings the open instance's window forward, unless it's hidden (moving the library), and
 * sends it args' paths, made absolute, as its working folder may not be this one's. */
static void forward_instance(HWND wnd, const app_args *args) {
  if (IsWindowVisible(wnd)) {
    if (IsIconic(wnd)) ShowWindow(wnd, SW_RESTORE);
    SetForegroundWindow(GetLastActivePopup(wnd)); /* a dialog it has open, or the window */
  }
  size_t len = 0;
  for (int i = 0; i < args->npaths; i++) len += GetFullPathNameW(args->paths[i], 0, NULL, NULL);
  wchar_t *list = len ? malloc(len * sizeof *list) : NULL;
  if (!list) return;
  size_t used = 0;
  for (int i = 0; i < args->npaths; i++) {
    DWORD n = GetFullPathNameW(args->paths[i], (DWORD)(len - used), list + used, NULL);
    if (n && n < len - used) used += n + 1;
  }
  COPYDATASTRUCT cds = {INSTANCE_PATHS, (DWORD)(used * sizeof *list), list};
  DWORD_PTR result;
  if (used) SendMessageTimeoutW(wnd, WM_COPYDATA, 0, (LPARAM)&cds, SMTO_ABORTIFHUNG, 5000, &result);
  free(list);
}

/* Makes this the config folder's one instance, holding a named mutex until it exits. Local\
 * names are per session, and a packaged desktop app's are the same as the zip's, so the
 * Store and zip builds share it; another --config-dir is another instance. Returns FALSE
 * when another instance has it, after bringing that one forward (or giving up after 10 s). */
static BOOL claim_instance(const char *dir, const app_args *args) {
  g_instance_key = instance_key(dir);
  wchar_t name[64];
  swprintf(name, 64, L"Local\\Kithsong-%016llx", (unsigned long long)g_instance_key);
  HANDLE mutex = CreateMutexW(NULL, FALSE, name);
  if (!mutex) return TRUE; /* can't tell, so carry on as before */
  /* The other instance may still be starting (no window yet) or closing (its window gone,
   * the mutex held while sync stops): wait for its window, or for the mutex. */
  for (int i = 0; i < 100; i++) {
    if (WaitForSingleObject(mutex, i ? 100 : 0) != WAIT_TIMEOUT) return TRUE; /* kept until exit */
    instance_find f = {g_instance_key, NULL};
    EnumWindows(find_instance, (LPARAM)&f);
    if (f.found) {
      forward_instance(f.found, args);
      break;
    }
  }
  CloseHandle(mutex);
  return FALSE;
}

/* --- Library location --- */

/* Asks where the library should live instead: the folder picker, titled title_id, opening
 * in current. It takes an empty folder or one that holds a library; with empty_only only an
 * empty one, or one with an empty library. Returns a malloc'd UTF-8 path,
 * or NULL if the user gave up. (The first run asks with first_run.c.) */
static char *choose_library_dir(HWND owner, const char *current, enum mc_str_id title_id, BOOL empty_only) {
  for (;;) {
    char *dir = pick_folder(owner, mc_wstr(title_id), current);
    if (!dir) return NULL;
    if (empty_only && !mc_folder_takes_new_library(dir)) {
      message_dialog(owner, mc_wstr(MC_STR_LIBRARY_FOLDER), mc_wstr(MC_STR_LIBRARY_NOT_EMPTY), MB_ICONINFORMATION);
      free(dir);
      continue;
    }
    if (!mc_folder_fits_library(dir)) { /* a library would be made among other things */
      wchar_t text[MAX_PATH + 256];
      mc_wformat(text, MAX_PATH + 256, mc_str(MC_STR_FIRST_RUN_NOT_EMPTY), MC_S(dir));
      message_dialog(owner, mc_wstr(MC_STR_LIBRARY_FOLDER), text, MB_ICONINFORMATION);
      free(dir);
      continue;
    }
    if (!confirm_onedrive(owner, dir)) {
      free(dir);
      continue;
    }
    return dir;
  }
}

/* Clears playlists' pictures out of the cache when their playlists have gone, on a thread
 * with its own connection, since it looks each one up. arg is the library folder and the
 * cache folder, malloc'd. */
static DWORD WINAPI prune_art_main(void *arg) {
  char **dirs = arg;
  mc_library *lib = NULL;
  if (mc_library_open(dirs[0], &lib) == MC_OK) {
    mc_playlist_art_prune(lib, dirs[1]);
    mc_library_close(lib);
  }
  free(dirs[0]);
  free(dirs[1]);
  free(dirs);
  return 0;
}

static void prune_art(void) {
  char **dirs = g_app.lib && g_app.art_dir ? calloc(2, sizeof *dirs) : NULL;
  if (!dirs) return;
  dirs[0] = _strdup(mc_library_dir(g_app.lib));
  dirs[1] = _strdup(g_app.art_dir);
  HANDLE thread = dirs[0] && dirs[1] ? CreateThread(NULL, 0, prune_art_main, dirs, 0, NULL) : NULL;
  if (thread) {
    CloseHandle(thread);
  } else {
    free(dirs[0]);
    free(dirs[1]);
    free(dirs);
  }
}

/* The library in dir, or NULL after saying why it couldn't be opened. */
static mc_library *open_library(const char *dir) {
  mc_library *lib = NULL;
  mc_status st = mc_library_open(dir, &lib);
  if (st != MC_OK) {
    wchar_t text[1024];
    mc_wformat(text, 1024, mc_str(MC_STR_WINDOWS_LIBRARY_OPEN_FAILED_IN),
               MC_S(dir ? dir : mc_str(MC_STR_UNKNOWN_VALUE)), MC_S(mc_status_str(st)));
    message_dialog(g_app.wnd, mc_wstr(MC_STR_APP_NAME), text, MB_ICONERROR);
    return NULL;
  }
  return lib;
}

/* The first run, over the main window: asks where the library goes, then opens it there.
 * FALSE if the user closed it instead. */
static BOOL open_first_library(void) {
  album_grid_set_first_run(TRUE);
  char *dir = first_run_choose(g_app.wnd);
  album_grid_set_first_run(FALSE);
  if (!dir) return FALSE;
  mc_settings_set_library_dir(g_app.settings, dir);
  if (mc_settings_save(g_app.settings) != MC_OK)
    message_dialog(g_app.wnd, mc_wstr(MC_STR_APP_NAME), mc_wstr(MC_STR_SETTINGS_FIRST_SAVE_FAILED), MB_ICONWARNING);
  g_app.lib = open_library(dir); /* on failure the window says so */
  free(dir);
  import_view_set_library(g_app.lib);
  settings_view_set_library_dir(mc_settings_library_dir(g_app.settings));
  refresh_library();
  return TRUE;
}

/* Switches to the library in dir. It opens first, so if it can't, nothing changes; sync then
 * moves from the old one to it. TRUE if it switched. */
static BOOL switch_library(const char *dir) {
  mc_library *lib = open_library(dir);
  if (!lib) return FALSE;
  stop_sync();
  edit_dialog_wait();
  mc_library_close(g_app.lib);
  g_app.lib = lib;
  mc_settings_set_library_dir(g_app.settings, dir);
  if (mc_settings_save(g_app.settings) != MC_OK)
    message_dialog(g_app.wnd, mc_wstr(MC_STR_SETTINGS), mc_wstr(MC_STR_SETTINGS_SAVE_FAILED), MB_ICONWARNING);
  import_view_set_library(g_app.lib);
  settings_view_set_library_dir(dir);
  refresh_library();
  start_sync();
  return TRUE;
}

/* Settings' Open Another Library: switch to the library in another folder. With new_empty (the
 * pairing dialog's Create a New Library) the folder must be empty, so the library made there
 * is too. TRUE if it switched. */
static BOOL change_library(BOOL new_empty) {
  const char *current = mc_settings_library_dir(g_app.settings);
  if (!import_view_confirm_close()) return FALSE;

  char *dir = choose_library_dir(g_app.wnd, current, new_empty ? MC_STR_LIBRARY_FOLDER_NEW : MC_STR_LIBRARY_FOLDER_CHOOSE,
                                 new_empty);
  BOOL changed = dir && switch_library(dir);
  free(dir);
  return changed;
}

/* Opens the stress test library in dir, remembering the library to go back to: the one open
 * now, unless that's a stress library too. It stays open at the next start, to measure it. */
static void open_stress(const char *dir) {
  if (!import_view_confirm_close()) return;
  if (!mc_library_is_stress(g_app.lib))
    mc_settings_set_string(g_app.settings, MC_STRESS_RETURN_SETTING, mc_settings_library_dir(g_app.settings));
  switch_library(dir);
}

/* Back to My Library, and the banner. With no way back saved, the user picks the folder. */
static void back_to_library(void) {
  const char *saved = mc_settings_string(g_app.settings, MC_STRESS_RETURN_SETTING);
  if (!saved) {
    change_library(FALSE);
    return;
  }
  char *dir = _strdup(saved); /* it's cleared on the way */
  if (dir && import_view_confirm_close()) switch_library(dir);
  free(dir);
}

/* --- The scroll test (kithcore/perf.h) --- */

/* Each view in turn scrolls by the fixed script, one frame per display refresh. A frame paints
 * before the next is asked for, so the frame times include all of its work. */
enum { SCROLL_ALBUMS, SCROLL_ARTISTS, SCROLL_TRACKS, SCROLL_VIEWS };

static struct {
  int view, frame;
  int pass, passes; /* mc_perf_run_passes(): each pass is every view again */
  BOOL running;
} g_scroll_test;

static scroller *scroll_test_view(void) {
  switch (g_scroll_test.view) {
    case SCROLL_ALBUMS: return album_grid_scroller();
    case SCROLL_ARTISTS: return artist_view_scroller();
    default: return track_list_scroller();
  }
}

/* Opens the view the test has got to, at its top. */
static void scroll_test_enter(void) {
  g_scroll_test.frame = 0;
  /* Its run starts here: on a later pass the view may have painted, moved, while another was
   * tested, and the time since then is no frame. */
  mc_perf_frame_break(scroll_test_view()->perf_name);
  if (g_scroll_test.view == SCROLL_ALBUMS) {
    show_page(PAGE_ALBUMS);
    album_grid_unselect();
    show_album();
  } else if (g_scroll_test.view == SCROLL_ARTISTS) {
    show_page(PAGE_ARTISTS);
  } else {
    char uid[MC_UID_STRLEN];
    if (mc_library_biggest_album(g_app.lib, uid) == MC_OK) open_album(uid, NULL);
  }
  RedrawWindow(g_app.wnd, NULL, NULL, RDW_UPDATENOW | RDW_ALLCHILDREN);
  mc_perf_frame_break(scroll_test_view()->perf_name); /* nor is opening it */
  scroller_jump(scroll_test_view(), 0);
}

static void scroll_test_start(void) {
  if (!mc_perf_enabled() || !g_app.lib || g_scroll_test.running) return;
  mc_perf_memory_sample("scroll_start");
  mc_perf_auto_begin();
  g_scroll_test.running = TRUE;
  g_scroll_test.view = SCROLL_ALBUMS;
  g_scroll_test.pass = 1;
  g_scroll_test.passes = mc_perf_run_passes();
  scroll_test_enter();
  PostMessageW(g_app.wnd, WM_APP_SCROLL_TEST, 0, 0);
}

static void scroll_test_end(void) {
  mc_perf_auto_end();
  g_scroll_test.running = FALSE;
  show_page(PAGE_SETTINGS);
  testing_view_update(); /* the report, before scrolling to it */
  settings_view_reveal_testing();
  /* Scripted: the report goes to the file named, and the app closes. */
  const wchar_t *out = _wgetenv(L"KITHSONG_PERF_RUN");
  if (out && *out) {
    char *path = wstr_to_utf8(out);
    if (path) mc_perf_save(path);
    free(path);
    PostMessageW(g_app.wnd, WM_CLOSE, 0, 0);
  }
}

static void scroll_test_frame(void) {
  if (!g_scroll_test.running) return;
  scroller *s = scroll_test_view();
  if (g_scroll_test.frame >= MC_PERF_SCROLL_FRAMES || s->pos >= scroller_max(s)) {
    /* It may paint again, moved, as the next view opens: that's no frame of this run. */
    s->perf_pos = -1;
    mc_perf_frame_break(s->perf_name);
    if (++g_scroll_test.view == SCROLL_VIEWS) {
      char name[16];
      snprintf(name, sizeof name, "pass_%d", g_scroll_test.pass);
      mc_perf_memory_sample(name);
      if (g_scroll_test.pass++ >= g_scroll_test.passes) {
        scroll_test_end();
        return;
      }
      g_scroll_test.view = SCROLL_ALBUMS; /* the same again: what it holds should stay level */
    }
    scroll_test_enter();
  } else {
    scroller_jump(s, s->pos + scale(MC_PERF_SCROLL_DP_PER_FRAME));
    UpdateWindow(s->wnd);
    DwmFlush(); /* waits for the next refresh */
    g_scroll_test.frame++;
  }
  PostMessageW(g_app.wnd, WM_APP_SCROLL_TEST, 0, 0);
}

/* For the message loop: Space in the main window plays or pauses, as the Play button does.
 * Controls that use Space themselves keep it: edits, push buttons, checkboxes and the device
 * page's checkbox list. The playback buttons and the sidebar take focus when clicked, so
 * Space plays there too, rather than pressing Next again. TRUE if msg was that Space. */
static BOOL space_play_pause(const MSG *msg) {
  if (msg->message != WM_KEYDOWN || msg->wParam != VK_SPACE) return FALSE;
  if (!g_app.wnd || GetAncestor(msg->hwnd, GA_ROOT) != g_app.wnd) return FALSE;
  if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_SHIFT) < 0) return FALSE;
  BOOL ours = msg->hwnd == g_app.sidebar;
  for (int i = 0; i < NUM_BUTTONS; i++) ours |= msg->hwnd == g_app.buttons[i];
  const LRESULT wants = DLGC_WANTCHARS | DLGC_HASSETSEL | DLGC_WANTALLKEYS | DLGC_BUTTON;
  if (!ours && (SendMessageW(msg->hwnd, WM_GETDLGCODE, msg->wParam, (LPARAM)msg) & wants)) return FALSE;
  BOOL repeat = (msg->lParam & 0x40000000) != 0; /* held down: once is enough */
  if (!repeat && IsWindowEnabled(g_app.buttons[ID_PLAY - ID_PREV])) on_command(ID_PLAY, BN_CLICKED);
  return TRUE;
}

/* Back, as the page's own Back does: from an artist's page to the artists, or from an album
 * group to the groups. FALSE if there's nowhere to go back to. */
static BOOL go_back(void) {
  if (artist_page()) {
    on_command(ID_TRACKS, TLN_CLOSE);
    return TRUE;
  }
  return g_app.page == PAGE_ALBUM_GROUPS && album_grid_back();
}

#define WEBSITE L"https://kithso.ng"


/* F1, or About in the sidebar: the app's name, version and build, what it is, the library's
 * database version, where other devices reach this PC, and its website. */
static void show_about(void) {
  wchar_t title[64], version[160], details[256 + MAX_ADDRESSES * 64], content[512 + MAX_ADDRESSES * 64];
  mc_wformat(title, 64, mc_str(MC_STR_ABOUT_TITLE), MC_S(mc_str(MC_STR_APP_NAME)));
  mc_wformat(version, 160, mc_str(MC_STR_WINDOWS_ABOUT_VERSION), MC_S(mc_version()), MC_D(KITHSONG_BUILD_NUMBER));
  about_details(details, sizeof details / sizeof details[0]);
  swprintf(content, sizeof content / sizeof content[0], L"%ls\n\n%ls", mc_wstr(MC_STR_APP_SUMMARY), details);
  prompt_dialog about = {.caption = title, .heading = version, .text = content, .link = WEBSITE,
                         .ok = mc_wstr_mnemonic(MC_STR_OK)};
  prompt_dialog_show(g_app.wnd, &about);
}

/* A sidebar page from the keyboard, with the focus in it, as Enter in the sidebar does (or
 * on the sidebar for a page that has nowhere to put it, as a click does); not while an import
 * has the others greyed out. */
static void shortcut_page(sidebar_page page) {
  if (g_app.locked && page != g_app.page) return;
  show_page(page);
  if (page == PAGE_SEARCH || page == PAGE_ARTISTS || library_page()) on_command(ID_SIDEBAR, SBN_ENTER_PAGE);
  else SetFocus(g_app.sidebar);
}

/* For the message loop: the keyboard shortcuts in the main window (see README.md), besides
 * Space. In a text box (the search box, Settings' fields) the arrow keys, + and - and
 * Backspace stay the box's; the Ctrl+letter, digit and comma ones, F1 and F3 work there
 * too. Toggles act once while a key is held; volume and seeking repeat. Alt+F4 is the only
 * key that quits, as is usual on Windows. TRUE if msg was a shortcut. */
static BOOL shortcut(const MSG *msg) {
  if (msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) return FALSE;
  if (!g_app.wnd || GetAncestor(msg->hwnd, GA_ROOT) != g_app.wnd || !IsWindowEnabled(g_app.wnd)) return FALSE;
  BOOL ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
  BOOL repeat = (msg->lParam & 0x40000000) != 0;
  BOOL text = (SendMessageW(msg->hwnd, WM_GETDLGCODE, msg->wParam, (LPARAM)msg) & DLGC_HASSETSEL) != 0;
  UINT key = (UINT)msg->wParam;

  if (alt || msg->message == WM_SYSKEYDOWN) /* Alt+Left, as in a browser; other Alt keys are mnemonics */
    return key == VK_LEFT && !ctrl && !shift && go_back();
  if (key == VK_BROWSER_BACK || (key == VK_BACK && !ctrl && !shift && !text)) return go_back();
  if ((key == VK_F1 || key == VK_F3) && !ctrl && !shift) {
    if (repeat) return TRUE;
    if (key == VK_F1) show_about();
    else shortcut_page(PAGE_SEARCH); /* F3, as in Explorer */
    return TRUE;
  }

  if (ctrl && !shift) {
    switch (key) {
      case VK_RIGHT:
      case VK_LEFT:
        if (text) return FALSE; /* a word at a time */
        if (!repeat && key == VK_RIGHT) playback_next();
        else if (!repeat) playback_previous();
        return TRUE;
      case VK_UP:
      case VK_DOWN:
        if (text) return FALSE;
        set_volume(playback_volume() + (key == VK_UP ? VOLUME_STEP : -VOLUME_STEP));
        return TRUE;
      case 'M':
        if (!repeat) toggle_muted();
        return TRUE;
      case 'S':
        if (!repeat) set_shuffle_flash(!playback_shuffle());
        return TRUE;
      case 'F':
      case 'E': /* as in Explorer */
        if (!repeat) shortcut_page(PAGE_SEARCH);
        return TRUE;
      case 'O': /* Import's Choose Folders... */
        if (repeat || (g_app.locked && g_app.page != PAGE_IMPORT)) return TRUE;
        show_page(PAGE_IMPORT);
        import_view_browse();
        return TRUE;
      case 'N': /* the Playlists page's + */
        if (repeat || g_app.locked || !g_app.lib) return TRUE;
        shortcut_page(PAGE_PLAYLISTS);
        album_grid_new_playlist();
        return TRUE;
      case '1':
      case '2':
      case '3':
      case '4': {
        static const sidebar_page pages[] = {PAGE_ALBUMS, PAGE_ARTISTS, PAGE_PLAYLISTS, PAGE_ALBUM_GROUPS};
        if (!repeat) shortcut_page(pages[key - '1']);
        return TRUE;
      }
      case VK_OEM_COMMA:
        if (!repeat) shortcut_page(PAGE_SETTINGS);
        return TRUE;
    }
    return FALSE;
  }
  if (text || ctrl) return FALSE;
  switch (key) {
    case VK_RIGHT:
    case VK_LEFT:
      if (!shift) return FALSE; /* the arrows alone move about in lists */
      playback_seek_by(key == VK_RIGHT ? SEEK_STEP_MS : -SEEK_STEP_MS);
      return TRUE;
    case VK_OEM_PLUS: /* the = key, so + with or without Shift */
    case VK_ADD:
      set_volume(playback_volume() + VOLUME_STEP);
      return TRUE;
    case VK_OEM_MINUS:
    case VK_SUBTRACT:
      set_volume(playback_volume() - VOLUME_STEP);
      return TRUE;
  }
  return FALSE;
}

/* Before Move Library moves the files: everything that reads the library or plays its files
 * lets go, and it closes. */
static void move_close_library(void) {
  KillTimer(g_app.wnd, TIMER_REFRESH_LIBRARY);
  g_app.library_pending = FALSE;
  playback_clear();
  stop_sync();
  import_view_set_library(NULL);
  album_grid_unselect();
  edit_dialog_wait();
  mc_library_close(g_app.lib);
  g_app.lib = NULL;
  refresh_library();
}

/* After the move: the library opens where it is now (settings already say so). */
static void move_reopen_library(const char *dir) {
  g_app.lib = open_library(dir); /* on failure the window says so */
  import_view_set_library(g_app.lib);
  settings_view_set_library_dir(mc_settings_library_dir(g_app.settings));
  refresh_library();
  start_sync();
  ShowWindow(g_app.wnd, SW_SHOW);
  SetForegroundWindow(g_app.wnd);
}

/* Settings' Move Library. Like switching library, it ends a finished import first. */
static void move_library(void) {
  static const move_library_host host = {move_close_library, move_reopen_library};
  if (move_library_running() || !import_view_confirm_close()) return;
  move_library_start(g_app.wnd, g_app.settings, &host);
}

/* Windows 10 1903, the oldest Windows Kithsong runs on: the first to honour the manifest's
 * UTF-8 code page. Keep in step with MinVersion in packaging/msix/AppxManifest.xml.in. */
#define MIN_WINDOWS_BUILD 18362

/* Before 1703 this never runs: the exe's imports stop it loading, with Windows' own error.
 * RtlGetVersion, as GetVersionEx reports what the manifest declares rather than the truth. */
static BOOL windows_supported(void) {
  typedef LONG(WINAPI * rtl_get_version)(OSVERSIONINFOW *);
  rtl_get_version get =
    (rtl_get_version)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
  return !get || get(&v) != 0 || v.dwBuildNumber >= MIN_WINDOWS_BUILD;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;
  uint64_t main_us = mc_perf_now_us();

  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                            ICC_PROGRESS_CLASS | ICC_BAR_CLASSES /* tooltips */};
  InitCommonControlsEx(&icc);
  wstr_init(); /* before any thread looks up a string */
  if (!windows_supported()) {
    MessageBoxW(NULL, mc_wstr(MC_STR_WINDOWS_TOO_OLD), mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONERROR);
    return 1;
  }
  int helper_code;
  if (firewall_helper_main(&helper_code)) return helper_code; /* the elevated --add-firewall-rule */
  app_args args;
  int exit_code = parse_args(&args);
  if (exit_code >= 0) {
    free_args(&args);
    return exit_code;
  }
  g_window_w = args.window_w;
  g_window_h = args.window_h;

  CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); /* folder picker */

  mc_init();
  theme_update();

  /* Settings say where the library folder is; the first run asks, once the window is up. */
  wchar_t shown_dir[MAX_PATH];
  g_app.from_source = mc_source_run() != 0;
  g_app.config_dir = config_dir(&args, g_app.from_source, shown_dir, MAX_PATH);
  if (g_app.config_dir && !claim_instance(g_app.config_dir, &args)) {
    free(g_app.config_dir);
    free_args(&args);
    return 0;
  }
  /* Settings' Reset Kithsong… marked the folder and quit: clear it before anything opens it. */
  char *left = NULL;
  mc_status reset = g_app.config_dir ? mc_source_reset_if_requested(g_app.config_dir, g_app.from_source, NULL, &left)
                                     : MC_OK;
  if (reset != MC_OK) {
    wchar_t text[MAX_PATH + 512];
    mc_wformat(text, MAX_PATH + 512, mc_str(MC_STR_RESET_APP_FAILED), MC_S(left ? left : g_app.config_dir),
               MC_S(mc_status_str(reset)));
    message_dialog(NULL, mc_wstr(MC_STR_APP_NAME), text, MB_ICONWARNING);
  }
  mc_free(left);
  if (!g_app.config_dir || mc_settings_load(g_app.config_dir, &g_app.settings) != MC_OK) {
    wchar_t text[MAX_PATH + 128];
    char *shown = wstr_to_utf8(shown_dir);
    mc_wformat(text, MAX_PATH + 128, mc_str(MC_STR_WINDOWS_SETTINGS_LOAD_FAILED), MC_S(shown));
    free(shown);
    message_dialog(NULL, mc_wstr(MC_STR_APP_NAME), text, MB_ICONERROR);
    free(g_app.config_dir);
    free_args(&args);
    return 1;
  }
  /* As early as it can be known; the start itself comes from the system. */
  mc_perf_set_enabled(mc_settings_bool(g_app.settings, MC_PERF_SETTING, 0) || mc_perf_env_enabled());
  mc_perf_mark_at("main_entered", main_us);
  mc_perf_mark("settings_loaded");
  /* Plugins the settings have on: before any library or tethered device needs them. */
  mc_plugins_load(g_app.settings, NULL, 0);
  /* From a checkout there's no asking: a library in the temp folder, unless a test wants the
   * first run itself. If that fails, the first run asks, with no Music folder to offer. */
  if (args.demo_library && !use_demo_library(args.page)) {
    mc_plugins_unload();
    mc_settings_free(g_app.settings);
    free(g_app.config_dir);
    free_args(&args);
    return 2;
  }
  if (g_app.from_source && !args.demo_library && !first_run_test_music())
    mc_source_library_prepare(g_app.config_dir, g_app.settings);
  BOOL first_run = !mc_settings_library_dir(g_app.settings);
  if (!first_run) g_app.lib = open_library(mc_settings_library_dir(g_app.settings)); /* on failure the window says so */
  mc_perf_mark("library_opened");
  size_t art_len = strlen(g_app.config_dir) + sizeof "\\" MC_PLAYLIST_ART_CACHE_DIR;
  if ((g_app.art_dir = malloc(art_len)))
    snprintf(g_app.art_dir, art_len, "%s\\%s", g_app.config_dir, MC_PLAYLIST_ART_CACHE_DIR);
  delete_dialog_init(g_app.config_dir, before_delete);
  remove_here_init(before_remove_here);

  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .lpfnWndProc = wnd_proc,
    .hInstance = inst,
    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
    .hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)),
    .hIconSm = LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                          GetSystemMetrics(SM_CYSMICON), 0),
    .lpszClassName = L"kithsong", /* no background brush: WM_ERASEBKGND paints */
  };
  RegisterClassExW(&wc);

  /* A config folder of its own is shown in the title, to tell instances apart. */
  wchar_t title[MAX_PATH + 32];
  lstrcpynW(title, mc_wstr(MC_STR_APP_NAME), MAX_PATH + 32);
  if (args.config_dir) {
    char *shown = wstr_to_utf8(shown_dir);
    mc_wformat(title, MAX_PATH + 32, mc_str(MC_STR_WINDOWS_TITLE_CONFIG_DIR), MC_S(shown));
    free(shown);
  }
  HWND wnd = CreateWindowExW(0, L"kithsong", title, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                             CW_USEDEFAULT, CW_USEDEFAULT, 1200, 800, NULL, NULL, inst, NULL);
  if (!wnd) return 1;
  if (g_instance_key) SetPropW(wnd, INSTANCE_PROP, (HANDLE)g_instance_key);
  DragAcceptFiles(wnd, TRUE);
  mc_perf_mark("window_created");
  if (mc_perf_enabled()) {
    HDC screen = GetDC(wnd);
    mc_perf_set_refresh_hz(GetDeviceCaps(screen, VREFRESH));
    ReleaseDC(wnd, screen);
  }
  ShowWindow(wnd, show);
  if (args.window_w) set_window_size(wnd, args.window_w, args.window_h);
  mc_perf_mark("window_shown");
  if (first_run && !open_first_library()) {
    DestroyWindow(wnd); /* the first run was closed: quit */
  } else {
    prune_art();
    pair_dialog_init(wnd, &k_pair_host);
    start_sync();
#ifndef NDEBUG
    if (_wgetenv(L"KITHSONG_DEBUG_PAIR")) SetTimer(wnd, TIMER_DEBUG_PAIR, 2000, NULL);
#endif
    firewall_check_later(wnd, g_app.settings);
    import_args(&args);
    mc_perf_mark("sync_started");
    const wchar_t *perf_run = _wgetenv(L"KITHSONG_PERF_RUN");
    if (perf_run && *perf_run) SetTimer(wnd, TIMER_PERF_RUN, 2000, NULL); /* once it has settled */
    if (args.page) g_start_page = utf8_from_wide(args.page);
    if (args.demo_playing) g_demo_playing = utf8_from_wide(args.demo_playing);
    if (g_start_page || g_demo_playing) SetTimer(wnd, TIMER_START_PAGE, START_PAGE_MS, NULL);
  }
  free_args(&args);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (pair_dialog_translate(&msg) || settings_view_translate(&msg) || analysis_dialog_translate(&msg)) continue;
    if (space_play_pause(&msg) || shortcut(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  edit_dialog_wait();
  mc_library_close(g_app.lib); /* sync stopped in WM_DESTROY, tethered devices too */
  if (mc_plugins_flush_settings()) mc_settings_save(g_app.settings); /* what plugins stored themselves */
  mc_plugins_unload();
  mc_settings_free(g_app.settings);
  free(g_app.config_dir);
  free(g_app.art_dir);
  mc_shutdown();
  CoUninitialize();
  if (g_app.font) DeleteObject(g_app.font);
  if (g_app.icon_font) DeleteObject(g_app.icon_font);
  return (int)msg.wParam;
}
