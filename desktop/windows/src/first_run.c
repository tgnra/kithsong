#define COBJMACROS
#include "first_run.h"

#include <dwmapi.h>
#include <shlobj.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomation.h>
#include <wchar.h>
#include <wincodec.h>
#include <windowsx.h>

#include "canvas.h"
#include "musiccore/library.h"
#include "musiccore/library_move.h"
#include "pick_folder.h"
#include "resource.h"
#include "theme.h"
#include "uia.h"
#include "wstr.h"

/* Layout at 96 DPI. The column of text, cards and buttons is COL_W wide in the middle. */
enum {
  WIN_W = 600,
  COL_W = 440,
  CLOSE_W = 46,  /* the close button, as wide and tall as Windows' own */
  CLOSE_H = 32,
  LOGO_H = 80,   /* the wordmark, 352 x 100 in its SVG */
  GAP_LOGO = 28, /* logo to heading */
  GAP_HEADING = 8,
  GAP_TEXT = 28, /* text to the first card */
  CARD_H = 64,
  GAP_CARD = 8,
  CHOOSE_H = 40,
  GAP_START = 32,
  START_W = 160, /* at least */
  START_H = 32,
  BOTTOM = 40,
  EDGE = 32,     /* kept clear above and below the window on a short screen */
};

/* What the mouse and keyboard act on, in tab order (the close button isn't in it). */
enum part { PART_NONE = -1, PART_SUGGESTED, PART_CHOSEN, PART_CHOOSE, PART_START, PART_CLOSE, NUM_PARTS };

/* DwmSetWindowAttribute's DWMWA_WINDOW_CORNER_PREFERENCE (Windows 11), DWMWCP_ROUND. */
enum { ATTR_CORNERS = 33, CORNERS_ROUND = 2 };

static struct {
  HWND owner, wnd;
  UINT dpi;
  HFONT font, heading_font, title_font, icon_font, close_font;
  canvas buf;
  uint32_t *logo; /* premultiplied, logo_rc's size */
  BOOL logo_dark;
  char *music;     /* the user's Music folder, UTF-8; NULL if unknown */
  char *suggested; /* Kithsong Library in it */
  char *chosen;    /* the folder the user chose, once there is one */
  wchar_t *suggested_w, *chosen_w;
  int selected; /* PART_SUGGESTED, PART_CHOSEN or PART_NONE */
  int hot, pressed, focus;
  BOOL show_focus; /* the keyboard has been used, so the focus ring shows */
  BOOL tracking;   /* TrackMouseEvent is on */
  BOOL done;
  char *result;
  int w, h, line_h;
  RECT rc[NUM_PARTS]; /* empty while the part isn't shown */
  RECT logo_rc, heading_rc, text_rc;
  uia_host ui;
} g_fr;

static int scale(int px) {
  return MulDiv(px, (int)g_fr.dpi, USER_DEFAULT_SCREEN_DPI);
}

static char *dup(const char *s) {
  char *d = s ? malloc(strlen(s) + 1) : NULL;
  if (d) strcpy(d, s);
  return d;
}

/* dir\name, malloc'd; dir may end in a separator, as a drive's root does. */
static char *join(const char *dir, const char *name) {
  size_t n = strlen(dir);
  BOOL sep = n > 0 && (dir[n - 1] == '\\' || dir[n - 1] == '/');
  char *out = malloc(n + 1 + strlen(name) + 1);
  if (out) {
    strcpy(out, dir);
    if (!sep) strcat(out, "\\");
    strcat(out, name);
  }
  return out;
}

/* The user's Music folder; in debug builds KITHSONG_TEST_MUSIC_DIR stands in for it, so a
 * test's Start doesn't make a library in the real one. */
static char *music_folder(void) {
#ifndef NDEBUG
  const wchar_t *test = _wgetenv(L"KITHSONG_TEST_MUSIC_DIR");
  if (test && *test) return wstr_to_utf8(test);
#endif
  wchar_t *path = NULL;
  char *out = NULL;
  if (SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Music, 0, NULL, &path))) out = wstr_to_utf8(path);
  CoTaskMemFree(path);
  return out;
}

