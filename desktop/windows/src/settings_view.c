#include "settings_view.h"

#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "canvas.h"
#include "playback.h"
#include "scrobble_service.h"
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
  ID_SET_LFM_HEADING,
  ID_SET_LFM_STATUS,
  ID_SET_LFM_BUTTON,
  ID_SET_LFM_CHECK,
  ID_SET_LFM_CHECK_LABEL,
  ID_SET_LFM_NOTE,
  ID_SET_LFM_SERVICE,
};

enum { MAX_ADDRESSES = 8 };

/* Signing in: the auth thread polls mc_scrobbler_auth_finish() every AUTH_POLL_MS until the
 * user has approved the token in the browser, for up to AUTH_LIMIT_MS (tokens last an hour,
 * but nobody waits that long). The status line is refreshed every STATUS_MS while shown,
 * since an account arriving by sync, or plays being sent, changes it. */
enum { AUTH_POLL_MS = 3000, AUTH_LIMIT_MS = 10 * 60 * 1000, STATUS_MS = 2000, STATUS_TIMER = 1 };
enum { WM_AUTH_URL = WM_APP + 1, WM_AUTH_DONE }; /* to the status label, from the auth thread */

static struct {
  HWND parent;
  HWND title, heading, path, move, open, note;
  HWND sync_heading, name_label, name, addr_label, addr, leave, sync_note;
  HWND lfm_heading, lfm_status, lfm_button, lfm_service, lfm_check, lfm_check_label, lfm_note;
  HFONT font, title_font, heading_font;
  BOOL shown;
  mc_settings *settings;
  void (*devices_changed)(void);
  char shown_name[MC_SYNC_DEVICE_NAME_MAX + 1]; /* what the name box was last set to */

  /* Signing in to Last.fm (or the service chosen instead). */
  HANDLE auth_thread, auth_cancel; /* while signing in; auth_cancel is an event */
  mc_status auth_result;           /* of the last sign-in, shown until the next action */
  BOOL auth_timed_out;             /* it wasn't approved in time */
  wchar_t lfm_shown[512];          /* the status line as last set */
  wchar_t lfm_button_shown[128];   /* the button's text */
  wchar_t lfm_note_shown[512];     /* and the note's, which names the service */
} g_set;

static void update_scrobble(void);
static void stop_auth(void);
static LRESULT CALLBACK lfm_status_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

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
  g_set.move = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LIBRARY_MOVE), BS_PUSHBUTTON | WS_TABSTOP, ID_SET_MOVE);
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
  g_set.leave = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LEAVE_GROUP_BUTTON),
                      BS_PUSHBUTTON | WS_TABSTOP | WS_DISABLED, ID_SET_LEAVE);
  g_set.sync_note = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SYNC_SETTINGS_NOTE), label, ID_SET_SYNC_NOTE);

  g_set.lfm_heading = child(WC_STATICW, mc_wstr(MC_STR_SCROBBLING), label, ID_SET_LFM_HEADING);
  g_set.lfm_status = child(WC_STATICW, NULL, label, ID_SET_LFM_STATUS);
  SetWindowSubclass(g_set.lfm_status, lfm_status_proc, 0, 0); /* the auth thread's news, the timer */
  g_set.lfm_button = child(WC_BUTTONW, NULL, BS_PUSHBUTTON | WS_TABSTOP, ID_SET_LFM_BUTTON); /* update_scrobble() */
  g_set.lfm_service = child(WC_BUTTONW, mc_wstr(MC_STR_SCROBBLE_CHANGE_SERVICE), BS_PUSHBUTTON | WS_TABSTOP,
                            ID_SET_LFM_SERVICE);
  /* No text of its own: a themed checkbox's text stays black in dark mode, so the label
   * beside it says it, and toggles it too (as on the device page). */
  g_set.lfm_check = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_SET_LFM_CHECK);
  g_set.lfm_check_label = child(WC_STATICW, mc_wstr(MC_STR_SCROBBLE_SWITCH), label | SS_NOTIFY, ID_SET_LFM_CHECK_LABEL);
  g_set.lfm_note = child(WC_STATICW, NULL, label, ID_SET_LFM_NOTE); /* names the service: update_scrobble() */
  playback_on_scrobbler_closing(stop_auth);
}

