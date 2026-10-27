#include "settings_view.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "sync_bridge.h"
#include "theme.h"
#include "wstr.h"

enum {
  ID_SET_TITLE = 300,
  ID_SET_HEADING,
  ID_SET_PATH,
  ID_SET_MOVE,
  ID_SET_OPEN,
  ID_SET_NOTE,
  ID_SET_SYNC_HEADING,
  ID_SET_NAME_LABEL,
  ID_SET_NAME,
  ID_SET_ADDR_LABEL,
  ID_SET_ADDR,
  ID_SET_LEAVE,
  ID_SET_SYNC_NOTE,
};

enum { MAX_ADDRESSES = 8 };

static struct {
  HWND parent;
  HWND title, heading, path, move, open, note;
  HWND sync_heading, name_label, name, addr_label, addr, leave, sync_note;
  HFONT font, title_font, heading_font;
  BOOL shown;
  mc_settings *settings;
  void (*devices_changed)(void);
  char shown_name[MC_SYNC_DEVICE_NAME_MAX + 1]; /* what the name box was last set to */
} g_set;

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, g_set.parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

static void apply_name(void);

/* Enter in the name box applies it (a plain window has no default button to take it). */
static LRESULT CALLBACK name_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        apply_name();
        SendMessageW(wnd, EM_SETSEL, 0, -1);
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r') return 0; /* no beep */
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, name_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

void settings_view_create(HWND parent) {
  g_set.parent = parent;
  DWORD label = SS_LEFT | SS_NOPREFIX;
  g_set.title = child(WC_STATICW, mc_wstr(MC_STR_SETTINGS), label, ID_SET_TITLE);
  g_set.heading = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_LOCATION), label, ID_SET_HEADING);
  /* One line: a long path loses folders from its middle, rather than wrapping mid-name. */
  g_set.path = child(WC_STATICW, NULL, label | SS_PATHELLIPSIS, ID_SET_PATH);
  g_set.move = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_MOVE), BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED,
                     ID_SET_MOVE);
  g_set.open = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_OPEN_ANOTHER), BS_PUSHBUTTON | WS_TABSTOP,
                     ID_SET_OPEN);
  g_set.note = child(WC_STATICW, mc_wstr(MC_STR_LIBRARY_LOCATION_TEXT), label, ID_SET_NOTE);

  g_set.sync_heading = child(WC_STATICW, mc_wstr(MC_STR_SYNC), label, ID_SET_SYNC_HEADING);
  g_set.name_label = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_THIS_PC_NAME), label, ID_SET_NAME_LABEL);
  g_set.name = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, NULL, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0,
                               0, parent, (HMENU)(INT_PTR)ID_SET_NAME, GetModuleHandleW(NULL), NULL);
  SendMessageW(g_set.name, EM_LIMITTEXT, MC_SYNC_DEVICE_NAME_MAX, 0);
  SetWindowSubclass(g_set.name, name_proc, 0, 0);
  theme_center_edit(g_set.name);
  g_set.addr_label = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_THIS_PC_ADDRESS), label, ID_SET_ADDR_LABEL);
  g_set.addr = child(WC_STATICW, NULL, label | SS_EDITCONTROL, ID_SET_ADDR);
  g_set.leave = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_WINDOWS_LEAVE_GROUP_BUTTON),
                      BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_SET_LEAVE);
  g_set.sync_note = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SYNC_SETTINGS_NOTE), label, ID_SET_SYNC_NOTE);
}

void settings_view_destroy(void) {
  if (g_set.title_font) DeleteObject(g_set.title_font);
  if (g_set.heading_font) DeleteObject(g_set.heading_font);
  g_set.title_font = g_set.heading_font = NULL;
}

void settings_view_set_library_dir(const char *dir) {
  wchar_t buf[1024];
  if (!dir || !MultiByteToWideChar(CP_UTF8, 0, dir, -1, buf, 1024)) buf[0] = L'\0';
  buf[1023] = L'\0';
  SetWindowTextW(g_set.path, buf);
  SendMessageW(g_set.parent, WM_SIZE, 0, 0); /* the path may be cut differently */
}