/* Whether dir already has a library's database at the top. */
static BOOL holds_library(const char *dir) {
  char *db = join(dir, MC_LIBRARY_DB_NAME);
  wchar_t *w = wstr_dup_from_utf8(db);
  DWORD attr = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
  free(w);
  free(db);
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static BOOL usable(const char *dir) {
  return mc_folder_is_empty(dir) || holds_library(dir);
}

/* Where the library goes for a folder the user chose: the folder itself if it's empty (or
 * holds a library already), else Kithsong Library inside it, if that is. NULL if neither. */
static char *library_in(const char *dir) {
  if (usable(dir)) return dup(dir);
  char *sub = join(dir, mc_str(MC_STR_WINDOWS_FIRST_RUN_FOLDER_NAME));
  if (sub && usable(sub)) return sub;
  free(sub);
  return NULL;
}

/* --- The logo --- */

/* The wordmark PNG (IDR_WORDMARK, or IDR_WORDMARK_DARK for a dark background) decoded at
 * w x h, premultiplied, top-down. NULL if it can't be. */
static uint32_t *load_logo(int w, int h, BOOL dark) {
  HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(dark ? IDR_WORDMARK_DARK : IDR_WORDMARK), RT_RCDATA);
  HGLOBAL mem = res ? LoadResource(NULL, res) : NULL;
  void *data = mem ? LockResource(mem) : NULL;
  DWORD size = res ? SizeofResource(NULL, res) : 0;
  if (!data || !size || w <= 0 || h <= 0) return NULL;

  IWICImagingFactory *wic = NULL;
  IWICStream *stream = NULL;
  IWICBitmapDecoder *decoder = NULL;
  IWICBitmapFrameDecode *frame = NULL;
  IWICFormatConverter *converter = NULL;
  IWICBitmapScaler *scaler = NULL;
  uint32_t *px = NULL;
  HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&wic);
  if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(wic, &stream);
  if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(stream, data, size);
  if (SUCCEEDED(hr))
    hr = IWICImagingFactory_CreateDecoderFromStream(wic, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand,
                                                    &decoder);
  if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
  /* Premultiplied before scaling, so the edges blend as they should. */
  if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(wic, &converter);
  if (SUCCEEDED(hr))
    hr = IWICFormatConverter_Initialize(converter, (IWICBitmapSource *)frame, &GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
  if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateBitmapScaler(wic, &scaler);
  if (SUCCEEDED(hr))
    hr = IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)converter, (UINT)w, (UINT)h,
                                     WICBitmapInterpolationModeFant);
  if (SUCCEEDED(hr)) {
    px = malloc((size_t)w * (size_t)h * sizeof *px);
    hr = px ? IWICBitmapScaler_CopyPixels(scaler, NULL, (UINT)w * 4, (UINT)w * (UINT)h * 4, (BYTE *)px) : E_OUTOFMEMORY;
  }
  if (FAILED(hr)) {
    free(px);
    px = NULL;
  }
  if (scaler) IWICBitmapScaler_Release(scaler);
  if (converter) IWICFormatConverter_Release(converter);
  if (frame) IWICBitmapFrameDecode_Release(frame);
  if (decoder) IWICBitmapDecoder_Release(decoder);
  if (stream) IWICStream_Release(stream);
  if (wic) IWICImagingFactory_Release(wic);
  return px;
}

static void update_logo(void) {
  free(g_fr.logo);
  g_fr.logo_dark = theme.dark;
  g_fr.logo = load_logo(g_fr.logo_rc.right - g_fr.logo_rc.left, g_fr.logo_rc.bottom - g_fr.logo_rc.top, theme.dark);
}

/* --- Layout --- */

static void free_fonts(void) {
  HFONT *fonts[] = {&g_fr.font, &g_fr.heading_font, &g_fr.title_font, &g_fr.icon_font, &g_fr.close_font};
  for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++) {
    if (*fonts[i]) DeleteObject(*fonts[i]);
    *fonts[i] = NULL;
  }
}

