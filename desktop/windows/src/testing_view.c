#include "testing_view.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "canvas.h"
#include "theme.h"
#include "wstr.h"

enum {
  ID_TEST_CHECK = 380,
  ID_TEST_CHECK_LABEL,
  ID_TEST_NOTE,
  ID_TEST_RUN,
  ID_TEST_COPY,
  ID_TEST_SAVE,
  ID_TEST_CLEAR,
  ID_TEST_REPORT,
  ID_TEST_STRESS_LABEL,
  ID_TEST_PRESET,
  ID_TEST_GENERATE,
  ID_TEST_CANCEL,
  ID_TEST_STATUS,
  ID_TEST_BACK,
  ID_TEST_DELETE,
  ID_TEST_STRESS_NOTE,
};

enum { JOB_POLL_MS = 150, REPORT_MS = 2000, TIMER_JOB = 1, TIMER_REPORT = 2 };

static struct {
  HWND parent;
  HWND check, check_label, note, run, copy, save, clear, report;
  HWND stress_label, preset, generate, cancel, status, back, del, stress_note;
  HFONT font, mono;
  UINT dpi;
  BOOL shown;
  void (*relayout)(void);
  mc_settings *settings;
  char *config_dir;
  testing_host host;
  mc_stress_job *job;
  char *job_dir;
  HANDLE deleting; /* the thread deleting stress libraries, which takes a while for a big one */
  char *delete_open_dir;
  mc_status delete_status;
  int deleted;
  wchar_t *report_shown;
} t;

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, t.parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

static void set_status(const wchar_t *text) {
  SetWindowTextW(t.status, text);
}

/* The report as the edit box wants it: Windows line ends. malloc'd. */
static wchar_t *report_text(void) {
  char *report = mc_perf_report();
  wchar_t *wide = report ? wstr_dup_from_utf8(report) : NULL;
  mc_free(report);
  if (!wide) return NULL;
  size_t lines = 0;
  for (const wchar_t *p = wide; *p; p++) lines += *p == L'\n';
  wchar_t *out = malloc((wcslen(wide) + lines + 1) * sizeof *out), *q = out;
  if (out) {
    for (const wchar_t *p = wide; *p; p++) {
      if (*p == L'\n') *q++ = L'\r';
      *q++ = *p;
    }
    *q = L'\0';
  }
  free(wide);
  return out;
}

/* Shows the report, if it has changed. */
static void update_report(void) {
  if (!t.report) return;
  wchar_t *text = mc_perf_enabled() ? report_text() : NULL;
  const wchar_t *shown = text ? text : mc_wstr(MC_STR_PERF_OFF);
  if (t.report_shown && !wcscmp(t.report_shown, shown)) {
    free(text);
    return;
  }
  SetWindowTextW(t.report, shown);
  free(t.report_shown);
  t.report_shown = text ? text : _wcsdup(shown);
  if (t.relayout) t.relayout(); /* it's as tall as its lines */
}

static BOOL in_stress(void) {
  return mc_library_is_stress(t.host.library ? t.host.library() : NULL);
}

static void update_buttons(void) {
  if (!t.check) return;
  BOOL on = mc_perf_enabled(), busy = t.job != NULL || t.deleting != NULL, stress = in_stress();
  SendMessageW(t.check, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  EnableWindow(t.run, on && !mc_perf_auto_running());
  EnableWindow(t.copy, on);
  EnableWindow(t.save, on);
  EnableWindow(t.clear, on);
  EnableWindow(t.preset, !busy);
  EnableWindow(t.generate, !busy);
  EnableWindow(t.cancel, t.job != NULL);
  EnableWindow(t.back, stress && !busy);
  EnableWindow(t.del, !stress && !busy && t.config_dir && mc_stress_count(t.config_dir) > 0);
}

void testing_view_update(void) {
  update_buttons();
  update_report();
}

static void CALLBACK on_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time);

