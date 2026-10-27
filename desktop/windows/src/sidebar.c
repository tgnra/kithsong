#include "sidebar.h"

#include <commctrl.h>
#include <stdlib.h>
#include <string.h>

#include "canvas.h"
#include "musiccore/sync.h"
#include "theme.h"
#include "wstr.h"

typedef enum {
  ROW_NAV,
  ROW_HEADER,
  ROW_DEVICE,    /* a paired device: a page, online or not */
  ROW_ACCORDION, /* "Unpaired devices (n)": shows or hides the nearby rows */
  ROW_NEARBY,    /* a nearby device: a button that asks to pair */
  ROW_NOTE,
  ROW_SPACER,
  ROW_FOOTER, /* two small lines, s.footer */
} row_kind;

/* The rows are rebuilt from the fixed ones, the devices and the footer whenever any of them
 * change (rebuild). The list box holds one item per row, except the spacer, which takes as
 * many items as it needs to fill the height (an item can't be taller than 255 pixels). Each
 * item's data is its index in s.rows. */
typedef struct sb_row {
  row_kind kind;
  wchar_t *text;
  wchar_t icon; /* Segoe Fluent Icons */
  sidebar_page page; /* navigation and device rows */
  char uid[37];      /* device and nearby rows */
  unsigned flags;    /* device and nearby rows: SB_DEV_* */
  int indent;        /* at 96 DPI */
} sb_row;

static const struct {
  enum mc_str_id text;
  wchar_t icon;
  sidebar_page page;
} nav_top[] = {
  {MC_STR_SECTION_ALBUMS, 0xE93C, PAGE_ALBUMS},
  {MC_STR_SECTION_ARTISTS, 0xE77B, PAGE_ARTISTS},
  {MC_STR_SECTION_PLAYLISTS, 0xE90B, PAGE_PLAYLISTS},
}, nav_bottom[] = {
  {MC_STR_IMPORT_MUSIC, 0xE8B5, PAGE_IMPORT},
  {MC_STR_SETTINGS, 0xE713, PAGE_SETTINGS},
};
#define MAX_ITEM_H 255

/* Metrics at 96 DPI. */
enum {
  NAV_H = 36,
  HEADER_H = 40, /* includes the gap above the section */
  NOTE_H = 32,
  FOOTER_H = 42, /* two lines of FOOTER_LINE, with room above */
  FOOTER_LINE = 15,
  BOTTOM = 6, /* below the last row */
  INSET = 6,  /* between the list's edges and the selection fill */
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

static struct {
  HWND wnd;
  UINT dpi;
  HFONT font, header_font, icon_font, small_icon_font, footer_font;
  HBRUSH bg; /* below the rows; black (transparent) over Mica */
  canvas buf;
  int hot;     /* selectable or clickable item under the mouse, or -1 */
  int pressed; /* clickable item the mouse went down on, or -1 */
  BOOL locked;
  HWND tooltip;
  BOOL add_hot, add_pressed; /* the mouse is over the +, or went down on it */

