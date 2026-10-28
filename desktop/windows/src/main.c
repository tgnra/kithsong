/* Win32 frontend. Layout:
 *
 *   +--------------------------------------------------------------+
 *   | |<  >  >| [] 0:00 ==o----------------- -3:12 [S]    _ [] X |  toolbar = title bar
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
 * with the library); clicking one shows it on its page. Home is greyed out until there's a
 * home page. Artists lists the artists (artist_view.c); an artist's page is that view's
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
 * minimise, maximise and close buttons. On Windows 11 the toolbar and sidebar show Mica
 * through the extended frame. Everything drawn there needs real alpha, which GDI doesn't
 * give, so it's drawn with canvas.c.
 *
 * The album grid is album_grid.c (which also shows the playlists, on the Playlists page, and
 * the album groups, on the Album Groups page, making their changes itself), the
 * sidebar sidebar.c, the track list track_list.c and the seekbar beside the playback buttons
 * now_playing.c. The track list's Add to Playlist menu is add_to_playlist.c, and the New,
 * Rename and Delete Playlist dialogs playlist_dialogs.c; the track list asks for playlist
 * changes and they're made here. The toolbar buttons, and the shuffle toggle after the seekbar
 * ([S]), are owner-drawn here; shuffle is kept in the settings.
 *
 * Sync (sync_bridge.c) runs while a library is open, and stops before it closes, so switching
 * library restarts it. Its events arrive as WM_APP_SYNC. The sidebar's DEVICES section lists
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
 * Command line: kithsong.exe [--config-dir <dir> | --config-dir=<dir>] [--] [paths...]
 * --config-dir keeps settings.json and sync.db somewhere other than %LOCALAPPDATA%\kithsong,
 * so two instances can sync on one PC; the paths are imported. --add-firewall-rule alone is
 * firewall.c's elevated helper, which adds the Windows Firewall rule sync needs and exits.
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
#include "artist_view.h"
#include "build_info.h"
#include "canvas.h"
#include "delete_dialog.h"
#include "device_view.h"
#include "firewall.h"
#include "import_view.h"
#include "move_library.h"
#include "musiccore/musiccore.h"
#include "now_playing.h"
#include "pair_dialog.h"
#include "pick_folder.h"
#include "playback.h"
#include "playlist_dialogs.h"
#include "resource.h"
#include "search_view.h"
#include "settings_view.h"
#include "sidebar.h"
#include "sync_bridge.h"
#include "theme.h"
#include "track_list.h"
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
  NOW_PLAYING_GAP = 16, /* either side of the seekbar strip and the shuffle button */
  SHUFFLE_GAP = 6,      /* between the seekbar strip and the shuffle button */
  NOW_PLAYING_MAX = 640,
};

/* The main window's timers. */
enum {
  TIMER_REFRESH_DEVICES = 0x5D01, /* one-shot: a sync event asked for a refresh */
  TIMER_POLL_DEVICES,             /* repeating: renames elsewhere fire no event */
  TIMER_REFRESH_LIBRARY,          /* one-shot: sync brought library rows or files */
  TIMER_SAVE_VOLUME,              /* one-shot: the volume changed, so a held key saves once */
  REFRESH_DEVICES_MS = 200,
  SAVE_VOLUME_MS = 1000,
  VOLUME_STEP = 5, /* per press of a volume shortcut */
  SEEK_STEP_MS = 5000,
  REFRESH_LIBRARY_MS = 500,
  POLL_DEVICES_MS = 10000,
  MAX_SYNCING = 16, /* devices marked as syncing in the sidebar at once */
  MAX_ADDRESSES = 8,
  MAX_RECENTS = 10, /* in the sidebar */
};

typedef struct app {
  HWND wnd;
  HWND buttons[NUM_BUTTONS];
  HWND shuffle; /* a toggle, drawn like the playback buttons */
  HWND tooltip;
  HWND sidebar;
  HWND grid;
  HWND tracks;
  HWND now_playing;
  HWND search;
  HWND artists;
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
  char *art_dir;           /* UTF-8: playlists' pictures, in config_dir */
  BOOL refresh_pending;    /* TIMER_REFRESH_DEVICES is set */
  BOOL library_pending;    /* TIMER_REFRESH_LIBRARY is set */
  BOOL volume_pending;     /* TIMER_SAVE_VOLUME is set */
  BOOL locked;             /* an import session has the other pages greyed out */
  char syncing[MAX_SYNCING][MC_UID_STRLEN]; /* devices with a session in progress */
} app;

static app g_app;

