#include "theme.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <string.h>
#include <uxtheme.h>
#include <wchar.h>

/* Newer than some SDK headers. */
#define DWMWA_DARK_MODE_OLD 19 /* before Windows 10 20H1 */
#define DWMWA_DARK_MODE 20
#define DWMWA_BACKDROP 38 /* DWMWA_SYSTEMBACKDROP_TYPE, Windows 11 22H2 */
#define BACKDROP_NONE 1
#define BACKDROP_MICA 2

theme_colors theme;

static DWORD windows_build(void) {
  typedef LONG(WINAPI * rtl_get_version)(OSVERSIONINFOW *);
  rtl_get_version get =
    (rtl_get_version)(void (*)(void))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  OSVERSIONINFOW v = {.dwOSVersionInfoSize = sizeof v};
  return get && get(&v) == 0 ? v.dwBuildNumber : 0;
}

/* Menus (the window menu, edit boxes' context menus) only go dark through uxtheme's
 * undocumented SetPreferredAppMode, ordinal 135 since Windows 10 1903. */
static void set_menu_mode(BOOL dark) {
  static BOOL loaded;
  static int(WINAPI * set_preferred_app_mode)(int);
  static void(WINAPI * flush_menu_themes)(void);
  if (!loaded) {
    loaded = TRUE;
    HMODULE ux = windows_build() >= 18362 ? LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32) : NULL;
    if (ux) {
      set_preferred_app_mode = (int(WINAPI *)(int))(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(135));
      flush_menu_themes = (void(WINAPI *)(void))(void (*)(void))GetProcAddress(ux, MAKEINTRESOURCEA(136));
    }
  }
  if (set_preferred_app_mode) set_preferred_app_mode(dark ? 2 : 3); /* ForceDark, ForceLight */
  if (flush_menu_themes) flush_menu_themes();
}

/* The accent colour as Windows 11 uses it for fills: a shade darker than the user's choice
 * in light mode, two lighter in dark mode. */
static BOOL read_accent(BOOL dark, COLORREF *out) {
  BYTE palette[32]; /* light 3, 2, 1, base, dark 1, 2, 3, unused: RGBx each */
  DWORD size = sizeof palette;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                   L"AccentPalette", RRF_RT_REG_BINARY, NULL, palette, &size) == ERROR_SUCCESS &&
      size >= sizeof palette) {
    const BYTE *p = palette + 4 * (dark ? 1 : 4);
    *out = RGB(p[0], p[1], p[2]);
    return TRUE;
  }
  DWORD abgr;
  size = sizeof abgr;
  if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD,
                   NULL, &abgr, &size) == ERROR_SUCCESS) {
    *out = abgr & 0xFFFFFF; /* same byte order as COLORREF */
    return TRUE;
  }
  return FALSE;
}

void theme_update(void) {
  HIGHCONTRASTW hc = {.cbSize = sizeof hc};
  theme.high_contrast =
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
  DWORD light = 1, size = sizeof light;
  theme.dark = !theme.high_contrast &&
               RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                            L"AppsUseLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size) == ERROR_SUCCESS &&
               !light;
  wchar_t forced[8]; /* KITHSONG_THEME=light or dark overrides the app mode, for testing */
  if (!theme.high_contrast && GetEnvironmentVariableW(L"KITHSONG_THEME", forced, 8))
    theme.dark = !_wcsicmp(forced, L"dark");

  /* Windows 11's palette: the Mica fallback colours, and its text and stroke opacities
   * flattened onto the backgrounds. */
  if (theme.high_contrast) {
    theme.bg = theme.frame = theme.field = GetSysColor(COLOR_WINDOW);
    theme.text = theme.text2 = theme.line = GetSysColor(COLOR_WINDOWTEXT);
    theme.text3 = GetSysColor(COLOR_GRAYTEXT);
    theme.accent = GetSysColor(COLOR_HIGHLIGHT);
    theme.on_accent = GetSysColor(COLOR_HIGHLIGHTTEXT);
  } else if (theme.dark) {
    theme.bg = RGB(28, 28, 28);
    theme.frame = RGB(32, 32, 32);
    theme.field = RGB(45, 45, 45);
    theme.text = RGB(255, 255, 255);
    theme.text2 = RGB(206, 206, 206);
    theme.text3 = RGB(150, 150, 150);
    theme.line = RGB(50, 50, 50);
    theme.accent = RGB(96, 205, 255);
    theme.on_accent = RGB(0, 0, 0);
  } else {
    theme.bg = theme.field = RGB(255, 255, 255);
    theme.frame = RGB(243, 243, 243);
    theme.text = RGB(26, 26, 26);
    theme.text2 = RGB(96, 96, 96);
    theme.text3 = RGB(138, 138, 138);
    theme.line = RGB(229, 229, 229);
    theme.accent = RGB(0, 95, 184);
    theme.on_accent = RGB(255, 255, 255);
  }
  if (!theme.high_contrast) read_accent(theme.dark, &theme.accent);

  if (theme.bg_brush) DeleteObject(theme.bg_brush);
  if (theme.field_brush) DeleteObject(theme.field_brush);
  theme.bg_brush = CreateSolidBrush(theme.bg);
  theme.field_brush = CreateSolidBrush(theme.field);

  set_menu_mode(theme.dark);
  canvas_theme_changed();
}

