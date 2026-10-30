#include "sidebar.h"

#include <windowsx.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomation.h>

#include "canvas.h"
#include "drag.h"
#include "scroller.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

typedef enum {
  ROW_NAV,
  ROW_HEADER,
  ROW_RECENT,    /* a recent: a button that opens it */
  ROW_DEVICE,    /* a paired device: a page, online or not */
  ROW_ACCORDION, /* "Unpaired devices (n)": shows or hides the nearby rows */
  ROW_NEARBY,    /* a nearby device: a button that asks to pair */
  ROW_NOTE,
  ROW_ABOUT,   /* a button that opens the About box */
  ROW_DIVIDER, /* a line between Home and Search and the library's pages */
} row_kind;

/* The rows are rebuilt from the fixed ones, the recents and the devices whenever any of them
 * change (rebuild), and laid out by layout(): the scrolling rows from the top of the
 * content, the pinned ones (Import, Settings, About) up from the bottom of the
 * window. Rows are referred to by their index in s.rows. */
typedef struct sb_row {
  row_kind kind;
  wchar_t *text;
  wchar_t *tip; /* recent rows: the full name and the detail */
  wchar_t icon; /* Segoe Fluent Icons */
  sidebar_page page; /* navigation and device rows */
  char uid[MC_UID_STRLEN]; /* recent, device and nearby rows */
  unsigned flags;    /* device and nearby rows: SB_DEV_*; recent rows: the mc_recent_kind */
  int indent;        /* at 96 DPI */
  BOOL unavailable;  /* a page that isn't there yet: greyed out, and can't be selected */
  BOOL add;          /* a heading with a + at its end */
  BOOL pinned;       /* at the bottom, out of the scrolling */
  int top, height;   /* pixels: in the content if it scrolls, else in the client area */
  BOOL smart;        /* recent rows: a smart playlist or group, which takes no drops */
} sb_row;

static const struct {
  enum mc_str_id text;
  wchar_t icon;
  sidebar_page page;
} nav_first[] = {
  {MC_STR_SECTION_HOME, 0xE80F, PAGE_HOME},
  {MC_STR_SEARCH, 0xE721, PAGE_SEARCH},
}, nav_top[] = {
  {MC_STR_SECTION_ALBUMS, 0xE93C, PAGE_ALBUMS},
  {MC_STR_SECTION_ARTISTS, 0xE77B, PAGE_ARTISTS},
  {MC_STR_SECTION_PLAYLISTS, 0xE90B, PAGE_PLAYLISTS},
  {MC_STR_SECTION_ALBUM_GROUPS, 0xE8F1, PAGE_ALBUM_GROUPS}, /* Library, as its recents */
}, nav_bottom[] = {
  {MC_STR_IMPORT_MUSIC, 0xE8B5, PAGE_IMPORT},
  {MC_STR_SETTINGS, 0xE713, PAGE_SETTINGS},
};

/* Metrics at 96 DPI. */
enum {
  NAV_H = 36,
  HEADER_H = 40, /* includes the gap above the section */
  NOTE_H = 32,
  DIVIDER_H = 7,
  BOTTOM = 6, /* below the last row, of the scrolling ones and of the pinned ones */
  INSET = 6,  /* between the sidebar's edges and the selection fill */
  TEXT_X = 18,
  ICON_W = 28, /* icon plus the gap after it */
  NEARBY_INDENT = 12,
  RADIUS = 6,
  ADD_W = 28, /* the + on the DEVICES heading: a square this wide, ending INSET from the edge */
  ADD_BOTTOM = 2,
  TIP_W = 400, /* tooltips wrap at this width */
};

/* Tooltip ids: the +'s, then one per row that has a tip (TIP_ROWS + k). */
enum { TIP_ADD = 1, TIP_ROWS };

/* A device as sidebar_set_devices gave it. */
typedef struct device {
  char uid[37];
  wchar_t *name;
  int kind;
  unsigned flags;
} device;

/* A recent as sidebar_set_recents gave it. */
typedef struct recent {
  char uid[MC_UID_STRLEN];
  mc_recent_kind kind;
  BOOL smart;
  wchar_t *name, *detail;
} recent;

static struct {
  HWND wnd;
  UINT dpi;
  HFONT font, header_font, icon_font, small_icon_font;
  canvas buf;
  scroller scroll; /* the rows above Import */
  int pinned_top;  /* where the pinned rows start, in the client area */
  int sel;     /* the selected page's row, or -1 */
  int focus;   /* the row with the keyboard focus (a page or a button), or -1 */
  int hot;     /* selectable or clickable row under the mouse, or -1 */
  int pressed; /* clickable row the mouse went down on, or -1 */
  int drop;    /* the recent a dragged track or album would be added to, or -1 */
  BOOL locked, tracking;
  HWND tooltip;
  BOOL add_hot, add_pressed; /* the mouse is over the +, or went down on it */

  sb_row *rows;
  int nrows, cap;
  recent *recents;
  int nrecents;
  device *paired, *nearby;
  int npaired, nnearby;
  BOOL expanded, expanded_set; /* the accordion; expanded_set once the user toggles it */
  int ntips; /* row tooltips, TIP_ROWS onwards */
  char clicked[37]; /* the nearby device of the last SBN_PAIR_DEVICE, or "" */
  char clicked_recent[MC_UID_STRLEN]; /* the recent of the last SBN_OPEN_RECENT, or "" */
  mc_recent_kind clicked_kind;
} s;

/* Outside s, which is cleared when the window goes: elements clients still hold use it. */
static uia_host ui;
static int ui_node_of(int row, BOOL add);

static void set_hot(int i);
static void update_tooltip(void);
static void free_devices(struct device *d, int n);
static void free_recents(void);

static int scale(int px) {
  return MulDiv(px, (int)s.dpi, USER_DEFAULT_SCREEN_DPI);
}

static wchar_t *wcs_dup(const wchar_t *a, const wchar_t *b) {
  size_t na = a ? wcslen(a) : 0, nb = b ? wcslen(b) : 0;
  wchar_t *r = malloc((na + nb + 1) * sizeof *r);
  if (!r) return NULL;
  if (na) memcpy(r, a, na * sizeof *r);
  if (nb) memcpy(r + na, b, nb * sizeof *r);
  r[na + nb] = 0;
  return r;
}

static const sb_row *row(int i) {
  static const sb_row none = {ROW_NOTE, NULL, NULL, 0, PAGE_ALBUMS, "", 0, 0, FALSE, FALSE, FALSE, 0, 0, FALSE};
  return i >= 0 && i < s.nrows ? &s.rows[i] : &none;
}