  sb_row *rows;
  int nrows, cap;
  device *paired, *nearby;
  int npaired, nnearby;
  BOOL expanded, expanded_set; /* the accordion; expanded_set once the user toggles it */
  wchar_t *footer[2], *footer_tip;
  int ntips; /* row tooltips, TIP_ROWS onwards */
  char clicked[37]; /* the nearby device of the last SBN_PAIR_DEVICE, or "" */
} s;

static void set_hot(int i);
static void free_devices(struct device *d, int n);

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

static int item_count(void) {
  return (int)SendMessageW(s.wnd, LB_GETCOUNT, 0, 0);
}

static int row_index(int item) {
  return (int)SendMessageW(s.wnd, LB_GETITEMDATA, (WPARAM)item, 0);
}

static const sb_row *row_of(int item) {
  static const sb_row none = {ROW_SPACER, NULL, 0, PAGE_ALBUMS, "", 0, 0};
  int i = row_index(item);
  return i >= 0 && i < s.nrows ? &s.rows[i] : &none;
}

static int selected_item(void) {
  return (int)SendMessageW(s.wnd, LB_GETCURSEL, 0, 0);
}

/* A page row, which can be selected unless the sidebar is locked. */
static BOOL selectable(const sb_row *r) {
  return r->kind == ROW_NAV || r->kind == ROW_DEVICE;
}

/* A page item that can be selected now. */
static BOOL enabled(int item) {
  return selectable(row_of(item)) && (!s.locked || item == selected_item());
}

/* An item that acts when clicked, like a button: the accordion, or a nearby device. */
static BOOL clickable(int item) {
  row_kind kind = row_of(item)->kind;
  return kind == ROW_ACCORDION || (kind == ROW_NEARBY && !s.locked);
}

/* The next enabled page item after from in direction dir (+1 or -1), or -1 if there is none. */
static int next_nav(int from, int dir) {
  for (int i = from + dir; i >= 0 && i < item_count(); i += dir)
    if (enabled(i)) return i;
  return -1;
}

/* The enabled page item or clickable item at a WM_MOUSE* lParam, or -1. */
static int item_at(LPARAM lp) {
  LRESULT r = SendMessageW(s.wnd, LB_ITEMFROMPOINT, 0, lp);
  int i = LOWORD(r);
  return !HIWORD(r) && i < item_count() && (enabled(i) || clickable(i)) ? i : -1;
}

static int row_height(const sb_row *r) {
  static const int heights[] = {[ROW_NAV] = NAV_H,       [ROW_HEADER] = HEADER_H, [ROW_DEVICE] = NAV_H,
                                [ROW_ACCORDION] = NAV_H, [ROW_NEARBY] = NAV_H,    [ROW_NOTE] = NOTE_H,
                                [ROW_SPACER] = 0,        [ROW_FOOTER] = FOOTER_H};
  return scale(heights[r->kind]);
}

static void add_item(int at, int row) {
  const sb_row *r = &s.rows[row];
  int item = (int)SendMessageW(s.wnd, LB_INSERTSTRING, (WPARAM)at, (LPARAM)(r->text ? r->text : L""));
  SendMessageW(s.wnd, LB_SETITEMDATA, (WPARAM)item, row);
  SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)item, row_height(r));
}

/* Gives the spacer the height left over, so the rows after it sit at the bottom. When there
 * isn't enough room it shrinks to nothing and the last rows are cut off (the list doesn't
 * scroll). */
static void fit_spacer(void) {
  RECT rc;
  GetClientRect(s.wnd, &rc);
  int first = -1, n = 0, used = scale(BOTTOM), spacer_row = 0;
  for (int i = 0; i < item_count(); i++) {
    const sb_row *r = row_of(i);
    if (r->kind != ROW_SPACER) {
      used += row_height(r);
    } else {
      if (first < 0) first = i;
      spacer_row = row_index(i);
      n++;
    }
  }
  if (first < 0) return;

  int gap = max(0, rc.bottom - used), want = max(1, (gap + MAX_ITEM_H - 1) / MAX_ITEM_H);
  set_hot(-1); /* item indexes shift */
  for (; n < want; n++) add_item(first, spacer_row);
  for (; n > want; n--) SendMessageW(s.wnd, LB_DELETESTRING, (WPARAM)first, 0);
  for (int k = 0; k < n; k++)
    SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)(first + k), gap / n + (k < gap % n));
  SendMessageW(s.wnd, LB_SETTOPINDEX, 0, 0);
  InvalidateRect(s.wnd, NULL, TRUE);
}

static void invalidate_row(int i) {
  RECT rc;
  if (i >= 0 && SendMessageW(s.wnd, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&rc) != LB_ERR)
    InvalidateRect(s.wnd, &rc, FALSE);
}

static void set_hot(int i) {
  if (i == s.hot) return;
  invalidate_row(s.hot);
  invalidate_row(i);
  s.hot = i;
}

static void select_item(int i) {
  if (i < 0 || i == selected_item()) return;
  SendMessageW(s.wnd, LB_SETCURSEL, (WPARAM)i, 0);
  SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), LBN_SELCHANGE),
               (LPARAM)s.wnd);
}

/* --- The + on the DEVICES heading --- */