/* The thread deleting stress libraries ended. */
static void delete_finished(void) {
  WaitForSingleObject(t.deleting, INFINITE);
  CloseHandle(t.deleting);
  t.deleting = NULL;
  free(t.delete_open_dir);
  t.delete_open_dir = NULL;
  KillTimer(t.parent, TIMER_JOB);
  wchar_t text[256];
  if (t.delete_status == MC_OK) mc_wformat(text, 256, mc_str_plural(MC_STR_STRESS_DELETED, t.deleted), MC_D(t.deleted));
  else mc_wformat(text, 256, mc_str(MC_STR_STRESS_FAILED), MC_S(mc_status_str(t.delete_status)));
  set_status(text);
  update_buttons();
}

/* The job ended: the library it made opens. */
static void job_finished(void) {
  mc_status st = MC_OK;
  mc_stress_job_done(t.job, &st);
  mc_stress_job_free(t.job);
  t.job = NULL;
  KillTimer(t.parent, TIMER_JOB);
  char *dir = t.job_dir;
  t.job_dir = NULL;
  if (st == MC_OK) {
    set_status(L"");
    if (t.host.open_stress) t.host.open_stress(dir);
  } else if (st == MC_ERR_NOT_FOUND) { /* cancelled */
    set_status(L"");
  } else {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_STRESS_FAILED), MC_S(mc_status_str(st)));
    set_status(text);
  }
  free(dir);
  update_buttons();
}

static void CALLBACK on_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)wnd;
  (void)msg;
  (void)time;
  if (id == TIMER_REPORT) {
    if (t.shown && mc_perf_enabled() && !mc_perf_auto_running()) update_report();
    return;
  }
  if (t.deleting) {
    if (WaitForSingleObject(t.deleting, 0) == WAIT_OBJECT_0) delete_finished();
    return;
  }
  if (!t.job) return;
  if (mc_stress_job_done(t.job, NULL)) {
    job_finished();
    return;
  }
  wchar_t text[128];
  mc_wformat(text, 128, mc_str(MC_STR_STRESS_GENERATING), MC_D(mc_stress_job_percent(t.job)));
  set_status(text);
}

static void generate(void) {
  if (t.job || !t.config_dir) return;
  int preset = (int)SendMessageW(t.preset, CB_GETCURSEL, 0, 0);
  char *dir = mc_stress_dir(t.config_dir, (mc_stress_preset)preset);
  if (!dir) return;
  if (mc_stress_is_complete(dir)) { /* made before: the same library, so it's used again */
    if (t.host.open_stress) t.host.open_stress(dir);
    mc_free(dir);
    update_buttons();
    return;
  }
  mc_status st = mc_stress_start(dir, (mc_stress_preset)preset, &t.job);
  if (st != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_STRESS_FAILED), MC_S(mc_status_str(st)));
    set_status(text);
    mc_free(dir);
    return;
  }
  t.job_dir = _strdup(dir);
  mc_free(dir);
  SetTimer(t.parent, TIMER_JOB, JOB_POLL_MS, on_timer);
  update_buttons();
}

static DWORD WINAPI delete_main(void *arg) {
  (void)arg;
  t.delete_status = mc_stress_delete_all(t.config_dir, t.delete_open_dir, &t.deleted);
  return 0;
}