void settings_view_set_font(HFONT font, UINT dpi) {
  g_set.font = font;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 20, 9); /* 9pt -> 20pt, Windows 11's title size */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_set.title_font) DeleteObject(g_set.title_font);
  g_set.title_font = CreateFontIndirectW(&lf);
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 14, 9); /* subtitle */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_set.heading_font) DeleteObject(g_set.heading_font);
  g_set.heading_font = CreateFontIndirectW(&lf);

  SendMessageW(g_set.title, WM_SETFONT, (WPARAM)g_set.title_font, TRUE);
  SendMessageW(g_set.heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  SendMessageW(g_set.sync_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  HWND body[] = {g_set.path,  g_set.move,       g_set.open, g_set.note,  g_set.name_label,
                 g_set.name,  g_set.addr_label, g_set.addr, g_set.leave, g_set.sync_note};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
}

void settings_view_show(BOOL show) {
  g_set.shown = show;
  HWND all[] = {g_set.title,      g_set.heading,      g_set.path,  g_set.move,       g_set.open,
                g_set.note,       g_set.sync_heading, g_set.name_label, g_set.name,  g_set.addr_label,
                g_set.addr,       g_set.leave,        g_set.sync_note};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(all[i], show ? SW_SHOW : SW_HIDE);
  if (show) settings_view_update_sync();
}

/* The height wnd's text needs at width w, wrapped as its static control wraps it. */
static int text_height(HWND wnd, HFONT font, int w, UINT format) {
  wchar_t buf[1024];
  int n = GetWindowTextW(wnd, buf, 1024);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, font);
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, n ? buf : L" ", n ? n : 1, &rc, format | DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return rc.bottom;
}

static int button_width(HWND wnd, int pad) {
  wchar_t buf[128];
  int n = GetWindowTextW(wnd, buf, 128);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, g_set.font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return size.cx + 2 * pad;
}