/* Where the + is, in client coordinates; empty if the heading isn't there. */
static RECT add_rect(void) {
  RECT rc = {0, 0, 0, 0};
  for (int i = 0; i < item_count(); i++) {
    if (row_of(i)->kind != ROW_HEADER) continue;
    if (SendMessageW(s.wnd, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&rc) == LB_ERR) break;
    rc.right -= scale(INSET);
    rc.left = rc.right - scale(ADD_W);
    rc.bottom -= scale(ADD_BOTTOM);
    rc.top = rc.bottom - scale(ADD_W);
    return rc;
  }
  SetRectEmpty(&rc);
  return rc;
}

static BOOL in_add(LPARAM lp) {
  RECT rc = add_rect();
  POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
  return !s.locked && PtInRect(&rc, pt);
}

static void set_add_hot(BOOL hot) {
  if (hot == s.add_hot) return;
  s.add_hot = hot;
  RECT rc = add_rect();
  InvalidateRect(s.wnd, &rc, FALSE);
}

/* What a row's tooltip says, or NULL. */
static const wchar_t *row_tip(const sb_row *r) {
  switch (r->kind) {
    case ROW_FOOTER: return s.footer_tip;
    case ROW_DEVICE: return r->flags & SB_DEV_ONLINE ? NULL : mc_wstr(MC_STR_WINDOWS_DEVICE_OFFLINE);
    case ROW_NEARBY: return r->flags & SB_DEV_OTHER_GROUP ? mc_wstr(MC_STR_WINDOWS_DEVICE_OTHER_GROUP_TIP) : NULL;
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
  for (int i = 0; i < item_count(); i++) {
    const wchar_t *tip = row_tip(row_of(i));
    if (!tip || !*tip) continue;
    TTTOOLINFOW add = {.cbSize = sizeof add, .uFlags = TTF_SUBCLASS, .hwnd = s.wnd,
                       .uId = (UINT_PTR)(TIP_ROWS + s.ntips), .lpszText = (wchar_t *)tip};
    if (SendMessageW(s.wnd, LB_GETITEMRECT, (WPARAM)i, (LPARAM)&add.rect) == LB_ERR) continue;
    if (SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&add)) s.ntips++;
  }
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

static void push_nav(const wchar_t *text, wchar_t icon, sidebar_page page) {
  sb_row *r = push_row(ROW_NAV, text, NULL);
  if (!r) return;
  r->icon = icon;
  r->page = page;
}

static void push_device(row_kind kind, const device *d) {
  static const wchar_t icons[] = {[MC_DEVICE_DESKTOP] = 0xE977, /* PC */
                                  [MC_DEVICE_PHONE] = 0xE8EA,   /* CellPhone */
                                  [MC_DEVICE_WATCH] = 0xE823};  /* Recent: a clock face */
  const wchar_t *name = d->name && *d->name ? d->name : mc_wstr(MC_STR_WINDOWS_DEVICE_UNNAMED);
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
  for (int i = 0; i < s.nrows; i++) free(s.rows[i].text);
  s.nrows = 0;
}

/* Remakes the rows and the list's items, keeping the selected page (and device). If the
 * selected device is gone, selects Albums. */
static void rebuild(void) {
  sidebar_page page = PAGE_ALBUMS;
  char uid[37] = "";
  int sel = selected_item();
  if (sel >= 0) {
    const sb_row *r = row_of(sel);
    page = r->page;
    memcpy(uid, r->uid, sizeof uid);
  }

  /* Item indexes shift, so nothing can stay hot or pressed. */
  if (s.pressed >= 0 && GetCapture() == s.wnd) ReleaseCapture();
  s.pressed = -1;
  s.hot = -1;

  free_rows();
  for (size_t i = 0; i < sizeof nav_top / sizeof nav_top[0]; i++)
    push_nav(mc_wstr(nav_top[i].text), nav_top[i].icon, nav_top[i].page);
  wchar_t heading[64];
  lstrcpynW(heading, mc_wstr(MC_STR_DEVICES), 64);
  CharUpperBuffW(heading, (DWORD)wcslen(heading));
  push_row(ROW_HEADER, heading, NULL);
  for (int i = 0; i < s.npaired; i++) push_device(ROW_DEVICE, &s.paired[i]);
  if (s.nnearby > 0) {
    wchar_t text[64];
    mc_wformat(text, 64, mc_str(MC_STR_WINDOWS_DEVICES_UNPAIRED), MC_D(s.nnearby));
    sb_row *r = push_row(ROW_ACCORDION, text, NULL);
    if (r) r->icon = accordion_expanded() ? 0xE70D : 0xE76C; /* ChevronDown, ChevronRight */
    if (accordion_expanded())
      for (int i = 0; i < s.nnearby; i++) push_device(ROW_NEARBY, &s.nearby[i]);
  }
  if (s.npaired == 0 && s.nnearby == 0) push_row(ROW_NOTE, mc_wstr(MC_STR_DEVICES_NONE), NULL);
  push_row(ROW_SPACER, L"", NULL); /* pushes the rest to the bottom */
  for (size_t i = 0; i < sizeof nav_bottom / sizeof nav_bottom[0]; i++)
    push_nav(mc_wstr(nav_bottom[i].text), nav_bottom[i].icon, nav_bottom[i].page);
  if (s.footer[0] || s.footer[1]) push_row(ROW_FOOTER, s.footer[0], NULL);

  BOOL visible = IsWindowVisible(s.wnd);
  if (visible) SendMessageW(s.wnd, WM_SETREDRAW, FALSE, 0);
  SendMessageW(s.wnd, LB_RESETCONTENT, 0, 0);
  for (int i = 0; i < s.nrows; i++) add_item(i, i);
  int found = -1, albums = 0;
  for (int i = 0; i < item_count(); i++) {
    const sb_row *r = row_of(i);
    if (r->kind == ROW_NAV && r->page == PAGE_ALBUMS) albums = i;
    if ((r->kind == ROW_NAV || r->kind == ROW_DEVICE) && r->page == page &&
        (page != PAGE_DEVICE || strcmp(r->uid, uid) == 0))
      found = i;
  }
  SendMessageW(s.wnd, LB_SETCURSEL, (WPARAM)(found >= 0 ? found : albums), 0);
  fit_spacer();
  if (visible) SendMessageW(s.wnd, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(s.wnd, NULL, TRUE);
  update_tooltip();
}

/* A click on a clickable item. */
static void activate(int item) {
  const sb_row *r = row_of(item);
  if (r->kind == ROW_ACCORDION) {
    s.expanded = !accordion_expanded();
    s.expanded_set = TRUE;
    rebuild();
  } else if (r->kind == ROW_NEARBY) {
    memcpy(s.clicked, r->uid, sizeof s.clicked);
    SendMessageW(GetParent(s.wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(s.wnd), SBN_PAIR_DEVICE), (LPARAM)s.wnd);
  }
}

/* Takes over the mouse and keyboard so only pages can be selected, and the accordion and
 * nearby devices act like buttons. */
static LRESULT CALLBACK sidebar_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id,
                                     DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
      TrackMouseEvent(&tme);
      int i = item_at(lp);
      set_hot(s.add_pressed ? -1 : s.pressed >= 0 ? (i == s.pressed ? i : -1) : i);
      set_add_hot(in_add(lp));
      return 0;
    }
    case WM_MOUSELEAVE:
      if (s.pressed < 0) set_hot(-1);
      if (!s.add_pressed) set_add_hot(FALSE);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      if (in_add(lp)) { /* the + acts on release, like a button */
        s.add_pressed = TRUE;
        SetCapture(wnd);
        RECT rc = add_rect();
        InvalidateRect(wnd, &rc, FALSE);
        return 0;
      }
      SetFocus(wnd);
      int i = item_at(lp);
      if (i >= 0 && clickable(i)) { /* so do the accordion and nearby devices */
        s.pressed = i;
        SetCapture(wnd);
        set_hot(i);
        invalidate_row(i);
        return 0;
      }
      select_item(i);
      return 0;
    }
    case WM_LBUTTONUP:
      if (s.add_pressed) {
        BOOL clicked = in_add(lp);
        ReleaseCapture(); /* WM_CAPTURECHANGED clears add_pressed */
        if (clicked)
          SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), SBN_ADD_DEVICE), (LPARAM)wnd);
      } else if (s.pressed >= 0) {
        int i = s.pressed;
        BOOL clicked = item_at(lp) == i;
        ReleaseCapture(); /* WM_CAPTURECHANGED clears pressed */
        if (clicked) activate(i);
      }
      return 0;
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
      break;
    case WM_KEYDOWN: {
      /* Using the keyboard shows focus rectangles, as in a dialog. */
      SendMessageW(GetAncestor(wnd, GA_ROOT), WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
      int cur = selected_item();
      switch (wp) {
        case VK_UP: select_item(next_nav(cur, -1)); return 0;
        case VK_DOWN: select_item(next_nav(cur, +1)); return 0;
        case VK_HOME:
        case VK_PRIOR: select_item(next_nav(-1, +1)); return 0;
        case VK_END:
        case VK_NEXT: select_item(next_nav(item_count(), -1)); return 0;
      }
      break;
    }
    case WM_CHAR:
      return 0; /* type-ahead would land on headers */
    case WM_SIZE: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      fit_spacer();
      update_tooltip();
      return r;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, sidebar_proc, id);
      if (s.font) DeleteObject(s.font);
      if (s.header_font) DeleteObject(s.header_font);
      if (s.icon_font) DeleteObject(s.icon_font);
      if (s.small_icon_font) DeleteObject(s.small_icon_font);
      if (s.footer_font) DeleteObject(s.footer_font);
      if (s.bg) DeleteObject(s.bg);
      if (s.tooltip) DestroyWindow(s.tooltip);
      canvas_free(&s.buf);
      free_rows();
      free(s.rows);
      free_devices(s.paired, s.npaired);
      free_devices(s.nearby, s.nnearby);
      free(s.footer[0]);
      free(s.footer[1]);
      free(s.footer_tip);
      memset(&s, 0, sizeof s);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