/* A page row, which can be selected unless the sidebar is locked. */
static BOOL selectable(const sb_row *r) {
  return r->kind == ROW_NAV || r->kind == ROW_DEVICE;
}

/* A page row that can be selected now. */
static BOOL enabled(int i) {
  return selectable(row(i)) && !row(i)->unavailable && (!s.locked || i == s.sel);
}

/* A row that acts when clicked, like a button: the accordion, a recent, a nearby device or
 * About. */
static BOOL clickable(int i) {
  row_kind kind = row(i)->kind;
  return kind == ROW_ACCORDION || kind == ROW_ABOUT || ((kind == ROW_NEARBY || kind == ROW_RECENT) && !s.locked);
}

/* The next row after from in direction dir (+1 or -1) that the keyboard can land on, or -1
 * if there is none. */
static int next_focus(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < s.nrows; i += dir)
    if (enabled(i) || clickable(i)) return i;
  return -1;
}

static int row_height(const sb_row *r) {
  static const int heights[] = {[ROW_NAV] = NAV_H,       [ROW_HEADER] = HEADER_H, [ROW_RECENT] = NAV_H,
                                [ROW_DEVICE] = NAV_H,    [ROW_ACCORDION] = NAV_H, [ROW_NEARBY] = NAV_H,
                                [ROW_NOTE] = NOTE_H,     [ROW_ABOUT] = NAV_H,     [ROW_DIVIDER] = DIVIDER_H};
  return scale(heights[r->kind]);
}

/* --- Layout --- */

/* Places the rows: the scrolling ones from the top of the content, the pinned ones so the
 * last ends BOTTOM above the bottom of the window. What's left above them is the scroller's
 * view. */
static void layout(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int content = 0, pinned = scale(BOTTOM);
  for (int i = 0; i < s.nrows; i++) {
    sb_row *r = &s.rows[i];
    r->height = row_height(r);
    if (r->pinned) {
      pinned += r->height;
    } else {
      r->top = content;
      content += r->height;
    }
  }
  s.pinned_top = max(0, rc.bottom - pinned);
  for (int i = 0, y = s.pinned_top; i < s.nrows; i++)
    if (s.rows[i].pinned) {
      s.rows[i].top = y;
      y += s.rows[i].height;
    }
  RECT view = {0, 0, rc.right, s.pinned_top};
  scroller_resize(&s.scroll, &view, content + scale(BOTTOM));
}

/* Row i in the client area. */
static RECT row_rect(int i) {
  const sb_row *r = row(i);
  int top = r->pinned ? r->top : r->top - s.scroll.pos;
  RECT rc = {0, top, s.scroll.view.right, top + r->height};
  return rc;
}

/* The part of row i that shows: a scrolling row is cut off at the edges of the view. */
static RECT visible_rect(int i) {
  RECT rc = row_rect(i), out;
  if (row(i)->pinned) return rc;
  if (!IntersectRect(&out, &rc, &s.scroll.view)) SetRectEmpty(&out);
  return out;
}

static void invalidate_row(int i) {
  if (i < 0 || i >= s.nrows) return;
  RECT rc = visible_rect(i);
  InvalidateRect(s.wnd, &rc, FALSE);
}

/* The row at a point in the client area, or -1. */
static int row_at(int x, int y) {
  POINT pt = {x, y};
  BOOL in_view = PtInRect(&s.scroll.view, pt);
  for (int i = 0; i < s.nrows; i++) {
    if (s.rows[i].pinned == in_view) continue;
    RECT rc = row_rect(i);
    if (PtInRect(&rc, pt)) return i;
  }
  return -1;
}

/* The enabled page row or clickable row at a point, or -1. */
static int target_at(int x, int y) {
  int i = row_at(x, y);
  return i >= 0 && (enabled(i) || clickable(i)) ? i : -1;
}

/* Scrolls as little as needed to show row i, and the heading right above it if it's the
 * first of its section. */
static void reveal(int i) {
  if (i < 0 || i >= s.nrows || s.rows[i].pinned) return;
  int top = s.rows[i].top;
  if (i > 0 && s.rows[i - 1].kind == ROW_HEADER) top = s.rows[i - 1].top;
  scroller_reveal(&s.scroll, top, s.rows[i].top + s.rows[i].height);
}

static void set_hot(int i) {
  if (i == s.hot) return;
  invalidate_row(s.hot);
  invalidate_row(i);
  s.hot = i;
}

static void set_focus(int i) {
  if (i == s.focus) return;
  invalidate_row(s.focus);
  invalidate_row(i);
  s.focus = i;
  uia_focus(&ui);
}

static void select_item(int i) {
  if (i < 0 || i == s.sel) return;
  invalidate_row(s.sel);
  invalidate_row(i);
  s.sel = i;
  uia_selected(&ui, ui_node_of(i, FALSE));
  SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), LBN_SELCHANGE), (LPARAM)s.wnd);
}

/* Selects page row i without notifying the parent. The focus follows, unless it's on a
 * button (as after clicking a recent, which then selects the page it opens on). */
static void select_quietly(int i) {
  if (i < 0 || i == s.sel) return;
  invalidate_row(s.sel);
  invalidate_row(i);
  s.sel = i;
  uia_selected(&ui, ui_node_of(i, FALSE));
  if (!clickable(s.focus)) set_focus(i);
}

/* --- The + on the DEVICES heading --- */

/* Where the + is, in client coordinates; empty if the heading isn't there or is scrolled
 * out of view. */
static RECT add_rect(void) {
  RECT rc;
  for (int i = 0; i < s.nrows; i++) {
    if (!s.rows[i].add) continue;
    RECT row_rc = row_rect(i), view;
    rc.right = row_rc.right - scale(INSET);
    rc.left = rc.right - scale(ADD_W);
    rc.bottom = row_rc.bottom - scale(ADD_BOTTOM);
    rc.top = rc.bottom - scale(ADD_W);
    if (!IntersectRect(&view, &rc, &s.scroll.view) || !EqualRect(&view, &rc)) break;
    return rc;
  }
  SetRectEmpty(&rc);
  return rc;
}

static BOOL in_add(int x, int y) {
  RECT rc = add_rect();
  POINT pt = {x, y};
  return !s.locked && PtInRect(&rc, pt);
}

static void set_add_hot(BOOL hot) {
  if (hot == s.add_hot) return;
  s.add_hot = hot;
  RECT rc = add_rect();
  InvalidateRect(s.wnd, &rc, FALSE);
}

