/* See stats_dialog.h. */
#include "stats_dialog.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomation.h>

#include "canvas.h"
#include "dlg_template.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

#define WM_APP_READ (WM_APP + 1) /* the thread has finished */

/* Metrics at 96 DPI. */
enum {
  MARGIN = 16,
  GAP = 12,       /* between the cards, and above the button */
  HEADING_H = 28, /* a card's heading, above it */
  ROW_H = 32,
  PAD = 14,       /* between a card's edges and its text */
  COLUMN_W = 250, /* at least */
  LABEL_GAP = 24, /* at least, between a row's label and its number */
  MESSAGE_H = 200,
  MESSAGE_ICON_H = 64,
  BUTTON_W = 96,
  BUTTON_H = 32,
  BUTTON_RADIUS = 4,
};

enum { MAX_SECTIONS = 4, MAX_ROWS = 18 };

/* How long opening waits for the numbers, in ms: too short to notice, and long enough for them
 * to be there in all but a huge library on a cold disk, so that the dialog opens as it'll stay. */
#define OPEN_WAIT_MS 100

typedef struct stat_row {
  const wchar_t *label;
  wchar_t value[48];
  RECT rc; /* in the panel */
} stat_row;

/* A card, with its heading above it. */
typedef struct section {
  const wchar_t *heading;
  int column;
  int first, n; /* its rows in g.rows */
  RECT head, card;
} section;

static struct {
  HWND dlg, panel, close;
  UINT dpi;
  BOOL mica, hot; /* hot: the mouse is over Close */
  HFONT font, heading_font, title_font, icon_font;
  canvas surface;
  uia_host ui;

  section sections[MAX_SECTIONS];
  int nsections;
  stat_row rows[MAX_ROWS];
  int nrows;
  /* In place of the sections: Counting…, the empty library's text, or why they can't be read.
   * The title and icon are for the empty library only. */
  const wchar_t *message_title;
  wchar_t message[256];

  /* The thread is one read: it fills in status and stats, which are left alone until it has
   * ended (read_finished()). */
  char *dir;
  HANDLE thread;
  BOOL again; /* the library changed during this read */
  mc_status status;
  mc_library_stats stats;
} g;

static int scale(int v) {
  return MulDiv(v, (int)g.dpi, USER_DEFAULT_SCREEN_DPI);
}

/* --- Reading --- */

static DWORD WINAPI read_main(void *arg) {
  mc_library *lib = NULL;
  mc_status st = mc_library_open(g.dir, &lib);
  if (st == MC_OK) {
    st = mc_library_read_stats(lib, &g.stats);
    mc_library_close(lib);
  }
  g.status = st;
  PostMessageW((HWND)arg, WM_APP_READ, 0, 0);
  return 0;
}

static void read_start(void) {
  if (g.thread) {
    g.again = TRUE;
    return;
  }
  g.thread = CreateThread(NULL, 0, read_main, g.dlg, 0, NULL);
  if (!g.thread) g.status = MC_ERR_NOMEM;
}

/* --- What's shown --- */

static stat_row *add_row(const wchar_t *label) {
  if (g.nrows >= MAX_ROWS) return NULL;
  stat_row *r = &g.rows[g.nrows++];
  r->label = label;
  r->value[0] = L'\0';
  if (g.nsections) g.sections[g.nsections - 1].n++;
  return r;
}

static void add_section(enum mc_str_id heading, int column) {
  if (g.nsections >= MAX_SECTIONS) return;
  g.sections[g.nsections++] = (section){.heading = mc_wstr(heading), .column = column, .first = g.nrows};
}

static void add_count(enum mc_str_id label, int64_t n) {
  stat_row *r = add_row(mc_wstr(label));
  if (r) wstr_count(r->value, sizeof r->value / sizeof r->value[0], n);
}

static void add_time(enum mc_str_id label, int64_t ms) {
  stat_row *r = add_row(mc_wstr(label));
  if (!r) return;
  size_t n = sizeof r->value / sizeof r->value[0];
  char text[64];
  if (mc_format_playing_time(text, sizeof text, ms)) wstr_from_utf8(r->value, n, text);
  else mc_wformat(r->value, n, mc_str(MC_STR_DURATION_MINUTES), MC_D(0));
}

static void add_tally(const mc_library_tally *t) {
  add_count(MC_STR_TRACKS_TITLE, t->tracks);
  add_count(MC_STR_SECTION_ALBUMS, t->albums);
  add_count(MC_STR_SECTION_SINGLES, t->singles);
  add_count(MC_STR_SECTION_ARTISTS, t->artists);
  add_count(MC_STR_STATS_GENRES, t->genres);
  add_time(MC_STR_STATS_PLAYING_TIME, t->duration_ms);
}