static HFONT icon_font(int px) {
  return CreateFontW(-scale(px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

static void update_fonts(void) {
  free_fonts();
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_fr.dpi);
  g_fr.font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfWeight = FW_SEMIBOLD;
  g_fr.title_font = CreateFontIndirectW(&lf);
  lf.lfHeight = -scale(20); /* Windows 11's subtitle */
  g_fr.heading_font = CreateFontIndirectW(&lf);
  g_fr.icon_font = icon_font(16);
  g_fr.close_font = icon_font(10);
}

static int text_width(HFONT font, const wchar_t *s) {
  HDC dc = GetDC(NULL);
  HGDIOBJ old = SelectObject(dc, font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, s, (int)wcslen(s), &size);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  return size.cx;
}

static const UINT k_para = DT_CENTER | DT_WORDBREAK | DT_NOPREFIX;

/* The height s needs in font, w wide, wrapped as k_para wraps it. */
static int text_height(HFONT font, const wchar_t *s, int w) {
  HDC dc = GetDC(NULL);
  HGDIOBJ old = SelectObject(dc, font);
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, s, -1, &rc, k_para | DT_CALCRECT);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  return rc.bottom;
}

/* Works out the window's size and where everything goes. The logo sits a third of the way
 * down, unless the screen is too short for that and what's beneath it. */
static void layout(void) {
  int col = scale(COL_W);
  g_fr.w = scale(WIN_W);
  int x = (g_fr.w - col) / 2;
  int heading_h = text_height(g_fr.heading_font, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_HEADING), col);
  int text_h = text_height(g_fr.font, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_TEXT), col);
  g_fr.line_h = text_height(g_fr.font, L"Ag", col);
  int logo_h = scale(LOGO_H), logo_w = MulDiv(logo_h, 352, 100);
  int cards = (g_fr.suggested ? 1 : 0) + 1; /* room for the chosen card is always kept */
  int below = scale(GAP_LOGO) + heading_h + scale(GAP_HEADING) + text_h + scale(GAP_TEXT) +
              cards * (scale(CARD_H) + scale(GAP_CARD)) + scale(CHOOSE_H) + scale(GAP_START) + scale(START_H) +
              scale(BOTTOM);
  g_fr.h = 3 * (logo_h / 2 + below) / 2;

  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromWindow(g_fr.owner ? g_fr.owner : g_fr.wnd, MONITOR_DEFAULTTONEAREST), &mi)) {
    int room = (int)(mi.rcWork.bottom - mi.rcWork.top) - 2 * scale(EDGE);
    if (g_fr.h > room) g_fr.h = max(room, scale(CLOSE_H) + logo_h + below);
  }
  int top = g_fr.h / 3 - logo_h / 2;
  if (top + logo_h + below > g_fr.h) top = g_fr.h - below - logo_h;
  if (top < scale(CLOSE_H)) top = scale(CLOSE_H);

  SetRect(&g_fr.logo_rc, (g_fr.w - logo_w) / 2, top, (g_fr.w - logo_w) / 2 + logo_w, top + logo_h);
  int y = top + logo_h + scale(GAP_LOGO);
  SetRect(&g_fr.heading_rc, x, y, x + col, y + heading_h);
  y += heading_h + scale(GAP_HEADING);
  SetRect(&g_fr.text_rc, x, y, x + col, y + text_h);
  y += text_h + scale(GAP_TEXT);
  for (int p = PART_SUGGESTED; p <= PART_CHOSEN; p++) {
    SetRectEmpty(&g_fr.rc[p]);
    if (!(p == PART_SUGGESTED ? g_fr.suggested : g_fr.chosen)) continue;
    SetRect(&g_fr.rc[p], x, y, x + col, y + scale(CARD_H));
    y += scale(CARD_H) + scale(GAP_CARD);
  }
  SetRect(&g_fr.rc[PART_CHOOSE], x, y, x + col, y + scale(CHOOSE_H));
  y += scale(CHOOSE_H) + scale(GAP_START);
  int start_w = max(scale(START_W), text_width(g_fr.font, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_START)) + scale(48));
  SetRect(&g_fr.rc[PART_START], (g_fr.w - start_w) / 2, y, (g_fr.w - start_w) / 2 + start_w, y + scale(START_H));
  SetRect(&g_fr.rc[PART_CLOSE], g_fr.w - scale(CLOSE_W), 0, g_fr.w, scale(CLOSE_H));
}