HWND sidebar_create(HWND parent, int id) {
  memset(&s, 0, sizeof s);
  s.hot = s.pressed = -1;
  s.dpi = USER_DEFAULT_SCREEN_DPI;
  s.wnd = CreateWindowExW(0, WC_LISTBOXW, NULL,
                          WS_CHILD | WS_VISIBLE | LBS_OWNERDRAWVARIABLE | LBS_HASSTRINGS |
                            LBS_NOINTEGRALHEIGHT,
                          0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  if (!s.wnd) return NULL;
  rebuild(); /* selects Albums */
  SetWindowSubclass(s.wnd, sidebar_proc, 0, 0);

  s.tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                              CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, s.wnd, NULL,
                              GetModuleHandleW(NULL), NULL);
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_SUBCLASS, .hwnd = s.wnd, .uId = TIP_ADD,
                    .lpszText = (wchar_t *)mc_wstr(MC_STR_ADD_DEVICE_BY_ADDRESS)};
  SendMessageW(s.tooltip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
  SendMessageW(s.tooltip, TTM_SETMAXTIPWIDTH, 0, TIP_W); /* so the footer's lines break */
  update_tooltip();
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
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 8, 9); /* 9pt -> 8pt */
  if (s.footer_font) DeleteObject(s.footer_font);
  s.footer_font = CreateFontIndirectW(&lf);
  if (s.icon_font) DeleteObject(s.icon_font);
  s.icon_font = icon_font(16);
  if (s.small_icon_font) DeleteObject(s.small_icon_font);
  s.small_icon_font = icon_font(12);

  if (s.bg) DeleteObject(s.bg);
  s.bg = CreateSolidBrush(theme.mica ? RGB(0, 0, 0) : theme.frame);

  for (int i = 0; i < item_count(); i++)
    if (row_of(i)->kind != ROW_SPACER) SendMessageW(s.wnd, LB_SETITEMHEIGHT, (WPARAM)i, row_height(row_of(i)));
  fit_spacer();
  update_tooltip();
  if (s.tooltip) SendMessageW(s.tooltip, TTM_SETMAXTIPWIDTH, 0, scale(TIP_W));
  theme_apply_control(s.tooltip);
}