/* Makes the sections or the message from g.stats and g.status. */
static void build(void) {
  const mc_library_stats *s = &g.stats;
  g.nsections = g.nrows = 0;
  g.message_title = NULL;
  g.message[0] = L'\0';
  if (g.status != MC_OK) {
    mc_wformat(g.message, 256, mc_str(MC_STR_WINDOWS_STATS_FAILED), MC_S(mc_status_str(g.status)));
    return;
  }
  if (!s->all.tracks) {
    g.message_title = mc_wstr(MC_STR_EMPTY_TITLE);
    lstrcpynW(g.message, mc_wstr(MC_STR_STATS_EMPTY_TEXT), 256);
    return;
  }
  /* The same numbers twice say nothing when all of it is here. */
  BOOL whole = memcmp(&s->all, &s->here, sizeof s->all) != 0;
  add_section(MC_STR_WINDOWS_STATS_THIS_PC, 0);
  add_tally(&s->here);
  stat_row *size = add_row(mc_wstr(MC_STR_STATS_SIZE));
  if (size) wstr_size(size->value, sizeof size->value / sizeof size->value[0], s->size_bytes);
  if (whole) {
    add_section(MC_STR_STATS_ALL, 1);
    add_tally(&s->all);
  }
  add_section(MC_STR_STATS_LISTS, whole ? 0 : 1);
  add_count(MC_STR_SECTION_PLAYLISTS, s->playlists + s->smart_playlists);
  add_count(MC_STR_SECTION_ALBUM_GROUPS, s->album_groups + s->smart_album_groups);
  add_section(MC_STR_STATS_LISTENING, 1);
  add_count(MC_STR_STATS_PLAYS, s->plays);
  add_count(MC_STR_STATS_TRACKS_PLAYED, s->tracks_played);
  add_time(MC_STR_STATS_TIME_LISTENED, s->listened_ms);
}

/* --- Layout --- */

static int text_w(HDC dc, HFONT font, const wchar_t *text) {
  SIZE size = {0};
  HGDIOBJ old = SelectObject(dc, font);
  GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
  SelectObject(dc, old);
  return size.cx;
}

/* As wide as the longest row needs, whatever the language. */
static int column_w(void) {
  int label = 0, value = 0;
  HDC dc = GetDC(g.panel);
  for (int i = 0; i < g.nrows; i++) {
    label = max(label, text_w(dc, g.font, g.rows[i].label));
    value = max(value, text_w(dc, g.font, g.rows[i].value));
  }
  for (int i = 0; i < g.nsections; i++) label = max(label, text_w(dc, g.heading_font, g.sections[i].heading) - value);
  ReleaseDC(g.panel, dc);
  return max(scale(COLUMN_W), label + value + scale(LABEL_GAP + 2 * PAD));
}

/* Places the cards and rows, and sizes the dialog around them with its centre at (cx, cy), on
 * the screen. */
static void place(int cx, int cy) {
  int w = column_w(), y[2] = {scale(MARGIN), scale(MARGIN)};
  for (int i = 0; i < g.nsections; i++) {
    section *s = &g.sections[i];
    int x = scale(MARGIN) + s->column * (w + scale(GAP)), top = y[s->column];
    SetRect(&s->head, x, top, x + w, top + scale(HEADING_H));
    SetRect(&s->card, x, s->head.bottom, x + w, s->head.bottom + s->n * scale(ROW_H));
    for (int j = 0; j < s->n; j++) {
      int row_top = s->card.top + j * scale(ROW_H);
      SetRect(&g.rows[s->first + j].rc, x, row_top, x + w, row_top + scale(ROW_H));
    }
    y[s->column] = s->card.bottom + scale(GAP);
  }
  int bottom = g.nsections ? max(y[0], y[1]) - scale(GAP) : scale(MARGIN + MESSAGE_H);
  RECT client = {0, 0, 2 * (scale(MARGIN) + w) + scale(GAP), bottom + scale(GAP + BUTTON_H + MARGIN)};
  SetWindowPos(g.panel, NULL, 0, 0, client.right, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
  SetWindowPos(g.close, NULL, client.right - scale(MARGIN + BUTTON_W), client.bottom - scale(MARGIN + BUTTON_H),
               scale(BUTTON_W), scale(BUTTON_H), SWP_NOZORDER | SWP_NOACTIVATE);

  DWORD style = (DWORD)GetWindowLongW(g.dlg, GWL_STYLE), ex = (DWORD)GetWindowLongW(g.dlg, GWL_EXSTYLE);
  AdjustWindowRectExForDpi(&client, style, FALSE, ex, g.dpi);
  int width = client.right - client.left, height = client.bottom - client.top;
  int x = cx - width / 2, top = cy - height / 2;
  POINT centre = {cx, cy};
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromPoint(centre, MONITOR_DEFAULTTONEAREST), &mi)) {
    x = max((int)mi.rcWork.left, min(x, (int)mi.rcWork.right - width));
    top = max((int)mi.rcWork.top, min(top, (int)mi.rcWork.bottom - height));
  }
  SetWindowPos(g.dlg, NULL, x, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(g.panel, NULL, FALSE);
  InvalidateRect(g.close, NULL, FALSE);
}