/* What a row's tooltip says, or NULL. An archive's says so, with its state ("Online.
 * Keeps the whole library"), in a buffer valid until the next call: the tooltip copies it. */
static const wchar_t *row_tip(const sb_row *r) {
  static wchar_t archive[128];
  switch (r->kind) {
    case ROW_RECENT: return r->tip;
    case ROW_DEVICE:
      if (r->flags & SB_DEV_ARCHIVE) {
        mc_wformat(archive, 128, mc_str(MC_STR_DEVICE_ARCHIVE),
                   MC_S(mc_str(r->flags & SB_DEV_ONLINE ? MC_STR_DEVICE_ONLINE : MC_STR_DEVICE_OFFLINE)));
        return archive;
      }
      return r->flags & SB_DEV_ONLINE ? NULL : mc_wstr(MC_STR_DEVICE_OFFLINE);
    case ROW_NEARBY: return r->flags & SB_DEV_OTHER_GROUP ? mc_wstr(MC_STR_DEVICE_OTHER_GROUP_TIP) : NULL;
    default: return NULL;
  }
}

/* The tooltips follow the + and the rows when they move. */
static void update_tooltip(void) {
  if (!s.tooltip) return;
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = s.wnd, .uId = TIP_ADD, .rect = add_rect()};
  SendMessageW(s.tooltip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
  for (int k = 0; k < s.ntips; k++) {
    TTTOOLINFOW del = {.cbSize = sizeof del, .hwnd = s.wnd, .uId = (UINT_PTR)(TIP_ROWS + k)};
    SendMessageW(s.tooltip, TTM_DELTOOLW, 0, (LPARAM)&del);
  }
  s.ntips = 0;
  for (int i = 0; i < s.nrows; i++) {
    const wchar_t *tip = row_tip(&s.rows[i]);
    if (!tip || !*tip) continue;
    TTTOOLINFOW add = {.cbSize = sizeof add, .uFlags = TTF_SUBCLASS, .hwnd = s.wnd,
                       .uId = (UINT_PTR)(TIP_ROWS + s.ntips), .lpszText = (wchar_t *)tip,
                       .rect = visible_rect(i)};
    if (IsRectEmpty(&add.rect)) continue;
    if (SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&add)) s.ntips++;
  }
}

/* Re-checks what's under the mouse, after the rows moved under it. */
static void update_hot(void) {
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(s.wnd, &pt);
  BOOL over = s.tracking && !s.scroll.hot && !s.scroll.dragging;
  if (s.pressed < 0) set_hot(over ? target_at(pt.x, pt.y) : -1);
  if (!s.add_pressed) set_add_hot(over && in_add(pt.x, pt.y));
}

/* The scroller already repainted; the rows moved. */
static void on_scrolled(void) {
  update_hot();
  update_tooltip();
}

/* --- Rows --- */

static sb_row *push_row(row_kind kind, const wchar_t *text, const wchar_t *suffix) {
  if (s.nrows == s.cap) {
    int cap = s.cap ? s.cap * 2 : 16;
    sb_row *rows = realloc(s.rows, (size_t)cap * sizeof *rows);
    if (!rows) return NULL;
    s.rows = rows;
    s.cap = cap;
  }
  sb_row *r = &s.rows[s.nrows++];
  memset(r, 0, sizeof *r);
  r->kind = kind;
  r->text = wcs_dup(text, suffix);
  return r;
}

static void push_nav(const wchar_t *text, wchar_t icon, sidebar_page page, BOOL pinned) {
  sb_row *r = push_row(ROW_NAV, text, NULL);
  if (!r) return;
  r->icon = icon;
  r->page = page;
  r->pinned = pinned;
  r->unavailable = page == PAGE_HOME; /* there's no home page yet */
}

/* A section heading, in capitals. */
static sb_row *push_header(enum mc_str_id text) {
  wchar_t heading[64];
  lstrcpynW(heading, mc_wstr(text), 64);
  CharUpperBuffW(heading, (DWORD)wcslen(heading));
  return push_row(ROW_HEADER, heading, NULL);
}

static void push_recent(const recent *rc) {
  static const wchar_t icons[] = {[MC_RECENT_ALBUM] = 0xE93C,        /* as Albums */
                                  [MC_RECENT_ARTIST] = 0xE77B,       /* as Artists */
                                  [MC_RECENT_PLAYLIST] = 0xE90B,     /* as Playlists */
                                  [MC_RECENT_ALBUM_GROUP] = 0xE8F1}; /* Library */
  const wchar_t *name = rc->name && *rc->name ? rc->name : mc_wstr(MC_STR_UNTITLED);
  sb_row *r = push_row(ROW_RECENT, name, NULL);
  if (!r) return;
  r->icon = (unsigned)rc->kind < sizeof icons / sizeof icons[0] ? icons[rc->kind] : icons[0];
  r->flags = (unsigned)rc->kind;
  r->smart = rc->smart;
  memcpy(r->uid, rc->uid, sizeof r->uid);
  r->uid[sizeof r->uid - 1] = 0;
  /* The name may be cut off, so the tooltip has it whole. */
  if (rc->detail && *rc->detail) {
    wchar_t *line = wcs_dup(name, L"\n");
    r->tip = line ? wcs_dup(line, rc->detail) : NULL;
    free(line);
  } else {
    r->tip = wcs_dup(name, NULL);
  }
}

static void push_device(row_kind kind, const device *d) {
  static const wchar_t icons[] = {[MC_DEVICE_DESKTOP] = 0xE977, /* PC */
                                  [MC_DEVICE_PHONE] = 0xE8EA,   /* CellPhone */
                                  [MC_DEVICE_WATCH] = 0xE823};  /* Recent: a clock face */
  const wchar_t *name = d->name && *d->name ? d->name : mc_wstr(MC_STR_DEVICE_UNNAMED);
  BOOL self = kind == ROW_DEVICE && (d->flags & SB_DEV_SELF);
  sb_row *r = push_row(kind, name, self ? mc_wstr(MC_STR_WINDOWS_THIS_PC_SUFFIX) : NULL);
  if (!r) return;
  r->icon = d->kind >= 0 && d->kind < (int)(sizeof icons / sizeof icons[0]) ? icons[d->kind] : icons[0];
  r->page = PAGE_DEVICE;
  memcpy(r->uid, d->uid, sizeof r->uid);
  r->uid[sizeof r->uid - 1] = 0;
  r->flags = d->flags;
  r->indent = kind == ROW_NEARBY ? NEARBY_INDENT : 0;
}