void settings_view_destroy(void) {
  stop_auth();
  playback_on_scrobbler_closing(NULL);
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
  SendMessageW(g_set.lfm_heading, WM_SETFONT, (WPARAM)g_set.heading_font, TRUE);
  HWND body[] = {g_set.path,       g_set.move,       g_set.open,       g_set.note,
                 g_set.name_label, g_set.name,       g_set.addr_label, g_set.addr,
                 g_set.leave,      g_set.sync_note,  g_set.lfm_status, g_set.lfm_button,
                 g_set.lfm_service, g_set.lfm_check, g_set.lfm_check_label, g_set.lfm_note};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
}

void settings_view_show(BOOL show) {
  g_set.shown = show;
  HWND all[] = {g_set.title,      g_set.heading,      g_set.path,  g_set.move,       g_set.open,
                g_set.note,       g_set.sync_heading, g_set.name_label, g_set.name,  g_set.addr_label,
                g_set.addr,       g_set.leave,        g_set.sync_note, g_set.lfm_heading, g_set.lfm_status,
                g_set.lfm_button, g_set.lfm_service,  g_set.lfm_check, g_set.lfm_check_label, g_set.lfm_note};
  for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ShowWindow(all[i], show ? SW_SHOW : SW_HIDE);
  if (show) {
    settings_view_update_sync();
    update_scrobble();
    SetTimer(g_set.lfm_status, STATUS_TIMER, STATUS_MS, NULL);
  } else {
    KillTimer(g_set.lfm_status, STATUS_TIMER);
  }
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

  int th = canvas_text_height(g_set.title, g_set.title_font, text_w, 0);
  MoveWindow(g_set.title, left, top, text_w, th, TRUE);
  top += th + S(24);

  int hh = canvas_text_height(g_set.heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int ph = canvas_text_height(g_set.path, g_set.font, text_w, DT_SINGLELINE);
  MoveWindow(g_set.path, left, top, text_w, ph, TRUE);
  top += ph + S(12);

  int btn_h = S(32), move_w = button_width(g_set.move, S(16)), open_w = button_width(g_set.open, S(16));
  MoveWindow(g_set.move, left, top, move_w, btn_h, TRUE);
  MoveWindow(g_set.open, left + move_w + S(8), top, open_w, btn_h, TRUE);
  top += btn_h + S(12);

  int nh = canvas_text_height(g_set.note, g_set.font, text_w, 0);
  MoveWindow(g_set.note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Sync */
  hh = canvas_text_height(g_set.sync_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.sync_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int lh = canvas_text_height(g_set.name_label, g_set.font, text_w, 0);
  MoveWindow(g_set.name_label, left, top, text_w, lh, TRUE);
  top += lh + S(4);
  MoveWindow(g_set.name, left, top, min(text_w, S(320)), S(28), TRUE);
  top += S(28) + S(12);

  lh = canvas_text_height(g_set.addr_label, g_set.font, text_w, 0);
  MoveWindow(g_set.addr_label, left, top, text_w, lh, TRUE);
  top += lh + S(4);
  int ah = canvas_text_height(g_set.addr, g_set.font, text_w, DT_EDITCONTROL);
  MoveWindow(g_set.addr, left, top, text_w, ah, TRUE);
  top += ah + S(12);

  MoveWindow(g_set.leave, left, top, button_width(g_set.leave, S(16)), btn_h, TRUE);
  top += btn_h + S(12);

  nh = canvas_text_height(g_set.sync_note, g_set.font, text_w, 0);
  MoveWindow(g_set.sync_note, left, top, text_w, nh, TRUE);
  top += nh + S(32);

  /* Scrobbling */
  hh = canvas_text_height(g_set.lfm_heading, g_set.heading_font, text_w, 0);
  MoveWindow(g_set.lfm_heading, left, top, text_w, hh, TRUE);
  top += hh + S(8);

  int sh = canvas_text_height(g_set.lfm_status, g_set.font, text_w, 0);
  MoveWindow(g_set.lfm_status, left, top, text_w, sh, TRUE);
  top += sh + S(12);

  int sign_in_w = button_width(g_set.lfm_button, S(16));
  MoveWindow(g_set.lfm_button, left, top, sign_in_w, btn_h, TRUE);
  MoveWindow(g_set.lfm_service, left + sign_in_w + S(8), top, button_width(g_set.lfm_service, S(16)), btn_h, TRUE);
  top += btn_h + S(12);

  int box = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi);
  lh = canvas_text_height(g_set.lfm_check_label, g_set.font, text_w - box - S(8), 0);
  int row_h = max(box, lh);
  MoveWindow(g_set.lfm_check, left, top + (row_h - box) / 2, box, box, TRUE);
  MoveWindow(g_set.lfm_check_label, left + box + S(8), top + (row_h - lh) / 2, text_w - box - S(8), lh, TRUE);
  top += row_h + S(12);

  nh = canvas_text_height(g_set.lfm_note, g_set.font, text_w, 0);
  MoveWindow(g_set.lfm_note, left, top, text_w, nh, TRUE);
#undef S
}

/* --- Sync --- */

void settings_view_init_sync(mc_settings *settings, void (*devices_changed)(void)) {
  g_set.settings = settings;
  g_set.devices_changed = devices_changed;
  BOOL on = !settings || mc_settings_scrobble(settings);
  playback_set_scrobble(on);
  SendMessageW(g_set.lfm_check, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
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
    mc_wformat(out, cap, mc_str(MC_STR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
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
      MessageBoxW(g_set.parent, mc_wstr(MC_STR_NAME_SAVE_FAILED), mc_wstr(MC_STR_SETTINGS),
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
    mc_wformat(text, 256, mc_str(MC_STR_LEAVE_GROUP_FAILED), MC_S(mc_status_str(st)));
    MessageBoxW(g_set.parent, text, mc_wstr(MC_STR_LEAVE_GROUP), MB_OK | MB_ICONERROR);
  }
  if (g_set.devices_changed) g_set.devices_changed();
  settings_view_update_sync();
}

/* --- Scrobbling --- */

/* Brings the Scrobbling section up to date with the scrobbler: the service, signed in or
 * not, whether the build can sign in at all, plays waiting and the last error. */
static void update_scrobble(void) {
  if (!g_set.lfm_status) return;
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st = {0};
  BOOL have = sc && mc_scrobbler_status_get(sc, &st) == MC_OK;
  BOOL waiting = g_set.auth_thread != NULL;
  const char *service = have && st.service_name ? st.service_name : mc_str(MC_STR_SCROBBLE_SERVICE);
  wchar_t text[512], line[256], button[128], note[512];
  mc_wformat(text, 511, mc_str(MC_STR_WINDOWS_SCROBBLE_SERVICE_CURRENT), MC_S(service));
  wcscat(text, L"\n"); /* room was left for it */
  size_t len = wcslen(text);
  wstr_mnemonic_format(button, 128, MC_STR_SCROBBLE_SIGN_IN, service);
  BOOL enabled = have;

  if (waiting) {
    mc_wformat(text + len, 512 - len, mc_str(MC_STR_SCROBBLE_WAITING), MC_S(service));
    wcscpy(button, mc_wstr_mnemonic(MC_STR_SCROBBLE_CANCEL_SIGN_IN));
    enabled = TRUE;
  } else if (have && st.username) {
    char *name = st.username;
    mc_wformat(text + len, 512 - len, mc_str(MC_STR_SCROBBLE_SIGNED_IN), MC_S(name));
    wcscpy(button, mc_wstr_mnemonic(MC_STR_SCROBBLE_SIGN_OUT));
    line[0] = L'\0';
    if (st.last_error == MC_ERR_AUTH) mc_wformat(line, 256, mc_str(MC_STR_SCROBBLE_REFUSED), MC_S(service));
    else if (st.last_error == MC_ERR_NET) mc_wformat(line, 256, mc_str(MC_STR_SCROBBLE_OFFLINE), MC_S(service));
    else if (st.pending > 0) mc_wformat(line, 256, mc_str_plural(MC_STR_SCROBBLE_PENDING, st.pending), MC_D(st.pending));
    if (line[0]) {
      wcsncat(text, L"\n", 511 - wcslen(text));
      wcsncat(text, line, 511 - wcslen(text));
    }
  } else if (have && !st.has_key) {
    wcsncat(text, mc_wstr(MC_STR_SCROBBLE_NO_KEY), 511 - len);
    enabled = FALSE;
  } else {
    wcsncat(text, mc_wstr(MC_STR_SCROBBLE_SIGNED_OUT), 511 - len);
  }
  if (!waiting && g_set.auth_result != MC_OK) {
    if (g_set.auth_timed_out)
      wcscpy(line, mc_wstr(MC_STR_SCROBBLE_SIGN_IN_TIMED_OUT));
    else
      mc_wformat(line, 256, mc_str(MC_STR_SCROBBLE_SIGN_IN_FAILED), MC_S(mc_status_str(g_set.auth_result)));
    wcsncat(text, L"\n", 511 - wcslen(text));
    wcsncat(text, line, 511 - wcslen(text));
  }
  mc_wformat(note, 512, mc_str(MC_STR_SCROBBLE_NOTE), MC_S(service));
  if (have) mc_scrobbler_status_clear(&st); /* not before: service may point into it */

  EnableWindow(g_set.lfm_button, enabled);
  EnableWindow(g_set.lfm_service, sc != NULL);
  BOOL relayout = FALSE;
  if (wcscmp(button, g_set.lfm_button_shown)) {
    wcscpy(g_set.lfm_button_shown, button);
    SetWindowTextW(g_set.lfm_button, button);
    relayout = TRUE; /* its width */
  }
  if (wcscmp(note, g_set.lfm_note_shown)) {
    wcscpy(g_set.lfm_note_shown, note);
    SetWindowTextW(g_set.lfm_note, note);
    relayout = TRUE; /* it may take more lines */
  }
  if (wcscmp(text, g_set.lfm_shown)) {
    wcscpy(g_set.lfm_shown, text);
    SetWindowTextW(g_set.lfm_status, text);
    relayout = TRUE; /* it may take more lines */
  }
  if (relayout && g_set.shown) SendMessageW(g_set.parent, WM_SIZE, 0, 0);
}

typedef struct auth_job {
  mc_scrobbler *sc;
  HWND notify;
  HANDLE cancel;
} auth_job;

/* The auth thread: gets a token and the page to approve it on (posted as WM_AUTH_URL, the
 * URL to free with mc_free()), then polls until it's approved, cancelled or too late; posts
 * WM_AUTH_DONE with the result. */
static DWORD WINAPI auth_main(void *arg) {
  auth_job *job = arg;
  char *url = NULL;
  mc_status st = mc_scrobbler_auth_begin(job->sc, &url);
  BOOL polled = FALSE;
  if (st == MC_OK) {
    polled = TRUE;
    if (!PostMessageW(job->notify, WM_AUTH_URL, 0, (LPARAM)url)) mc_free(url);
    ULONGLONG end = GetTickCount64() + AUTH_LIMIT_MS;
    do {
      if (WaitForSingleObject(job->cancel, AUTH_POLL_MS) == WAIT_OBJECT_0) break; /* stop_auth() */
      st = mc_scrobbler_auth_finish(job->sc);
    } while (st == MC_ERR_AUTH && GetTickCount64() < end);
  }
  PostMessageW(job->notify, WM_AUTH_DONE, (WPARAM)(INT_PTR)st, polled && st == MC_ERR_AUTH);
  free(job);
  return 0;
}

static void start_auth(void) {
  mc_scrobbler *sc = playback_scrobbler();
  if (!sc || g_set.auth_thread) return;
  auth_job *job = malloc(sizeof *job);
  HANDLE cancel = CreateEventW(NULL, TRUE, FALSE, NULL);
  if (!job || !cancel) {
    free(job);
    if (cancel) CloseHandle(cancel);
    return;
  }
  *job = (auth_job){sc, g_set.lfm_status, cancel};
  g_set.auth_result = MC_OK;
  g_set.auth_timed_out = FALSE;
  g_set.auth_cancel = cancel;
  g_set.auth_thread = CreateThread(NULL, 0, auth_main, job, 0, NULL);
  if (!g_set.auth_thread) {
    free(job);
    CloseHandle(cancel);
    g_set.auth_cancel = NULL;
  }
  update_scrobble();
}

/* Cancels a sign-in in progress and waits for its thread, which may be in the middle of a
 * call to the service (cut short by mc_scrobbler_auth_cancel()). Also called just before
 * the scrobbler is freed. */
static void stop_auth(void) {
  if (!g_set.auth_thread) return;
  SetEvent(g_set.auth_cancel);
  mc_scrobbler_auth_cancel(playback_scrobbler());
  WaitForSingleObject(g_set.auth_thread, INFINITE);
  CloseHandle(g_set.auth_thread);
  CloseHandle(g_set.auth_cancel);
  g_set.auth_thread = g_set.auth_cancel = NULL;
  /* Its WM_AUTH_URL may still be queued: drop it, and the URL with it. */
  MSG msg;
  while (PeekMessageW(&msg, g_set.lfm_status, WM_AUTH_URL, WM_AUTH_DONE, PM_REMOVE))
    if (msg.message == WM_AUTH_URL) mc_free((void *)msg.lParam);
  update_scrobble();
}

static void sign_out(const char *service) {
  mc_scrobbler *sc = playback_scrobbler();
  if (!sc) return;
  wchar_t title[256];
  mc_wformat(title, 256, mc_str(MC_STR_SCROBBLE_SIGN_OUT_TITLE), MC_S(service));
  if (MessageBoxW(g_set.parent, mc_wstr(MC_STR_SCROBBLE_SIGN_OUT_TEXT), title,
                  MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
    return;
  g_set.auth_result = mc_scrobbler_sign_out(sc);
  g_set.auth_timed_out = FALSE;
  update_scrobble();
}

/* Use a Different Service: the dialog, then the service it chose, which signs out if it's
 * another one. A sign-in under way is given up first, since it's for the old service. */
static void change_service(void) {
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st;
  if (!sc || mc_scrobbler_status_get(sc, &st) != MC_OK) return;
  scrobble_service_choice c;
  BOOL chosen = scrobble_service_show(g_set.parent, st.service, st.api_url, st.auth_url, &c);
  mc_scrobbler_status_clear(&st);
  if (!chosen) return;
  stop_auth();
  sc = playback_scrobbler(); /* the library may have changed while the dialog was open */
  mc_status r = sc ? mc_scrobbler_set_service(sc, c.api_url, c.auth_url, c.api_key, c.api_secret) : MC_OK;
  scrobble_service_choice_clear(&c);
  g_set.auth_result = MC_OK;
  g_set.auth_timed_out = FALSE;
  if (r != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_SCROBBLE_SERVICE_FAILED), MC_S(mc_status_str(r)));
    MessageBoxW(g_set.parent, text, mc_wstr(MC_STR_SCROBBLE_SERVICE_TITLE), MB_OK | MB_ICONERROR);
  }
  update_scrobble();
}

static void on_scrobble_button(void) {
  mc_scrobbler *sc = playback_scrobbler();
  mc_scrobbler_status st;
  if (g_set.auth_thread) {
    stop_auth();
  } else if (sc && mc_scrobbler_status_get(sc, &st) == MC_OK) {
    if (st.username) sign_out(st.service_name ? st.service_name : mc_str(MC_STR_SCROBBLE_SERVICE));
    else start_auth();
    mc_scrobbler_status_clear(&st);
  }
}

static void on_scrobble_check(void) {
  BOOL on = SendMessageW(g_set.lfm_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
  playback_set_scrobble(on);
  if (g_set.settings) {
    mc_settings_set_scrobble(g_set.settings, on);
    mc_settings_save(g_set.settings);
  }
}

static LRESULT CALLBACK lfm_status_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_AUTH_URL: {
      char *url = (char *)lp;
      wchar_t w[2048];
      if (wstr_from_utf8(w, 2048, url)) ShellExecuteW(g_set.parent, L"open", w, NULL, NULL, SW_SHOWNORMAL);
      mc_free(url);
      return 0;
    }
    case WM_AUTH_DONE: {
      mc_status st = (mc_status)(INT_PTR)wp;
      if (g_set.auth_thread) {
        WaitForSingleObject(g_set.auth_thread, INFINITE); /* it's just posted this */
        CloseHandle(g_set.auth_thread);
        CloseHandle(g_set.auth_cancel);
        g_set.auth_thread = g_set.auth_cancel = NULL;
      }
      g_set.auth_result = st;
      g_set.auth_timed_out = lp != 0;
      if (st == MC_OK) SetForegroundWindow(g_set.parent); /* back from the browser */
      update_scrobble();
      return 0;
    }
    case WM_TIMER:
      if (wp == STATUS_TIMER) {
        update_scrobble();
        return 0;
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, lfm_status_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

BOOL settings_view_on_command(int id, int code) {
  switch (id) {
    case ID_SET_OPEN: PostMessageW(g_set.parent, WM_APP_OPEN_LIBRARY, 0, 0); return TRUE;
    case ID_SET_MOVE: PostMessageW(g_set.parent, WM_APP_MOVE_LIBRARY, 0, 0); return TRUE;
    case ID_SET_NAME:
      if (code == EN_KILLFOCUS) apply_name();
      return TRUE;
    case ID_SET_LEAVE: leave_group(); return TRUE;
    case ID_SET_LFM_BUTTON: on_scrobble_button(); return TRUE;
    case ID_SET_LFM_SERVICE: change_service(); return TRUE;
    case ID_SET_LFM_CHECK:
      if (code == BN_CLICKED) on_scrobble_check();
      return TRUE;
    case ID_SET_LFM_CHECK_LABEL:
      if (code == STN_CLICKED) {
        BOOL on = SendMessageW(g_set.lfm_check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(g_set.lfm_check, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        on_scrobble_check();
      }
      return TRUE;
    default: return FALSE;
  }
}

HBRUSH settings_view_ctlcolor(HWND wnd, HDC dc) {
  if (wnd == g_set.lfm_check) { /* WM_CTLCOLORBTN: just its background */
    SetBkColor(dc, theme.bg);
    return theme.bg_brush;
  }
  BOOL note = wnd == g_set.note || wnd == g_set.sync_note || wnd == g_set.lfm_note;
  if (!note && wnd != g_set.title && wnd != g_set.heading && wnd != g_set.path && wnd != g_set.sync_heading &&
      wnd != g_set.name_label && wnd != g_set.addr_label && wnd != g_set.addr && wnd != g_set.lfm_heading &&
      wnd != g_set.lfm_status && wnd != g_set.lfm_check_label)
    return NULL;
  SetTextColor(dc, note ? theme.text2 : theme.text);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