void settings_view_layout(int x, int y, int w, int h, UINT dpi) {
  (void)h;
  if (!g_set.shown || !g_set.font) return;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int pad = S(32), text_w = min(w - 2 * pad, S(720));
  int left = x + pad, top = y + S(24);

  int th = text_height(g_set.title, g_set.title_font, text_w, 0);
  MoveWindow(g_set.title, left, top, text_w, th, TRUE);
  top += th + S(24);

  int hh = text_height(g_set.heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int ph = text_height(g_set.path, g_set.font, text_w, DT_SINGLELINE);
  MoveWindow(g_set.path, left, top, text_w, ph, TRUE);
  top += ph + S(12);

  int btn_h = S(32), move_w = button_width(g_set.move, S(16)), open_w = button_width(g_set.open, S(16));
  MoveWindow(g_set.move, left, top, move_w, btn_h, TRUE);
  MoveWindow(g_set.open, left + move_w + S(8), top, open_w, btn_h, TRUE);
  top += btn_h + S(12);

  int nh = text_height(g_set.note, g_set.font, text_w, 0);
  MoveWindow(g_set.note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Sync */
  hh = text_height(g_set.sync_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.sync_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int lh = text_height(g_set.name_label, g_set.font, text_w, 0);
  MoveWindow(g_set.name_label, left, top, text_w, lh, TRUE);
  top += lh + S(4);
  MoveWindow(g_set.name, left, top, min(text_w, S(320)), S(28), TRUE);
  top += S(28) + S(12);

  lh = text_height(g_set.addr_label, g_set.font, text_w, 0);
  MoveWindow(g_set.addr_label, left, top, text_w, lh, TRUE);
  top += lh + S(4);
  int ah = text_height(g_set.addr, g_set.font, text_w, DT_EDITCONTROL);
  MoveWindow(g_set.addr, left, top, text_w, ah, TRUE);
  top += ah + S(12);

  MoveWindow(g_set.leave, left, top, button_width(g_set.leave, S(16)), btn_h, TRUE);
  top += btn_h + S(12);

  nh = text_height(g_set.sync_note, g_set.font, text_w, 0);
  MoveWindow(g_set.sync_note, left, top, text_w, nh, TRUE);
#undef S
}

/* --- Sync --- */

void settings_view_init_sync(mc_settings *settings, void (*devices_changed)(void)) {
  g_set.settings = settings;
  g_set.devices_changed = devices_changed;
  settings_view_update_sync();
}

/* This PC's name as sync has it, else as saved, else the host name; UTF-8 into out. */
static void current_name(char *out, size_t cap) {
  mc_sync *sync = sync_bridge_get();
  out[0] = '\0';
  if (sync && mc_sync_get_device_name(sync, out, cap) == MC_OK && out[0]) return;
  const char *saved = g_set.settings ? mc_settings_device_name(g_set.settings) : NULL;
  if (saved && *saved) {
    snprintf(out, cap, "%s", saved);
    return;
  }
  wchar_t host[256];
  DWORD n = 256;
  if (GetComputerNameExW(ComputerNameDnsHostname, host, &n) && n)
    WideCharToMultiByte(CP_UTF8, 0, host, -1, out, (int)cap, NULL, NULL);
  out[cap - 1] = '\0';
}

/* "addr:port" for each of this PC's addresses, IPv6 ones in brackets. */
static void format_addresses(wchar_t *out, size_t cap) {
  mc_sync *sync = sync_bridge_get();
  out[0] = L'\0';
  if (!sync) {
    mc_wformat(out, cap, mc_str(MC_STR_WINDOWS_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
    return;
  }
  char addrs[MAX_ADDRESSES][MC_SYNC_ADDR_STRLEN];
  int n = mc_sync_local_addresses(addrs, MAX_ADDRESSES);
  unsigned port = mc_sync_port(sync);
  if (n <= 0) {
    mc_wformat(out, cap, mc_str(MC_STR_WINDOWS_PORT_NO_NETWORK), MC_D(port));
    return;
  }
  size_t len = 0;
  for (int i = 0; i < n && len < cap; i++) {
    BOOL v6 = strchr(addrs[i], ':') != NULL;
    int w = swprintf(out + len, cap - len, L"%ls%hs%hs%hs:%u", i ? mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR) : L"",
                     v6 ? "[" : "", addrs[i],
                     v6 ? "]" : "", port);
    if (w < 0) break;
    len += (size_t)w;
  }
}

/* Whether this PC is in a group: listed devices, its own row included, that aren't removed. */
static BOOL in_group(void) {
  mc_sync *sync = sync_bridge_get();
  mc_sync_device_list *list = NULL;
  if (!sync || mc_sync_list_devices(sync, &list) != MC_OK) return FALSE;
  BOOL member = FALSE;
  for (int i = 0; i < list->count && !member; i++) member = !list->items[i].removed;
  mc_sync_device_list_free(list);
  return member;
}

void settings_view_update_sync(void) {
  if (!g_set.name) return;
  if (GetFocus() != g_set.name) {
    current_name(g_set.shown_name, sizeof g_set.shown_name);
    wchar_t w[MC_SYNC_DEVICE_NAME_MAX + 1];
    if (!MultiByteToWideChar(CP_UTF8, 0, g_set.shown_name, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1)) w[0] = L'\0';
    SetWindowTextW(g_set.name, w);
  }
  wchar_t addr[1024], old[1024];
  format_addresses(addr, 1024);
  GetWindowTextW(g_set.addr, old, 1024);
  if (wcscmp(addr, old)) {
    SetWindowTextW(g_set.addr, addr);
    if (g_set.shown) SendMessageW(g_set.parent, WM_SIZE, 0, 0); /* it may wrap differently */
  }
  EnableWindow(g_set.leave, in_group());
}

/* Trims spaces at either end, in place. */
static char *trim(char *s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = '\0';
  return s;
}

/* Renames this PC to what's in the name box, if it changed. An empty box goes back to the
 * current name. */
static void apply_name(void) {
  wchar_t w[MC_SYNC_DEVICE_NAME_MAX + 1];
  char buf[4 * (MC_SYNC_DEVICE_NAME_MAX + 1)];
  GetWindowTextW(g_set.name, w, MC_SYNC_DEVICE_NAME_MAX + 1);
  if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, (int)sizeof buf, NULL, NULL)) buf[0] = '\0';
  char *name = trim(buf);
  if (!*name || !strcmp(name, g_set.shown_name)) {
    current_name(g_set.shown_name, sizeof g_set.shown_name);
    if (MultiByteToWideChar(CP_UTF8, 0, g_set.shown_name, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1))
      SetWindowTextW(g_set.name, w);
    return;
  }

  mc_sync *sync = sync_bridge_get();
  mc_status st = sync ? mc_sync_set_device_name(sync, name) : MC_OK;
  if (st != MC_OK && st != MC_ERR_UNIMPLEMENTED) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_RENAME_SELF_FAILED), MC_S(mc_status_str(st)));
    MessageBoxW(g_set.parent, text, mc_wstr(MC_STR_SETTINGS), MB_OK | MB_ICONERROR);
    if (MultiByteToWideChar(CP_UTF8, 0, g_set.shown_name, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1))
      SetWindowTextW(g_set.name, w); /* back to the name it has, so it isn't tried again */
  } else if (g_set.settings) {
    /* sync may have cut it to fit; save what it now goes by */
    char saved[MC_SYNC_DEVICE_NAME_MAX + 1];
    if (!sync || mc_sync_get_device_name(sync, saved, sizeof saved) != MC_OK || !saved[0])
      snprintf(saved, sizeof saved, "%s", name);
    mc_settings_set_device_name(g_set.settings, saved);
    if (mc_settings_save(g_set.settings) != MC_OK)
      MessageBoxW(g_set.parent, mc_wstr(MC_STR_WINDOWS_NAME_SAVE_FAILED), mc_wstr(MC_STR_SETTINGS),
                  MB_OK | MB_ICONWARNING);
    snprintf(g_set.shown_name, sizeof g_set.shown_name, "%s", saved);
    if (MultiByteToWideChar(CP_UTF8, 0, saved, -1, w, MC_SYNC_DEVICE_NAME_MAX + 1)) SetWindowTextW(g_set.name, w);
  }
  if (g_set.devices_changed) g_set.devices_changed();
}