static BOOL accordion_expanded(void) {
  return s.expanded_set ? s.expanded : s.npaired == 0;
}

static void free_rows(void) {
  for (int i = 0; i < s.nrows; i++) {
    free(s.rows[i].text);
    free(s.rows[i].tip);
  }
  s.nrows = 0;
}

/* Whether rows a and b stand for the same thing, across a rebuild. */
static BOOL same_row(const sb_row *a, const sb_row *b) {
  return a->kind == b->kind && a->page == b->page && !strcmp(a->uid, b->uid);
}

/* Remakes the rows, keeping the selected page (and device) and the focused row. If the
 * selected device is gone, selects Albums. */
static void rebuild(void) {
  sb_row sel = *row(s.sel), focus = *row(s.focus);
  BOOL had_sel = s.sel >= 0, had_focus = s.focus >= 0;

  /* Row indexes shift, so nothing can stay hot or pressed. */
  if (s.pressed >= 0 && GetCapture() == s.wnd) ReleaseCapture();
  s.pressed = -1;
  s.hot = -1;
  s.drop = -1;

  free_rows();
  for (size_t i = 0; i < sizeof nav_first / sizeof nav_first[0]; i++)
    push_nav(mc_wstr(nav_first[i].text), nav_first[i].icon, nav_first[i].page, FALSE);
  push_row(ROW_DIVIDER, NULL, NULL);
  for (size_t i = 0; i < sizeof nav_top / sizeof nav_top[0]; i++)
    push_nav(mc_wstr(nav_top[i].text), nav_top[i].icon, nav_top[i].page, FALSE);
  if (s.nrecents > 0) {
    push_header(MC_STR_SECTION_RECENTS);
    for (int i = 0; i < s.nrecents; i++) push_recent(&s.recents[i]);
  }
  sb_row *devices = push_header(MC_STR_DEVICES);
  if (devices) devices->add = TRUE;
  for (int i = 0; i < s.npaired; i++) push_device(ROW_DEVICE, &s.paired[i]);
  if (s.nnearby > 0) {
    wchar_t text[64];
    mc_wformat(text, 64, mc_str(MC_STR_DEVICES_UNPAIRED), MC_D(s.nnearby));
    sb_row *r = push_row(ROW_ACCORDION, text, NULL);
    if (r) r->icon = accordion_expanded() ? 0xE70D : 0xE76C; /* ChevronDown, ChevronRight */
    if (accordion_expanded())
      for (int i = 0; i < s.nnearby; i++) push_device(ROW_NEARBY, &s.nearby[i]);
  }
  if (s.npaired == 0 && s.nnearby == 0) push_row(ROW_NOTE, mc_wstr(MC_STR_DEVICES_NONE), NULL);
  for (size_t i = 0; i < sizeof nav_bottom / sizeof nav_bottom[0]; i++)
    push_nav(mc_wstr(nav_bottom[i].text), nav_bottom[i].icon, nav_bottom[i].page, TRUE);
  sb_row *about = push_row(ROW_ABOUT, mc_wstr(MC_STR_WINDOWS_ABOUT), NULL);
  if (about) {
    about->icon = 0xE946; /* Info */
    about->pinned = TRUE;
  }

  s.sel = s.focus = -1;
  int albums = 0;
  for (int i = 0; i < s.nrows; i++) {
    const sb_row *r = &s.rows[i];
    if (r->kind == ROW_NAV && r->page == PAGE_ALBUMS) albums = i;
    if (had_sel && selectable(r) && same_row(r, &sel)) s.sel = i;
    if (had_focus && same_row(r, &focus)) s.focus = i;
  }
  if (s.sel < 0) s.sel = albums;
  if (s.focus < 0) s.focus = s.sel;

  layout();
  InvalidateRect(s.wnd, NULL, FALSE);
  update_tooltip();

  /* What the rows are, for UI Automation: clients only start over when they changed. */
  unsigned long sig = 0;
  for (int i = 0; i < s.nrows; i++) {
    const sb_row *r = &s.rows[i];
    unsigned what[] = {r->kind, r->page, r->add};
    sig = uia_hash(sig, what, sizeof what);
    sig = uia_hash(sig, r->uid, strlen(r->uid));
  }
  uia_changed(&ui, sig);
}

/* A click (or Enter) on a clickable row. */
static void activate(int i) {
  const sb_row *r = row(i);
  if (r->kind == ROW_ACCORDION) {
    s.expanded = !accordion_expanded();
    s.expanded_set = TRUE;
    rebuild();
  } else if (r->kind == ROW_NEARBY) {
    memcpy(s.clicked, r->uid, sizeof s.clicked);
    SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), SBN_PAIR_DEVICE), (LPARAM)s.wnd);
  } else if (r->kind == ROW_RECENT) {
    memcpy(s.clicked_recent, r->uid, sizeof s.clicked_recent);
    s.clicked_kind = (mc_recent_kind)r->flags;
    SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), SBN_OPEN_RECENT), (LPARAM)s.wnd);
  } else if (r->kind == ROW_ABOUT) {
    SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), SBN_ABOUT), (LPARAM)s.wnd);
  }
}

/* --- Painting --- */

/* A button's look: pressed while the mouse is down on it and over it, else hot. */
static control_state button_state(int i) {
  return s.pressed == i && s.hot == i ? STATE_PRESSED : s.hot == i || s.pressed == i ? STATE_HOT : STATE_NORMAL;
}