static void delete_all(void) {
  if (t.deleting) return;
  if (MessageBoxW(GetAncestor(t.parent, GA_ROOT), mc_wstr(MC_STR_STRESS_DELETE_CONFIRM), mc_wstr(MC_STR_STRESS_HEADING),
                  MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
    return;
  mc_library *lib = t.host.library ? t.host.library() : NULL;
  t.delete_open_dir = lib ? _strdup(mc_library_dir(lib)) : NULL;
  t.deleting = CreateThread(NULL, 0, delete_main, NULL, 0, NULL);
  if (!t.deleting) {
    free(t.delete_open_dir);
    t.delete_open_dir = NULL;
    return;
  }
  set_status(mc_wstr(MC_STR_STRESS_DELETING));
  SetTimer(t.parent, TIMER_JOB, JOB_POLL_MS, on_timer);
  update_buttons();
}

static void copy_report(void) {
  wchar_t *text = report_text();
  if (!text) return;
  size_t bytes = (wcslen(text) + 1) * sizeof *text;
  HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
  void *p = mem ? GlobalLock(mem) : NULL;
  if (p) {
    memcpy(p, text, bytes);
    GlobalUnlock(mem);
    if (OpenClipboard(t.parent)) {
      EmptyClipboard();
      if (SetClipboardData(CF_UNICODETEXT, mem)) mem = NULL; /* the clipboard's now */
      CloseClipboard();
    }
  }
  if (mem) GlobalFree(mem);
  free(text);
}

static void save_report(void) {
  wchar_t path[MAX_PATH] = L"kithsong-profile.txt";
  OPENFILENAMEW ofn = {.lStructSize = sizeof ofn, .hwndOwner = GetAncestor(t.parent, GA_ROOT),
                       .lpstrFilter = L"*.txt\0*.txt\0", .lpstrFile = path, .nMaxFile = MAX_PATH,
                       .lpstrDefExt = L"txt", .Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR | OFN_PATHMUSTEXIST};
  if (!GetSaveFileNameW(&ofn)) return;
  char *utf8 = wstr_to_utf8(path);
  mc_status st = utf8 ? mc_perf_save(utf8) : MC_ERR_NOMEM;
  free(utf8);
  if (st != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_PERF_SAVE_FAILED), MC_S(mc_status_str(st)));
    MessageBoxW(ofn.hwndOwner, text, mc_wstr(MC_STR_TESTING_HEADING), MB_OK | MB_ICONERROR);
  }
}

static void on_check(void) {
  BOOL on = SendMessageW(t.check, BM_GETCHECK, 0, 0) == BST_CHECKED;
  mc_perf_set_enabled(on);
  if (t.settings) {
    mc_settings_set_bool(t.settings, MC_PERF_SETTING, on);
    mc_settings_save(t.settings); /* failing, it's saved with the next setting */
  }
  testing_view_update();
}

void testing_view_create(HWND parent, void (*relayout)(void)) {
  t.parent = parent;
  t.relayout = relayout;
  t.check = child(WC_BUTTONW, L"", WS_TABSTOP | BS_AUTOCHECKBOX, ID_TEST_CHECK);
  t.check_label = child(WC_STATICW, mc_wstr(MC_STR_PERF_ENABLE), SS_NOTIFY, ID_TEST_CHECK_LABEL);
  t.note = child(WC_STATICW, mc_wstr(MC_STR_PERF_ENABLE_TEXT), 0, ID_TEST_NOTE);
  t.run = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_PERF_RUN_SCROLL), WS_TABSTOP, ID_TEST_RUN);
  t.copy = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_PERF_COPY), WS_TABSTOP, ID_TEST_COPY);
  t.save = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_PERF_SAVE), WS_TABSTOP, ID_TEST_SAVE);
  t.clear = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_PERF_CLEAR), WS_TABSTOP, ID_TEST_CLEAR);
  t.stress_label = child(WC_STATICW, mc_wstr(MC_STR_STRESS_HEADING), 0, ID_TEST_STRESS_LABEL);
  t.preset = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, ID_TEST_PRESET);
  for (int i = 0; i < MC_STRESS_PRESETS; i++) {
    wchar_t text[64];
    mc_wformat(text, 64, mc_str(MC_STR_STRESS_PRESET), MC_D(mc_stress_preset_tracks((mc_stress_preset)i)));
    SendMessageW(t.preset, CB_ADDSTRING, 0, (LPARAM)text);
  }
  SendMessageW(t.preset, CB_SETCURSEL, MC_STRESS_10K, 0);
  t.generate = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_STRESS_GENERATE), WS_TABSTOP, ID_TEST_GENERATE);
  t.cancel = child(WC_BUTTONW, mc_wstr(MC_STR_CANCEL), WS_TABSTOP, ID_TEST_CANCEL);
  t.status = child(WC_STATICW, L"", 0, ID_TEST_STATUS);
  t.back = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_STRESS_BACK), WS_TABSTOP, ID_TEST_BACK);
  t.del = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_STRESS_DELETE), WS_TABSTOP, ID_TEST_DELETE);
  t.stress_note = child(WC_STATICW, mc_wstr(MC_STR_STRESS_TEXT), 0, ID_TEST_STRESS_NOTE);
  t.report = child(WC_EDITW, L"", WS_TABSTOP | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, ID_TEST_REPORT);
}