static void leave_group(void) {
  mc_sync *sync = sync_bridge_get();
  if (!sync) return;
  if (MessageBoxW(g_set.parent, mc_wstr(MC_STR_WINDOWS_PC_LEAVE_GROUP_CONFIRM), mc_wstr(MC_STR_LEAVE_GROUP),
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    return;
  HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT));
  mc_status st = mc_sync_leave_group(sync); /* blocks for a few seconds at most */
  SetCursor(old);
  if (st == MC_ERR_LOCKED)
    MessageBoxW(g_set.parent, mc_wstr(MC_STR_WINDOWS_LEAVE_ONLY_ARCHIVE), mc_wstr(MC_STR_LEAVE_GROUP),
                MB_OK | MB_ICONINFORMATION);
  else if (st != MC_OK && st != MC_ERR_NOT_FOUND) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_LEAVE_GROUP_FAILED), MC_S(mc_status_str(st)));
    MessageBoxW(g_set.parent, text, mc_wstr(MC_STR_LEAVE_GROUP), MB_OK | MB_ICONERROR);
  }
  if (g_set.devices_changed) g_set.devices_changed();
  settings_view_update_sync();
}

BOOL settings_view_on_command(int id, int code) {
  switch (id) {
    case ID_SET_OPEN: PostMessageW(g_set.parent, WM_APP_OPEN_LIBRARY, 0, 0); return TRUE;
    case ID_SET_MOVE: return TRUE; /* not implemented yet; the button is disabled */
    case ID_SET_NAME:
      if (code == EN_KILLFOCUS) apply_name();
      return TRUE;
    case ID_SET_LEAVE: leave_group(); return TRUE;
    default: return FALSE;
  }
}

HBRUSH settings_view_ctlcolor(HWND wnd, HDC dc) {
  BOOL note = wnd == g_set.note || wnd == g_set.sync_note;
  if (!note && wnd != g_set.title && wnd != g_set.heading && wnd != g_set.path && wnd != g_set.sync_heading &&
      wnd != g_set.name_label && wnd != g_set.addr_label && wnd != g_set.addr)
    return NULL;
  SetTextColor(dc, note ? theme.text2 : theme.text);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