/* Draws row i into c at rc. focused: the window has the focus and shows focus rectangles. */
static void draw_row(canvas *c, int i, const RECT *rc, BOOL focused) {
  const sb_row *r = &s.rows[i];
  int w = rc->right - rc->left, h = rc->bottom - rc->top, x = rc->left, y = rc->top;

  HFONT font = s.font;
  COLORREF color = theme.text;
  RECT text = {x + scale(TEXT_X + r->indent), y, x + w - scale(INSET), y + h};
  RECT fill = {x + scale(INSET + r->indent), y + scale(2), x + w - scale(INSET), y + h - scale(2)};
  UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
  BOOL focus_ring = FALSE;
  switch (r->kind) {
    case ROW_NAV:
    case ROW_DEVICE: {
      BOOL selected = i == s.sel;
      color = theme_draw_state(c, &fill, scale(RADIUS),
                               selected ? STATE_SELECTED : i == s.hot ? STATE_HOT : STATE_NORMAL);
      if (!enabled(i)) color = theme.text3; /* locked, or not there yet */
      if (selected && !theme.high_contrast) { /* accent marker at the left edge */
        int mark_h = scale(16);
        RECT mark = {fill.left, y + (h - mark_h) / 2, fill.left + scale(3), y + (h + mark_h) / 2};
        canvas_fill(c, &mark, scale(2), theme.accent, 255);
      }
      focus_ring = TRUE;
      RECT icon = {text.left, y, text.left + scale(ICON_W), y + h};
      canvas_text(c, &r->icon, 1, &icon, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.icon_font, color);
      text.left += scale(ICON_W);
      /* A small glyph at the end: Sync while syncing, else NetworkOffline while offline. */
      wchar_t badge = r->flags & SB_DEV_SYNCING                               ? 0xE895
                      : r->kind == ROW_DEVICE && !(r->flags & SB_DEV_ONLINE) ? 0xF384
                                                                              : 0;
      if (badge) {
        RECT at = {text.right - scale(ICON_W - 8), y, text.right - scale(4), y + h};
        canvas_text(c, &badge, 1, &at, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.small_icon_font,
                    color == theme.text ? theme.text2 : color);
        text.right = at.left;
      }
      break;
    }
    case ROW_RECENT:
    case ROW_NEARBY:
    case ROW_ABOUT: { /* buttons: hot and pressed, never selected */
      color = theme_draw_state(c, &fill, scale(RADIUS), i == s.drop ? STATE_HOT : button_state(i));
      if (i == s.drop) canvas_frame(c, &fill, scale(RADIUS), scale(2), theme.accent, 255); /* where a drop lands */
      if (!clickable(i)) color = theme.text3;
      focus_ring = TRUE;
      RECT icon = {text.left, y, text.left + scale(ICON_W), y + h};
      canvas_text(c, &r->icon, 1, &icon, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.icon_font, color);
      text.left += scale(ICON_W);
      break;
    }
    case ROW_ACCORDION: { /* the heading of the nearby rows, with a chevron at its end */
      control_state state = button_state(i);
      theme_draw_state(c, &fill, scale(RADIUS), state);
      color = state == STATE_NORMAL ? theme.text2 : theme.text;
      focus_ring = TRUE;
      RECT chevron = {fill.right - scale(ADD_W), y, fill.right, y + h};
      canvas_text(c, &r->icon, 1, &chevron, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                  s.small_icon_font, color);
      text.right = chevron.left;
      break;
    }
    case ROW_HEADER: { /* the heading, at the bottom of the row: level with the + if it has one */
      RECT add = {x + w - scale(INSET + ADD_W), y + h - scale(ADD_BOTTOM + ADD_W), x + w - scale(INSET),
                  y + h - scale(ADD_BOTTOM)};
      if (r->add) {
        control_state state = s.locked                     ? STATE_NORMAL
                              : s.add_pressed && s.add_hot ? STATE_PRESSED
                              : s.add_hot || s.add_pressed ? STATE_HOT
                                                           : STATE_NORMAL;
        COLORREF add_color = theme_draw_state(c, &add, scale(4), state);
        if (s.locked) add_color = theme.text3;
        canvas_text(c, L"\xE710", 1, &add, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, s.icon_font,
                    add_color); /* Add */
        text.right = add.left - scale(4);
      }
      font = s.header_font;
      color = theme.text2;
      text.top = add.top;
      text.bottom = add.bottom;
      break;
    }
    case ROW_NOTE:
      color = theme.text3;
      break;
    case ROW_DIVIDER: {
      RECT line = {fill.left + scale(6), y + h / 2, fill.right - scale(6), y + h / 2 + 1};
      canvas_clear(c, &line, theme.line, 255);
      return;
    }
  }
  if (focus_ring && focused && i == s.focus) canvas_frame(c, &fill, scale(RADIUS), scale(2), theme.text, 255);
  canvas_text(c, r->text ? r->text : L"", -1, &text, format, font, color);
}

/* Everything goes into a canvas, which gets the alpha right over Mica: the scrolling rows,
 * then the pinned ones over whatever scrolled under them. */