static void place_where_it_is(void) {
  RECT rc;
  GetWindowRect(g.dlg, &rc);
  place((rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2);
}

/* What the panel shows changed: which nodes there are, for UI Automation, then their values. */
static void changed(void) {
  unsigned what[] = {(unsigned)g.nsections, (unsigned)g.nrows};
  uia_changed(&g.ui, uia_hash(0, what, sizeof what));
}

/* The thread has ended, or ended a while ago (its message comes after opening took the numbers
 * itself): shows what it read. A read that fails with numbers on show leaves them there. */
static void read_finished(void) {
  if (!g.thread) return;
  WaitForSingleObject(g.thread, INFINITE);
  CloseHandle(g.thread);
  g.thread = NULL;
  if (g.status == MC_OK || !g.nsections) build();
  if (g.again) {
    g.again = FALSE;
    read_start();
  }
}

/* --- Drawing --- */

static void draw_card(canvas *c, const section *s) {
  int radius = scale(CONTENT_RADIUS);
  /* Over Mica, Windows 11's card fill and stroke, as white or black at a low opacity. */
  COLORREF stroke = !g.mica ? theme.line : theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
  int stroke_alpha = !g.mica ? 255 : theme.dark ? 24 : 15;
  if (g.mica) canvas_fill(c, &s->card, radius, RGB(255, 255, 255), theme.dark ? 13 : 179);
  else canvas_fill(c, &s->card, radius, theme.bg, 255);
  canvas_frame(c, &s->card, radius, 1, stroke, stroke_alpha);
  canvas_text(c, s->heading, -1, &s->head, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g.heading_font,
              theme.text);
  for (int i = 0; i < s->n; i++) {
    const stat_row *r = &g.rows[s->first + i];
    if (i) {
      RECT line = {r->rc.left + scale(PAD), r->rc.top, r->rc.right - scale(PAD), r->rc.top + 1};
      canvas_fill(c, &line, 0, stroke, stroke_alpha);
    }
    RECT text = {r->rc.left + scale(PAD), r->rc.top, r->rc.right - scale(PAD), r->rc.bottom};
    UINT format = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    canvas_text(c, r->value, -1, &text, DT_RIGHT | format, g.font, theme.text);
    canvas_text(c, r->label, -1, &text, DT_LEFT | DT_END_ELLIPSIS | format, g.font, theme.text2);
  }
}

/* An icon and title over the text for the empty library, else just the text, in the middle of
 * the space above the button. */
static void draw_message(canvas *c, const RECT *client) {
  RECT area = {scale(MARGIN), scale(MARGIN), client->right - scale(MARGIN), scale(MARGIN + MESSAGE_H)};
  if (!g.message_title) {
    canvas_text(c, g.message, -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, g.font,
                theme.text2);
    return;
  }
  RECT icon = {area.left, area.top + scale(16), area.right, area.top + scale(16 + MESSAGE_ICON_H)};
  canvas_text(c, L"\xE8D6", 1, &icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g.icon_font, theme.text2); /* MusicNote */
  RECT title = {area.left, icon.bottom + scale(8), area.right, icon.bottom + scale(8 + 28)};
  canvas_text(c, g.message_title, -1, &title, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, g.title_font, theme.text);
  RECT text = {area.left, title.bottom + scale(4), area.right, area.bottom};
  canvas_text(c, g.message, -1, &text, DT_CENTER | DT_TOP | DT_WORDBREAK | DT_NOPREFIX, g.font, theme.text2);
}

static void paint(HWND wnd) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(wnd, &ps);
  RECT client;
  GetClientRect(wnd, &client);
  canvas *c = &g.surface;
  if (canvas_reserve(c, client.right, client.bottom)) {
    canvas_clear(c, &client, theme.frame, g.mica ? 0 : 255);
    if (g.nsections)
      for (int i = 0; i < g.nsections; i++) draw_card(c, &g.sections[i]);
    else
      draw_message(c, &client);
    canvas_blit(c, dc, ps.rcPaint.left, ps.rcPaint.top, &ps.rcPaint);
  }
  EndPaint(wnd, &ps);
}

