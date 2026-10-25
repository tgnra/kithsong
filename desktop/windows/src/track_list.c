#include "track_list.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "scroller.h"
#include "theme.h"

/* Metrics at 96 DPI. */
enum {
  PAD_X = 20,   /* header text from the edges */
  PAD_TOP = 20, /* above the title */
  SUB_GAP = 2,  /* between the title and the line under it */
  HEADER_BOTTOM = 14,
  ROW_H = 36,
  INSET = 8,   /* rows' fill from the edges */
  CELL_X = 12, /* text inside the fill */
  NUM_W = 28,  /* track number column */
  NUM_GAP = 12,
  DUR_W = 52,
  RADIUS = 4,
};

/* Context menu items. Stubs until playlists and a play queue exist. */
enum { CMD_ADD_TO_PLAYLIST = 1, CMD_PLAY_NEXT, CMD_PLAY_LAST };

typedef struct row {
  char uid[MC_UID_STRLEN];
  wchar_t *title;
  wchar_t num[16];
  wchar_t duration[16];
  int duration_ms;
  char *path;
} row;

static struct {
  HWND wnd;
  UINT dpi;
  HFONT font, title_font, sub_font, icon_font;
  canvas buf;
  scroller scroll;

  mc_library *lib;
  char album_uid[MC_UID_STRLEN];
  wchar_t *album_title;
  wchar_t summary[64]; /* "12 tracks · 47 min" */
  row *rows;
  int count;

  int header_h; /* the fixed part above the rows */
  int hot, selected;
  BOOL tracking;
  int play_index; /* the track TLN_PLAY was last sent for */
  char playing_uid[MC_UID_STRLEN];
} s;

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

static wchar_t *widen(const char *u) {
  if (!u) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n);
  return w;
}

static void format_duration(int ms, wchar_t *out, size_t n) {
  int sec = (ms + 500) / 1000;
  if (ms <= 0) out[0] = L'\0';
  else if (sec >= 3600) swprintf(out, n, L"%d:%02d:%02d", sec / 3600, sec / 60 % 60, sec % 60);
  else swprintf(out, n, L"%d:%02d", sec / 60, sec % 60);
}

/* --- Layout --- */

static int font_height(HFONT font) {
  HDC dc = GetDC(s.wnd);
  HGDIOBJ old = SelectObject(dc, font);
  TEXTMETRICW tm;
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(s.wnd, dc);
  return tm.tmHeight;
}

static void relayout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  s.header_h = scale(PAD_TOP) + font_height(s.title_font) + scale(SUB_GAP) + font_height(s.sub_font) +
               scale(HEADER_BOTTOM);
  RECT view = {0, s.header_h + 1, rc.right, rc.bottom}; /* below the header's divider */
  scroller_resize(&s.scroll, &view, s.count * scale(ROW_H) + 2 * scale(INSET));
}

/* Row i's fill, in client coordinates. */
static RECT row_rect(int i) {
  int top = s.scroll.view.top + scale(INSET) + i * scale(ROW_H) - s.scroll.pos;
  RECT r = {scale(INSET), top, s.scroll.view.right - scale(INSET), top + scale(ROW_H)};
  return r;
}

static void invalidate_row(int i) {
  if (i < 0 || i >= s.count) return;
  RECT r = row_rect(i), isect;
  if (IntersectRect(&isect, &r, &s.scroll.view)) InvalidateRect(s.wnd, &isect, FALSE);
}

static int row_at(int x, int y) {
  POINT pt = {x, y};
  if (!PtInRect(&s.scroll.view, pt)) return -1;
  for (int i = 0; i < s.count; i++) {
    RECT r = row_rect(i);
    if (PtInRect(&r, pt)) return i;
  }
  return -1;
}

static void set_hot(int i) {
  if (i == s.hot) return;
  invalidate_row(s.hot);
  invalidate_row(i);
  s.hot = i;
}

static void select_row(int i) {
  if (i < 0 || i >= s.count) return;
  invalidate_row(s.selected);
  invalidate_row(i);
  s.selected = i;
  int top = scale(INSET) + i * scale(ROW_H);
  scroller_reveal(&s.scroll, top - scale(INSET), top + scale(ROW_H) + scale(INSET));
}

static void on_scrolled(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(s.wnd, &pt);
  set_hot(s.tracking && !s.scroll.hot && !s.scroll.dragging ? row_at(pt.x, pt.y) : -1);
}