static void paint(HDC dc, const RECT *dirty) {
  RECT client;
  GetClientRect(s.wnd, &client);
  if (!canvas_reserve(&s.buf, client.right, client.bottom)) return;
  canvas *c = &s.buf;
  int alpha = theme.mica ? 0 : 255;
  canvas_clear(c, &client, theme.frame, alpha);

  BOOL focused = GetFocus() == s.wnd && !(SendMessageW(s.wnd, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
  RECT isect;
  for (int i = 0; i < s.nrows; i++) {
    RECT rc = row_rect(i);
    if (!s.rows[i].pinned && IntersectRect(&isect, &rc, &s.scroll.view)) draw_row(c, i, &rc, focused);
  }
  RECT pinned = {0, s.pinned_top, client.right, client.bottom};
  canvas_clear(c, &pinned, theme.frame, alpha);
  if (scroller_max(&s.scroll) > 0) { /* a line between what scrolls and what doesn't */
    RECT line = {scale(INSET), s.pinned_top, client.right - scale(INSET), s.pinned_top + 1};
    canvas_clear(c, &line, theme.line, 255);
  }
  for (int i = 0; i < s.nrows; i++) {
    RECT rc = row_rect(i);
    if (s.rows[i].pinned) draw_row(c, i, &rc, focused);
  }
  scroller_paint(&s.scroll, c);
  canvas_blit(c, dc, dirty->left, dirty->top, dirty);
}

/* --- UI Automation (uia.h) ---
 *
 * A list of the rows, as they show from the top: pages, buttons (the recents, the
 * accordion, the nearby devices, About) and text (headings, notes), with the + after
 * the DEVICES heading. The dividers are left out. */

/* Node i's row, and in *add whether it's the + on that row; -1 if there's no node i. */
static int ui_row_of(int i, BOOL *add) {
  int n = 0;
  for (int r = 0; r < s.nrows; r++) {
    if (s.rows[r].kind == ROW_DIVIDER) continue;
    if (n++ == i) {
      *add = FALSE;
      return r;
    }
    if (s.rows[r].add && n++ == i) {
      *add = TRUE;
      return r;
    }
  }
  return -1;
}

static int ui_node_of(int row, BOOL add) {
  int n = 0;
  for (int r = 0; r < s.nrows; r++) {
    if (s.rows[r].kind == ROW_DIVIDER) continue;
    if (r == row && !add) return n;
    n++;
    if (s.rows[r].add && r == row) return n;
    if (s.rows[r].add) n++;
  }
  return UIA_NONE;
}

static BOOL ui_node(int i, uia_node *n) {
  if (i == UIA_ROOT) {
    n->type = UIA_ListControlTypeId;
    n->flags = UIA_SELECTION;
    lstrcpynW(n->name, mc_wstr(MC_STR_WINDOWS_SIDEBAR_NAME), 512);
    for (int r = 0; r < s.nrows; r++) n->count += (s.rows[r].kind != ROW_DIVIDER) + s.rows[r].add;
    return TRUE;
  }
  BOOL add;
  int r = ui_row_of(i, &add);
  if (r < 0) return FALSE;
  const sb_row *row = &s.rows[r];
  n->parent = UIA_ROOT;
  n->rect = visible_rect(r);
  if (add) {
    n->type = UIA_ButtonControlTypeId;
    n->flags = UIA_INVOKABLE | (s.locked ? UIA_DISABLED : 0);
    n->rect = add_rect();
    n->automation_id = L"AddDevice";
    lstrcpynW(n->name, mc_wstr(MC_STR_ADD_DEVICE_BY_ADDRESS), 512);
    return TRUE;
  }
  lstrcpynW(n->name, row->text ? row->text : L"", 512);
  const wchar_t *tip = row_tip(row);
  if (tip) lstrcpynW(n->help, tip, 256);
  switch (row->kind) {
    case ROW_NAV:
    case ROW_DEVICE:
      n->type = UIA_ListItemControlTypeId;
      n->flags = UIA_FOCUSABLE | UIA_SELECTABLE | (r == s.sel ? UIA_SELECTED : 0) | (enabled(r) ? 0 : UIA_DISABLED);
      if (row->flags & SB_DEV_SYNCING) lstrcpynW(n->status, mc_wstr(MC_STR_SYNCING_PROGRESS), 64);
      else if (row->kind == ROW_DEVICE && !(row->flags & SB_DEV_ONLINE))
        lstrcpynW(n->status, mc_wstr(MC_STR_DEVICE_OFFLINE), 64);
      break;
    case ROW_RECENT:
    case ROW_NEARBY:
    case ROW_ABOUT:
      n->type = UIA_ListItemControlTypeId;
      n->flags = UIA_FOCUSABLE | UIA_INVOKABLE | (clickable(r) ? 0 : UIA_DISABLED);
      if (row->kind == ROW_ABOUT) n->automation_id = L"About";
      break;
    case ROW_ACCORDION:
      n->type = UIA_ButtonControlTypeId;
      n->flags = UIA_FOCUSABLE | UIA_INVOKABLE | (accordion_expanded() ? UIA_EXPANDED : UIA_COLLAPSED);
      break;
    default: /* headings and notes */
      n->type = UIA_TextControlTypeId;
      break;
  }
  return TRUE;
}

static int ui_focus(void) {
  return s.focus >= 0 && s.focus < s.nrows ? ui_node_of(s.focus, FALSE) : UIA_ROOT;
}

static int ui_at(POINT pt) {
  if (in_add(pt.x, pt.y)) {
    for (int r = 0; r < s.nrows; r++)
      if (s.rows[r].add) return ui_node_of(r, TRUE);
  }
  int r = row_at(pt.x, pt.y);
  return r >= 0 && s.rows[r].kind != ROW_DIVIDER ? ui_node_of(r, FALSE) : UIA_ROOT;
}

static int ui_selection(int i) {
  return i == UIA_ROOT && s.sel >= 0 ? ui_node_of(s.sel, FALSE) : UIA_NONE;
}

/* As the keyboard does: the focus moves to the row, and a page is selected. */
static void ui_set_focus(int i) {
  BOOL add;
  int r = ui_row_of(i, &add);
  if (r < 0 || add || !(enabled(r) || clickable(r))) return;
  set_focus(r);
  if (enabled(r)) select_item(r);
  reveal(r);
}

static void ui_invoke(int i) {
  BOOL add;
  int r = ui_row_of(i, &add);
  if (r < 0) return;
  if (add && !s.locked) {
    SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), SBN_ADD_DEVICE), (LPARAM)s.wnd);
  } else if (!add && clickable(r)) {
    set_focus(r);
    activate(r);
  }
}

static void ui_expand(int i, BOOL expand) {
  BOOL add;
  int r = ui_row_of(i, &add);
  if (r >= 0 && !add && s.rows[r].kind == ROW_ACCORDION && expand != accordion_expanded()) activate(r);
}

static void ui_reveal(int i) {
  BOOL add;
  int r = ui_row_of(i, &add);
  if (r >= 0) reveal(r);
}

static const uia_ops ui_ops = {
  .node = ui_node,
  .focus = ui_focus,
  .at = ui_at,
  .selection = ui_selection,
  .select = ui_set_focus,
  .invoke = ui_invoke,
  .expand = ui_expand,
  .reveal = ui_reveal,
  .set_focus = ui_set_focus,
};

/* --- Window --- */

/* The arrow keys, Home, End, Page Up and Page Down move the focus through the pages and
 * buttons, selecting a page as they land on it; Enter and Space press a button. */
static void on_key(WPARAM key) {
  /* Using the keyboard shows focus rectangles, as in a dialog. */
  SendMessageW(GetAncestor(s.wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  int to;
  switch (key) {
    case VK_UP: to = next_focus(s.focus, -1); break;
    case VK_DOWN: to = next_focus(s.focus, +1); break;
    case VK_HOME:
    case VK_PRIOR: to = next_focus(-1, +1); break;
    case VK_END:
    case VK_NEXT: to = next_focus(s.nrows, -1); break;
    case VK_RETURN:
    case VK_SPACE:
      if (clickable(s.focus)) activate(s.focus);
      else if (key == VK_RETURN && s.focus == s.sel && s.sel >= 0)
        SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), SBN_ENTER_PAGE), (LPARAM)s.wnd);
      return;
    default: return;
  }
  if (to < 0) return;
  set_focus(to);
  if (enabled(to)) select_item(to);
  reveal(to);
}

/* --- Drops (drag.h) --- */

/* A track dragged onto a manual playlist's recent, or an album onto a manual group's, is
 * added to it; the row is marked while it's under the pointer. */