static BOOL change_library(BOOL new_empty);
static void move_library(void);
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
  search_view_update(g_app.dpi, font);
  artist_view_update(g_app.dpi, font);
  import_view_set_font(font);
  settings_view_set_font(font, g_app.dpi);
  device_view_set_font(font, g_app.dpi);
  for (int i = 0; i < NUM_BUTTONS; i++) InvalidateRect(g_app.buttons[i], NULL, FALSE);
  InvalidateRect(g_app.shuffle, NULL, FALSE);

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

  HDWP dwp = BeginDeferWindowPos(NUM_BUTTONS + 6);
  int x = pad;
  for (int i = 0; i < NUM_BUTTONS; i++, x += icon + scale(2))
    dwp = DeferWindowPos(dwp, g_app.buttons[i], NULL, x, (bar - icon) / 2, icon, icon, SWP_NOZORDER);
  /* The seekbar and the shuffle button after it take the space between the playback and
   * caption buttons, up to a point, centred in the window where that leaves them clear of
   * both. */
  int min_left = x - scale(2) + scale(NOW_PLAYING_GAP);
  int max_right = w - caption_buttons_width() - scale(NOW_PLAYING_GAP);
  int np_w = max_right - min_left;
  if (np_w > scale(NOW_PLAYING_MAX)) np_w = scale(NOW_PLAYING_MAX);
  int np_left = (w - np_w) / 2;
  if (np_left + np_w > max_right) np_left = max_right - np_w;
  if (np_left < min_left) np_left = min_left;
  int strip_w = np_w - icon - scale(SHUFFLE_GAP);
  dwp = DeferWindowPos(dwp, g_app.now_playing, NULL, np_left, 0, strip_w > 0 ? strip_w : 0, bar, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.shuffle, NULL, np_left + (strip_w > 0 ? strip_w : 0) + scale(SHUFFLE_GAP),
                       (bar - icon) / 2, icon, icon, SWP_NOZORDER);
  dwp = DeferWindowPos(dwp, g_app.sidebar, NULL, 0, bar + side_top, side_w, h - bar - side_top, SWP_NOZORDER);
  UINT library = library_page() ? SWP_SHOWWINDOW : SWP_HIDEWINDOW;
  dwp = DeferWindowPos(dwp, g_app.grid, NULL, side_w, bar, grid_w, h - bar, SWP_NOZORDER | library);
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

static void show_album(void);

/* For PAGE_DEVICE, the device selected in the sidebar (sidebar_select_device()) is shown. */
static void show_page(sidebar_page page) {
  g_app.page = page;
  if (page != PAGE_DEVICE) sidebar_select(page);
  if (library_page())
    album_grid_show(page == PAGE_PLAYLISTS ? AGV_PLAYLISTS : page == PAGE_ALBUM_GROUPS ? AGV_GROUPS : AGV_ALBUMS);
  if (library_page() || page == PAGE_ARTISTS) show_album(); /* the track list is the page's */
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
  track_list_set_details(album_grid_selection_artist(), album_grid_selection_year(), album_grid_selection_cover());
  update_buttons();
  if (g_app.show_tracks != (title != NULL)) {
    g_app.show_tracks = title != NULL;
    layout();
  }
}

/* The sidebar's recents: the albums and playlists last played and the artists and album
 * groups last opened, on any device, smart playlists and groups included. */