/* --- Data --- */

static void clear_rows(void) {
  for (int i = 0; i < s.count; i++) {
    free(s.rows[i].title);
    free(s.rows[i].path);
  }
  free(s.rows);
  s.rows = NULL;
  s.count = 0;
}

void track_list_show(mc_library *lib, const char *album_uid, const wchar_t *title) {
  BOOL same = album_uid && !strcmp(album_uid, s.album_uid);
  char keep[MC_UID_STRLEN] = "";
  if (same && s.selected >= 0 && s.selected < s.count) strcpy(keep, s.rows[s.selected].uid);

  clear_rows();
  free(s.album_title);
  s.album_title = title ? _wcsdup(title) : NULL;
  s.lib = lib;
  s.album_uid[0] = '\0';
  s.hot = s.selected = -1;
  if (!same) s.scroll.pos = s.scroll.target = 0;

  mc_track_list *list = NULL;
  if (lib && album_uid && strlen(album_uid) < MC_UID_STRLEN) {
    strcpy(s.album_uid, album_uid);
    mc_library_list_album_tracks(lib, album_uid, &list);
  }
  int n = list ? list->count : 0, total_ms = 0;
  BOOL discs = FALSE;
  for (int i = 0; i < n; i++) discs |= list->items[i].disc_no > 1;
  s.rows = n ? calloc((size_t)n, sizeof *s.rows) : NULL;
  if (!s.rows) n = 0;
  for (int i = 0; i < n; i++) {
    const mc_track_info *t = &list->items[i];
    row *r = &s.rows[i];
    memcpy(r->uid, t->uid, sizeof r->uid);
    r->title = widen(t->title && *t->title ? t->title : "Untitled");
    if (t->track_no && discs) swprintf(r->num, 16, L"%d-%d", t->disc_no ? t->disc_no : 1, t->track_no);
    else if (t->track_no) swprintf(r->num, 16, L"%d", t->track_no);
    format_duration(t->duration_ms, r->duration, 16);
    r->duration_ms = t->duration_ms;
    r->path = t->path ? _strdup(t->path) : NULL;
    total_ms += t->duration_ms > 0 ? t->duration_ms : 0;
    if (keep[0] && !strcmp(r->uid, keep)) s.selected = i;
  }
  s.count = n;
  mc_track_list_free(list);

  int min = (total_ms + 30000) / 60000;
  const wchar_t *tracks = n == 1 ? L"track" : L"tracks";
  if (!n) wcscpy(s.summary, L"No tracks");
  else if (min >= 60) swprintf(s.summary, 64, L"%d %ls \x00B7 %d hr %d min", n, tracks, min / 60, min % 60);
  else swprintf(s.summary, 64, L"%d %ls \x00B7 %d min", n, tracks, min);

  relayout();
  InvalidateRect(s.wnd, NULL, FALSE);
}

int track_list_count(void) {
  return s.count;
}

track_list_item track_list_item_at(int i) {
  track_list_item item = {0};
  if (i < 0 || i >= s.count) return item;
  const row *r = &s.rows[i];
  item.uid = r->uid;
  item.title = r->title;
  item.duration_ms = r->duration_ms;
  item.path = r->path;
  return item;
}

int track_list_play_index(void) {
  return s.play_index;
}

void track_list_set_playing(const char *uid) {
  const char *now = uid ? uid : "";
  if (!strcmp(now, s.playing_uid) || strlen(now) >= MC_UID_STRLEN) return;
  strcpy(s.playing_uid, now);
  if (s.wnd) InvalidateRect(s.wnd, &s.scroll.view, FALSE);
}

/* The parent starts playback, which then marks the track through track_list_set_playing. */
static void play(int i) {
  if (i < 0 || i >= s.count) return;
  s.play_index = i;
  SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), TLN_PLAY), (LPARAM)s.wnd);
}

/* The menu for the selected track, at pt (screen coordinates). */
static void show_menu(POINT pt) {
  if (s.selected < 0) return;
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, CMD_ADD_TO_PLAYLIST, L"Add to Playlist");
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_PLAY_NEXT, L"Play Next");
  AppendMenuW(menu, MF_STRING, CMD_PLAY_LAST, L"Play Last");
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
  int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, s.wnd, NULL);
  DestroyMenu(menu);
  switch (cmd) {
    case CMD_ADD_TO_PLAYLIST: /* not yet: needs playlists */
    case CMD_PLAY_NEXT:       /* not yet: needs a play queue */
    case CMD_PLAY_LAST:
      break;
  }
}