static BOOL drop_hover(drag_kind kind, const POINT *pt, char uid[MC_UID_STRLEN]) {
  int i = pt ? row_at(pt->x, pt->y) : -1;
  if (i >= 0) {
    const sb_row *r = &s.rows[i];
    mc_recent_kind want = kind == DRAG_TRACK ? MC_RECENT_PLAYLIST : MC_RECENT_ALBUM_GROUP;
    if (r->kind != ROW_RECENT || r->smart || r->flags != (unsigned)want || s.locked) i = -1;
  }
  if (i != s.drop) {
    invalidate_row(s.drop);
    invalidate_row(i);
    s.drop = i;
  }
  if (i >= 0) memcpy(uid, s.rows[i].uid, MC_UID_STRLEN);
  return i >= 0;
}

static LRESULT CALLBACK sidebar_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (!s.wnd) s.wnd = s.scroll.wnd = wnd; /* messages arrive before CreateWindowExW returns */
  LRESULT result;
  if (uia_message(&ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_SIZE:
      layout();
      update_tooltip();
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
      BOOL on_bar = scroller_mouse_move(&s.scroll, x, y);
      int i = on_bar ? -1 : target_at(x, y);
      set_hot(s.add_pressed ? -1 : s.pressed >= 0 ? (i == s.pressed ? i : -1) : i);
      set_add_hot(!on_bar && in_add(x, y));
      return 0;
    }
    case WM_MOUSELEAVE:
      s.tracking = FALSE;
      scroller_mouse_leave(&s.scroll);
      if (s.pressed < 0) set_hot(-1);
      if (!s.add_pressed) set_add_hot(FALSE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      SetFocus(wnd);
      if (scroller_mouse_down(&s.scroll, x, y)) return 0;
      if (in_add(x, y)) { /* the + acts on release, like a button */
        s.add_pressed = TRUE;
        SetCapture(wnd);
        RECT rc = add_rect();
        InvalidateRect(wnd, &rc, FALSE);
        return 0;
      }
      int i = target_at(x, y);
      if (i < 0) return 0;
      set_focus(i);
      if (clickable(i)) { /* so do the accordion, the recents and nearby devices */
        s.pressed = i;
        SetCapture(wnd);
        set_hot(i);
        invalidate_row(i);
        return 0;
      }
      select_item(i);
      return 0;
    }
    case WM_LBUTTONUP: {
      int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
      if (s.add_pressed) {
        BOOL clicked = in_add(x, y);
        ReleaseCapture(); /* WM_CAPTURECHANGED clears add_pressed */
        if (clicked)
          SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), SBN_ADD_DEVICE), (LPARAM)wnd);
      } else if (s.pressed >= 0) {
        int i = s.pressed;
        BOOL clicked = target_at(x, y) == i;
        ReleaseCapture(); /* WM_CAPTURECHANGED clears pressed */
        if (clicked) activate(i);
      } else {
        scroller_mouse_up(&s.scroll);
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (s.add_pressed) {
        s.add_pressed = FALSE;
        RECT rc = add_rect();
        InvalidateRect(wnd, &rc, FALSE);
      }
      if (s.pressed >= 0) {
        invalidate_row(s.pressed);
        s.pressed = -1;
      }
      scroller_capture_changed(&s.scroll);
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_CONTEXTMENU: { /* Playlists and Album Groups offer a new smart one, a recent its removal */
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int i;
      if (lp == -1) {
        i = s.focus;
        RECT rc = i >= 0 ? row_rect(i) : (RECT){0};
        pt = (POINT){rc.left, rc.bottom};
        ClientToScreen(wnd, &pt);
      } else {
        POINT client = pt;
        ScreenToClient(wnd, &client);
        i = target_at(client.x, client.y);
      }
      UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
      if (i >= 0 && i < s.nrows && s.rows[i].kind == ROW_RECENT && clickable(i)) {
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, 1, mc_wstr_mnemonic(MC_STR_RECENT_REMOVE_MENU));
        AppendMenuW(menu, MF_STRING, 2, mc_wstr_mnemonic(MC_STR_RECENTS_CLEAR_MENU));
        int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, wnd, NULL);
        DestroyMenu(menu);
        if (cmd == 1) {
          memcpy(s.clicked_recent, s.rows[i].uid, sizeof s.clicked_recent);
          s.clicked_kind = (mc_recent_kind)s.rows[i].flags;
        }
        if (cmd)
          SendMessageW(GetParent(wnd), WM_COMMAND,
                       MAKEWPARAM(GetDlgCtrlID(wnd), cmd == 1 ? SBN_REMOVE_RECENT : SBN_CLEAR_RECENTS), (LPARAM)wnd);
        return 0;
      }
      if (i < 0 || i >= s.nrows || s.rows[i].kind != ROW_NAV || !enabled(i) ||
          (s.rows[i].page != PAGE_PLAYLISTS && s.rows[i].page != PAGE_ALBUM_GROUPS))
        break;
      BOOL playlists = s.rows[i].page == PAGE_PLAYLISTS;
      HMENU menu = CreatePopupMenu();
      AppendMenuW(menu, MF_STRING, 1,
                  mc_wstr_mnemonic(playlists ? MC_STR_WINDOWS_SMART_NEW_PLAYLIST_MENU : MC_STR_WINDOWS_SMART_NEW_GROUP_MENU));
      int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, wnd, NULL);
      DestroyMenu(menu);
      if (cmd == 1)
        SendMessageW(GetParent(wnd), WM_COMMAND,
                     MAKEWPARAM(GetDlgCtrlID(wnd), playlists ? SBN_NEW_SMART_PLAYLIST : SBN_NEW_SMART_GROUP), (LPARAM)wnd);
      return 0;
    }
    case WM_GETDLGCODE:
      return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_UPDATEUISTATE:
      invalidate_row(s.focus);
      break;
    case WM_DESTROY:
      uia_destroy(&ui);
      if (s.font) DeleteObject(s.font);
      if (s.header_font) DeleteObject(s.header_font);
      if (s.icon_font) DeleteObject(s.icon_font);
      if (s.small_icon_font) DeleteObject(s.small_icon_font);
      if (s.tooltip) DestroyWindow(s.tooltip);
      canvas_free(&s.buf);
      free_rows();
      free(s.rows);
      free_recents();
      free_devices(s.paired, s.npaired);
      free_devices(s.nearby, s.nnearby);
      memset(&s, 0, sizeof s);
      s.sel = s.focus = s.hot = s.pressed = s.drop = -1;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