static void refresh_recents(void) {
  mc_recent_list *list = NULL;
  if (!g_app.lib || mc_library_list_recents(g_app.lib, 0, MC_LIST_PLAYABLE, &list) != MC_OK) list = NULL;
  sidebar_recent items[MAX_RECENTS];
  wchar_t *names[MAX_RECENTS], *artists[MAX_RECENTS];
  int n = 0;
  for (int i = 0; list && i < list->count && n < MAX_RECENTS; i++) {
    const mc_recent_info *r = &list->items[i];
    BOOL album = r->kind == MC_RECENT_ALBUM;
    names[n] = wide_from_utf8(r->name ? r->name : "");
    artists[n] = album ? wide_from_utf8(r->artist ? r->artist : mc_str(MC_STR_UNKNOWN_ARTIST)) : NULL;
    const wchar_t *kind = mc_wstr(r->kind == MC_RECENT_ARTIST     ? MC_STR_ARTIST
                                  : r->kind == MC_RECENT_PLAYLIST ? (r->is_smart ? MC_STR_SMART_PLAYLIST : MC_STR_PLAYLIST)
                                  : r->is_smart                   ? MC_STR_SMART_ALBUM_GROUP
                                                                  : MC_STR_ALBUM_GROUP);
    items[n] = (sidebar_recent){.kind = r->kind, .name = names[n], .detail = album ? artists[n] : kind};
    snprintf(items[n].uid, sizeof items[n].uid, "%s", r->uid);
    n++;
  }
  mc_recent_list_free(list);
  sidebar_set_recents(items, n);
  for (int i = 0; i < n; i++) {
    free(names[i]);
    free(artists[i]);
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

/* A recent was clicked in the sidebar: its page, with it selected. */
static void open_recent(void) {
  mc_recent_kind kind;
  const char *clicked = sidebar_clicked_recent(&kind);
  if (!clicked) return;
  char uid[MC_UID_STRLEN];
  snprintf(uid, sizeof uid, "%s", clicked); /* the sidebar's copy goes when the recents refresh */
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

/* After the library changed: an import, a playlist edit, or rows from sync, which may have
 * removed the shown album or a recent, or brought recents from other devices. */
static void refresh_library(void) {
  album_grid_set_library(g_app.lib);
  artist_view_set_library(g_app.lib); /* if the open artist went, it goes back (ARN_BACK) */
  show_album(); /* if the album went, the grid says so when it reloads */
  refresh_recents();
  if (g_app.lib) mc_library_search_prepare(g_app.lib); /* the index is ready before the user types */
  search_view_set_library(g_app.lib);
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

/* The playback buttons, and the shuffle button, are bare icons with a fill on hover. Drawn
 * with alpha, as they sit over Mica. Shuffle, while on, is in the accent colour with a dot
 * under it, as Windows' Media Player shows its toggles. */
static void draw_button(const DRAWITEMSTRUCT *dis) {
  static const wchar_t glyphs[NUM_BUTTONS] = {0xE892, 0xE768, 0xE893}; /* Previous, Play, Next */
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas *c = &g_app.button;
  if (!canvas_reserve(c, rc.right, rc.bottom)) return;
  canvas_clear(c, &rc, theme.frame, theme.mica ? 0 : 255);

  BOOL shuffle = dis->CtlID == ID_SHUFFLE, on = shuffle && playback_shuffle();
  BOOL pressed = (dis->itemState & ODS_SELECTED) != 0, disabled = (dis->itemState & ODS_DISABLED) != 0;
  control_state state = disabled ? STATE_NORMAL
                        : pressed ? STATE_PRESSED
                        : dis->hwndItem == g_app.hot_button ? STATE_HOT
                                                            : STATE_NORMAL;
  int radius = scale(6);
  COLORREF color = theme_draw_state(c, &rc, radius, state);
  if (disabled) color = theme.text3;
  else if (on && state != STATE_PRESSED) color = theme.accent;
  wchar_t glyph = shuffle ? 0xE8B1                                         /* Shuffle */
                  : dis->CtlID == ID_PLAY && playback_is_playing() ? 0xE769 /* Pause */
                                                                   : glyphs[dis->CtlID - ID_PREV];
  canvas_text(c, &glyph, 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g_app.icon_font,
              color);
  if (on) {
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
  album_grid_theme_changed();
  layout();
  RedrawWindow(g_app.wnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void create_controls(void) {
  /* The labels aren't shown; they name the buttons for tooltips and screen readers. */
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

  g_app.sidebar = sidebar_create(g_app.wnd, ID_SIDEBAR);

  g_app.grid = album_grid_create(g_app.wnd, ID_GRID);
  album_grid_set_playlist_art_dir(g_app.art_dir);
  g_app.tracks = track_list_create(g_app.wnd, ID_TRACKS);
  g_app.now_playing = now_playing_create(g_app.wnd, ID_NOW_PLAYING);
  g_app.search = search_view_create(g_app.wnd, ID_SEARCH);
  g_app.artists = artist_view_create(g_app.wnd, ID_ARTISTS);
  refresh_library();

  import_view_create(g_app.wnd, g_app.lib);
  settings_view_create(g_app.wnd);
  settings_view_set_library_dir(mc_settings_library_dir(g_app.settings));
  settings_view_init_sync(g_app.settings, schedule_refresh);
  static const device_view_host host = {rename_self, schedule_refresh};
  device_view_create(g_app.wnd, &host);
}

/* Plays the shown album, playlist or artist's tracks in order, from track start on; or with
 * start -1 from the first track, which shuffled is any. Tracks with no file on this device
 * are left out of the queue. Playing it puts it first in the recents, as on Android. */
static void play_album(int start) {
  int n = track_list_count(), count = 0, first = start < 0 ? -1 : 0;
  playback_track *queue = n ? calloc((size_t)n, sizeof *queue) : NULL;
  if (!queue) return;
  for (int i = 0; i < n; i++) {
    track_list_item t = track_list_item_at(i);
    if (!t.path) continue;
    if (start >= 0 && i <= start) first = count;
    queue[count++] = (playback_track){t.uid, t.path, t.title, t.artist, t.album, t.cover_dir, t.duration_ms};
  }
  if (count) playback_play(queue, count, first);
  free(queue);

  char uid[MC_UID_STRLEN] = "";
  mc_recent_kind kind = album_grid_showing_playlists() ? MC_RECENT_PLAYLIST : MC_RECENT_ALBUM;
  if (artist_page()) {
    kind = MC_RECENT_ARTIST;
    snprintf(uid, sizeof uid, "%s", artist_view_artist());
  } else if (!album_grid_selection(uid)) {
    uid[0] = '\0';
  }
  if (count && g_app.lib && uid[0] && mc_library_record_opened(g_app.lib, kind, uid) == MC_OK) refresh_recents();
}

/* A button's name, for screen readers, and its tooltip. */
static void set_button_label(HWND button, const wchar_t *label) {
  SetWindowTextW(button, label);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = g_app.wnd, .uId = (UINT_PTR)button, .lpszText = (wchar_t *)label};
  SendMessageW(g_app.tooltip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
  InvalidateRect(button, NULL, FALSE);
}

/* The Play button turns into Pause while playing; Shuffle shows whether it's on. */
static void playback_changed(void) {
  update_buttons();
  set_button_label(g_app.buttons[ID_PLAY - ID_PREV], mc_wstr(playback_is_playing() ? MC_STR_PAUSE : MC_STR_PLAY));
  set_button_label(g_app.shuffle, mc_wstr(playback_shuffle() ? MC_STR_SHUFFLE_ON : MC_STR_SHUFFLE_OFF));
}

/* Shuffle on or off, kept in the settings for next time. */
static void set_shuffle(BOOL on) {
  if (!on == !playback_shuffle()) return;
  playback_set_shuffle(on);
  mc_settings_set_shuffle(g_app.settings, on);
  mc_settings_save(g_app.settings); /* failing, it's saved with the next setting */
}

/* Volume 0 to 100, kept in the settings for next time. Changing it unmutes. Says so in the
 * now-playing strip for a moment, as there's no volume control to show it. */
static void set_volume(int volume) {
  playback_set_muted(FALSE);
  playback_set_volume(volume);
  wchar_t text[64];
  mc_wformat(text, 64, mc_str(MC_STR_VOLUME_LEVEL), MC_D(playback_volume()));
  now_playing_flash(text);
  mc_settings_set_volume(g_app.settings, playback_volume());
  g_app.volume_pending = TRUE;
  SetTimer(g_app.wnd, TIMER_SAVE_VOLUME, SAVE_VOLUME_MS, NULL);
}

static void save_volume(void) {
  KillTimer(g_app.wnd, TIMER_SAVE_VOLUME);
  if (g_app.volume_pending) mc_settings_save(g_app.settings); /* failing, it's saved with the next setting */
  g_app.volume_pending = FALSE;
}

static void toggle_muted(void) {
  playback_set_muted(!playback_muted());
  wchar_t text[64];
  if (playback_muted()) lstrcpynW(text, mc_wstr(MC_STR_VOLUME_MUTED), 64);
  else mc_wformat(text, 64, mc_str(MC_STR_VOLUME_LEVEL), MC_D(playback_volume()));
  now_playing_flash(text);
}

/* The track list's Play and Shuffle buttons: the whole album, playlist or artist's tracks,
 * in order from the first track, or shuffled. */
static void play_all(BOOL shuffled) {
  set_shuffle(shuffled);
  play_album(shuffled ? -1 : 0);
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
      if (!changed)
        MessageBoxW(g_app.wnd, mc_wstr(MC_STR_PLAYLIST_REMOVE_FAILED), mc_wstr(MC_STR_PLAYLIST), MB_OK | MB_ICONERROR);
      break;
    case TLN_MOVE:
      changed = mc_playlist_move(g_app.lib, req->entry_uid, req->after_uid[0] ? req->after_uid : NULL) == MC_OK;
      if (!changed)
        MessageBoxW(g_app.wnd, mc_wstr(MC_STR_PLAYLIST_MOVE_FAILED), mc_wstr(MC_STR_PLAYLIST), MB_OK | MB_ICONERROR);
      break;
    case TLN_RENAME_PLAYLIST:
      if (name && !strcmp(uid, req->playlist_uid)) changed = playlist_rename_dialog(g_app.wnd, g_app.lib, uid, name);
      break;
    case TLN_DELETE_PLAYLIST:
      if (name && !strcmp(uid, req->playlist_uid)) changed = playlist_delete_dialog(g_app.wnd, g_app.lib, uid, name);
      break;
    case TLN_DELETE_TRACK:
      for (int i = 0, n = track_list_count(); i < n; i++) {
        track_list_item it = track_list_item_at(i);
        if (strcmp(it.uid, req->track_uid)) continue;
        changed = delete_dialog(g_app.wnd, g_app.lib, MC_DELETE_TRACKS, req->track_uid, it.title);
        break;
      }
      break;
    case TLN_RESHUFFLE: /* a smart playlist's new order */
      if (!name || strcmp(uid, req->playlist_uid)) break;
      changed = mc_smart_reshuffle(g_app.lib, uid) == MC_OK;
      if (!changed)
        MessageBoxW(g_app.wnd, mc_wstr(MC_STR_RESHUFFLE_FAILED), mc_wstr(MC_STR_SMART_PLAYLIST), MB_OK | MB_ICONERROR);
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

/* The sidebar's footer: the version, and where other devices reach this PC (the first
 * address; its tooltip lists them all). */
static void update_footer(void) {
  wchar_t line1[160], line2[160], tip[64 + MAX_ADDRESSES * 64] = L"";
  if (g_app.lib)
    mc_wformat(line1, 160, mc_str(MC_STR_WINDOWS_FOOTER_VERSION_DB), MC_S(mc_version()), MC_D(KITHSONG_BUILD_NUMBER),
               MC_D(mc_library_schema_version(g_app.lib)));
  else
    mc_wformat(line1, 160, mc_str(MC_STR_WINDOWS_FOOTER_VERSION), MC_S(mc_version()), MC_D(KITHSONG_BUILD_NUMBER));

  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    mc_wformat(line2, 160, mc_str(MC_STR_WINDOWS_FOOTER_SYNC_OFF),
               MC_S(g_app.lib ? mc_status_str(sync_bridge_error()) : mc_str(MC_STR_WINDOWS_NO_LIBRARY)));
  } else {
    char addrs[MAX_ADDRESSES][MC_SYNC_ADDR_STRLEN];
    unsigned port = mc_sync_port(sync);
    int n = mc_sync_local_addresses(addrs, MAX_ADDRESSES);
    if (n > 0) format_address(line2, 160, addrs[0], port);
    else mc_wformat(line2, 160, mc_str(MC_STR_WINDOWS_PORT_NO_NETWORK), MC_D(port));
    size_t used = (size_t)swprintf(tip, 64, L"%ls",
                                   mc_wstr(n > 0 ? MC_STR_WINDOWS_FOOTER_REACHED_AT : MC_STR_NO_NETWORK));
    for (int i = 0; i < n; i++) {
      wchar_t addr[64];
      format_address(addr, 64, addrs[i], port);
      int k = swprintf(tip + used, sizeof tip / sizeof tip[0] - used, L"\n%ls", addr);
      if (k > 0) used += (size_t)k;
    }
  }
  sidebar_set_footer(line1, line2);
  sidebar_set_footer_tip(tip[0] ? tip : NULL);
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
    /* This PC first, then the online devices, then the offline ones, each in the order
     * listed (by name); removed ones left out. */
    for (int pass = 0; pass < 3; pass++)
      for (int i = 0; i < np; i++) {
        const mc_sync_device *d = &paired->items[i];
        int in_pass = d->is_self ? 0 : d->online ? 1 : 2;
        if (d->removed || in_pass != pass) continue;
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
  /* For the empty pages: pairing while this PC isn't in a group, the sync profile once it is. */
  int members = 0;
  for (int i = 0; i < np; i++) members += !paired->items[i].removed;
  album_grid_set_sync_state(!sync ? AG_SYNC_OFF : members ? AG_SYNC_GROUPED : AG_SYNC_ALONE);
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
    MessageBoxW(g_app.wnd, mc_wstr(MC_STR_NAME_SAVE_FAILED), mc_wstr(MC_STR_SETTINGS), MB_OK | MB_ICONWARNING);
  schedule_refresh();
}

/* Deleted tracks whose files are back from the Recycle Bin come back into the library
 * (musiccore/delete.h). Cheap: run at startup, on switching libraries and whenever the
 * window is activated. */
static void restore_deleted(void) {
  int restored = 0;
  if (g_app.lib && g_app.config_dir && mc_delete_restore(g_app.lib, g_app.config_dir, &restored) == MC_OK &&
      restored)
    refresh_library();
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

static void start_sync(void) {
  restore_deleted(); /* the library just opened */
  memset(g_app.syncing, 0, sizeof g_app.syncing);
  sync_bridge_start(g_app.wnd, g_app.lib, g_app.config_dir, mc_settings_device_name(g_app.settings));
  playback_set_library(g_app.lib, g_app.config_dir);
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
  playback_set_library(NULL, NULL);

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
  }
  free(ev);
}

static void sync_off_message(void) {
  wchar_t text[256];
  mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_PAIR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
  MessageBoxW(g_app.wnd, text, mc_wstr(MC_STR_PAIR_DEVICE), MB_OK | MB_ICONINFORMATION);
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
  MessageBoxW(g_app.wnd, text, mc_wstr(MC_STR_PAIR_DEVICE), MB_OK | MB_ICONINFORMATION);
}

/* Asks nearby device clicked to pair: one clicked in the sidebar, or picked from the empty
 * Albums page's Pair with a Device... menu. Even one in another group: pairing then says why
 * it can't. */
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
        name = wide_from_utf8(nearby->items[i].name ? nearby->items[i].name : "");
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

/* The empty Albums page's Pair with a Device...: a menu of the devices nearby, as the
 * sidebar's DEVICES lists them, then Add Device by Address..., beneath the button. */
static void pair_menu(void) {
  enum { CMD_ADDRESS = 1, CMD_DEVICE_FIRST = 100 };
  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    sync_off_message();
    return;
  }
  RECT button;
  if (!album_grid_action_rect(&button)) return;
  mc_sync_nearby_list *nearby = NULL;
  if (mc_sync_list_nearby(sync, &nearby) != MC_OK) nearby = NULL;
  HMENU menu = CreatePopupMenu();
  int n = nearby ? nearby->count : 0;
  for (int i = 0; i < n; i++) {
    const mc_sync_nearby *d = &nearby->items[i];
    wchar_t *name = wide_from_utf8(d->name && *d->name ? d->name : mc_str(MC_STR_DEVICE_UNNAMED));
    AppendMenuW(menu, MF_STRING, CMD_DEVICE_FIRST + i, name ? name : L"");
    free(name);
  }
  if (n == 0) AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, mc_wstr(MC_STR_DEVICES_NONE));
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_ADDRESS, mc_wstr_mnemonic(MC_STR_ADD_DEVICE_BY_ADDRESS_MENU));
  UINT flags = TPM_RETURNCMD | TPM_LEFTBUTTON | TPM_CENTERALIGN | TPM_TOPALIGN;
  TPMPARAMS avoid = {sizeof avoid, button};
  int cmd = TrackPopupMenuEx(menu, flags, (button.left + button.right) / 2, button.bottom, g_app.wnd, &avoid);
  DestroyMenu(menu);
  if (cmd >= CMD_DEVICE_FIRST && cmd - CMD_DEVICE_FIRST < n) {
    char uid[MC_UID_STRLEN];
    snprintf(uid, sizeof uid, "%s", nearby->items[cmd - CMD_DEVICE_FIRST].uid);
    mc_sync_nearby_list_free(nearby);
    pair_nearby_uid(uid);
    return;
  }
  mc_sync_nearby_list_free(nearby);
  if (cmd == CMD_ADDRESS) add_device_show(g_app.wnd, on_add_device_connect, NULL);
}

/* The empty Playlists page's Choose What to Sync: this PC's own page, whose What to Sync
 * picks the playlists it holds. */
static void show_own_page(void) {
  mc_sync *sync = sync_bridge_get();
  if (!sync) return;
  sidebar_select_device(mc_sync_device_uid(sync));
  if (sidebar_selected_device()) show_page(PAGE_DEVICE);
}


/* --- The pairing dialogs' side --- */

/* Choose What to Sync: the device's page (this PC's own, when it joined). */
static void pair_show_device(const char *uid) {
  refresh_devices();
  sidebar_select_device(uid);
  if (sidebar_selected_device()) show_page(PAGE_DEVICE);
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
      } else if (code == SBN_ADD_DEVICE) add_device_show(g_app.wnd, on_add_device_connect, NULL);
      else if (code == SBN_PAIR_DEVICE) pair_nearby_uid(sidebar_clicked_nearby());
      else if (code == SBN_OPEN_RECENT) open_recent();
      break;
    case ID_GRID:
      if (code == AGN_SELCHANGE) show_album();
      else if (code == AGN_LIBRARY_CHANGED) refresh_library();
      else if (code == AGN_PAIR) pair_menu();
      else if (code == AGN_SYNC_SETTINGS) show_own_page();
      else if (code == AGN_OPEN_GROUP && g_app.lib && album_grid_group() &&
               mc_library_record_opened(g_app.lib, MC_RECENT_ALBUM_GROUP, album_grid_group()) == MC_OK)
        refresh_recents();
      break;
    case ID_ARTISTS:
      if (code == ARN_OPEN) {
        artist_opened();
      } else if (code == ARN_LIBRARY_CHANGED) {
        refresh_library();
      } else if (code == ARN_BACK) {
        show_album(); /* empties the track list */
        layout();
      } else if (code == ARN_OPEN_ALBUM) {
        char uid[MC_UID_STRLEN];
        snprintf(uid, sizeof uid, "%s", artist_view_opened_album());
        show_page(PAGE_ALBUMS);
        /* The track list comes in now, narrowing the grid, which drops a scroll under way;
         * selecting again scrolls the album into view at the grid's new width. */
        if (album_grid_select(uid)) {
          show_album();
          album_grid_select(uid);
        }
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
    case ID_TRACKS:
      if (code == TLN_PLAY) play_album(track_list_play_index());
      else if (code == TLN_PLAY_ALL || code == TLN_SHUFFLE_ALL) play_all(code == TLN_SHUFFLE_ALL);
      else if (code == TLN_CLOSE && artist_page()) { /* Escape on an artist's page: back to the list */
        artist_view_back();
        show_album();
        layout();
        SetFocus(g_app.artists);
      } else if (code == TLN_CLOSE) { /* unselecting the album hides the list */
        album_grid_unselect();
        SetFocus(g_app.grid);
      } else on_track_request(code);
      break;
    case ID_SEARCH:
      if (code == SVN_OPEN) open_search_result();
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
      if ((dis->CtlID >= ID_PREV && dis->CtlID < ID_PREV + NUM_BUTTONS) || dis->CtlID == ID_SHUFFLE) draw_button(dis);
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
      if (wp) restore_deleted();
      break;
    case WM_APPCOMMAND: /* the mouse's back button, over the track list on an artist's page */
      if (GET_APPCOMMAND_LPARAM(lp) == APPCOMMAND_BROWSER_BACKWARD && artist_page()) {
        on_command(ID_TRACKS, TLN_CLOSE);
        return TRUE;
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
      if (wp == TIMER_REFRESH_LIBRARY) {
        KillTimer(wnd, TIMER_REFRESH_LIBRARY);
        g_app.library_pending = FALSE;
        refresh_library();
        return 0;
      }
      if (wp == TIMER_SAVE_VOLUME) {
        save_volume();
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
      g_app.locked = (BOOL)wp;
      sidebar_set_locked((BOOL)wp);
      layout(); /* the import view switches between the drop zone and the list */
      return 0;
    case WM_APP_OPEN_LIBRARY:
      change_library(FALSE);
      return 0;
    case WM_APP_MOVE_LIBRARY:
      move_library();
      return 0;
    case WM_CLOSE:
      if (import_view_confirm_close()) DestroyWindow(wnd);
      return 0;
    case WM_DESTROY:
      KillTimer(wnd, TIMER_REFRESH_DEVICES);
      KillTimer(wnd, TIMER_POLL_DEVICES);
      KillTimer(wnd, TIMER_REFRESH_LIBRARY);
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

/* Where settings.json and sync.db live: --config-dir made absolute, or
 * %LOCALAPPDATA%\kithsong. Created if missing. Returns UTF-8, or NULL (*shown says which
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
      lstrcpynW(shown, L"%LOCALAPPDATA%\\kithsong", (int)cap);
      return NULL;
    }
    wchar_t old[MAX_PATH];
    wcscpy(old, dir);
    wcscat(old, L"\\musicapp");
    wcscat(dir, L"\\kithsong");
    lstrcpynW(shown, dir, (int)cap);
    char *old8 = utf8_from_wide(old), *dir8 = utf8_from_wide(dir);
    mc_settings_move_old_dir(old8, dir8);
    free(old8);
    free(dir8);
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
 * is offered; otherwise the folder picker opens straight away, titled title_id, and with
 * empty_only it takes only an empty folder. Returns a malloc'd UTF-8 path, or NULL if the
 * user gave up. */
static char *choose_library_dir(HWND owner, const char *current, enum mc_str_id title_id, BOOL empty_only) {
  char *suggested = current ? NULL : music_folder();
  for (;;) {
    char *dir = NULL;
    if (!current && suggested) {
      wchar_t text[2048];
      mc_wformat(text, 2048, mc_str(MC_STR_WINDOWS_WELCOME_TEXT), MC_S(suggested));
      int answer = MessageBoxW(owner, text, mc_wstr(MC_STR_WINDOWS_WELCOME_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION);
      if (answer == IDCANCEL) break;
      if (answer == IDYES && (dir = malloc(strlen(suggested) + 1))) strcpy(dir, suggested);
    }
    if (!dir) dir = pick_folder(owner, mc_wstr(title_id), current ? current : suggested);
    if (!dir) {
      if (current) break; /* changing location: cancel means keep the current one */
      continue;
    }
    if (empty_only && !mc_folder_is_empty(dir)) {
      MessageBoxW(owner, mc_wstr(MC_STR_LIBRARY_NOT_EMPTY), mc_wstr(MC_STR_LIBRARY_FOLDER), MB_OK | MB_ICONINFORMATION);
      free(dir);
      continue;
    }
    if (in_onedrive(dir)) {
      int answer = MessageBoxW(owner, mc_wstr(MC_STR_WINDOWS_ONEDRIVE_WARNING), mc_wstr(MC_STR_LIBRARY_FOLDER),
                               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
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
    MessageBoxW(g_app.wnd, text, mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONERROR);
    return NULL;
  }
  return lib;
}

/* Settings' Open Another Library: switch to the library in another folder. The new one opens
 * first, so if it can't, nothing changes; sync then moves from the old one to it. With
 * new_empty (the pairing dialog's Create a New Library) the folder must be empty, so the
 * library made there is too. TRUE if it switched. */
static BOOL change_library(BOOL new_empty) {
  const char *current = mc_settings_library_dir(g_app.settings);
  if (!import_view_confirm_close()) return FALSE;

  char *dir = choose_library_dir(g_app.wnd, current, new_empty ? MC_STR_LIBRARY_FOLDER_NEW : MC_STR_LIBRARY_FOLDER_CHOOSE,
                                 new_empty);
  mc_library *lib = dir ? open_library(dir) : NULL;
  if (lib) {
    stop_sync();
    mc_library_close(g_app.lib);
    g_app.lib = lib;
    mc_settings_set_library_dir(g_app.settings, dir);
    if (mc_settings_save(g_app.settings) != MC_OK)
      MessageBoxW(g_app.wnd, mc_wstr(MC_STR_SETTINGS_SAVE_FAILED), mc_wstr(MC_STR_SETTINGS), MB_OK | MB_ICONWARNING);
    import_view_set_library(g_app.lib);
    settings_view_set_library_dir(dir);
    refresh_library();
    start_sync();
  }
  free(dir);
  return lib != NULL;
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
 * Backspace stay the box's; the Ctrl+letter, digit and comma ones work there too. Toggles
 * act once while a key is held; volume and seeking repeat. TRUE if msg was a shortcut. */
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
        if (!repeat) {
          set_shuffle(!playback_shuffle());
          now_playing_flash(mc_wstr(playback_shuffle() ? MC_STR_SHUFFLE_ON : MC_STR_SHUFFLE_OFF));
        }
        return TRUE;
      case 'F':
        if (!repeat) shortcut_page(PAGE_SEARCH);
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

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
  (void)prev;
  (void)cmd;

  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES |
                                            ICC_PROGRESS_CLASS | ICC_BAR_CLASSES /* tooltips */};
  InitCommonControlsEx(&icc);
  wstr_init(); /* before any thread looks up a string */
  int helper_code;
  if (firewall_helper_main(&helper_code)) return helper_code; /* the elevated --add-firewall-rule */
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
    char *shown = wstr_to_utf8(shown_dir);
    mc_wformat(text, MAX_PATH + 128, mc_str(MC_STR_WINDOWS_SETTINGS_LOAD_FAILED), MC_S(shown));
    free(shown);
    MessageBoxW(NULL, text, mc_wstr(MC_STR_APP_NAME), MB_OK | MB_ICONERROR);
    free(g_app.config_dir);
    free_args(&args);
    return 1;
  }
  if (!mc_settings_library_dir(g_app.settings)) {
    char *dir = choose_library_dir(NULL, NULL, MC_STR_LIBRARY_FOLDER_CHOOSE, FALSE);
    if (!dir) return 0;
    mc_settings_set_library_dir(g_app.settings, dir);
    free(dir);
    if (mc_settings_save(g_app.settings) != MC_OK)
      MessageBoxW(NULL, mc_wstr(MC_STR_WINDOWS_SETTINGS_FIRST_SAVE_FAILED), mc_wstr(MC_STR_APP_NAME),
                  MB_OK | MB_ICONWARNING);
  }
  g_app.lib = open_library(mc_settings_library_dir(g_app.settings)); /* on failure the window says so */
  size_t art_len = strlen(g_app.config_dir) + sizeof "\\" MC_PLAYLIST_ART_CACHE_DIR;
  if ((g_app.art_dir = malloc(art_len)))
    snprintf(g_app.art_dir, art_len, "%s\\%s", g_app.config_dir, MC_PLAYLIST_ART_CACHE_DIR);
  prune_art();
  delete_dialog_init(g_app.config_dir, before_delete);

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
  DragAcceptFiles(wnd, TRUE);
  ShowWindow(wnd, show);
  pair_dialog_init(wnd, &k_pair_host);
  start_sync();
  firewall_check_later(wnd, g_app.settings);
  import_args(&args);
  free_args(&args);

  MSG msg;
  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    if (pair_dialog_translate(&msg)) continue;
    if (space_play_pause(&msg) || shortcut(&msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  mc_library_close(g_app.lib); /* sync stopped in WM_DESTROY */
  mc_settings_free(g_app.settings);
  free(g_app.config_dir);
  free(g_app.art_dir);
  mc_shutdown();
  CoUninitialize();
  if (g_app.font) DeleteObject(g_app.font);
  if (g_app.icon_font) DeleteObject(g_app.icon_font);
  return (int)msg.wParam;
}