BOOL theme_is_change(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_SETTINGCHANGE:
      return wp == SPI_SETHIGHCONTRAST || (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet"));
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED:
      return TRUE;
  }
  return FALSE;
}

void theme_apply_window(HWND wnd, BOOL mica) {
  BOOL dark = theme.dark;
  if (FAILED(DwmSetWindowAttribute(wnd, DWMWA_DARK_MODE, &dark, sizeof dark)))
    DwmSetWindowAttribute(wnd, DWMWA_DARK_MODE_OLD, &dark, sizeof dark);
  if (!mica) return;
  int type = theme.high_contrast ? BACKDROP_NONE : BACKDROP_MICA;
  theme.mica = SUCCEEDED(DwmSetWindowAttribute(wnd, DWMWA_BACKDROP, &type, sizeof type)) && type == BACKDROP_MICA;
}

/* Dark list view headers keep black text unless it's set while they draw. */
static LRESULT CALLBACK listview_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  if (msg == WM_NOTIFY && theme.dark) {
    NMCUSTOMDRAW *cd = (NMCUSTOMDRAW *)lp;
    if (cd->hdr.code == NM_CUSTOMDRAW && cd->hdr.hwndFrom == ListView_GetHeader(wnd)) {
      if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
      if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
        SetTextColor(cd->hdc, theme.text);
        return CDRF_DODEFAULT;
      }
    }
  } else if (msg == WM_NCDESTROY) {
    RemoveWindowSubclass(wnd, listview_proc, id);
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* Progress bars are drawn here, Windows 11 style: a thin track and a rounded accent bar.
 * The stock ones have no dark mode. */
static LRESULT CALLBACK progress_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      RECT rc;
      GetClientRect(wnd, &rc);
      canvas c = {0};
      if (canvas_reserve(&c, rc.right, rc.bottom)) {
        canvas_clear(&c, &rc, theme.bg, 255);
        int lo = (int)SendMessageW(wnd, PBM_GETRANGE, TRUE, 0), hi = (int)SendMessageW(wnd, PBM_GETRANGE, FALSE, 0);
        int pos = (int)SendMessageW(wnd, PBM_GETPOS, 0, 0), h = rc.bottom;
        int track_h = h / 3 > 1 ? h / 3 : 1, bar_h = h / 2 > 2 ? h / 2 : 2;
        RECT track = {0, (h - track_h) / 2, rc.right, (h + track_h) / 2};
        canvas_fill(&c, &track, track_h, theme.text3, 255);
        if (hi > lo && pos > lo) {
          RECT bar = {0, (h - bar_h) / 2, MulDiv(rc.right, pos - lo, hi - lo), (h + bar_h) / 2};
          if (bar.right < bar_h) bar.right = bar_h;
          canvas_fill(&c, &bar, bar_h, theme.accent, 255);
        }
        canvas_blit(&c, dc, 0, 0, &rc);
        canvas_free(&c);
      }
      EndPaint(wnd, &ps);
      return 0;
    }
    case PBM_SETPOS:
    case PBM_SETRANGE:
    case PBM_SETRANGE32:
    case PBM_DELTAPOS:
    case PBM_STEPIT: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      InvalidateRect(wnd, NULL, FALSE);
      return r;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, progress_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