HWND sidebar_create(HWND parent, int id) {
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .style = CS_DBLCLKS, .lpfnWndProc = sidebar_proc,
                    .hInstance = GetModuleHandleW(NULL),
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
                    .lpszClassName = L"kithsong_sidebar"};
  RegisterClassExW(&wc);

  memset(&s, 0, sizeof s);
  s.sel = s.focus = s.hot = s.pressed = s.drop = -1;
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  s.scroll.moved = on_scrolled;
  if (!CreateWindowExW(0, L"kithsong_sidebar", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, parent,
                       (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL))
    return NULL;
  uia_init(&ui, s.wnd, &ui_ops);
  drag_add_target(s.wnd, drop_hover);

  s.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s.wnd, NULL,
                              GetModuleHandleW(NULL), NULL);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = s.wnd, .uId = TIP_ADD,
                    .lpszText = (wchar_t *)mc_wstr(MC_STR_ADD_DEVICE_BY_ADDRESS)};
  SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  SendMessageW(s.tooltip, TTM_SETMAXTIPWIDTH, 0, TIP_W); /* so long names break */
  rebuild(); /* selects Albums */
  return s.wnd;
}

static HFONT icon_font(int px) {
  return CreateFontW(-scale(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

void sidebar_update(UINT dpi) {
  s.dpi = dpi;

  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 7, 6); /* 9pt -> 10.5pt */
  if (s.font) DeleteObject(s.font);
  s.font = CreateFontIndirectW(&lf);
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 11, 12);
  lf.lfWeight = FW_SEMIBOLD;
  if (s.header_font) DeleteObject(s.header_font);
  s.header_font = CreateFontIndirectW(&lf);
  if (s.icon_font) DeleteObject(s.icon_font);
  s.icon_font = icon_font(16);
  if (s.small_icon_font) DeleteObject(s.small_icon_font);
  s.small_icon_font = icon_font(12);

  layout();
  update_tooltip();
  if (s.tooltip) SendMessageW(s.tooltip, TTM_SETMAXTIPWIDTH, 0, scale(TIP_W));
  theme_apply_control(s.tooltip);
  InvalidateRect(s.wnd, NULL, FALSE);
}

sidebar_page sidebar_selection(void) {
  return s.sel >= 0 ? row(s.sel)->page : PAGE_ALBUMS;
}

void sidebar_select(sidebar_page page) {
  for (int i = 0; i < s.nrows; i++)
    if (s.rows[i].kind == ROW_NAV && s.rows[i].page == page) select_quietly(i);
}

void sidebar_set_locked(BOOL locked) {
  if (locked == s.locked) return;
  s.locked = locked;
  set_hot(-1);
  set_add_hot(FALSE);
  InvalidateRect(s.wnd, NULL, FALSE);
}

/* --- Recents --- */

static void free_recents(void) {
  for (int i = 0; i < s.nrecents; i++) {
    free(s.recents[i].name);
    free(s.recents[i].detail);
  }
  free(s.recents);
  s.recents = NULL;
  s.nrecents = 0;
}

void sidebar_set_recents(const sidebar_recent *recents, int n) {
  free_recents();
  s.recents = n > 0 ? calloc((size_t)n, sizeof *s.recents) : NULL;
  s.nrecents = s.recents ? n : 0;
  for (int i = 0; i < s.nrecents; i++) {
    memcpy(s.recents[i].uid, recents[i].uid, sizeof s.recents[i].uid);
    s.recents[i].uid[sizeof s.recents[i].uid - 1] = 0;
    s.recents[i].kind = recents[i].kind;
    s.recents[i].smart = recents[i].smart;
    s.recents[i].name = wcs_dup(recents[i].name, NULL);
    s.recents[i].detail = recents[i].detail ? wcs_dup(recents[i].detail, NULL) : NULL;
  }
  s.clicked_recent[0] = 0;
  if (s.wnd) rebuild();
}

const char *sidebar_clicked_recent(mc_recent_kind *kind) {
  if (!s.clicked_recent[0]) return NULL;
  if (kind) *kind = s.clicked_kind;
  return s.clicked_recent;
}

/* --- Devices --- */

static void free_devices(device *d, int n) {
  for (int i = 0; i < n; i++) free(d[i].name);
  free(d);
}

static device *copy_devices(const sidebar_device *from, int n) {
  if (n <= 0) return NULL;
  device *d = calloc((size_t)n, sizeof *d);
  if (!d) return NULL;
  for (int i = 0; i < n; i++) {
    memcpy(d[i].uid, from[i].uid, sizeof d[i].uid);
    d[i].uid[sizeof d[i].uid - 1] = 0;
    d[i].name = wcs_dup(from[i].name, NULL);
    d[i].kind = from[i].kind;
    d[i].flags = from[i].flags;
  }
  return d;
}

/* Whether the devices have are the ones from gives, as they are. */
static BOOL same_devices(const device *have, int nhave, const sidebar_device *from, int nfrom) {
  if (nhave != nfrom) return FALSE;
  for (int i = 0; i < nfrom; i++) {
    const wchar_t *name = have[i].name ? have[i].name : L"", *given = from[i].name ? from[i].name : L"";
    if (strcmp(have[i].uid, from[i].uid) || wcscmp(name, given) || have[i].kind != from[i].kind ||
        have[i].flags != from[i].flags)
      return FALSE;
  }
  return TRUE;
}

void sidebar_set_devices(const sidebar_device *paired, int npaired, const sidebar_device *nearby, int nnearby) {
  /* Polled every few seconds: nothing to rebuild (or tell screen readers) when nothing changed. */
  if (same_devices(s.paired, s.npaired, paired, npaired) && same_devices(s.nearby, s.nnearby, nearby, nnearby))
    return;
  free_devices(s.paired, s.npaired);
  free_devices(s.nearby, s.nnearby);
  s.paired = copy_devices(paired, npaired);
  s.npaired = s.paired ? npaired : 0;
  s.nearby = copy_devices(nearby, nnearby);
  s.nnearby = s.nearby ? nnearby : 0;
  s.clicked[0] = 0;
  if (s.wnd) rebuild();
}

const char *sidebar_selected_device(void) {
  return s.sel >= 0 && row(s.sel)->kind == ROW_DEVICE ? row(s.sel)->uid : NULL;
}

void sidebar_select_device(const char *uid) {
  if (!uid) return;
  for (int i = 0; i < s.nrows; i++)
    if (s.rows[i].kind == ROW_DEVICE && strcmp(s.rows[i].uid, uid) == 0) {
      select_quietly(i);
      return;
    }
}

const char *sidebar_clicked_nearby(void) {
  return s.clicked[0] ? s.clicked : NULL;
}