/* Sizes the window for layout(), centred on (cx, cy) and kept on its screen. */
static void place(int cx, int cy) {
  RECT r = {cx - g_fr.w / 2, cy - g_fr.h / 2, cx - g_fr.w / 2 + g_fr.w, cy - g_fr.h / 2 + g_fr.h};
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
    if (r.bottom > mi.rcWork.bottom) OffsetRect(&r, 0, mi.rcWork.bottom - r.bottom);
    if (r.right > mi.rcWork.right) OffsetRect(&r, mi.rcWork.right - r.right, 0);
    if (r.top < mi.rcWork.top) OffsetRect(&r, 0, mi.rcWork.top - r.top);
    if (r.left < mi.rcWork.left) OffsetRect(&r, mi.rcWork.left - r.left, 0);
  }
  SetWindowPos(g_fr.wnd, NULL, r.left, r.top, g_fr.w, g_fr.h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* --- State --- */

static BOOL start_enabled(void) {
  return g_fr.selected != PART_NONE;
}

static BOOL shown(int part) {
  return part >= 0 && part < NUM_PARTS && !IsRectEmpty(&g_fr.rc[part]) && (part != PART_START || start_enabled());
}

static int part_at(POINT pt) {
  for (int p = 0; p < NUM_PARTS; p++)
    if (PtInRect(&g_fr.rc[p], pt)) return p;
  return PART_NONE;
}

static void ui_update(void);

static void select_card(int part) {
  g_fr.selected = part;
  g_fr.focus = part;
  InvalidateRect(g_fr.wnd, NULL, FALSE);
  ui_update();
}

/* Choose a Folder...: the folder picker, then the folder's card, selected. */
static void choose(void) {
  char *dir = pick_folder(g_fr.wnd, mc_wstr(MC_STR_LIBRARY_FOLDER_CHOOSE), g_fr.chosen ? g_fr.chosen : g_fr.music);
  if (!dir) return;
  char *lib = library_in(dir);
  if (!lib) {
    wchar_t text[MAX_PATH + 256];
    mc_wformat(text, MAX_PATH + 256, mc_str(MC_STR_WINDOWS_FIRST_RUN_NOT_EMPTY), MC_S(dir));
    MessageBoxW(g_fr.wnd, text, mc_wstr(MC_STR_LIBRARY_FOLDER), MB_OK | MB_ICONINFORMATION);
    free(dir);
    return;
  }
  free(dir);
  wchar_t *lib_w = wstr_dup_from_utf8(lib);
  if (g_fr.suggested_w && lib_w && !lstrcmpiW(lib_w, g_fr.suggested_w)) { /* the suggestion after all */
    free(lib);
    free(lib_w);
    select_card(PART_SUGGESTED);
    return;
  }
  free(g_fr.chosen);
  free(g_fr.chosen_w);
  g_fr.chosen = lib;
  g_fr.chosen_w = lib_w;
  layout();
  select_card(PART_CHOSEN);
}

static void start(void) {
  const char *dir = g_fr.selected == PART_SUGGESTED ? g_fr.suggested : g_fr.selected == PART_CHOSEN ? g_fr.chosen : NULL;
  if (!dir || !confirm_onedrive(g_fr.wnd, dir)) return;
  g_fr.result = dup(dir);
  g_fr.done = g_fr.result != NULL;
}

static void activate(int part) {
  if (!shown(part)) return;
  if (part == PART_SUGGESTED || part == PART_CHOSEN) select_card(part);
  else if (part == PART_CHOOSE) choose();
  else if (part == PART_START) start();
  else if (part == PART_CLOSE) g_fr.done = TRUE;
}

/* Tab and Shift+Tab: the next or previous part that can take the focus. */
static void move_focus(int step) {
  int p = shown(g_fr.focus) && g_fr.focus != PART_CLOSE ? g_fr.focus : step > 0 ? PART_CLOSE - 1 : 0;
  for (int i = 0; i < PART_CLOSE; i++) {
    p = (p + step + PART_CLOSE) % PART_CLOSE;
    if (shown(p)) break;
  }
  g_fr.focus = p;
  InvalidateRect(g_fr.wnd, NULL, FALSE);
  uia_focus(&g_fr.ui);
}

static void on_key(WPARAM key) {
  g_fr.show_focus = TRUE;
  BOOL card = g_fr.focus == PART_SUGGESTED || g_fr.focus == PART_CHOSEN;
  if (key == VK_TAB) {
    move_focus(GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
  } else if ((key == VK_UP || key == VK_DOWN) && card) { /* the cards are a group of radio buttons */
    int other = g_fr.focus == PART_SUGGESTED ? PART_CHOSEN : PART_SUGGESTED;
    if (shown(other) && (key == VK_DOWN) == (other == PART_CHOSEN)) select_card(other);
  } else if (key == VK_RETURN || key == VK_SPACE) {
    activate(g_fr.focus);
  }
  InvalidateRect(g_fr.wnd, NULL, FALSE);
}

/* --- Painting --- */

static COLORREF ink(void) {
  return theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

static control_state state_of(int part) {
  if (g_fr.pressed == part && g_fr.hot == part) return STATE_PRESSED;
  return g_fr.hot == part ? STATE_HOT : STATE_NORMAL;
}

/* A folder's card: a radio button, then what it is over its path. */
static void paint_card(canvas *c, int part, enum mc_str_id title, const wchar_t *path) {
  RECT r = g_fr.rc[part];
  if (IsRectEmpty(&r)) return;
  int radius = scale(8);
  BOOL selected = g_fr.selected == part;
  /* Windows 11's card: white at 70% over Mica, or white at 5% in dark mode. */
  if (theme.high_contrast) canvas_fill(c, &r, radius, theme.bg, 255);
  else canvas_fill(c, &r, radius, RGB(255, 255, 255), theme.dark ? 13 : 179);
  theme_draw_state(c, &r, radius, state_of(part));
  COLORREF edge = selected ? theme.accent : theme.high_contrast ? theme.text : theme.line;
  canvas_frame(c, &r, radius, selected ? scale(2) : 1, edge, 255);

  int d = scale(20);
  RECT dot = {r.left + scale(16), (r.top + r.bottom - d) / 2, r.left + scale(16) + d, (r.top + r.bottom - d) / 2 + d};
  if (selected) {
    canvas_fill(c, &dot, d / 2, theme.accent, 255);
    RECT inner = dot;
    InflateRect(&inner, -scale(6), -scale(6));
    canvas_fill(c, &inner, (inner.right - inner.left) / 2, theme.on_accent, 255);
  } else {
    canvas_frame(c, &dot, d / 2, 1, theme.text2, 255);
  }

  int text_top = (r.top + r.bottom) / 2 - g_fr.line_h;
  RECT line = {dot.right + scale(12), text_top, r.right - scale(16), text_top + g_fr.line_h};
  canvas_text(c, mc_wstr(title), -1, &line, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, g_fr.title_font, theme.text);
  OffsetRect(&line, 0, g_fr.line_h);
  canvas_text(c, path, -1, &line, DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX, g_fr.font, theme.text2);
}

/* An icon, a gap and a label, centred in r together. */
static void paint_label(canvas *c, const RECT *r, const wchar_t *icon, const wchar_t *label, COLORREF color) {
  int gap = icon ? scale(8) : 0, icon_w = icon ? text_width(g_fr.icon_font, icon) : 0;
  int w = icon_w + gap + text_width(g_fr.font, label);
  RECT part = {(r->left + r->right - w) / 2, r->top, (r->left + r->right - w) / 2 + icon_w, r->bottom};
  if (icon) canvas_text(c, icon, -1, &part, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, g_fr.icon_font, color);
  part.left = part.right + gap;
  part.right = r->right;
  canvas_text(c, label, -1, &part, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, g_fr.font, color);
}

/* Choose a Folder...: a standard button, as wide as the cards. */
static void paint_choose(canvas *c) {
  RECT r = g_fr.rc[PART_CHOOSE];
  int radius = scale(4);
  if (!theme.high_contrast) canvas_fill(c, &r, radius, ink(), theme.dark ? 15 : 6);
  COLORREF fg = theme_draw_state(c, &r, radius, state_of(PART_CHOOSE));
  canvas_frame(c, &r, radius, 1, theme.high_contrast ? theme.text : theme.line, 255);
  paint_label(c, &r, L"\xE838" /* FolderOpen */, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_CHOOSE), fg);
}

/* Start: an accent button, greyed out until a card is selected. */
static void paint_start(canvas *c) {
  RECT r = g_fr.rc[PART_START];
  int radius = scale(4);
  COLORREF fg;
  if (!start_enabled()) {
    if (theme.high_contrast) {
      canvas_frame(c, &r, radius, 1, theme.text3, 255);
      fg = theme.text3;
    } else {
      canvas_fill(c, &r, radius, ink(), theme.dark ? 40 : 55);
      fg = theme.dark ? RGB(135, 135, 135) : RGB(255, 255, 255);
    }
  } else {
    canvas_fill(c, &r, radius, theme.accent, 255);
    control_state st = state_of(PART_START);
    if (st != STATE_NORMAL) canvas_fill(c, &r, radius, theme.bg, st == STATE_PRESSED ? 56 : 28);
    fg = theme.on_accent;
  }
  paint_label(c, &r, NULL, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_START), fg);
}

/* The close button, as Windows draws its own: red while the mouse is on it. */
static void paint_close(canvas *c) {
  RECT r = g_fr.rc[PART_CLOSE];
  control_state st = state_of(PART_CLOSE);
  COLORREF fg = theme.text;
  if (st != STATE_NORMAL && !theme.high_contrast) {
    canvas_fill(c, &r, 0, RGB(196, 43, 28), st == STATE_PRESSED ? 230 : 255);
    fg = RGB(255, 255, 255);
  } else if (st != STATE_NORMAL) {
    fg = theme_draw_state(c, &r, 0, st);
  }
  canvas_text(c, L"\xE8BB" /* ChromeClose */, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
              g_fr.close_font, fg);
}

static void paint(HDC dc) {
  RECT all = {0, 0, g_fr.w, g_fr.h};
  if (!canvas_reserve(&g_fr.buf, g_fr.w, g_fr.h)) return;
  canvas *c = &g_fr.buf;
  canvas_clear(c, &all, theme.bg, theme.mica ? 0 : 255);
  if (g_fr.logo_dark != theme.dark) update_logo();
  if (g_fr.logo)
    canvas_draw_image(c, &g_fr.logo_rc, g_fr.logo, g_fr.logo_rc.right - g_fr.logo_rc.left,
                      g_fr.logo_rc.bottom - g_fr.logo_rc.top);
  canvas_text(c, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_HEADING), -1, &g_fr.heading_rc, k_para, g_fr.heading_font, theme.text);
  canvas_text(c, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_TEXT), -1, &g_fr.text_rc, k_para, g_fr.font, theme.text2);
  paint_card(c, PART_SUGGESTED, MC_STR_WINDOWS_FIRST_RUN_SUGGESTED, g_fr.suggested_w);
  paint_card(c, PART_CHOSEN, MC_STR_WINDOWS_FIRST_RUN_CHOSEN, g_fr.chosen_w);
  paint_choose(c);
  paint_start(c);
  paint_close(c);
  if (g_fr.show_focus && GetFocus() == g_fr.wnd && shown(g_fr.focus) && g_fr.focus != PART_CLOSE) {
    RECT ring = g_fr.rc[g_fr.focus];
    int radius = g_fr.focus == PART_SUGGESTED || g_fr.focus == PART_CHOSEN ? scale(8) : scale(4);
    InflateRect(&ring, scale(3), scale(3));
    canvas_frame(c, &ring, radius + scale(3), scale(2), theme.text, 255);
  }
  canvas_blit(c, dc, 0, 0, &all);
}