void theme_apply_control(HWND ctl) {
  wchar_t cls[64];
  if (!ctl || !GetClassNameW(ctl, cls, 64)) return;
  const wchar_t *dark_or_default = theme.dark ? L"DarkMode_Explorer" : NULL;
  LONG style = GetWindowLongW(ctl, GWL_STYLE);

  if (!_wcsicmp(cls, WC_BUTTONW)) {
    if ((style & BS_TYPEMASK) != BS_OWNERDRAW) SetWindowTheme(ctl, dark_or_default, NULL);
  } else if (!_wcsicmp(cls, WC_LISTVIEWW)) {
    SetWindowTheme(ctl, theme.dark ? L"DarkMode_ItemsView" : L"Explorer", NULL);
    SetWindowTheme(ListView_GetHeader(ctl), theme.dark ? L"DarkMode_ItemsView" : NULL, NULL);
    ListView_SetBkColor(ctl, theme.bg);
    ListView_SetTextBkColor(ctl, theme.bg);
    ListView_SetTextColor(ctl, theme.text);
    SetWindowSubclass(ctl, listview_proc, 0, 0);
  } else if (!_wcsicmp(cls, WC_EDITW)) {
    SetWindowTheme(ctl, theme.dark ? L"DarkMode_CFD" : NULL, NULL);
  } else if (!_wcsicmp(cls, WC_COMBOBOXW)) {
    /* The file dialog's dark combo box; its drop-down list is a popup of its own, not a
     * child, so it's themed here too (WM_CTLCOLORLISTBOX gives it its colours). */
    SetWindowTheme(ctl, theme.dark ? L"DarkMode_CFD" : NULL, NULL);
    COMBOBOXINFO info = {.cbSize = sizeof info};
    if (GetComboBoxInfo(ctl, &info) && info.hwndList) {
      SetWindowTheme(info.hwndList, dark_or_default, NULL);
      InvalidateRect(info.hwndList, NULL, TRUE);
    }
  } else if (!_wcsicmp(cls, PROGRESS_CLASSW)) {
    /* No visual style, so no glow animation drawing behind our back; and without one it
     * gets a sunken edge, which goes. */
    SetWindowTheme(ctl, L"", L"");
    LONG ex = GetWindowLongW(ctl, GWL_EXSTYLE);
    SetWindowLongW(ctl, GWL_EXSTYLE, ex & ~(WS_EX_STATICEDGE | WS_EX_CLIENTEDGE));
    SetWindowLongW(ctl, GWL_STYLE, style & ~WS_BORDER);
    SetWindowPos(ctl, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowSubclass(ctl, progress_proc, 0, 0);
  } else if (!_wcsicmp(cls, WC_LISTBOXW) || !_wcsicmp(cls, TOOLTIPS_CLASSW)) {
    if (!(style & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) || !_wcsicmp(cls, TOOLTIPS_CLASSW))
      SetWindowTheme(ctl, dark_or_default, NULL);
  }
  InvalidateRect(ctl, NULL, TRUE);
}

static BOOL CALLBACK apply_child(HWND ctl, LPARAM lp) {
  (void)lp;
  theme_apply_control(ctl);
  return TRUE;
}

void theme_apply_controls(HWND wnd) {
  EnumChildWindows(wnd, apply_child, 0);
}

HBRUSH theme_ctlcolor(UINT msg, HDC dc) {
  switch (msg) {
    case WM_CTLCOLOREDIT:
      SetTextColor(dc, theme.text);
      SetBkColor(dc, theme.field);
      return theme.field_brush;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
      SetTextColor(dc, theme.text);
      SetBkColor(dc, theme.bg);
      return theme.bg_brush;
  }
  return NULL;
}

COLORREF theme_draw_state(canvas *c, const RECT *r, int radius, control_state state) {
  if (theme.high_contrast) {
    if (state == STATE_SELECTED || state == STATE_PRESSED) {
      canvas_fill(c, r, radius, theme.accent, 255);
      return theme.on_accent;
    }
    if (state == STATE_HOT) canvas_frame(c, r, radius, 1, theme.accent, 255);
    return theme.text;
  }
  /* Windows 11's subtle fills: black (or in dark mode white) at a low opacity. */
  static const int alpha[] = {[STATE_NORMAL] = 0, [STATE_HOT] = 15, [STATE_PRESSED] = 10, [STATE_SELECTED] = 22};
  canvas_fill(c, r, radius, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), alpha[state]);
  return state == STATE_PRESSED ? theme.text2 : theme.text;
}