static void draw_close(const DRAWITEMSTRUCT *dis) {
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas c = {0};
  if (!canvas_reserve(&c, rc.right, rc.bottom)) return;
  canvas_clear(&c, &rc, theme.frame, g.mica ? 0 : 255);
  wchar_t title[64];
  GetWindowTextW(dis->hwndItem, title, 64);
  int radius = scale(BUTTON_RADIUS);
  control_state state = dis->itemState & ODS_SELECTED ? STATE_PRESSED : g.hot ? STATE_HOT : STATE_NORMAL;
  theme_draw_button(&c, &rc, radius, title, g.font, dis->itemState & ODS_NOACCEL ? DT_HIDEPREFIX : 0, TRUE, FALSE, state);
  if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
    RECT focus = rc;
    InflateRect(&focus, -1, -1);
    canvas_frame(&c, &focus, radius, 2, theme.text, 255);
  }
  canvas_blit(&c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
  canvas_free(&c);
}

/* --- UI Automation: a group per card with a text per number, or the message as one text --- */

static BOOL ui_node(int i, uia_node *n) {
  if (i == UIA_ROOT) {
    lstrcpynW(n->name, mc_wstr(MC_STR_STATS_TITLE), 512);
    n->count = g.nsections ? g.nsections : 1;
    return TRUE;
  }
  n->parent = UIA_ROOT;
  if (!g.nsections) {
    if (i) return FALSE;
    n->type = UIA_TextControlTypeId;
    GetClientRect(g.panel, &n->rect);
    uia_name(n, g.message_title ? g.message_title : L"", g.message, NULL);
    return TRUE;
  }
  if (i < g.nsections) {
    const section *s = &g.sections[i];
    n->type = UIA_GroupControlTypeId;
    n->first = g.nsections + s->first;
    n->count = s->n;
    n->rect = s->card;
    lstrcpynW(n->name, s->heading, 512);
    return TRUE;
  }
  int r = i - g.nsections;
  if (r >= g.nrows) return FALSE;
  for (int k = 0; k < g.nsections; k++)
    if (r >= g.sections[k].first && r < g.sections[k].first + g.sections[k].n) n->parent = k;
  n->type = UIA_TextControlTypeId;
  n->rect = g.rows[r].rc;
  uia_name(n, g.rows[r].label, g.rows[r].value, NULL); /* a number and what it counts, read out as one */
  return TRUE;
}

static int ui_focus(void) {
  return UIA_ROOT;
}

static int ui_at(POINT pt) {
  for (int i = 0; i < g.nrows; i++)
    if (PtInRect(&g.rows[i].rc, pt)) return g.nsections + i;
  return UIA_ROOT;
}

static const uia_ops ui_ops = {.node = ui_node, .focus = ui_focus, .at = ui_at};

/* --- The windows --- */