sidebar_page sidebar_selection(void) {
  int i = selected_item();
  return i >= 0 ? row_of(i)->page : PAGE_ALBUMS;
}

void sidebar_select(sidebar_page page) {
  for (int i = 0; i < item_count(); i++)
    if (row_of(i)->kind == ROW_NAV && row_of(i)->page == page) SendMessageW(s.wnd, LB_SETCURSEL, (WPARAM)i, 0);
}

void sidebar_set_locked(BOOL locked) {
  if (locked == s.locked) return;
  s.locked = locked;
  set_hot(-1);
  set_add_hot(FALSE);
  InvalidateRect(s.wnd, NULL, FALSE);
}

void sidebar_draw(const DRAWITEMSTRUCT *dis) {
  int item = (int)dis->itemID, i = (int)dis->itemData;
  int w = dis->rcItem.right - dis->rcItem.left, h = dis->rcItem.bottom - dis->rcItem.top;
  if (item < 0 || i < 0 || i >= s.nrows || !canvas_reserve(&s.buf, w, h)) return;
  const sb_row *r = &s.rows[i];

  /* Drawn off-screen, which also gets the alpha right over Mica. */
  canvas *c = &s.buf;
  RECT rc = {0, 0, w, h};
  canvas_clear(c, &rc, theme.frame, theme.mica ? 0 : 255);

  HFONT font = s.font;
  COLORREF color = theme.text;
  RECT text = {scale(TEXT_X + r->indent), 0, w - scale(INSET), h};
  UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
  switch (r->kind) {
    case ROW_NAV:
    case ROW_DEVICE: {
      BOOL selected = (dis->itemState & ODS_SELECTED) != 0;
      RECT fill = {scale(INSET), scale(2), w - scale(INSET), h - scale(2)};
      color = theme_draw_state(c, &fill, scale(RADIUS),
                               selected ? STATE_SELECTED : item == s.hot ? STATE_HOT : STATE_NORMAL);
      if (!enabled(item)) color = theme.text3; /* locked */
      if (selected && !theme.high_contrast) { /* accent marker at the left edge */
        int mark_h = scale(16);
        RECT mark = {fill.left, (h - mark_h) / 2, fill.left + scale(3), (h + mark_h) / 2};
        canvas_fill(c, &mark, scale(2), theme.accent, 255);
      }
      if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT))
        canvas_frame(c, &fill, scale(RADIUS), scale(2), theme.text, 255);
      RECT icon = {text.left, 0, text.left + scale(ICON_W), h};
      canvas_text(c, &r->icon, 1, &icon, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.icon_font, color);
      text.left += scale(ICON_W);
      /* A small glyph at the end: Sync while syncing, else NetworkOffline while offline. */
      wchar_t badge = r->flags & SB_DEV_SYNCING                               ? 0xE895
                      : r->kind == ROW_DEVICE && !(r->flags & SB_DEV_ONLINE) ? 0xF384
                                                                              : 0;
      if (badge) {
        RECT at = {text.right - scale(ICON_W - 8), 0, text.right - scale(4), h};
        canvas_text(c, &badge, 1, &at, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.small_icon_font,
                    color == theme.text ? theme.text2 : color);
        text.right = at.left;
      }
      break;
    }
    case ROW_NEARBY: { /* a button: hot and pressed, never selected */
      RECT fill = {scale(INSET + r->indent), scale(2), w - scale(INSET), h - scale(2)};
      control_state state = s.pressed == item && s.hot == item ? STATE_PRESSED
                            : s.hot == item || s.pressed == item ? STATE_HOT
                                                                  : STATE_NORMAL;
      color = theme_draw_state(c, &fill, scale(RADIUS), state);
      if (s.locked) color = theme.text3;
      RECT icon = {text.left, 0, text.left + scale(ICON_W), h};
      canvas_text(c, &r->icon, 1, &icon, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s.icon_font, color);
      text.left += scale(ICON_W);
      break;
    }
    case ROW_ACCORDION: { /* the heading of the nearby rows, with a chevron at its end */
      RECT fill = {scale(INSET), scale(2), w - scale(INSET), h - scale(2)};
      control_state state = s.pressed == item && s.hot == item ? STATE_PRESSED
                            : s.hot == item || s.pressed == item ? STATE_HOT
                                                                  : STATE_NORMAL;
      theme_draw_state(c, &fill, scale(RADIUS), state);
      color = state == STATE_NORMAL ? theme.text2 : theme.text;
      RECT chevron = {fill.right - scale(ADD_W), 0, fill.right, h};
      canvas_text(c, &r->icon, 1, &chevron, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX,
                  s.small_icon_font, color);
      text.right = chevron.left;
      break;
    }
    case ROW_HEADER: { /* the heading, level with the + at its end */
      RECT add = {w - scale(INSET + ADD_W), h - scale(ADD_BOTTOM + ADD_W), w - scale(INSET), h - scale(ADD_BOTTOM)};
      control_state state = s.locked                     ? STATE_NORMAL
                            : s.add_pressed && s.add_hot ? STATE_PRESSED
                            : s.add_hot || s.add_pressed ? STATE_HOT
                                                         : STATE_NORMAL;
      COLORREF add_color = theme_draw_state(c, &add, scale(4), state);
      if (s.locked) add_color = theme.text3;
      canvas_text(c, L"\xE710", 1, &add, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, s.icon_font,
                  add_color); /* Add */
      font = s.header_font;
      color = theme.text2;
      text.top = add.top;
      text.bottom = add.bottom;
      text.right = add.left - scale(4);
      break;
    }
    case ROW_NOTE:
      color = theme.text3;
      break;
    case ROW_SPACER:
      break;
    case ROW_FOOTER: { /* two small lines at the bottom */
      int line = scale(FOOTER_LINE);
      for (int k = 0; k < 2; k++) {
        RECT rl = {text.left, h - (2 - k) * line, text.right, h - (1 - k) * line};
        if (s.footer[k])
          canvas_text(c, s.footer[k], -1, &rl, format, s.footer_font, theme.text3);
      }
      canvas_blit(c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
      return;
    }
  }
  canvas_text(c, r->text ? r->text : L"", -1, &text, format, font, color);
  canvas_blit(c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
}

