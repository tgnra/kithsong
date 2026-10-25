#define COBJMACROS
#include "taskbar.h"

#include <shobjidl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "theme.h"

enum { PREVIOUS, PLAY, NEXT, NUM_BUTTONS };

static struct {
  HWND wnd;
  UINT created_msg; /* "TaskbarButtonCreated" */
  ITaskbarList3 *list; /* NULL until the taskbar button exists */
  HICON icons[NUM_BUTTONS + 1]; /* the last one is Pause */
  BOOL has_track, playing, has_previous, has_next;
} s;

/* The taskbar follows "Choose your default Windows mode", which can differ from apps'. */
static BOOL taskbar_is_light(void) {
  DWORD light = 0, size = sizeof light;
  RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
               L"SystemUsesLightTheme", RRF_RT_REG_DWORD, NULL, &light, &size);
  return light != 0;
}

/* A glyph as a size x size icon in color, antialiased into the alpha channel. */
static HICON glyph_icon(wchar_t glyph, int size, COLORREF color) {
  BITMAPINFO bi = {.bmiHeader = {sizeof bi.bmiHeader, size, -size, 1, 32, BI_RGB, 0, 0, 0, 0, 0}};
  void *bits = NULL;
  HDC dc = CreateCompatibleDC(NULL);
  HBITMAP color_bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
  BYTE *zeros = calloc((size_t)(size + 15) / 16 * 2, (size_t)size); /* rows are WORD-aligned */
  HBITMAP mask = zeros ? CreateBitmap(size, size, 1, 1, zeros) : NULL;
  free(zeros);
  HICON icon = NULL;
  if (color_bmp && mask) {
    HGDIOBJ old_bmp = SelectObject(dc, color_bmp);
    HFONT font = CreateFontW(-size, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, theme_icon_face());
    HGDIOBJ old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255)); /* white on black: each pixel's level is its coverage */
    RECT r = {0, 0, size, size};
    DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    SelectObject(dc, old_font);
    DeleteObject(font);
    SelectObject(dc, old_bmp);

    uint32_t *px = bits, rgb = (uint32_t)GetRValue(color) << 16 | (uint32_t)GetGValue(color) << 8 | GetBValue(color);
    for (int i = 0; i < size * size; i++) px[i] = (px[i] >> 8 & 0xff) << 24 | rgb; /* straight alpha */
    ICONINFO ii = {TRUE, 0, 0, mask, color_bmp};
    icon = CreateIconIndirect(&ii);
  }
  if (mask) DeleteObject(mask);
  if (color_bmp) DeleteObject(color_bmp);
  DeleteDC(dc);
  return icon;
}

static void make_icons(void) {
  static const wchar_t glyphs[NUM_BUTTONS + 1] = {0xE892, 0xE768, 0xE893, 0xE769}; /* Previous, Play, Next, Pause */
  int size = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(s.wnd));
  COLORREF color = taskbar_is_light() ? RGB(0, 0, 0) : RGB(255, 255, 255);
  for (int i = 0; i <= NUM_BUTTONS; i++) {
    if (s.icons[i]) DestroyIcon(s.icons[i]);
    s.icons[i] = glyph_icon(glyphs[i], size, color);
  }
}

static void fill_buttons(THUMBBUTTON *b) {
  static const wchar_t *const tips[] = {L"Previous", L"Play", L"Next"};
  BOOL enabled[NUM_BUTTONS] = {s.has_previous, s.has_track, s.has_next};
  for (int i = 0; i < NUM_BUTTONS; i++) {
    memset(&b[i], 0, sizeof b[i]);
    b[i].dwMask = THB_ICON | THB_TOOLTIP | THB_FLAGS;
    b[i].iId = (UINT)(TASKBAR_PREVIOUS + i);
    b[i].hIcon = s.icons[i];
    b[i].dwFlags = enabled[i] ? THBF_ENABLED : THBF_DISABLED;
    wcscpy(b[i].szTip, tips[i]);
  }
  if (s.playing) {
    b[PLAY].hIcon = s.icons[NUM_BUTTONS];
    wcscpy(b[PLAY].szTip, L"Pause");
  }
}

static void update_buttons(void) {
  if (!s.list) return;
  THUMBBUTTON b[NUM_BUTTONS];
  fill_buttons(b);
  ITaskbarList3_ThumbBarUpdateButtons(s.list, s.wnd, NUM_BUTTONS, b);
}

void taskbar_init(HWND wnd) {
  s.wnd = wnd;
  s.created_msg = RegisterWindowMessageW(L"TaskbarButtonCreated");
}

void taskbar_free(void) {
  if (s.list) ITaskbarList3_Release(s.list);
  for (int i = 0; i <= NUM_BUTTONS; i++)
    if (s.icons[i]) DestroyIcon(s.icons[i]);
  memset(&s, 0, sizeof s);
}

BOOL taskbar_message(UINT msg, WPARAM wp, LPARAM lp) {
  if (!s.wnd) return FALSE;
  if (msg == s.created_msg && s.created_msg) { /* also after Explorer restarts */
    if (!s.list && FAILED(CoCreateInstance(&CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER, &IID_ITaskbarList3,
                                           (void **)&s.list)))
      s.list = NULL;
    if (s.list && FAILED(ITaskbarList3_HrInit(s.list))) {
      ITaskbarList3_Release(s.list);
      s.list = NULL;
    }
    if (s.list) {
      make_icons();
      THUMBBUTTON b[NUM_BUTTONS];
      fill_buttons(b);
      ITaskbarList3_ThumbBarAddButtons(s.list, s.wnd, NUM_BUTTONS, b);
    }
    return TRUE;
  }
  /* The taskbar's mode or the DPI changed: redraw the icons, but let the window see it too. */
  if ((msg == WM_SETTINGCHANGE && lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) || msg == WM_DPICHANGED) {
    if (s.list) {
      make_icons();
      update_buttons();
    }
  }
  (void)wp;
  return FALSE;
}

void taskbar_set_state(BOOL has_track, BOOL playing, BOOL has_previous, BOOL has_next) {
  if (has_track == s.has_track && playing == s.playing && has_previous == s.has_previous && has_next == s.has_next)
    return;
  s.has_track = has_track;
  s.playing = playing;
  s.has_previous = has_previous;
  s.has_next = has_next;
  update_buttons();
}