/* Dark or light to match, Mica behind everything (the frame fills the window), and Windows
 * 11's rounded corners. */
static void apply_frame(void) {
  theme_apply_window(g_fr.wnd, TRUE);
  MARGINS m = {theme.mica ? -1 : 0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(g_fr.wnd, &m);
  int corners = CORNERS_ROUND;
  DwmSetWindowAttribute(g_fr.wnd, ATTR_CORNERS, &corners, sizeof corners);
}

/* --- UI Automation: the text, then the parts shown, in order --- */

static int ui_parts(int *parts) {
  int n = 0;
  for (int p = 0; p < NUM_PARTS; p++)
    if (!IsRectEmpty(&g_fr.rc[p])) parts[n++] = p;
  return n;
}

static int ui_node_of(int part) {
  int parts[NUM_PARTS], n = ui_parts(parts);
  for (int i = 0; i < n; i++)
    if (parts[i] == part) return i + 1;
  return UIA_ROOT;
}

static int ui_part_of(int i) {
  int parts[NUM_PARTS], n = ui_parts(parts);
  return i >= 1 && i <= n ? parts[i - 1] : PART_NONE;
}

static BOOL ui_node(int i, uia_node *n) {
  int parts[NUM_PARTS], count = ui_parts(parts);
  n->parent = UIA_ROOT;
  if (i == UIA_ROOT) {
    n->type = UIA_PaneControlTypeId;
    n->flags = UIA_SELECTION;
    lstrcpynW(n->name, mc_wstr(MC_STR_WINDOWS_WELCOME_TITLE), 512);
    n->count = 1 + count;
    return TRUE;
  }
  if (i == 0) {
    n->type = UIA_TextControlTypeId;
    UnionRect(&n->rect, &g_fr.heading_rc, &g_fr.text_rc);
    uia_name(n, mc_wstr(MC_STR_WINDOWS_FIRST_RUN_HEADING), mc_wstr(MC_STR_WINDOWS_FIRST_RUN_TEXT), NULL);
    return TRUE;
  }
  int p = ui_part_of(i);
  if (p == PART_NONE) return FALSE;
  n->rect = g_fr.rc[p];
  if (p == PART_SUGGESTED || p == PART_CHOSEN) {
    n->type = UIA_RadioButtonControlTypeId;
    n->flags = UIA_FOCUSABLE | UIA_SELECTABLE | (g_fr.selected == p ? UIA_SELECTED : 0);
    n->automation_id = p == PART_SUGGESTED ? L"Suggested" : L"Chosen";
    uia_name(n, mc_wstr(p == PART_SUGGESTED ? MC_STR_WINDOWS_FIRST_RUN_SUGGESTED : MC_STR_WINDOWS_FIRST_RUN_CHOSEN),
             p == PART_SUGGESTED ? g_fr.suggested_w : g_fr.chosen_w, NULL);
    return TRUE;
  }
  n->type = UIA_ButtonControlTypeId;
  n->flags = UIA_INVOKABLE | (p == PART_CLOSE ? 0 : UIA_FOCUSABLE) | (p == PART_START && !start_enabled() ? UIA_DISABLED : 0);
  n->automation_id = p == PART_CHOOSE ? L"Choose" : p == PART_START ? L"Start" : L"Close";
  lstrcpynW(n->name, mc_wstr(p == PART_CHOOSE  ? MC_STR_WINDOWS_FIRST_RUN_CHOOSE
                             : p == PART_START ? MC_STR_WINDOWS_FIRST_RUN_START
                                               : MC_STR_CLOSE), 512);
  return TRUE;
}

static int ui_focus(void) {
  return shown(g_fr.focus) ? ui_node_of(g_fr.focus) : UIA_ROOT;
}

static int ui_at(POINT pt) {
  int p = part_at(pt);
  if (p != PART_NONE) return ui_node_of(p);
  return PtInRect(&g_fr.heading_rc, pt) || PtInRect(&g_fr.text_rc, pt) ? 0 : UIA_ROOT;
}

static int ui_selection(int i) {
  return i == UIA_ROOT && g_fr.selected != PART_NONE ? ui_node_of(g_fr.selected) : UIA_NONE;
}

static void ui_select(int i) {
  int p = ui_part_of(i);
  if (p == PART_SUGGESTED || p == PART_CHOSEN) select_card(p);
}

static void ui_invoke(int i) {
  activate(ui_part_of(i));
}

static void ui_set_focus(int i) {
  int p = ui_part_of(i);
  if (p == PART_NONE || p == PART_CLOSE) return;
  g_fr.focus = p;
  InvalidateRect(g_fr.wnd, NULL, FALSE);
}

static const uia_ops k_ui_ops = {
  .node = ui_node,
  .focus = ui_focus,
  .at = ui_at,
  .selection = ui_selection,
  .select = ui_select,
  .invoke = ui_invoke,
  .set_focus = ui_set_focus,
};

/* After the selection or the cards changed. */
static void ui_update(void) {
  int parts[NUM_PARTS], n = ui_parts(parts);
  uia_changed(&g_fr.ui, uia_hash(0, parts, (size_t)n * sizeof parts[0]));
  if (g_fr.selected != PART_NONE) uia_selected(&g_fr.ui, ui_node_of(g_fr.selected));
  uia_focus(&g_fr.ui);
}

/* --- The window --- */

static void set_hot(int part) {
  if (part != PART_NONE && !shown(part)) part = PART_NONE; /* a greyed-out Start doesn't light up */
  if (part == g_fr.hot) return;
  g_fr.hot = part;
  InvalidateRect(g_fr.wnd, NULL, FALSE);
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  LRESULT result;
  if (g_fr.wnd && uia_message(&g_fr.ui, msg, wp, lp, &result)) return result;
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wp) return 0; /* no title bar or borders: the client area is the whole window */
      break;
    case WM_NCHITTEST: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      ScreenToClient(wnd, &pt);
      return part_at(pt) == PART_NONE ? HTCAPTION : HTCLIENT; /* it drags by anything else */
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(dc);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_MOUSEMOVE: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (!g_fr.tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        g_fr.tracking = TrackMouseEvent(&tme);
      }
      set_hot(part_at(pt));
      return 0;
    }
    case WM_MOUSELEAVE:
      g_fr.tracking = FALSE;
      set_hot(PART_NONE);
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int p = part_at(pt);
      if (!shown(p)) return 0;
      g_fr.pressed = p;
      if (p != PART_CLOSE) g_fr.focus = p;
      SetCapture(wnd);
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int p = g_fr.pressed;
      if (p == PART_NONE) return 0;
      ReleaseCapture(); /* WM_CAPTURECHANGED clears pressed */
      if (part_at(pt) == p) activate(p);
      return 0;
    }
    case WM_CAPTURECHANGED:
      g_fr.pressed = PART_NONE;
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_KEYDOWN:
      on_key(wp);
      return 0;
    case WM_SYSKEYDOWN:
      if (wp == VK_F4) { /* there's no system menu to do it */
        g_fr.done = TRUE;
        return 0;
      }
      break;
    case WM_GETDLGCODE:
      return DLGC_WANTALLKEYS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      InvalidateRect(wnd, NULL, FALSE); /* the focus ring */
      return 0;
    case WM_CLOSE:
      g_fr.done = TRUE;
      return 0;
    case WM_DPICHANGED: {
      g_fr.dpi = HIWORD(wp);
      update_fonts();
      layout();
      update_logo();
      const RECT *r = (const RECT *)lp;
      place((r->left + r->right) / 2, (r->top + r->bottom) / 2);
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
      if (theme_is_change(msg, wp, lp)) {
        theme_update(); /* the main window does it too; once more does no harm */
        apply_frame();
        InvalidateRect(wnd, NULL, FALSE);
      }
      break;
    case WM_DESTROY:
      uia_destroy(&g_fr.ui);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static void free_state(void) {
  free_fonts();
  canvas_free(&g_fr.buf);
  free(g_fr.logo);
  free(g_fr.music);
  free(g_fr.suggested);
  free(g_fr.chosen);
  free(g_fr.suggested_w);
  free(g_fr.chosen_w);
}

char *first_run_choose(HWND owner) {
  memset(&g_fr, 0, sizeof g_fr);
  g_fr.owner = owner;
  g_fr.selected = g_fr.hot = g_fr.pressed = PART_NONE;
  g_fr.music = music_folder();
  if (g_fr.music) {
    g_fr.suggested = join(g_fr.music, mc_str(MC_STR_WINDOWS_FIRST_RUN_FOLDER_NAME));
    g_fr.suggested_w = wstr_dup_from_utf8(g_fr.suggested);
  }
  g_fr.focus = g_fr.suggested ? PART_SUGGESTED : PART_CHOOSE;

  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .lpfnWndProc = wnd_proc,
    .hInstance = GetModuleHandleW(NULL),
    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
    .lpszClassName = L"kithsong_first_run",
  };
  RegisterClassExW(&wc);
  /* A caption, for Windows 11's shadow and rounded corners, but no system menu, so Windows
   * draws no caption buttons in the extended frame. */
  g_fr.wnd = CreateWindowExW(0, wc.lpszClassName, mc_wstr(MC_STR_WINDOWS_WELCOME_TITLE), WS_POPUP | WS_CAPTION, 0, 0,
                             0, 0, owner, NULL, wc.hInstance, NULL);
  if (!g_fr.wnd) {
    free_state();
    return NULL;
  }
  g_fr.dpi = GetDpiForWindow(g_fr.wnd);
  uia_init(&g_fr.ui, g_fr.wnd, &k_ui_ops);
  apply_frame();
  update_fonts();
  layout();
  update_logo();
  RECT o;
  if (owner && GetWindowRect(owner, &o)) {
    place((o.left + o.right) / 2, (o.top + o.bottom) / 2);
  } else {
    MONITORINFO mi = {.cbSize = sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(g_fr.wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    place((mi.rcWork.left + mi.rcWork.right) / 2, (mi.rcWork.top + mi.rcWork.bottom) / 2);
  }
  /* Recalculates the frame, which drops the title bar (WM_NCCALCSIZE). */
  SetWindowPos(g_fr.wnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

  /* Modal: the main window takes no input until this closes. */
  if (owner) EnableWindow(owner, FALSE);
  ShowWindow(g_fr.wnd, SW_SHOW);
  SetForegroundWindow(g_fr.wnd);
  SetFocus(g_fr.wnd);
  MSG msg = {0};
  BOOL quit = FALSE;
  while (!g_fr.done) {
    int got = GetMessageW(&msg, NULL, 0, 0);
    if (got <= 0) {
      quit = got == 0;
      break;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (owner) EnableWindow(owner, TRUE); /* first, so the main window is activated next */
  DestroyWindow(g_fr.wnd);
  if (quit) PostQuitMessage((int)msg.wParam);

  char *result = g_fr.result;
  free_state();
  memset(&g_fr, 0, sizeof g_fr);
  return result;
}