void theme_draw_segment(canvas *c, const RECT *r, int radius, const wchar_t *text, HFONT font, BOOL first,
                        BOOL last, BOOL selected, control_state state) {
  int line = 1;
  canvas_clear(c, r, theme.bg, 255);
  /* The row's outline, drawn wider than r wherever it continues into the next segment so
   * only its own part shows, and only the row's two ends are rounded. */
  RECT outline = *r;
  if (!first) outline.left -= 2 * radius + line;
  if (!last) outline.right += 2 * radius + line;
  COLORREF fg;
  if (selected) {
    canvas_fill(c, &outline, radius, theme.accent, 255);
    fg = theme.on_accent;
  } else {
    canvas_frame(c, &outline, radius, line, theme.high_contrast ? theme.text : theme.line, 255);
    RECT inner = outline;
    InflateRect(&inner, -line, -line);
    fg = theme_draw_state(c, &inner, radius > line ? radius - line : 0, state);
    if (!first) { /* the divider between this segment and the one before */
      RECT div = {r->left, r->top + line, r->left + line, r->bottom - line};
      canvas_fill(c, &div, 0, theme.high_contrast ? theme.text : theme.line, 255);
    }
  }
  if (selected && state == STATE_PRESSED) canvas_fill(c, &outline, radius, RGB(0, 0, 0), 25);
  RECT tr = *r;
  InflateRect(&tr, -radius, 0);
  canvas_text(c, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS, font, fg);
}

HIMAGELIST theme_checkboxes(int size) {
  HIMAGELIST il = ImageList_Create(size, size, ILC_COLOR32, 2, 0);
  canvas c = {0};
  if (!il || !canvas_reserve(&c, size, size)) {
    if (il) ImageList_Destroy(il);
    return NULL;
  }
  int inset = MulDiv(size, 1, 16), line = size >= 24 ? 2 : 1, radius = MulDiv(size, 3, 16);
  HFONT tick_font = CreateFontW(-MulDiv(size, 12, 16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                                theme_icon_face());
  RECT all = {0, 0, size, size}, box = {inset, inset, size - inset, size - inset};
  for (int checked = 0; checked < 2; checked++) {
    canvas_clear(&c, &all, 0, 0);
    if (checked) {
      canvas_fill(&c, &box, radius, theme.accent, 255);
      /* CheckMark, drawn twice a pixel apart: on its own it's a hairline at this size. */
      for (int k = 0; k < 2; k++) {
        RECT at = box;
        OffsetRect(&at, k * line, 0);
        canvas_text(&c, L"\xE73E", 1, &at, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, tick_font,
                    theme.on_accent);
      }
    } else {
      canvas_frame(&c, &box, radius, line, theme.high_contrast ? theme.text : theme.text3, 255);
    }
    /* Image lists take straight alpha, as icons have; the canvas is premultiplied. */
    for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++) {
        uint32_t *p = &c.px[(size_t)y * (size_t)c.w + (size_t)x], a = *p >> 24;
        if (a && a < 255) {
          uint32_t r = ((*p >> 16) & 255) * 255 / a, g = ((*p >> 8) & 255) * 255 / a, b = (*p & 255) * 255 / a;
          *p = a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
        }
      }
    /* A bitmap of exactly this size: the canvas may be bigger. */
    BITMAPINFO bi = {.bmiHeader = {.biSize = sizeof bi.bmiHeader, .biWidth = size, .biHeight = -size, .biPlanes = 1,
                                   .biBitCount = 32, .biCompression = BI_RGB}};
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (bmp) {
      for (int y = 0; y < size; y++)
        memcpy((uint32_t *)bits + (size_t)y * (size_t)size, c.px + (size_t)y * (size_t)c.w, (size_t)size * 4);
      ImageList_Add(il, bmp, NULL);
      DeleteObject(bmp);
    }
  }
  if (tick_font) DeleteObject(tick_font);
  canvas_free(&c);
  return il;
}