void testing_view_destroy(void) {
  if (t.deleting) {
    WaitForSingleObject(t.deleting, INFINITE);
    CloseHandle(t.deleting);
    t.deleting = NULL;
  }
  free(t.delete_open_dir);
  t.delete_open_dir = NULL;
  if (t.job) {
    mc_stress_job_cancel(t.job);
    mc_stress_job_free(t.job); /* waits, which is short: it checks after each album */
    t.job = NULL;
  }
  free(t.job_dir);
  free(t.config_dir);
  free(t.report_shown);
  t.job_dir = t.config_dir = NULL;
  t.report_shown = NULL;
  if (t.mono) DeleteObject(t.mono);
  t.mono = NULL;
}

void testing_view_init(mc_settings *settings, const char *config_dir, const testing_host *host) {
  t.settings = settings;
  free(t.config_dir);
  t.config_dir = config_dir ? _strdup(config_dir) : NULL;
  t.host = *host;
}

void testing_view_set_font(HFONT font, UINT dpi) {
  t.font = font;
  t.dpi = dpi;
  if (t.mono) DeleteObject(t.mono);
  t.mono = CreateFontW(-MulDiv(12, (int)dpi, USER_DEFAULT_SCREEN_DPI), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH,
                       L"Consolas");
  HWND body[] = {t.check_label, t.note, t.run, t.copy, t.save, t.clear, t.stress_label, t.preset, t.generate,
                 t.cancel, t.status, t.back, t.del, t.stress_note};
  for (size_t i = 0; i < sizeof body / sizeof *body; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
  SendMessageW(t.report, WM_SETFONT, (WPARAM)t.mono, TRUE);
}

void testing_view_show(BOOL page_shown, BOOL open) {
  BOOL show = page_shown && open;
  t.shown = show;
  HWND all[] = {t.check, t.check_label, t.note, t.run, t.copy, t.save, t.clear, t.report, t.stress_label, t.preset,
                t.generate, t.cancel, t.status, t.back, t.del, t.stress_note};
  for (size_t i = 0; i < sizeof all / sizeof *all; i++) ShowWindow(all[i], show ? SW_SHOW : SW_HIDE);
  if (show) {
    SetTimer(t.parent, TIMER_REPORT, REPORT_MS, on_timer);
    testing_view_update();
  } else {
    KillTimer(t.parent, TIMER_REPORT);
  }
}

static int button_width(HWND wnd, int pad) {
  wchar_t buf[128];
  int n = GetWindowTextW(wnd, buf, 128);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, t.font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return size.cx + 2 * pad;
}

/* Puts wnds side by side at top, each as wide as its text, and returns the row's end. */
static int button_row(const HWND *wnds, int n, int left, int top, int gap, int pad, int h) {
  for (int i = 0; i < n; i++) {
    int w = button_width(wnds[i], pad);
    MoveWindow(wnds[i], left, top, w, h, TRUE);
    left += w + gap;
  }
  return top + h;
}

int testing_view_layout(int left, int top, int text_w, UINT dpi) {
  if (!t.shown) return top;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int box = GetSystemMetricsForDpi(SM_CXMENUCHECK, dpi), btn_h = S(32), pad = S(16), gap = S(8);
  int lh = canvas_text_height(t.check_label, t.font, text_w - box - S(8), 0), row_h = max(box, lh);
  MoveWindow(t.check, left, top + (row_h - box) / 2, box, box, TRUE);
  MoveWindow(t.check_label, left + box + S(8), top + (row_h - lh) / 2, text_w - box - S(8), lh, TRUE);
  top += row_h + S(12);

  int nh = canvas_text_height(t.note, t.font, text_w, 0);
  MoveWindow(t.note, left, top, text_w, nh, TRUE);
  top += nh + S(12);

  HWND perf[] = {t.run, t.copy, t.save, t.clear};
  top = button_row(perf, 4, left, top, gap, pad, btn_h) + S(12);

  top += S(12);
  lh = canvas_text_height(t.stress_label, t.font, text_w, 0);
  MoveWindow(t.stress_label, left, top, text_w, lh, TRUE);
  top += lh + S(8);

  int preset_w = S(180);
  /* A combo box's height is its drop-down's; the closed box sizes itself from the font. */
  MoveWindow(t.preset, left, top + S(2), preset_w, S(200), TRUE);
  HWND stress[] = {t.generate, t.cancel};
  top = button_row(stress, 2, left + preset_w + gap, top, gap, pad, btn_h) + S(8);

  int sh = canvas_text_height(t.status, t.font, text_w, DT_SINGLELINE);
  MoveWindow(t.status, left, top, text_w, sh, TRUE);
  top += sh + S(8);

  HWND library[] = {t.back, t.del};
  top = button_row(library, 2, left, top, gap, pad, btn_h) + S(12);

  nh = canvas_text_height(t.stress_note, t.font, text_w, 0);
  MoveWindow(t.stress_note, left, top, text_w, nh, TRUE);
  top += nh + S(24);

  /* Last: it can be long, and the way back shouldn't end up far below it. */
  int rh = canvas_text_height(t.report, t.mono, text_w, DT_EDITCONTROL) + S(8);
  MoveWindow(t.report, left, top, text_w, rh, TRUE);
  top += rh;
#undef S
  return top;
}

BOOL testing_view_on_command(int id, int code) {
  switch (id) {
    case ID_TEST_CHECK:
      if (code == BN_CLICKED) on_check();
      return TRUE;
    case ID_TEST_CHECK_LABEL:
      if (code == STN_CLICKED) {
        BOOL on = SendMessageW(t.check, BM_GETCHECK, 0, 0) == BST_CHECKED;
        SendMessageW(t.check, BM_SETCHECK, on ? BST_UNCHECKED : BST_CHECKED, 0);
        on_check();
      }
      return TRUE;
    case ID_TEST_RUN:
      if (t.host.run_scroll_test) t.host.run_scroll_test();
      return TRUE;
    case ID_TEST_COPY: copy_report(); return TRUE;
    case ID_TEST_SAVE: save_report(); return TRUE;
    case ID_TEST_CLEAR:
      mc_perf_clear();
      update_report();
      return TRUE;
    case ID_TEST_GENERATE: generate(); return TRUE;
    case ID_TEST_CANCEL:
      if (t.job) mc_stress_job_cancel(t.job);
      return TRUE;
    case ID_TEST_BACK:
      if (t.host.back) t.host.back();
      return TRUE;
    case ID_TEST_DELETE: delete_all(); return TRUE;
    case ID_TEST_NOTE:
    case ID_TEST_REPORT:
    case ID_TEST_PRESET:
    case ID_TEST_STRESS_LABEL:
    case ID_TEST_STATUS:
    case ID_TEST_STRESS_NOTE:
      return TRUE;
    default: return FALSE;
  }
}

HBRUSH testing_view_ctlcolor(HWND wnd, HDC dc) {
  if (wnd == t.check) { /* just its background */
    SetBkColor(dc, theme.bg);
    return theme.bg_brush;
  }
  BOOL note = wnd == t.note || wnd == t.stress_note || wnd == t.status;
  if (!note && wnd != t.check_label && wnd != t.stress_label && wnd != t.report) return NULL;
  SetTextColor(dc, note ? theme.text2 : theme.text);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