/* --- Painting --- */

static void paint_row(canvas *c, int i, BOOL focused) {
  const row *r = &s.rows[i];
  RECT fill = row_rect(i);
  BOOL playing = s.playing_uid[0] && !strcmp(r->uid, s.playing_uid);
  COLORREF text = theme.text, dim = theme.text2;

  if (playing && theme.high_contrast) {
    canvas_fill(c, &fill, scale(RADIUS), theme.accent, 255);
    text = dim = theme.on_accent;
  } else {
    control_state state = i == s.selected ? STATE_SELECTED : i == s.hot ? STATE_HOT : STATE_NORMAL;
    text = theme_draw_state(c, &fill, scale(RADIUS), state);
    if (state == STATE_SELECTED && theme.high_contrast) dim = text;
    if (playing) {
      canvas_fill(c, &fill, scale(RADIUS), theme.accent, theme.dark ? 40 : 30);
      text = dim = theme.accent;
    }
  }
  if (focused && i == s.selected) canvas_frame(c, &fill, scale(RADIUS), scale(2), theme.text, 255);

  UINT one_line = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
  RECT num = {fill.left + scale(CELL_X), fill.top, fill.left + scale(CELL_X + NUM_W), fill.bottom};
  RECT dur = {fill.right - scale(CELL_X + DUR_W), fill.top, fill.right - scale(CELL_X), fill.bottom};
  RECT title = {num.right + scale(NUM_GAP), fill.top, dur.left - scale(NUM_GAP), fill.bottom};
  if (playing) { /* a speaker in place of the number */
    SelectObject(c->dc, s.icon_font);
    SetTextColor(c->dc, text);
    DrawTextW(c->dc, L"\xE767", 1, &num, one_line | DT_RIGHT);
  } else {
    SelectObject(c->dc, s.font);
    SetTextColor(c->dc, dim);
    DrawTextW(c->dc, r->num, -1, &num, one_line | DT_RIGHT);
  }
  SelectObject(c->dc, s.font);
  SetTextColor(c->dc, dim);
  DrawTextW(c->dc, r->duration, -1, &dur, one_line | DT_RIGHT);
  SetTextColor(c->dc, text);
  DrawTextW(c->dc, r->title, -1, &title, one_line | DT_END_ELLIPSIS);
}

/* Like the album grid, everything goes into a canvas; GDI text needs its alpha set after. */
static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  canvas_clear(c, dirty, theme.bg, 255);
  HGDIOBJ old_font = SelectObject(c->dc, s.font);

  /* Rows. A row half scrolled under the header spills into it, so the header goes on top. */
  HRGN clip = CreateRectRgnIndirect(&s.scroll.view);
  SelectClipRgn(c->dc, clip);
  BOOL focused = GetFocus() == s.wnd && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
  for (int i = 0; i < s.count; i++) {
    RECT r = row_rect(i), isect;
    if (IntersectRect(&isect, &r, dirty) && IntersectRect(&isect, &r, &s.scroll.view)) paint_row(c, i, focused);
  }
  SelectClipRgn(c->dc, NULL);
  DeleteObject(clip);

  /* Header. */
  RECT header = {0, 0, client.right, s.scroll.view.top};
  canvas_clear(c, &header, theme.bg, 255);
  int x = scale(PAD_X), w = client.right - 2 * scale(PAD_X), y = scale(PAD_TOP);
  int title_h = font_height(s.title_font), sub_h = font_height(s.sub_font);
  RECT title = {x, y, x + w, y + title_h}, sub = {x, title.bottom + scale(SUB_GAP), x + w, title.bottom + scale(SUB_GAP) + sub_h};
  UINT one_line = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS;
  SelectObject(c->dc, s.title_font);
  SetTextColor(c->dc, theme.text);
  if (s.album_title) DrawTextW(c->dc, s.album_title, -1, &title, one_line);
  SelectObject(c->dc, s.sub_font);
  SetTextColor(c->dc, theme.text2);
  DrawTextW(c->dc, s.summary, -1, &sub, one_line);
  RECT divider = {x, s.header_h, x + w, s.header_h + 1};
  canvas_clear(c, &divider, theme.line, 255);
  SelectObject(c->dc, old_font);
  canvas_opaque(c, dirty);

  scroller_paint(&s.scroll, c);
  RECT top = {0, 0, client.right, 1}; /* continues the album grid's top border */
  canvas_clear(c, &top, theme.line, 255);
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- Window --- */