HBRUSH sidebar_ctlcolor(HWND wnd) {
  return wnd == s.wnd ? s.bg : NULL;
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

void sidebar_set_devices(const sidebar_device *paired, int npaired, const sidebar_device *nearby, int nnearby) {
  free_devices(s.paired, s.npaired);
  free_devices(s.nearby, s.nnearby);
  s.paired = copy_devices(paired, npaired);
  s.npaired = s.paired ? npaired : 0;
  s.nearby = copy_devices(nearby, nnearby);
  s.nnearby = s.nearby ? nnearby : 0;
  s.clicked[0] = 0;
  if (s.wnd) rebuild();
}

void sidebar_set_footer(const wchar_t *line1, const wchar_t *line2) {
  BOOL had = s.footer[0] || s.footer[1];
  free(s.footer[0]);
  free(s.footer[1]);
  s.footer[0] = line1 ? wcs_dup(line1, NULL) : NULL;
  s.footer[1] = line2 ? wcs_dup(line2, NULL) : NULL;
  if (!s.wnd) return;
  if (had != (s.footer[0] || s.footer[1])) {
    rebuild();
    return;
  }
  for (int i = 0; i < item_count(); i++)
    if (row_of(i)->kind == ROW_FOOTER) invalidate_row(i);
}

void sidebar_set_footer_tip(const wchar_t *tip) {
  if (s.footer_tip && tip && !wcscmp(s.footer_tip, tip)) return;
  free(s.footer_tip);
  s.footer_tip = tip ? wcs_dup(tip, NULL) : NULL;
  update_tooltip();
}

const char *sidebar_selected_device(void) {
  int i = selected_item();
  return i >= 0 && row_of(i)->kind == ROW_DEVICE ? row_of(i)->uid : NULL;
}

void sidebar_select_device(const char *uid) {
  if (!uid) return;
  for (int i = 0; i < item_count(); i++)
    if (row_of(i)->kind == ROW_DEVICE && strcmp(row_of(i)->uid, uid) == 0) {
      SendMessageW(s.wnd, LB_SETCURSEL, (WPARAM)i, 0);
      return;
    }
}

const char *sidebar_clicked_nearby(void) {
  return s.clicked[0] ? s.clicked : NULL;
}