/* The padding theme_center_edit puts above (and below) the text, for a client area height h
 * before it. */
static int edit_padding(HWND wnd, int h) {
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(wnd, WM_GETFONT, 0, 0));
  TEXTMETRICW tm = {0};
  GetTextMetricsW(dc, &tm);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  int pad = (h - tm.tmHeight) / 2;
  return pad > 0 ? pad : 0;
}

/* The padding is non-client area, painted in the edit's background (its parent's
 * WM_CTLCOLOR* brush), and clicks on it go to the text. */
static LRESULT CALLBACK center_edit_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_NCCALCSIZE: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      RECT *rc = wp ? &((NCCALCSIZE_PARAMS *)lp)->rgrc[0] : (RECT *)lp;
      int pad = edit_padding(wnd, rc->bottom - rc->top);
      rc->top += pad;
      rc->bottom -= pad;
      return r;
    }
    case WM_NCPAINT: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      RECT win, cl;
      GetWindowRect(wnd, &win);
      GetClientRect(wnd, &cl);
      MapWindowPoints(wnd, NULL, (POINT *)&cl, 2);
      OffsetRect(&cl, -win.left, -win.top);
      int border = cl.left, h = win.bottom - win.top;
      RECT above = {cl.left, border, cl.right, cl.top}, below = {cl.left, cl.bottom, cl.right, h - border};
      HDC dc = GetWindowDC(wnd);
      BOOL enabled = IsWindowEnabled(wnd) && !(GetWindowLongW(wnd, GWL_STYLE) & ES_READONLY);
      HBRUSH brush = (HBRUSH)SendMessageW(GetParent(wnd), enabled ? WM_CTLCOLOREDIT : WM_CTLCOLORSTATIC, (WPARAM)dc,
                                          (LPARAM)wnd);
      if (!brush) brush = GetSysColorBrush(COLOR_WINDOW);
      if (above.bottom > above.top) FillRect(dc, &above, brush);
      if (below.bottom > below.top) FillRect(dc, &below, brush);
      ReleaseDC(wnd, dc);
      return r;
    }
    case WM_NCHITTEST: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      return r == HTBORDER || r == HTNOWHERE ? HTCLIENT : r;
    }
    case WM_SETFONT:
    case WM_ENABLE: {
      LRESULT r = DefSubclassProc(wnd, msg, wp, lp);
      SetWindowPos(wnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
      return r;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, center_edit_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

void theme_center_edit(HWND edit) {
  if (!edit) return;
  SetWindowSubclass(edit, center_edit_proc, 0, 0);
  SetWindowPos(edit, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static int CALLBACK font_found(const LOGFONTW *lf, const TEXTMETRICW *tm, DWORD type, LPARAM found) {
  (void)lf, (void)tm, (void)type;
  *(BOOL *)found = TRUE;
  return 0;
}

const wchar_t *theme_icon_face(void) {
  static const wchar_t *face;
  if (!face) {
    LOGFONTW lf = {.lfCharSet = DEFAULT_CHARSET, .lfFaceName = L"Segoe Fluent Icons"};
    BOOL found = FALSE;
    HDC dc = GetDC(NULL);
    EnumFontFamiliesExW(dc, &lf, font_found, (LPARAM)&found, 0);
    ReleaseDC(NULL, dc);
    face = found ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
  }
  return face;
}