static void on_key(WPARAM key) {
  /* Using the keyboard shows the focus rectangle. */
  SendMessageW(GetAncestor(s.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  int page = (s.scroll.view.bottom - s.scroll.view.top) / scale(ROW_H);
  int cur = s.selected;
  switch (key) {
    case VK_UP: select_row(cur > 0 ? cur - 1 : 0); break;
    case VK_DOWN: select_row(cur < s.count - 1 ? cur + 1 : s.count - 1); break;
    case VK_PRIOR: select_row(cur - page > 0 ? cur - page : 0); break;
    case VK_NEXT: select_row(cur + page < s.count ? cur + page : s.count - 1); break;
    case VK_HOME: select_row(0); break;
    case VK_END: select_row(s.count - 1); break;
    case VK_RETURN: play(s.selected); break;
  }
}

static LRESULT CALLBACK track_list_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!s.wnd) s.wnd = s.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  switch (msg) {
    case WM_SIZE:
      relayout();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc, &ps.rcPaint);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_MOUSEWHEEL:
      scroller_wheel(&s.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_TIMER:
      scroller_timer(&s.scroll, wp);
      return 0;
    case WM_MOUSEMOVE: {
      if (!s.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&tme);
        s.tracking = TRUE;
      }
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      set_hot(scroller_mouse_move(&s.scroll, x, y) ? -1 : row_at(x, y));
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      scroller_mouse_leave(&s.scroll);
      set_hot(-1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
      SetFocus(wnd);
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (msg == WM_RBUTTONDOWN || !scroller_mouse_down(&s.scroll, x, y)) select_row(row_at(x, y));
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      int i = row_at(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (i >= 0) play(i);
      return 0;
    }
    case WM_LBUTTONUP:
      scroller_mouse_up(&s.scroll);
      return 0;
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&s.scroll);
      return 0;
    case WM_CONTEXTMENU: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (lp == -1) { /* from the keyboard: under the selected track */
        if (s.selected < 0) return 0;
        RECT r = row_rect(s.selected);
        pt.x = r.left + scale(CELL_X);
        pt.y = r.bottom;
        ClientToScreen(wnd, &pt);
      } else {
        POINT client = pt;
        ScreenToClient(wnd, &client);
        if (row_at(client.x, client.y) < 0) return 0;
      }
      show_menu(pt);
      return 0;
    }
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      invalidate_row(s.selected);
      break;
    case WM_DESTROY:
      clear_rows();
      free(s.album_title);
      canvas_free(&s.buf);
      HFONT fonts[] = {s.font, s.title_font, s.sub_font, s.icon_font};
      for (int i = 0; i < 4; i++)
        if (fonts[i]) DeleteObject(fonts[i]);
      memset(&s, 0, sizeof s);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND track_list_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DBLCLKS, .lpfnWndProc = track_list_proc,
                    .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"musicapp_tracklist"};
  RegisterClassExW(&wc);

  memset(&s, 0, sizeof s);
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  s.hot = s.selected = -1;
  s.scroll.moved = on_scrolled;
  return CreateWindowExW(0, L"musicapp_tracklist", NULL, WS_CHILD, 0, 0, 0, 0, parent,
                         (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
}

void track_list_update(UINT dpi) {
  s.dpi = dpi;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  HFONT old[] = {s.font, s.title_font, s.sub_font, s.icon_font};

  LOGFONTW lf = ncm.lfMessageFont;
  s.sub_font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 7, 6); /* 9pt -> 10.5pt, as in the sidebar */
  s.font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 5, 3); /* 15pt */
  lf.lfWeight = FW_SEMIBOLD;
  s.title_font = CreateFontIndirectW(&lf);
  s.icon_font = CreateFontW(-scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                            theme_icon_face());
  for (int i = 0; i < 4; i++)
    if (old[i]) DeleteObject(old[i]);

  if (s.wnd) {
    relayout();
    InvalidateRect(s.wnd, NULL, FALSE);
  }
}