static LRESULT CALLBACK panel_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!g.panel) { /* messages arrive before CreateWindowExW returns */
    g.panel = wnd;
    uia_init(&g.ui, wnd, &ui_ops);
  }
  LRESULT result;
  if (uia_message(&g.ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_ERASEBKGND: /* paint() fills it all, with the alpha a brush wouldn't give */
      return 1;
    case WM_PAINT:
      paint(wnd);
      return 0;
    case WM_DESTROY:
      uia_destroy(&g.ui);
      break;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static LRESULT CALLBACK close_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_MOUSEMOVE:
      if (!g.hot) {
        g.hot = TRUE;
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&t);
        InvalidateRect(wnd, NULL, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      g.hot = FALSE;
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, close_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

static void make_fonts(void) {
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g.dpi);
  HFONT old[] = {g.font, g.heading_font, g.title_font, g.icon_font};
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 7, 6); /* 9pt -> 10.5pt, as the sidebar's */
  g.font = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_SEMIBOLD;
  g.heading_font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(lf.lfHeight, 4, 3);
  g.title_font = CreateFontIndirectW(&lf);
  g.icon_font = CreateFontW(-scale(48), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
  SendMessageW(g.close, WM_SETFONT, (WPARAM)g.font, FALSE);
  for (size_t i = 0; i < sizeof old / sizeof old[0]; i++)
    if (old[i]) DeleteObject(old[i]);
}

/* The title bar, and Mica through all of the client area where there is any. */
static void apply_theme(void) {
  theme_apply_window(g.dlg, FALSE);
  g.mica = theme_apply_backdrop(g.dlg);
  MARGINS m = {0};
  if (g.mica) m = (MARGINS){-1, -1, -1, -1};
  DwmExtendFrameIntoClientArea(g.dlg, &m);
}

static void on_init(HWND dlg) {
  g.dlg = dlg;
  SetDialogDpiChangeBehavior(dlg, DDC_DISABLE_ALL, DDC_DISABLE_ALL); /* WM_DPICHANGED does it */
  g.dpi = GetDpiForWindow(dlg);
  g.close = GetDlgItem(dlg, IDCANCEL);
  SetWindowSubclass(g.close, close_proc, 0, 0);
  SendMessageW(dlg, DM_SETDEFID, IDCANCEL, 0);
  make_fonts();

  static BOOL registered;
  if (!registered) {
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = panel_proc, .hInstance = GetModuleHandleW(NULL),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_stats"};
    registered = RegisterClassExW(&wc) != 0;
  }
  /* All of the client area, under Close, so nothing else needs painting. */
  g.panel = CreateWindowExW(0, L"kithsong_stats", NULL, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, dlg, NULL,
                            GetModuleHandleW(NULL), NULL);
  SetWindowPos(g.panel, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  apply_theme();

  read_start();
  lstrcpynW(g.message, mc_wstr(MC_STR_WINDOWS_STATS_READING), 256);
  if (!g.thread) build();
  else if (WaitForSingleObject(g.thread, OPEN_WAIT_MS) == WAIT_OBJECT_0) read_finished();

  RECT owner;
  GetWindowRect(GetParent(dlg), &owner);
  place((owner.left + owner.right) / 2, (owner.top + owner.bottom) / 2);
  changed();
}

static INT_PTR CALLBACK dialog_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG:
      on_init(dlg);
      return TRUE; /* the focus on Close */
    case WM_APP_READ:
      if (g.thread) {
        read_finished();
        place_where_it_is();
        changed();
      }
      return TRUE;
    case WM_DPICHANGED: {
      const RECT *suggested = (const RECT *)lp;
      g.dpi = HIWORD(wp);
      make_fonts();
      place((suggested->left + suggested->right) / 2, (suggested->top + suggested->bottom) / 2);
      return TRUE;
    }
    case WM_ERASEBKGND: /* the panel fills it all */
      SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
      return TRUE;
    case WM_DRAWITEM:
      draw_close((const DRAWITEMSTRUCT *)lp);
      return TRUE;
    case WM_COMMAND:
      if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) EndDialog(dlg, IDCANCEL);
      return TRUE;
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED: /* the accent colour, which Close is filled with */
      if (theme_is_change(msg, wp, lp)) {
        theme_update();
        apply_theme();
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_DESTROY:
      if (g.thread) { /* it uses g.dir and g.stats */
        WaitForSingleObject(g.thread, INFINITE);
        CloseHandle(g.thread);
        g.thread = NULL;
      }
      break;
  }
  return FALSE;
}

void stats_dialog_show(HWND owner, mc_library *lib) {
  if (g.dlg) return;
  g.dir = _strdup(mc_library_dir(lib));
  dlg_template *t = malloc(sizeof *t);
  if (g.dir && t) {
    dlg_template_header(t, 1, 200, 100, mc_wstr(MC_STR_STATS_TITLE));
    dlg_template_item(t, 0x0080, NULL, mc_wstr_mnemonic(MC_STR_CLOSE), BS_OWNERDRAW | WS_TABSTOP, 0, 0, 50, 14, IDCANCEL);
    DialogBoxIndirectParamW(GetModuleHandleW(NULL), (const DLGTEMPLATE *)t->buf, owner, dialog_proc, 0);
  }
  free(t);
  free(g.dir);
  HFONT fonts[] = {g.font, g.heading_font, g.title_font, g.icon_font};
  for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
    if (fonts[i]) DeleteObject(fonts[i]);
  canvas_free(&g.surface);
  memset(&g, 0, sizeof g);
}

void stats_dialog_refresh(void) {
  if (g.dlg) read_start();
}
