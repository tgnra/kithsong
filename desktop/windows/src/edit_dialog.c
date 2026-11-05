#define COBJMACROS
#include "edit_dialog.h"

#include <dwmapi.h>
#include <math.h>
#include <ole2.h>
#include <oleacc.h>
#include <process.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uiautomation.h>
#include <wchar.h>
#include <windowsx.h>

#include "album_grid.h"
#include "artist_view.h"
#include "canvas.h"
#include "cover.h"
#include "musiccore/edit.h"
#include "now_playing.h"
#include "pick_folder.h"
#include "playback.h"
#include "scroller.h"
#include "theme.h"
#include "track_list.h"
#include "uia.h"
#include "wstr.h"

#define WM_JOB_DONE (WM_APP + 1) /* lParam: the sheet's job, finished */

/* Layout at 96 DPI. */
enum {
  EDIT_W = 480,
  SEARCH_W = 860,
  PAD = 24,
  CLOSE_W = 46, /* the close button, as wide and tall as Windows' own */
  CLOSE_H = 32,
  HEADING_TOP = 18,
  HEADING_GAP = 14,
  LABEL_H = 22,
  FIELD_H = 32,
  FIELD_INSET = 10, /* the text inside its field */
  RADIUS = 4,
  CARD_RADIUS = 8,
  ROW_GAP = 10,
  COL_GAP = 12,
  CARD_PAD = 12,
  CHECK_SIZE = 20,
  CHECK_GAP = 12,
  CHECK_ROW_GAP = 4,
  MESSAGE_LINES = 2, /* kept for what a sheet has to say, so nothing jumps when it does */
  BUTTON_H = 32,
  BUTTON_W = 96, /* at least */
  BUTTON_GAP = 8,
  BOTTOM = 20,
  SPINNER = 16,
  LIST_H = 304,
  LIST_HEAD_H = 32,
  LIST_ROW_H = 32,
  LIST_INSET = 12,
  EDGE = 32, /* kept clear above and below a window on a short screen */
  ART_SIZE = 140, /* with the gap and Change…, as tall as the three rows of fields beside it */
  ART_GAP = 16,
  DROP_INSET = 4, /* the ring round the window while a file is over it */
};

enum { TIMER_SPIN = 1 }; /* the list's scroller has 101 and 102 */
enum { SPIN_MS = 90, SPIN_DOTS = 8 };

/* DwmSetWindowAttribute's DWMWA_WINDOW_CORNER_PREFERENCE (Windows 11), DWMWCP_ROUND. */
enum { ATTR_CORNERS = 33, CORNERS_ROUND = 2 };

/* The form's text boxes, by what they hold. */
enum field {
  F_TITLE,
  F_NAME,
  F_ARTIST,
  F_SORT_NAME,
  F_TRACK_NO,
  F_DISC_NO,
  F_YEAR,
  F_ORIGINAL_YEAR,
  F_GENRE,
  F_MB,
  NUM_FIELDS,
};

/* Its other parts, and the search window's, numbered on from those. */
enum { ID_ART_COVER = NUM_FIELDS, ID_ART_CHANGE, ID_ART_AUTO, ID_SEARCH_MB, ID_NO_SCROBBLE, ID_MIX, ID_STOP_RETRY, ID_SAVE, ID_CANCEL, ID_CLOSE, ID_SEARCH, ID_LIST, ID_USE };

typedef enum { KIND_ALBUM, KIND_ARTIST, KIND_TRACK } edit_kind;

/* --- Jobs: core calls off the UI thread --- */

/* One call, with copies of all it needs, so nothing it reads goes away while it runs. Whichever
 * of the thread and the sheet gets there second frees it: the thread as it finishes, unless
 * the sheet gave up on it, and the sheet when it hears back, or as it closes if it won't. */
enum { JOB_RUNNING, JOB_DONE, JOB_ABANDONED };

typedef struct job {
  volatile LONG state;
  HWND notify;
  void (*run)(struct job *j);
  void (*done)(struct job *j); /* on the UI thread, if the sheet is still there */
  mc_library *lib;
  char uid[MC_UID_STRLEN];
  mc_status st;
  /* Save. */
  edit_kind kind;
  char *text[NUM_FIELDS];
  mc_album_flags flags;
  BOOL set_tags, set_flags, tags_saved, flags_saved;
  /* A new cover: the image, to save or, read from art_path, to check. */
  wchar_t *art_path;
  unsigned char *art;
  size_t art_len;
  BOOL set_art, reset_art, art_tried, art_saved;
  char merge_into[MC_UID_STRLEN]; /* the artist to merge with, rather than save the name; "" for none */
  /* Search. */
  char *artist, *title;
  mc_release_list *list;
  /* Using a release. */
  char mb_albumid[MC_UID_STRLEN];
  mc_album_tags tags;
  BOOL has_tags;
} job;

static volatile LONG g_jobs; /* running */
static HANDLE g_jobs_idle;   /* set while none is */

static void job_free(job *j) {
  for (int i = 0; i < NUM_FIELDS; i++) free(j->text[i]);
  free(j->art_path);
  free(j->art);
  free(j->artist);
  free(j->title);
  mc_release_list_free(j->list);
  if (j->has_tags) mc_album_tags_clear(&j->tags);
  free(j);
}

static unsigned __stdcall job_main(void *arg) {
  job *j = arg;
  j->run(j);
  if (InterlockedCompareExchange(&j->state, JOB_DONE, JOB_RUNNING) == JOB_ABANDONED) job_free(j);
  else PostMessageW(j->notify, WM_JOB_DONE, 0, (LPARAM)j);
  if (InterlockedDecrement(&g_jobs) == 0) SetEvent(g_jobs_idle);
  return 0;
}

static BOOL job_start(job *j) {
  if (!g_jobs_idle) g_jobs_idle = CreateEventW(NULL, TRUE, TRUE, NULL);
  if (!g_jobs_idle) return FALSE;
  if (InterlockedIncrement(&g_jobs) == 1) ResetEvent(g_jobs_idle);
  HANDLE thread = (HANDLE)_beginthreadex(NULL, 0, job_main, j, 0, NULL);
  if (!thread) {
    if (InterlockedDecrement(&g_jobs) == 0) SetEvent(g_jobs_idle);
    return FALSE;
  }
  CloseHandle(thread);
  return TRUE;
}

/* The sheet is closing and won't hear from j. */
static void job_abandon(job *j) {
  if (InterlockedCompareExchange(&j->state, JOB_ABANDONED, JOB_RUNNING) == JOB_DONE) job_free(j);
}

void edit_dialog_wait(void) {
  if (g_jobs_idle) WaitForSingleObject(g_jobs_idle, INFINITE);
}

/* --- Sheets: the two windows' common ground --- */

typedef enum { PART_FIELD, PART_BUTTON, PART_ACCENT, PART_CHECK, PART_LIST, PART_COVER, PART_CLOSE } part_kind;

/* What the mouse and keyboard act on, in tab order (the close button and the cover, which has a
 * button of its own, aren't in it). */
typedef struct part {
  part_kind kind;
  int id;
  enum mc_str_id label;
  enum mc_str_id note;          /* the line under a check */
  const wchar_t *automation_id; /* not for a field, whose EDIT is what clients see */
  RECT rc;                      /* a field's box, a check's whole row */
  RECT label_rc, note_rc;
  HWND edit;      /* a field's */
  BOOL read_only; /* a field that's never typed in */
  BOOL checked, disabled;
} part;

enum { MAX_PARTS = 16 };

typedef struct sheet sheet;

typedef struct sheet_ops {
  void (*layout)(void);       /* sets w, h and the parts' places */
  void (*activate)(part *p);  /* a click, or Enter or Space on it */
  void (*enter)(void);        /* Enter in a text box */
  void (*cancel)(void);       /* Escape, the close button, Alt+F4 */
  void (*paint)(canvas *c);   /* what only this window has; may be NULL */
  void (*drop)(const wchar_t *path); /* a file dropped on it; may be NULL */
} sheet_ops;

struct sheet {
  const sheet_ops *ops;
  HWND owner, wnd;
  UINT dpi;
  HFONT font, label_font, heading_font, icon_font, close_font, tick_font;
  canvas buf;
  part parts[MAX_PARTS];
  int nparts;
  enum mc_str_id title;
  int hot, pressed, focus;
  BOOL show_focus; /* the keyboard has been used, so the focus ring shows */
  BOOL tracking;   /* TrackMouseEvent is on */
  BOOL done;
  int w, h, line_h;
  RECT heading_rc, message_rc;
  wchar_t message[512];
  BOOL error;   /* message is a failure */
  BOOL spin;    /* message is something happening: a spinner beside it */
  int spin_at;
  job *job;     /* running; one at a time */
  IAccPropServices *acc;
  uia_host ui;
  int ui_parts[MAX_PARTS]; /* the parts screen readers get from us: all but the fields */
  int ui_count;
};

static int scale(const sheet *s, int px) {
  return MulDiv(px, (int)s->dpi, USER_DEFAULT_SCREEN_DPI);
}

static part *part_of(sheet *s, int id) {
  for (int i = 0; i < s->nparts; i++)
    if (s->parts[i].id == id) return &s->parts[i];
  return NULL;
}

static part *add_part(sheet *s, part_kind kind, int id, enum mc_str_id label, const wchar_t *automation_id) {
  part *p = &s->parts[s->nparts++];
  memset(p, 0, sizeof *p);
  p->kind = kind;
  p->id = id;
  p->label = label;
  p->automation_id = automation_id;
  return p;
}

static void free_fonts(sheet *s) {
  HFONT *fonts[] = {&s->font, &s->label_font, &s->heading_font, &s->icon_font, &s->close_font, &s->tick_font};
  for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++) {
    if (*fonts[i]) DeleteObject(*fonts[i]);
    *fonts[i] = NULL;
  }
}

static HFONT icon_font(const sheet *s, int px) {
  return CreateFontW(-scale(s, px), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

static void update_fonts(sheet *s) {
  free_fonts(s);
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, s->dpi);
  s->font = CreateFontIndirectW(&ncm.lfMessageFont);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfWeight = FW_SEMIBOLD;
  s->label_font = CreateFontIndirectW(&lf);
  lf.lfHeight = -scale(s, 20); /* Windows 11's subtitle */
  s->heading_font = CreateFontIndirectW(&lf);
  s->icon_font = icon_font(s, 16);
  s->close_font = icon_font(s, 10);
  s->tick_font = icon_font(s, 13);
  for (int i = 0; i < s->nparts; i++)
    if (s->parts[i].edit) SendMessageW(s->parts[i].edit, WM_SETFONT, (WPARAM)s->font, FALSE);
}

static int text_width(HFONT font, const wchar_t *text) {
  HDC dc = GetDC(NULL);
  HGDIOBJ old = SelectObject(dc, font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  return size.cx;
}

static const UINT k_para = DT_WORDBREAK | DT_NOPREFIX;

static int text_height(HFONT font, const wchar_t *text, int w) {
  HDC dc = GetDC(NULL);
  HGDIOBJ old = SelectObject(dc, font);
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, text, -1, &rc, k_para | DT_CALCRECT);
  SelectObject(dc, old);
  ReleaseDC(NULL, dc);
  return rc.bottom;
}

static int button_width(const sheet *s, const part *p) {
  return max(scale(s, BUTTON_W), text_width(s->font, mc_wstr(p->label)) + scale(s, 32));
}

/* A label over field p at (x, y), w wide. Returns the y under it. */
static int layout_field(const sheet *s, part *p, int x, int y, int w) {
  SetRect(&p->label_rc, x, y, x + w, y + scale(s, LABEL_H));
  y += scale(s, LABEL_H);
  SetRect(&p->rc, x, y, x + w, y + scale(s, FIELD_H));
  return y + scale(s, FIELD_H);
}

/* The heading's place, under the top edge. Returns the y under it. */
static int layout_heading(sheet *s) {
  int x = scale(s, PAD), top = scale(s, HEADING_TOP), h = text_height(s->heading_font, L"Ag", s->w);
  s->line_h = text_height(s->font, L"Ag", s->w);
  SetRect(&s->heading_rc, x, top, s->w - scale(s, CLOSE_W), top + h);
  part *close = part_of(s, ID_CLOSE);
  SetRect(&close->rc, s->w - scale(s, CLOSE_W), 0, s->w, scale(s, CLOSE_H));
  return top + h + scale(s, HEADING_GAP);
}

/* The right-hand buttons in a row with top y, in the order given. Returns the left of the first. */
static int layout_buttons(sheet *s, int y, const int *ids, int n) {
  int right = s->w - scale(s, PAD);
  for (int i = n - 1; i >= 0; i--) {
    part *p = part_of(s, ids[i]);
    int w = button_width(s, p);
    SetRect(&p->rc, right - w, y, right, y + scale(s, BUTTON_H));
    right -= w + scale(s, BUTTON_GAP);
  }
  return right + scale(s, BUTTON_GAP);
}

/* A layered child keeps a surface of its own, so nothing paints it for being moved or shown,
 * and invalidating does nothing until it's visible. */
static void repaint_edit(const part *p) {
  RedrawWindow(p->edit, NULL, NULL, RDW_INVALIDATE | RDW_FRAME | RDW_ERASE | RDW_UPDATENOW);
}

/* Puts each EDIT inside its field. */
static void place_edits(sheet *s) {
  int inset = scale(s, FIELD_INSET), edge = scale(s, 2);
  for (int i = 0; i < s->nparts; i++) {
    part *p = &s->parts[i];
    if (!p->edit) continue;
    MoveWindow(p->edit, p->rc.left + inset, p->rc.top + edge, p->rc.right - p->rc.left - 2 * inset,
               p->rc.bottom - p->rc.top - 2 * edge - scale(s, 1), FALSE);
    repaint_edit(p);
  }
}

static void ui_rebuild(sheet *s) {
  s->ui_count = 0;
  for (int i = 0; i < s->nparts; i++)
    if (s->parts[i].kind != PART_FIELD) s->ui_parts[s->ui_count++] = i;
}

/* Sizes the window for its layout, centred on (cx, cy) and kept on its screen. */
static void place(sheet *s, int cx, int cy) {
  RECT r = {cx - s->w / 2, cy - s->h / 2, cx - s->w / 2 + s->w, cy - s->h / 2 + s->h};
  MONITORINFO mi = {.cbSize = sizeof mi};
  if (GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi)) {
    if (r.bottom > mi.rcWork.bottom) OffsetRect(&r, 0, mi.rcWork.bottom - r.bottom);
    if (r.right > mi.rcWork.right) OffsetRect(&r, mi.rcWork.right - r.right, 0);
    if (r.top < mi.rcWork.top) OffsetRect(&r, 0, mi.rcWork.top - r.top);
    if (r.left < mi.rcWork.left) OffsetRect(&r, mi.rcWork.left - r.left, 0);
  }
  SetWindowPos(s->wnd, NULL, r.left, r.top, s->w, s->h, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* --- State --- */

static BOOL can_focus(const part *p) {
  return p->kind != PART_CLOSE && p->kind != PART_COVER && !p->disabled;
}

static int part_at(const sheet *s, POINT pt) {
  for (int i = 0; i < s->nparts; i++)
    if (PtInRect(&s->parts[i].rc, pt)) return i;
  return -1;
}

static int ui_node_of(const sheet *s, int i);

/* Moves the focus to part i, into its EDIT if it's a field. */
static void focus_part(sheet *s, int i) {
  s->focus = i;
  part *p = &s->parts[i];
  if (p->edit) {
    SetFocus(p->edit);
    if (s->show_focus) SendMessageW(p->edit, EM_SETSEL, 0, -1);
  } else {
    SetFocus(s->wnd);
  }
  InvalidateRect(s->wnd, NULL, FALSE);
  uia_focus(&s->ui);
}

/* Tab and Shift+Tab: the next or previous part that can take the focus. */
static void move_focus(sheet *s, int step) {
  int i = s->focus;
  for (int n = 0; n < s->nparts; n++) {
    i = (i + step + s->nparts) % s->nparts;
    if (can_focus(&s->parts[i])) {
      focus_part(s, i);
      return;
    }
  }
}

/* What the sheet says beside its buttons, or for NULL nothing. spin: something is going on. */
static void set_message(sheet *s, const wchar_t *text, BOOL error, BOOL spin) {
  wcsncpy(s->message, text ? text : L"", sizeof s->message / sizeof s->message[0] - 1);
  s->error = error;
  if (spin && !s->spin) SetTimer(s->wnd, TIMER_SPIN, SPIN_MS, NULL);
  if (!spin && s->spin) KillTimer(s->wnd, TIMER_SPIN);
  s->spin = spin;
  InvalidateRect(s->wnd, NULL, FALSE);
  uia_refresh(&s->ui);
  if (text) uia_announce(&s->ui, 1);
}

/* Greys out the text boxes, or brings them back, as Save or a search runs. */
static void set_fields_locked(sheet *s, BOOL locked) {
  for (int i = 0; i < s->nparts; i++) {
    part *p = &s->parts[i];
    if (p->edit && !p->read_only) SendMessageW(p->edit, EM_SETREADONLY, locked, 0);
  }
}

/* The field's text, malloc'd UTF-8. */
static char *field_text(const part *p) {
  int n = GetWindowTextLengthW(p->edit);
  wchar_t *w = calloc((size_t)n + 1, sizeof *w);
  if (!w) return NULL;
  GetWindowTextW(p->edit, w, n + 1);
  char *out = wstr_to_utf8(w);
  free(w);
  return out;
}

static void set_field_text(const part *p, const char *text) {
  wchar_t *w = wstr_dup_from_utf8(text ? text : "");
  if (w) SetWindowTextW(p->edit, w);
  free(w);
}

/* --- Painting --- */

static COLORREF ink(void) {
  return theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

static COLORREF error_ink(void) {
  if (theme.high_contrast) return theme.text;
  return theme.dark ? RGB(255, 153, 164) : RGB(196, 43, 28); /* Windows 11's critical text */
}

static control_state state_of(const sheet *s, int i) {
  if (s->parts[i].disabled) return STATE_NORMAL;
  if (s->pressed == i && s->hot == i) return STATE_PRESSED;
  return s->hot == i ? STATE_HOT : STATE_NORMAL;
}

/* Windows 11's card: white at 70% over Mica, or white at 5% in dark mode. */
static void paint_card(canvas *c, const RECT *r, int radius) {
  if (theme.high_contrast) canvas_fill(c, r, radius, theme.bg, 255);
  else canvas_fill(c, r, radius, RGB(255, 255, 255), theme.dark ? 13 : 179);
  canvas_frame(c, r, radius, 1, theme.high_contrast ? theme.text : theme.line, 255);
}

/* The label, and the field's box: opaque, in the colour its EDIT is, with an accent line under
 * it while it has the focus, as Windows 11's text boxes do. */
static void paint_field(const sheet *s, canvas *c, int i) {
  const part *p = &s->parts[i];
  canvas_text(c, mc_wstr(p->label), -1, &p->label_rc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, s->font,
              theme.text);
  int radius = scale(s, RADIUS);
  BOOL focused = GetFocus() == p->edit;
  canvas_fill(c, &p->rc, radius, theme.field, 255);
  canvas_frame(c, &p->rc, radius, 1, theme.high_contrast ? theme.text : theme.line, 255);
  if (focused) {
    int line = scale(s, 2);
    RECT under = {p->rc.left + radius / 2, p->rc.bottom - line, p->rc.right - radius / 2, p->rc.bottom};
    canvas_fill(c, &under, 0, theme.accent, 255);
  }
}

static void paint_button(const sheet *s, canvas *c, int i) {
  const part *p = &s->parts[i];
  int radius = scale(s, RADIUS);
  COLORREF fg;
  if (p->kind == PART_ACCENT && !p->disabled) {
    canvas_fill(c, &p->rc, radius, theme.accent, 255);
    control_state st = state_of(s, i);
    if (st != STATE_NORMAL) canvas_fill(c, &p->rc, radius, theme.bg, st == STATE_PRESSED ? 56 : 28);
    fg = theme.on_accent;
  } else if (p->kind == PART_ACCENT) {
    if (theme.high_contrast) {
      canvas_frame(c, &p->rc, radius, 1, theme.text3, 255);
      fg = theme.text3;
    } else {
      canvas_fill(c, &p->rc, radius, ink(), theme.dark ? 40 : 55);
      fg = theme.dark ? RGB(135, 135, 135) : RGB(255, 255, 255);
    }
  } else {
    if (!theme.high_contrast) canvas_fill(c, &p->rc, radius, ink(), theme.dark ? 15 : 6);
    fg = theme_draw_state(c, &p->rc, radius, state_of(s, i));
    if (p->disabled) fg = theme.text3;
    canvas_frame(c, &p->rc, radius, 1, theme.high_contrast ? (p->disabled ? theme.text3 : theme.text) : theme.line, 255);
  }
  canvas_text(c, mc_wstr(p->label), -1, &p->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, s->font, fg);
}

/* A checkbox, as Windows 11 draws one, with its name and the line under it. */
static void paint_check(const sheet *s, canvas *c, int i) {
  const part *p = &s->parts[i];
  int radius = scale(s, RADIUS), size = scale(s, CHECK_SIZE);
  COLORREF fg = theme_draw_state(c, &p->rc, radius, state_of(s, i));
  if (p->disabled) fg = theme.text3;
  RECT box = {p->label_rc.left - size - scale(s, CHECK_GAP), p->label_rc.top, 0, 0};
  box.right = box.left + size;
  box.bottom = box.top + size;
  if (p->checked) {
    canvas_fill(c, &box, radius, p->disabled ? theme.text3 : theme.accent, 255);
    canvas_text(c, L"\xE73E" /* CheckMark */, -1, &box, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
                s->tick_font, theme.on_accent);
  } else {
    canvas_frame(c, &box, radius, 1, p->disabled ? theme.text3 : theme.text2, 255);
  }
  canvas_text(c, mc_wstr(p->label), -1, &p->label_rc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX, s->font, fg);
  canvas_text(c, mc_wstr(p->note), -1, &p->note_rc, k_para, s->font, p->disabled ? theme.text3 : theme.text2);
}

/* The close button, as Windows draws its own: red while the mouse is on it. */
static void paint_close(const sheet *s, canvas *c, int i) {
  const part *p = &s->parts[i];
  control_state st = state_of(s, i);
  COLORREF fg = p->disabled ? theme.text3 : theme.text;
  if (st != STATE_NORMAL && !theme.high_contrast) {
    canvas_fill(c, &p->rc, 0, RGB(196, 43, 28), st == STATE_PRESSED ? 230 : 255);
    fg = RGB(255, 255, 255);
  } else if (st != STATE_NORMAL) {
    fg = theme_draw_state(c, &p->rc, 0, st);
  }
  canvas_text(c, L"\xE8BB" /* ChromeClose */, -1, &p->rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX,
              s->close_font, fg);
}

/* A ring of dots, the brightest going round, centred on (cx, cy). */
static void paint_spinner(const sheet *s, canvas *c, int cx, int cy) {
  int size = scale(s, SPINNER), dot = max(scale(s, 3), 2);
  double r = (size - dot) / 2.0;
  for (int i = 0; i < SPIN_DOTS; i++) {
    double a = 2 * 3.14159265358979 * i / SPIN_DOTS;
    int x = cx + (int)lround(r * sin(a)) - dot / 2, y = cy - (int)lround(r * cos(a)) - dot / 2;
    RECT d = {x, y, x + dot, y + dot};
    int age = (s->spin_at - i + 2 * SPIN_DOTS) % SPIN_DOTS;
    canvas_fill(c, &d, dot / 2, theme.text2, 255 - age * 200 / SPIN_DOTS);
  }
}

/* What the sheet has to say, in r: centred (a list's empty space), or from the left. */
static void paint_message(const sheet *s, canvas *c, const RECT *r, BOOL centred) {
  if (!s->message[0]) return;
  RECT text = *r;
  int size = scale(s, SPINNER), gap = scale(s, 8);
  UINT format = k_para | DT_END_ELLIPSIS;
  if (centred) {
    int h = text_height(s->font, s->message, r->right - r->left - 2 * size);
    int w = min(text_width(s->font, s->message), r->right - r->left - 2 * size);
    text.left = (r->left + r->right - w + (s->spin ? size + gap : 0)) / 2;
    text.right = text.left + w + 1;
    text.top = (r->top + r->bottom - h) / 2;
    text.bottom = text.top + h;
  } else if (s->spin) {
    text.left += size + gap;
  }
  if (s->spin) paint_spinner(s, c, text.left - gap - size / 2, text.top + s->line_h / 2);
  canvas_text(c, s->message, -1, &text, format, s->font, s->error ? error_ink() : theme.text2);
}

static void paint(sheet *s, HDC dc) {
  RECT all = {0, 0, s->w, s->h};
  if (!canvas_reserve(&s->buf, s->w, s->h)) return;
  canvas *c = &s->buf;
  canvas_clear(c, &all, theme.bg, theme.mica ? 0 : 255);
  canvas_text(c, mc_wstr(s->title), -1, &s->heading_rc, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX, s->heading_font,
              theme.text);
  if (s->ops->paint) s->ops->paint(c);
  for (int i = 0; i < s->nparts; i++) {
    if (IsRectEmpty(&s->parts[i].rc)) continue;
    switch (s->parts[i].kind) {
      case PART_FIELD: paint_field(s, c, i); break;
      case PART_BUTTON:
      case PART_ACCENT: paint_button(s, c, i); break;
      case PART_CHECK: paint_check(s, c, i); break;
      case PART_CLOSE: paint_close(s, c, i); break;
      case PART_LIST:
      case PART_COVER: break; /* in ops.paint */
    }
  }
  const part *p = s->focus >= 0 ? &s->parts[s->focus] : NULL;
  if (s->show_focus && GetFocus() == s->wnd && p && can_focus(p)) {
    RECT ring = p->rc;
    int radius = scale(s, p->kind == PART_LIST ? CARD_RADIUS : RADIUS);
    InflateRect(&ring, scale(s, 3), scale(s, 3));
    canvas_frame(c, &ring, radius + scale(s, 3), scale(s, 2), theme.text, 255);
  }
  canvas_blit(c, dc, 0, 0, &all);
}

/* Dark or light to match, Mica behind everything (the frame fills the window), and Windows
 * 11's rounded corners. */
static void apply_frame(sheet *s) {
  theme_apply_window(s->wnd, TRUE);
  MARGINS m = {theme.mica ? -1 : 0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(s->wnd, &m);
  int corners = CORNERS_ROUND;
  DwmSetWindowAttribute(s->wnd, ATTR_CORNERS, &corners, sizeof corners);
}

/* --- UI Automation: the heading, what the sheet is saying, then its parts but the fields,
 * which are EDITs with names of their own. A list's rows are the list node's children. --- */

enum { NODE_HEADING, NODE_MESSAGE, NODE_PARTS };

static int ui_node_of(const sheet *s, int i) {
  for (int k = 0; k < s->ui_count; k++)
    if (s->ui_parts[k] == i) return NODE_PARTS + k;
  return UIA_ROOT;
}

static part *ui_part(sheet *s, int node) {
  int k = node - NODE_PARTS;
  return k >= 0 && k < s->ui_count ? &s->parts[s->ui_parts[k]] : NULL;
}

static BOOL ui_node(sheet *s, int i, uia_node *n) {
  n->parent = UIA_ROOT;
  if (i == UIA_ROOT) {
    n->type = UIA_PaneControlTypeId;
    lstrcpynW(n->name, mc_wstr(s->title), 512);
    n->first = 0;
    n->count = NODE_PARTS + s->ui_count;
    return TRUE;
  }
  if (i == NODE_HEADING) {
    n->type = UIA_TextControlTypeId;
    n->rect = s->heading_rc;
    lstrcpynW(n->name, mc_wstr(s->title), 512);
    return TRUE;
  }
  if (i == NODE_MESSAGE) {
    n->type = UIA_TextControlTypeId;
    n->rect = s->message_rc;
    n->live = Assertive;
    n->automation_id = L"Message";
    lstrcpynW(n->name, s->message, 512);
    return TRUE;
  }
  part *p = ui_part(s, i);
  if (!p) return FALSE;
  n->rect = p->rc;
  n->automation_id = p->automation_id;
  n->flags = (p->disabled ? UIA_DISABLED : 0) | (p->kind != PART_CLOSE && p->kind != PART_COVER ? UIA_FOCUSABLE : 0);
  lstrcpynW(n->name, mc_wstr(p->label), 512);
  if (p->kind == PART_COVER) {
    n->type = UIA_ImageControlTypeId;
    lstrcpynW(n->help, mc_wstr(p->note), 256);
  } else if (p->kind == PART_CHECK) {
    n->type = UIA_CheckBoxControlTypeId;
    n->flags |= p->checked ? UIA_TOGGLE_ON : UIA_TOGGLE_OFF;
    lstrcpynW(n->help, mc_wstr(p->note), 256);
  } else {
    n->type = UIA_ButtonControlTypeId;
    n->flags |= UIA_INVOKABLE;
  }
  return TRUE;
}

static int ui_focus(sheet *s) {
  if (s->focus < 0 || s->parts[s->focus].edit) return UIA_ROOT;
  return ui_node_of(s, s->focus);
}

static int ui_at(sheet *s, POINT pt) {
  int i = part_at(s, pt);
  return i >= 0 && !s->parts[i].edit ? ui_node_of(s, i) : UIA_ROOT;
}

static void ui_invoke(sheet *s, int node) {
  part *p = ui_part(s, node);
  if (p && !p->disabled) s->ops->activate(p);
}

static void ui_set_focus(sheet *s, int node) {
  part *p = ui_part(s, node);
  if (p && can_focus(p)) focus_part(s, (int)(p - s->parts));
}

/* A name for screen readers: a field's label is drawn, not a control of its own. */
static void set_name(sheet *s, HWND w, const wchar_t *name) {
  if (!s->acc)
    CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER, &IID_IAccPropServices, (void **)&s->acc);
  if (s->acc && w) IAccPropServices_SetHwndPropStr(s->acc, w, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name);
}

/* --- The search window's list --- */

static struct {
  sheet s;
  mc_library *lib;
  char uid[MC_UID_STRLEN];
  mc_release_list *results;
  int selected, hot;
  RECT card, head;
  scroller scroll;
  canvas rows;
  int rows_w, rows_h;
  BOOL searched; /* a search has come back, with results or not */
  mc_album_tags out; /* the release used */
  BOOL picked;
} g_search;

enum { COL_TITLE, COL_ARTIST, COL_YEAR, COL_COUNTRY, COL_LABEL, COL_FORMAT, COL_TRACKS, COL_FIT, NUM_COLS };

static const struct {
  enum mc_str_id name;
  int w; /* 0: shares what the others leave */
  BOOL right;
} k_cols[NUM_COLS] = {
  [COL_TITLE] = {MC_STR_TITLE, 0, FALSE},
  [COL_ARTIST] = {MC_STR_ARTIST, 128, FALSE},
  [COL_YEAR] = {MC_STR_COL_YEAR, 48, FALSE},
  [COL_COUNTRY] = {MC_STR_WINDOWS_COL_COUNTRY, 56, FALSE},
  [COL_LABEL] = {MC_STR_WINDOWS_COL_LABEL, 112, FALSE},
  [COL_FORMAT] = {MC_STR_WINDOWS_COL_FORMAT, 88, FALSE},
  [COL_TRACKS] = {MC_STR_EDIT_COL_TRACKS, 48, TRUE},
  [COL_FIT] = {MC_STR_EDIT_COL_FIT, 44, TRUE},
};

static int result_count(void) {
  return g_search.results ? g_search.results->count : 0;
}

/* Column c's left and right edges, from x in a list w wide. */
static void col_span(int c, int x, int w, int *left, int *right) {
  sheet *s = &g_search.s;
  int gap = scale(s, 12), rest = w - gap * (NUM_COLS - 1);
  for (int i = 0; i < NUM_COLS; i++) rest -= scale(s, k_cols[i].w);
  for (int i = 0; i <= c; i++) {
    *left = x;
    x += k_cols[i].w ? scale(s, k_cols[i].w) : rest;
    *right = x;
    x += gap;
  }
}

/* Cell c of release r, into buf (n wide characters). */
static const wchar_t *cell_text(const mc_release_info *r, int c, wchar_t *buf, size_t n) {
  buf[0] = L'\0';
  switch (c) {
    case COL_TITLE: return wstr_from_utf8(buf, n, r->title);
    case COL_ARTIST: return wstr_from_utf8(buf, n, r->artist);
    case COL_YEAR:
      if (r->year) swprintf(buf, n, L"%d", r->year);
      break;
    case COL_COUNTRY: return wstr_from_utf8(buf, n, r->country);
    case COL_LABEL: return wstr_from_utf8(buf, n, r->label);
    case COL_FORMAT: return wstr_from_utf8(buf, n, r->format);
    case COL_TRACKS:
      if (r->track_count) swprintf(buf, n, L"%d", r->track_count);
      break;
    case COL_FIT:
      return mc_wformat(buf, n, mc_str(MC_STR_EDIT_FIT_PERCENT), MC_D((int)lround(r->score * 100)));
  }
  return buf;
}

static RECT row_rect(int i) {
  sheet *s = &g_search.s;
  const RECT *v = &g_search.scroll.view;
  int h = scale(s, LIST_ROW_H), top = v->top + i * h - g_search.scroll.pos;
  RECT r = {v->left, top, v->right, top + h};
  return r;
}

static int row_at(POINT pt) {
  if (!PtInRect(&g_search.scroll.view, pt)) return -1;
  int i = (pt.y - g_search.scroll.view.top + g_search.scroll.pos) / scale(&g_search.s, LIST_ROW_H);
  return i < result_count() ? i : -1;
}

/* The rows go on a canvas of their own, the view's size, so a row half scrolled out is cut at
 * the view's edge, and it's laid over the card. */
static void paint_rows(canvas *c) {
  sheet *s = &g_search.s;
  const RECT *v = &g_search.scroll.view;
  int vw = v->right - v->left, vh = v->bottom - v->top;
  if (vw <= 0 || vh <= 0) return;
  canvas *rows = &g_search.rows;
  if (g_search.rows_w != vw || g_search.rows_h != vh) {
    canvas_free(rows);
    memset(rows, 0, sizeof *rows);
    g_search.rows_w = vw;
    g_search.rows_h = vh;
  }
  if (!canvas_reserve(rows, vw, vh)) return;
  RECT all = {0, 0, vw, vh};
  canvas_clear(rows, &all, 0, 0);
  int row_h = scale(s, LIST_ROW_H), inset = scale(s, LIST_INSET), radius = scale(s, RADIUS);
  int first = g_search.scroll.pos / row_h;
  for (int i = first; i < result_count(); i++) {
    int top = i * row_h - g_search.scroll.pos;
    if (top >= vh) break;
    RECT r = {0, top, vw, top + row_h};
    RECT back = r;
    InflateRect(&back, -scale(s, 4), -scale(s, 1));
    control_state st = i == g_search.selected ? STATE_SELECTED : i == g_search.hot ? STATE_HOT : STATE_NORMAL;
    COLORREF fg = theme_draw_state(rows, &back, radius, st);
    if (i == g_search.selected && !theme.high_contrast) { /* the accent mark on the selected row */
      int mark_h = (back.bottom - back.top) / 2, line = scale(s, 3);
      RECT mark = {back.left, (back.top + back.bottom - mark_h) / 2, back.left + line, 0};
      mark.bottom = mark.top + mark_h;
      canvas_fill(rows, &mark, line / 2, theme.accent, 255);
    }
    const mc_release_info *rel = &g_search.results->items[i];
    for (int col = 0; col < NUM_COLS; col++) {
      wchar_t text[256];
      RECT cell = r;
      int left = 0, right = 0;
      col_span(col, inset, vw - 2 * inset, &left, &right);
      cell.left = left;
      cell.right = right;
      UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (k_cols[col].right ? DT_RIGHT : 0);
      COLORREF color = col == COL_TITLE || theme.high_contrast ? fg : theme.text2;
      canvas_text(rows, cell_text(rel, col, text, 256), -1, &cell, format, s->font, color);
    }
  }
  canvas_draw_image(c, v, rows->px, vw, vh);
}

static void search_paint(canvas *c) {
  sheet *s = &g_search.s;
  int radius = scale(s, CARD_RADIUS);
  paint_card(c, &g_search.card, radius);
  const RECT *v = &g_search.scroll.view;
  int inset = scale(s, LIST_INSET);
  for (int col = 0; col < NUM_COLS; col++) {
    int left = 0, right = 0;
    col_span(col, v->left + inset, v->right - v->left - 2 * inset, &left, &right);
    RECT cell = {left, g_search.head.top, right, g_search.head.bottom};
    UINT format = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX | (k_cols[col].right ? DT_RIGHT : 0);
    canvas_text(c, mc_wstr(k_cols[col].name), -1, &cell, format, s->font, theme.text2);
  }
  RECT line = {g_search.card.left + 1, g_search.head.bottom, g_search.card.right - 1, g_search.head.bottom + 1};
  canvas_fill(c, &line, 0, theme.high_contrast ? theme.text : theme.line, 255);
  if (result_count()) {
    paint_rows(c);
    scroller_paint(&g_search.scroll, c);
    paint_message(s, c, &s->message_rc, FALSE);
  } else {
    paint_message(s, c, v, TRUE);
  }
}

static void invalidate_list(void) {
  InvalidateRect(g_search.s.wnd, &g_search.card, FALSE);
}

static void list_moved(void) {
  /* The view is invalidated already; the row under the pointer changed with it. */
  POINT pt;
  GetCursorPos(&pt);
  ScreenToClient(g_search.s.wnd, &pt);
  g_search.hot = row_at(pt);
}

static void list_select(int i) {
  int n = result_count();
  if (!n) return;
  i = i < 0 ? 0 : i >= n ? n - 1 : i;
  g_search.selected = i;
  int h = scale(&g_search.s, LIST_ROW_H);
  scroller_reveal(&g_search.scroll, i * h, (i + 1) * h);
  invalidate_list();
  uia_selected(&g_search.s.ui, NODE_PARTS + g_search.s.ui_count + i);
}

/* --- The window --- */

static void search_update(void);

static void set_hot(sheet *s, int i) {
  if (i >= 0 && s->parts[i].disabled) i = -1;
  if (i == s->hot) return;
  s->hot = i;
  InvalidateRect(s->wnd, NULL, FALSE);
}

static BOOL is_search(const sheet *s) {
  return s == &g_search.s;
}

static void on_click(sheet *s, int i) {
  part *p = &s->parts[i];
  if (!p->disabled) s->ops->activate(p);
}

/* The keys a text box leaves to the sheet, seen before they reach it. TRUE if they're used. */
static BOOL sheet_key(sheet *s, const MSG *m) {
  if (m->hwnd != s->wnd && !IsChild(s->wnd, m->hwnd)) return FALSE;
  if (m->message == WM_SYSKEYDOWN && m->wParam == VK_F4) { /* there's no system menu to do it */
    s->ops->cancel();
    return TRUE;
  }
  if (m->message != WM_KEYDOWN) return FALSE;
  part *p = s->focus >= 0 ? &s->parts[s->focus] : NULL;
  BOOL field = p && p->edit;
  switch (m->wParam) {
    case VK_TAB:
      s->show_focus = TRUE;
      move_focus(s, GetKeyState(VK_SHIFT) < 0 ? -1 : 1);
      return TRUE;
    case VK_ESCAPE:
      s->ops->cancel();
      return TRUE;
    case VK_RETURN:
      s->show_focus = TRUE;
      if (!field && p && p->kind != PART_LIST) on_click(s, s->focus);
      else s->ops->enter();
      return TRUE;
    case VK_SPACE:
      if (field || !p || p->kind == PART_LIST) return field ? FALSE : TRUE;
      s->show_focus = TRUE;
      on_click(s, s->focus);
      return TRUE;
  }
  if (p && p->kind == PART_LIST) {
    int page = (g_search.scroll.view.bottom - g_search.scroll.view.top) / scale(s, LIST_ROW_H);
    int sel = g_search.selected;
    switch (m->wParam) {
      case VK_UP: list_select(sel - 1); return TRUE;
      case VK_DOWN: list_select(sel + 1); return TRUE;
      case VK_PRIOR: list_select(sel - page); return TRUE;
      case VK_NEXT: list_select(sel + page); return TRUE;
      case VK_HOME: list_select(0); return TRUE;
      case VK_END: list_select(result_count() - 1); return TRUE;
    }
  }
  return FALSE;
}

/* The first file of a drop, malloc'd. */
static wchar_t *dropped_file(HDROP drop) {
  UINT len = DragQueryFileW(drop, 0, NULL, 0);
  wchar_t *path = len ? malloc((len + 1) * sizeof *path) : NULL;
  if (path) DragQueryFileW(drop, 0, path, len + 1);
  return path;
}

static LRESULT CALLBACK wnd_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW *)lp)->lpCreateParams);
    return DefWindowProcW(wnd, msg, wp, lp);
  }
  sheet *s = (sheet *)GetWindowLongPtrW(wnd, GWLP_USERDATA);
  if (!s) return DefWindowProcW(wnd, msg, wp, lp);
  LRESULT result;
  if (s->wnd && uia_message(&s->ui, msg, wp, lp, &result)) return result;
  BOOL list = is_search(s);
  POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  switch (msg) {
    case WM_NCCALCSIZE:
      if (wp) return 0; /* no title bar or borders: the client area is the whole window */
      break;
    case WM_NCHITTEST:
      ScreenToClient(wnd, &pt);
      return part_at(s, pt) < 0 ? HTCAPTION : HTCLIENT; /* it drags by anything else */
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      paint(s, dc);
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC: /* a read-only EDIT's */
      SetTextColor((HDC)wp, msg == WM_CTLCOLOREDIT ? theme.text : theme.text2);
      SetBkColor((HDC)wp, theme.field);
      return (LRESULT)theme.field_brush;
    case WM_COMMAND:
      if (HIWORD(wp) == EN_CHANGE && list) search_update(); /* no title, no Search */
      if (HIWORD(wp) == EN_SETFOCUS || HIWORD(wp) == EN_KILLFOCUS) {
        if (HIWORD(wp) == EN_SETFOCUS)
          for (int i = 0; i < s->nparts; i++)
            if (s->parts[i].edit == (HWND)lp) s->focus = i;
        InvalidateRect(wnd, NULL, FALSE);
      }
      return 0;
    case WM_MOUSEMOVE: {
      if (!s->tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        s->tracking = TrackMouseEvent(&tme);
      }
      if (list && scroller_mouse_move(&g_search.scroll, pt.x, pt.y)) {
        g_search.hot = -1;
      } else if (list) {
        int row = row_at(pt);
        if (row != g_search.hot) {
          g_search.hot = row;
          invalidate_list();
        }
      }
      set_hot(s, part_at(s, pt));
      return 0;
    }
    case WM_MOUSELEAVE:
      s->tracking = FALSE;
      if (list) {
        scroller_mouse_leave(&g_search.scroll);
        g_search.hot = -1;
        invalidate_list();
      }
      set_hot(s, -1);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
      int i = part_at(s, pt);
      if (i < 0 || s->parts[i].disabled) return 0;
      part *p = &s->parts[i];
      if (p->edit) {
        focus_part(s, i);
        return 0;
      }
      if (p->kind == PART_LIST) {
        focus_part(s, i);
        if (scroller_mouse_down(&g_search.scroll, pt.x, pt.y)) return 0;
        int row = row_at(pt);
        if (row >= 0) list_select(row);
        if (row >= 0 && msg == WM_LBUTTONDBLCLK) s->ops->enter();
        return 0;
      }
      s->pressed = i;
      if (can_focus(p)) focus_part(s, i);
      SetCapture(wnd);
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_LBUTTONUP: {
      if (list) scroller_mouse_up(&g_search.scroll);
      int i = s->pressed;
      if (i < 0) return 0;
      ReleaseCapture(); /* WM_CAPTURECHANGED clears pressed */
      if (part_at(s, pt) == i) on_click(s, i);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (list) scroller_capture_changed(&g_search.scroll);
      s->pressed = -1;
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_MOUSEWHEEL:
      if (list) scroller_wheel(&g_search.scroll, GET_WHEEL_DELTA_WPARAM(wp));
      return 0;
    case WM_TIMER:
      if (wp == TIMER_SPIN) {
        s->spin_at = (s->spin_at + 1) % SPIN_DOTS;
        InvalidateRect(wnd, NULL, FALSE);
      } else if (list) {
        scroller_timer(&g_search.scroll, wp);
      }
      return 0;
    case WM_SETFOCUS: /* activated: back into the text box the user left, as a dialog does */
      if (s->focus >= 0 && s->parts[s->focus].edit) SetFocus(s->parts[s->focus].edit);
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    case WM_KILLFOCUS:
      InvalidateRect(wnd, NULL, FALSE); /* the focus ring */
      return 0;
    case WM_CLOSE:
      s->ops->cancel();
      return 0;
    case WM_DROPFILES: { /* from what doesn't drag by OLE; those go to IDropTarget */
      wchar_t *path = dropped_file((HDROP)wp);
      DragFinish((HDROP)wp);
      if (path && s->ops->drop) s->ops->drop(path);
      free(path);
      return 0;
    }
    case WM_JOB_DONE: {
      job *j = (job *)lp;
      if (j == s->job) {
        s->job = NULL;
        j->done(j);
        job_free(j);
      }
      return 0;
    }
    case WM_DPICHANGED: {
      s->dpi = HIWORD(wp);
      update_fonts(s);
      s->ops->layout();
      place_edits(s);
      const RECT *r = (const RECT *)lp;
      place(s, (r->left + r->right) / 2, (r->top + r->bottom) / 2);
      if (list) search_update();
      InvalidateRect(wnd, NULL, FALSE);
      return 0;
    }
    case WM_SETTINGCHANGE:
    case WM_THEMECHANGED:
    case WM_SYSCOLORCHANGE:
      if (theme_is_change(msg, wp, lp)) {
        theme_update(); /* the main window does it too; once more does no harm */
        apply_frame(s);
        theme_apply_controls(wnd);
        InvalidateRect(wnd, NULL, FALSE);
      }
      break;
    case WM_DESTROY:
      /* Nothing can come of it now; if the main window is taking this one with it, the loop
       * in sheet_run() ends. */
      if (s->job) job_abandon(s->job);
      s->job = NULL;
      s->done = TRUE;
      RevokeDragDrop(wnd);
      KillTimer(wnd, TIMER_SPIN);
      uia_destroy(&s->ui);
      s->wnd = NULL;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static BOOL sheet_create(sheet *s, HWND owner, enum mc_str_id title, const sheet_ops *ops, const uia_ops *ui) {
  s->ops = ops;
  s->owner = owner;
  s->title = title;
  s->hot = s->pressed = s->focus = -1;
  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .style = CS_DBLCLKS,
    .lpfnWndProc = wnd_proc,
    .hInstance = GetModuleHandleW(NULL),
    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW),
    .lpszClassName = L"kithsong_edit",
  };
  RegisterClassExW(&wc);
  /* A caption, for Windows 11's shadow and rounded corners, but no system menu, so Windows
   * draws no caption buttons in the extended frame. */
  s->wnd = CreateWindowExW(0, wc.lpszClassName, mc_wstr(title), WS_POPUP | WS_CAPTION, 0, 0, 0, 0, owner, NULL,
                           wc.hInstance, s);
  if (!s->wnd) return FALSE;
  s->dpi = GetDpiForWindow(s->wnd);
  uia_init(&s->ui, s->wnd, ui);
  apply_frame(s);
  update_fonts(s);
  return TRUE;
}

/* A text box for part p, on a surface of its own (see edit_dialog.h). */
static void sheet_add_edit(sheet *s, part *p, DWORD style, int limit, enum mc_str_id cue) {
  p->read_only = (style & ES_READONLY) != 0;
  p->edit = CreateWindowExW(WS_EX_LAYERED, WC_EDITW, NULL, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | style, 0, 0, 0, 0,
                            s->wnd, (HMENU)(INT_PTR)(p->id + 100), GetModuleHandleW(NULL), NULL);
  if (!p->edit) return;
  SetLayeredWindowAttributes(p->edit, 0, 255, LWA_ALPHA);
  SendMessageW(p->edit, WM_SETFONT, (WPARAM)s->font, FALSE);
  if (limit) SendMessageW(p->edit, EM_LIMITTEXT, (WPARAM)limit, 0);
  if (cue) SendMessageW(p->edit, EM_SETCUEBANNER, TRUE, (LPARAM)mc_wstr(cue));
  theme_apply_control(p->edit);
  theme_center_edit(p->edit);
  set_name(s, p->edit, mc_wstr(p->label));
}

/* Lays it out and shows it over its owner, which takes no input until it closes. Returns when
 * s->done is set. */
static void sheet_run(sheet *s) {
  s->ops->layout();
  ui_rebuild(s);
  place_edits(s);
  RECT o = {0};
  if (s->owner) GetWindowRect(s->owner, &o);
  place(s, (o.left + o.right) / 2, (o.top + o.bottom) / 2);
  /* Recalculates the frame, which drops the title bar (WM_NCCALCSIZE). */
  SetWindowPos(s->wnd, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

  if (s->owner) EnableWindow(s->owner, FALSE);
  ShowWindow(s->wnd, SW_SHOW);
  for (int i = 0; i < s->nparts; i++)
    if (s->parts[i].edit) repaint_edit(&s->parts[i]);
  SetForegroundWindow(s->wnd);
  focus_part(s, s->focus >= 0 ? s->focus : 0);
  if (s->parts[s->focus].edit) SendMessageW(s->parts[s->focus].edit, EM_SETSEL, 0, -1);
  MSG msg = {0};
  BOOL quit = FALSE;
  while (!s->done) {
    int got = GetMessageW(&msg, NULL, 0, 0);
    if (got <= 0) {
      quit = got == 0;
      break;
    }
    if (sheet_key(s, &msg)) continue;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (s->owner) EnableWindow(s->owner, TRUE); /* first, so the owner is activated next */
  if (s->wnd) DestroyWindow(s->wnd);
  if (quit) PostQuitMessage((int)msg.wParam);
}

static void sheet_free(sheet *s) {
  free_fonts(s);
  canvas_free(&s->buf);
  if (s->acc) IAccPropServices_Release(s->acc);
  memset(s, 0, sizeof *s);
}

/* --- The search window --- */

static void search_update(void) {
  sheet *s = &g_search.s;
  BOOL busy = s->job != NULL;
  /* The core needs a title to search for. */
  part_of(s, ID_SEARCH)->disabled = busy || !GetWindowTextLengthW(part_of(s, F_TITLE)->edit);
  part_of(s, ID_LIST)->disabled = !result_count();
  part_of(s, ID_USE)->disabled = busy || g_search.selected < 0;
  set_fields_locked(s, busy);
  int h = scale(s, LIST_ROW_H);
  scroller_resize(&g_search.scroll, &g_search.scroll.view, result_count() * h);
  InvalidateRect(s->wnd, NULL, FALSE);
  uia_changed(&s->ui, uia_hash(0, &(int[]){result_count(), g_search.selected}, 2 * sizeof(int)));
}

static void search_layout(void) {
  sheet *s = &g_search.s;
  s->w = scale(s, SEARCH_W);
  int x = scale(s, PAD), col = s->w - 2 * x, gap = scale(s, COL_GAP);
  int y = layout_heading(s);
  part *search = part_of(s, ID_SEARCH);
  int button = button_width(s, search), field = (col - button - 2 * gap) / 2;
  layout_field(s, part_of(s, F_ARTIST), x, y, field);
  int under = layout_field(s, part_of(s, F_TITLE), x + field + gap, y, field);
  SetRect(&search->rc, s->w - x - button, under - scale(s, BUTTON_H), s->w - x, under);
  y = under + scale(s, ROW_GAP) + scale(s, 4);

  SetRect(&g_search.card, x, y, x + col, y + scale(s, LIST_H));
  SetRect(&g_search.head, x, y, x + col, y + scale(s, LIST_HEAD_H));
  part_of(s, ID_LIST)->rc = g_search.card;
  RECT view = {x + 1, g_search.head.bottom + 1, x + col - 1, g_search.card.bottom - scale(s, 2)};
  scroller_resize(&g_search.scroll, &view, result_count() * scale(s, LIST_ROW_H));
  y = g_search.card.bottom + scale(s, ROW_GAP) + scale(s, 4);

  static const int ids[] = {ID_USE, ID_CANCEL};
  int left = layout_buttons(s, y, ids, 2);
  SetRect(&s->message_rc, x, y, left - gap, y + scale(s, BUTTON_H) + s->line_h);
  s->h = y + scale(s, BUTTON_H) + scale(s, BOTTOM);
}

static void run_search(job *j) {
  j->st = mc_release_search(j->lib, j->uid, j->artist, j->title, &j->list);
}

static void search_done(job *j) {
  sheet *s = &g_search.s;
  wchar_t text[512] = L"";
  if (j->st == MC_OK && j->list) {
    g_search.results = j->list;
    j->list = NULL;
    g_search.selected = result_count() ? 0 : -1;
    g_search.searched = TRUE;
  }
  if (j->st == MC_ERR_NET) wcscpy(text, mc_wstr(MC_STR_IMPORT_ERROR_UNREACHABLE));
  else if (j->st != MC_OK) mc_wformat(text, 512, mc_str(MC_STR_EDIT_SEARCH_FAILED), MC_S(mc_status_str(j->st)));
  else if (!result_count()) wcscpy(text, mc_wstr(MC_STR_EDIT_SEARCH_NOTHING));
  set_message(s, text[0] ? text : NULL, j->st != MC_OK, FALSE);
  search_update();
  /* The Search button was pressed and is back: from it, on to what it found. */
  if (result_count() && s->focus >= 0 && s->parts[s->focus].id == ID_SEARCH)
    focus_part(s, (int)(part_of(s, ID_LIST) - s->parts));
}

static void search_start(void) {
  sheet *s = &g_search.s;
  if (s->job || part_of(s, ID_SEARCH)->disabled) return;
  job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->notify = s->wnd;
  j->run = run_search;
  j->done = search_done;
  j->lib = g_search.lib;
  memcpy(j->uid, g_search.uid, sizeof j->uid);
  j->artist = field_text(part_of(s, F_ARTIST));
  j->title = field_text(part_of(s, F_TITLE));
  mc_release_list_free(g_search.results);
  g_search.results = NULL;
  g_search.selected = g_search.hot = -1;
  if (!j->artist || !j->title || !job_start(j)) {
    job_free(j);
    return;
  }
  s->job = j;
  set_message(s, mc_wstr(MC_STR_IMPORT_SEARCHING), FALSE, TRUE);
  search_update();
}

static void run_release_tags(job *j) {
  j->st = mc_release_tags(j->lib, j->uid, j->mb_albumid, &j->tags);
  j->has_tags = j->st == MC_OK;
}

static void use_done(job *j) {
  sheet *s = &g_search.s;
  if (j->st == MC_OK) {
    g_search.out = j->tags; /* the form's now */
    j->has_tags = FALSE;
    g_search.picked = TRUE;
    s->done = TRUE;
    return;
  }
  wchar_t text[512];
  if (j->st == MC_ERR_NET) wcscpy(text, mc_wstr(MC_STR_IMPORT_ERROR_UNREACHABLE));
  else mc_wformat(text, 512, mc_str(MC_STR_EDIT_SEARCH_LOAD_FAILED), MC_S(mc_status_str(j->st)));
  set_message(s, text, TRUE, FALSE);
  search_update();
}

static void use_release(void) {
  sheet *s = &g_search.s;
  if (s->job || g_search.selected < 0 || g_search.selected >= result_count()) return;
  job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->notify = s->wnd;
  j->run = run_release_tags;
  j->done = use_done;
  j->lib = g_search.lib;
  memcpy(j->uid, g_search.uid, sizeof j->uid);
  memcpy(j->mb_albumid, g_search.results->items[g_search.selected].mb_albumid, sizeof j->mb_albumid);
  if (!job_start(j)) {
    job_free(j);
    return;
  }
  s->job = j;
  set_message(s, mc_wstr(MC_STR_EDIT_SEARCH_LOADING), FALSE, TRUE);
  search_update();
}

static void search_activate(part *p) {
  switch (p->id) {
    case ID_SEARCH: search_start(); break;
    case ID_USE: use_release(); break;
    case ID_CANCEL:
    case ID_CLOSE: g_search.s.done = TRUE; break;
  }
}

/* Enter: in a box searches, on the list uses what's selected. */
static void search_enter(void) {
  sheet *s = &g_search.s;
  if (s->focus >= 0 && s->parts[s->focus].kind == PART_LIST) use_release();
  else search_start();
}

/* Closing doesn't wait for a search: its job is abandoned, and finishes on its own. */
static void search_cancel(void) {
  g_search.s.done = TRUE;
}

static const sheet_ops k_search_ops = {
  .layout = search_layout,
  .activate = search_activate,
  .enter = search_enter,
  .cancel = search_cancel,
  .paint = search_paint,
};

static int list_node(void) {
  sheet *s = &g_search.s;
  return ui_node_of(s, (int)(part_of(s, ID_LIST) - s->parts));
}

static BOOL search_ui_node(int i, uia_node *n) {
  sheet *s = &g_search.s;
  int first = NODE_PARTS + s->ui_count;
  if (i >= first) { /* a row */
    int row = i - first;
    if (row >= result_count()) return FALSE;
    const mc_release_info *r = &g_search.results->items[row];
    n->parent = list_node();
    n->type = UIA_ListItemControlTypeId;
    n->flags = UIA_SELECTABLE | (row == g_search.selected ? UIA_SELECTED : 0);
    RECT rc = row_rect(row);
    IntersectRect(&n->rect, &rc, &g_search.scroll.view);
    wchar_t title[256], artist[256], rest[256], cell[64];
    wstr_from_utf8(title, 256, r->title);
    wstr_from_utf8(artist, 256, r->artist);
    rest[0] = L'\0';
    for (int c = COL_YEAR; c < NUM_COLS; c++) {
      cell_text(r, c, cell, 64);
      if (!cell[0]) continue;
      if (rest[0]) wcscat(rest, L", ");
      wcscat(rest, cell);
    }
    char *t = wstr_to_utf8(title), *a = wstr_to_utf8(artist), *d = wstr_to_utf8(rest);
    mc_wformat(n->name, 512, mc_str(MC_STR_EDIT_RESULT_NAME), MC_S(t ? t : ""), MC_S(a ? a : ""), MC_S(d ? d : ""));
    free(t);
    free(a);
    free(d);
    return TRUE;
  }
  if (!ui_node(s, i, n)) return FALSE;
  if (i == list_node()) {
    n->type = UIA_ListControlTypeId;
    n->flags = (n->flags & UIA_FOCUSABLE) | UIA_SELECTION;
    n->first = first;
    n->count = result_count();
    n->automation_id = L"Releases";
    lstrcpynW(n->name, mc_wstr(MC_STR_EDIT_SEARCH_RESULTS), 512);
  }
  return TRUE;
}

static int search_ui_focus(void) {
  sheet *s = &g_search.s;
  if (s->focus >= 0 && s->parts[s->focus].kind == PART_LIST && g_search.selected >= 0)
    return NODE_PARTS + s->ui_count + g_search.selected;
  return ui_focus(s);
}

static int search_ui_at(POINT pt) {
  sheet *s = &g_search.s;
  int row = row_at(pt);
  return row >= 0 ? NODE_PARTS + s->ui_count + row : ui_at(s, pt);
}

static int search_ui_selection(int i) {
  sheet *s = &g_search.s;
  return i == list_node() && g_search.selected >= 0 ? NODE_PARTS + s->ui_count + g_search.selected : UIA_NONE;
}

static void search_ui_select(int i) {
  list_select(i - NODE_PARTS - g_search.s.ui_count);
}

static void search_ui_invoke(int i) {
  ui_invoke(&g_search.s, i);
}

static void search_ui_set_focus(int i) {
  ui_set_focus(&g_search.s, i);
}

static const uia_ops k_search_ui = {
  .node = search_ui_node,
  .focus = search_ui_focus,
  .at = search_ui_at,
  .selection = search_ui_selection,
  .select = search_ui_select,
  .invoke = search_ui_invoke,
  .set_focus = search_ui_set_focus,
};

/* Searches MusicBrainz for album uid from artist and title, over owner. TRUE with *out set
 * (release with mc_album_tags_clear()) if the user used a release. */
static BOOL search_dialog(HWND owner, mc_library *lib, const char *uid, const char *artist, const char *title,
                          mc_album_tags *out) {
  memset(&g_search, 0, sizeof g_search);
  sheet *s = &g_search.s;
  g_search.lib = lib;
  snprintf(g_search.uid, sizeof g_search.uid, "%s", uid);
  g_search.selected = g_search.hot = -1;
  if (!sheet_create(s, owner, MC_STR_EDIT_SEARCH_TITLE, &k_search_ops, &k_search_ui)) return FALSE;
  g_search.scroll.wnd = s->wnd;
  g_search.scroll.moved = list_moved;

  sheet_add_edit(s, add_part(s, PART_FIELD, F_ARTIST, MC_STR_ARTIST, NULL), 0, 0, 0);
  sheet_add_edit(s, add_part(s, PART_FIELD, F_TITLE, MC_STR_TITLE, NULL), 0, 0, 0);
  add_part(s, PART_BUTTON, ID_SEARCH, MC_STR_SEARCH, L"Search");
  add_part(s, PART_LIST, ID_LIST, MC_STR_EDIT_SEARCH_RESULTS, L"Releases");
  add_part(s, PART_ACCENT, ID_USE, MC_STR_EDIT_SEARCH_USE, L"Use");
  add_part(s, PART_BUTTON, ID_CANCEL, MC_STR_CANCEL, L"Cancel");
  add_part(s, PART_CLOSE, ID_CLOSE, MC_STR_CLOSE, L"Close");
  set_field_text(part_of(s, F_ARTIST), artist);
  set_field_text(part_of(s, F_TITLE), title);
  s->focus = 0;
  /* What the button says is what the user came for, so it's under way as the window opens,
   * if there's a title to search for. */
  search_layout();
  search_update();
  search_start();
  sheet_run(s);

  BOOL picked = g_search.picked;
  if (picked) *out = g_search.out;
  mc_release_list_free(g_search.results);
  canvas_free(&g_search.rows);
  sheet_free(s);
  memset(&g_search, 0, sizeof g_search);
  return picked;
}

/* --- The form --- */

static struct {
  sheet s;
  edit_kind kind;
  mc_library *lib;
  char uid[MC_UID_STRLEN];
  char *saved[NUM_FIELDS]; /* what's in the library, as text; NULL for a field not on the form */
  mc_album_flags saved_flags;
  RECT checks_card;
  /* The cover. */
  IWICImagingFactory *wic;
  HBITMAP art;            /* what's shown: premultiplied, rounded; NULL shows the placeholder */
  const uint32_t *art_px;
  int art_px_size;
  BOOL art_ok;            /* the album has a folder here to put a cover in */
  unsigned char *new_art; /* the image that Save would use */
  size_t new_art_len;
  BOOL reset_art;         /* Automatic cover was pressed, for Save to do */
  BOOL art_saved;
  BOOL dragging;          /* a file is over the window */
  BOOL in_use; /* Save found a file open while something plays: Stop Playing and Retry shows */
  BOOL loaded; /* the tags were read, so there's something to save */
  BOOL track_mix; /* a track alone on its album, or on none: the form has Continuous mix */
  BOOL changed;
} g_form;

/* A number as a text box shows it: nothing for 0, which is unknown. */
static char *number_text(int n) {
  char buf[16] = "";
  if (n) snprintf(buf, sizeof buf, "%d", n);
  return _strdup(buf);
}

static BOOL is_number(int field) {
  return field == F_YEAR || field == F_ORIGINAL_YEAR || field == F_TRACK_NO || field == F_DISC_NO;
}

static char *dup_text(const char *s) {
  return _strdup(s ? s : "");
}

static void free_saved(void) {
  for (int i = 0; i < NUM_FIELDS; i++) {
    free(g_form.saved[i]);
    g_form.saved[i] = NULL;
  }
}

/* The tags and flags from the core, as text. */
static mc_status load(void) {
  char **f = g_form.saved;
  mc_status st = MC_ERR_INVALID_ARG;
  if (g_form.kind == KIND_ALBUM) {
    mc_album_tags t = {0};
    st = mc_album_tags_get(g_form.lib, g_form.uid, &t);
    if (st != MC_OK) return st;
    f[F_TITLE] = dup_text(t.title);
    f[F_ARTIST] = dup_text(t.artist);
    f[F_YEAR] = number_text(t.year);
    f[F_ORIGINAL_YEAR] = number_text(t.original_year);
    f[F_MB] = dup_text(t.mb_albumid);
    mc_album_tags_clear(&t);
    st = mc_album_flags_get(g_form.lib, g_form.uid, &g_form.saved_flags);
  } else if (g_form.kind == KIND_ARTIST) {
    mc_artist_tags t = {0};
    st = mc_artist_tags_get(g_form.lib, g_form.uid, &t);
    if (st != MC_OK) return st;
    f[F_NAME] = dup_text(t.name);
    f[F_SORT_NAME] = dup_text(t.sort_name);
    mc_artist_tags_clear(&t);
  } else {
    mc_track_tags t = {0};
    st = mc_track_tags_get(g_form.lib, g_form.uid, &t);
    if (st != MC_OK) return st;
    f[F_TITLE] = dup_text(t.title);
    f[F_ARTIST] = dup_text(t.artist);
    f[F_TRACK_NO] = number_text(t.track_no);
    f[F_DISC_NO] = number_text(t.disc_no);
    f[F_YEAR] = number_text(t.year);
    f[F_GENRE] = dup_text(t.genre);
    mc_track_tags_clear(&t);
    mc_track_mix m;
    st = mc_track_mix_get(g_form.lib, g_form.uid, &m);
    g_form.track_mix = st == MC_OK && m.can_set;
    g_form.saved_flags.mix = m.mix;
  }
  return st;
}

/* Shows the image at path; FALSE if it can't be shown. Decoded here, as one small image is. */
static BOOL show_art(const wchar_t *path) {
  sheet *s = &g_form.s;
  if (!g_form.wic)
    CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                     (void **)&g_form.wic);
  const uint32_t *px = NULL;
  int size = scale(s, ART_SIZE);
  HBITMAP bmp = g_form.wic ? cover_decode_file(g_form.wic, path, size, scale(s, RADIUS), &px) : NULL;
  if (!bmp) return FALSE;
  if (g_form.art) DeleteObject(g_form.art);
  g_form.art = bmp;
  g_form.art_px = px;
  g_form.art_px_size = size;
  return TRUE;
}

/* What the album has, and whether it could have another. */
static void load_art(void) {
  char *path = NULL;
  g_form.art_ok = mc_album_art_get(g_form.lib, g_form.uid, &path) == MC_OK;
  wchar_t *w = path ? wstr_dup_from_utf8(path) : NULL;
  if (w) show_art(w);
  free(w);
  mc_free(path);
}

/* On a thread of its own: the file's bytes, if they make a cover. */
static void run_read_art(job *j) {
  j->st = MC_ERR_IO;
  FILE *f = _wfopen(j->art_path, L"rb");
  if (!f) return;
  long long size = _fseeki64(f, 0, SEEK_END) == 0 ? _ftelli64(f) : -1;
  rewind(f);
  if (size > MC_ALBUM_ART_MAX_BYTES) j->st = MC_ERR_FORMAT;
  else if (size > 0 && (j->art = malloc((size_t)size)) && fread(j->art, 1, (size_t)size, f) == (size_t)size)
    j->st = mc_album_art_check(j->art, j->art_len = (size_t)size);
  fclose(f);
}

static void form_update(void);

static void read_art_done(job *j) {
  sheet *s = &g_form.s;
  if (j->st == MC_OK && show_art(j->art_path)) {
    free(g_form.new_art);
    g_form.new_art = j->art;
    g_form.new_art_len = j->art_len;
    g_form.reset_art = FALSE;
    j->art = NULL;
    part_of(s, ID_ART_COVER)->label = MC_STR_EDIT_ART_NEW_NAME;
    set_message(s, mc_wstr(MC_STR_EDIT_ART_NEW), FALSE, FALSE);
  } else {
    set_message(s, mc_wstr(MC_STR_EDIT_ART_BAD), TRUE, FALSE);
  }
  form_update();
}

/* Whether a file can be taken as the cover now: not with Save or a read under way, nor while
 * the search window is over this one. */
static BOOL can_take_art(void) {
  sheet *s = &g_form.s;
  return g_form.kind == KIND_ALBUM && g_form.loaded && !s->job && IsWindowEnabled(s->wnd);
}

/* A file picked or dropped: it's read, and if it makes a cover it's shown, for Save to keep. */
static void take_art(const wchar_t *path) {
  sheet *s = &g_form.s;
  if (!can_take_art()) return;
  if (!g_form.art_ok) {
    set_message(s, mc_wstr(MC_STR_EDIT_ART_NO_FILES), TRUE, FALSE);
    return;
  }
  job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->notify = s->wnd;
  j->run = run_read_art;
  j->done = read_art_done;
  j->art_path = _wcsdup(path);
  if (!j->art_path || !job_start(j)) {
    job_free(j);
    return;
  }
  s->job = j;
  set_message(s, mc_wstr(MC_STR_EDIT_ART_READING), FALSE, TRUE);
  form_update();
}

static void pick_art(void) {
  sheet *s = &g_form.s;
  wchar_t *path = pick_file(s->wnd, mc_wstr(MC_STR_EDIT_ART_PICK_TITLE),
                            mc_wstr(MC_STR_EDIT_ART_FILE_TYPES), L"*.jpg;*.jpeg;*.png");
  if (path && s->wnd) take_art(path); /* it may be gone with the main window meanwhile */
  free(path);
}

/* Dragging a file over the window. The form is one at a time, so the target is too. */
static void set_dragging(BOOL on) {
  if (g_form.dragging == on) return;
  g_form.dragging = on;
  InvalidateRect(g_form.s.wnd, NULL, FALSE);
}

static HRESULT STDMETHODCALLTYPE drop_query(IDropTarget *t, REFIID iid, void **out) {
  if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, &IID_IDropTarget)) {
    *out = NULL;
    return E_NOINTERFACE;
  }
  *out = t;
  return S_OK;
}

static ULONG STDMETHODCALLTYPE drop_ref(IDropTarget *t) {
  (void)t;
  return 1; /* static */
}

static FORMATETC drop_format(void) {
  return (FORMATETC){CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
}

static HRESULT STDMETHODCALLTYPE drop_enter(IDropTarget *t, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) {
  (void)t;
  (void)keys;
  (void)pt;
  FORMATETC format = drop_format();
  BOOL files = can_take_art() && IDataObject_QueryGetData(data, &format) == S_OK;
  *effect = files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  set_dragging(files);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_over(IDropTarget *t, DWORD keys, POINTL pt, DWORD *effect) {
  (void)t;
  (void)keys;
  (void)pt;
  *effect = g_form.dragging ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_leave(IDropTarget *t) {
  (void)t;
  set_dragging(FALSE);
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE drop_drop(IDropTarget *t, IDataObject *data, DWORD keys, POINTL pt, DWORD *effect) {
  (void)t;
  (void)keys;
  (void)pt;
  *effect = g_form.dragging ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  set_dragging(FALSE);
  FORMATETC format = drop_format();
  STGMEDIUM medium;
  if (*effect == DROPEFFECT_NONE || FAILED(IDataObject_GetData(data, &format, &medium))) return S_OK;
  wchar_t *path = dropped_file((HDROP)medium.hGlobal);
  ReleaseStgMedium(&medium);
  if (path) take_art(path);
  free(path);
  return S_OK;
}

static IDropTargetVtbl k_drop_vtbl = {drop_query, drop_ref, drop_ref, drop_enter, drop_over, drop_leave, drop_drop};
static IDropTarget k_drop_target = {&k_drop_vtbl};

/* On the save's thread: the tags, then the flags, each if it changed, then a new cover, which
 * goes last as tags can move the album's folder. */
static void run_save(job *j) {
  char **f = j->text;
  mc_status st = MC_OK;
  if (j->set_tags) {
    if (j->kind == KIND_ALBUM) {
      mc_album_tags t = {f[F_TITLE], f[F_ARTIST], atoi(f[F_YEAR]), atoi(f[F_ORIGINAL_YEAR]), f[F_MB]};
      st = mc_album_tags_set(j->lib, j->uid, &t);
    } else if (j->kind == KIND_ARTIST) {
      mc_artist_tags t = {f[F_NAME], f[F_SORT_NAME]};
      st = j->merge_into[0] ? mc_artist_merge(j->lib, j->uid, j->merge_into) : mc_artist_tags_set(j->lib, j->uid, &t);
    } else {
      mc_track_tags t = {f[F_TITLE], f[F_ARTIST], atoi(f[F_TRACK_NO]), atoi(f[F_DISC_NO]), atoi(f[F_YEAR]), f[F_GENRE]};
      st = mc_track_tags_set(j->lib, j->uid, &t);
    }
    j->tags_saved = st == MC_OK;
  }
  if (st == MC_OK && j->set_flags) {
    st = j->kind == KIND_TRACK ? mc_track_mix_set(j->lib, j->uid, j->flags.mix)
                               : mc_album_flags_set(j->lib, j->uid, &j->flags);
    j->flags_saved = st == MC_OK;
  }
  if (st == MC_OK && j->set_art) {
    j->art_tried = TRUE;
    st = j->reset_art ? mc_album_art_reset(j->lib, j->uid) : mc_album_art_set(j->lib, j->uid, j->art, j->art_len);
    j->art_saved = st == MC_OK;
  }
  j->st = st;
}

static void form_layout(void);

static void form_update(void) {
  sheet *s = &g_form.s;
  BOOL busy = s->job != NULL, off = busy || !g_form.loaded;
  part_of(s, ID_SAVE)->disabled = off;
  part_of(s, ID_STOP_RETRY)->disabled = off || !g_form.in_use;
  part_of(s, ID_CANCEL)->disabled = busy;
  part_of(s, ID_CLOSE)->disabled = busy;
  part *p = part_of(s, ID_SEARCH_MB);
  if (p) p->disabled = off;
  if ((p = part_of(s, ID_NO_SCROBBLE))) p->disabled = off;
  if ((p = part_of(s, ID_MIX))) p->disabled = off;
  if ((p = part_of(s, ID_ART_CHANGE))) p->disabled = off || !g_form.art_ok;
  if ((p = part_of(s, ID_ART_AUTO))) p->disabled = off || !g_form.art_ok || g_form.reset_art;
  if ((p = part_of(s, ID_ART_COVER))) p->disabled = off || !g_form.art_ok;
  set_fields_locked(s, off);
  InvalidateRect(s->wnd, NULL, FALSE);
}

static void save_done(job *j) {
  sheet *s = &g_form.s;
  /* What did get saved is saved, whatever came next: a second try leaves it out. */
  if (j->tags_saved)
    for (int i = 0; i < NUM_FIELDS; i++) {
      free(g_form.saved[i]);
      g_form.saved[i] = j->text[i];
      j->text[i] = NULL;
    }
  if (j->flags_saved) g_form.saved_flags = j->flags;
  if (j->art_saved) {
    free(g_form.new_art);
    g_form.new_art = NULL;
    g_form.reset_art = FALSE;
    g_form.art_saved = TRUE;
  }
  g_form.changed |= j->tags_saved || j->flags_saved || j->art_saved;
  if (j->st == MC_OK) {
    s->done = TRUE;
    return;
  }
  wchar_t text[512];
  if (j->st == MC_ERR_NOT_FOUND) wcscpy(text, mc_wstr(MC_STR_EDIT_GONE));
  else mc_wformat(text, 512, mc_str(MC_STR_EDIT_SAVE_FAILED), MC_S(mc_status_str(j->st)));
  set_message(s, text, TRUE, FALSE);
  /* Nothing changed, and it's most likely the track playing; a cover isn't played. */
  g_form.in_use = j->st == MC_ERR_IN_USE && !j->art_tried && playback_queue_count() > 0;
  form_layout();
  form_update();
}

/* Save: nothing to do closes the dialog, as there's nothing to lose. */
static void save(void) {
  sheet *s = &g_form.s;
  if (s->job || !g_form.loaded) return;
  job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->notify = s->wnd;
  j->run = run_save;
  j->done = save_done;
  j->lib = g_form.lib;
  j->kind = g_form.kind;
  memcpy(j->uid, g_form.uid, sizeof j->uid);
  for (int i = 0; i < NUM_FIELDS; i++) {
    part *p = part_of(s, i);
    if (!p) continue;
    j->text[i] = field_text(p);
    if (j->text[i] && is_number(i)) { /* 007 is 7, and 0 unknown */
      char *n = number_text(atoi(j->text[i]));
      free(j->text[i]);
      j->text[i] = n;
    }
    if (!j->text[i]) {
      job_free(j);
      return;
    }
    if (strcmp(j->text[i], g_form.saved[i])) j->set_tags = TRUE;
  }
  if (g_form.kind == KIND_ALBUM) {
    j->flags.no_scrobble = part_of(s, ID_NO_SCROBBLE)->checked;
    j->flags.mix = part_of(s, ID_MIX)->checked;
    j->set_flags = !j->flags.no_scrobble != !g_form.saved_flags.no_scrobble || !j->flags.mix != !g_form.saved_flags.mix;
  } else if (g_form.track_mix) {
    j->flags.mix = part_of(s, ID_MIX)->checked;
    j->set_flags = !j->flags.mix != !g_form.saved_flags.mix;
  } else if (g_form.kind == KIND_ARTIST && strcmp(j->text[F_NAME], g_form.saved[F_NAME]) &&
             mc_artist_same_name(g_form.lib, g_form.uid, j->text[F_NAME], j->merge_into) == MC_OK) {
    wchar_t text[512];
    mc_wformat(text, 512, mc_str(MC_STR_EDIT_MERGE_TEXT), MC_S(j->text[F_NAME]));
    int answer = MessageBoxW(s->wnd, text, mc_wstr(MC_STR_EDIT_MERGE_TITLE), MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDCANCEL) {
      job_free(j);
      return;
    }
    if (answer == IDNO) j->merge_into[0] = '\0';
  }
  if (g_form.reset_art) {
    j->reset_art = TRUE;
    j->set_art = TRUE;
  }
  if (g_form.new_art) {
    if (!(j->art = malloc(g_form.new_art_len))) {
      job_free(j);
      return;
    }
    memcpy(j->art, g_form.new_art, g_form.new_art_len);
    j->art_len = g_form.new_art_len;
    j->set_art = TRUE;
  }
  if (!j->set_tags && !j->set_flags && !j->set_art) {
    job_free(j);
    s->done = TRUE;
    return;
  }
  if (!job_start(j)) {
    job_free(j);
    return;
  }
  s->job = j;
  set_message(s, mc_wstr(MC_STR_EDIT_SAVING), FALSE, TRUE);
  g_form.in_use = FALSE;
  form_layout();
  form_update();
}

/* Search MusicBrainz…: the release used fills in the form, for Save to keep. */
static void search_mb(void) {
  sheet *s = &g_form.s;
  char *artist = field_text(part_of(s, F_ARTIST)), *title = field_text(part_of(s, F_TITLE));
  mc_album_tags t = {0};
  BOOL picked = artist && title && search_dialog(s->wnd, g_form.lib, g_form.uid, artist, title, &t);
  free(artist);
  free(title);
  if (!s->wnd) { /* gone with the main window meanwhile */
    if (picked) mc_album_tags_clear(&t);
    return;
  }
  if (picked) {
    char *year = number_text(t.year), *original = number_text(t.original_year);
    set_field_text(part_of(s, F_TITLE), t.title);
    set_field_text(part_of(s, F_ARTIST), t.artist);
    set_field_text(part_of(s, F_YEAR), year);
    set_field_text(part_of(s, F_ORIGINAL_YEAR), original);
    set_field_text(part_of(s, F_MB), t.mb_albumid);
    free(year);
    free(original);
    mc_album_tags_clear(&t);
    set_message(s, NULL, FALSE, FALSE);
  }
  focus_part(s, (int)(part_of(s, picked ? ID_SAVE : ID_SEARCH_MB) - s->parts));
}

static void form_cancel(void) {
  if (!g_form.s.job) g_form.s.done = TRUE; /* a save can't be taken back halfway */
}

static void form_activate(part *p) {
  sheet *s = &g_form.s;
  switch (p->id) {
    case ID_SAVE: save(); break;
    case ID_STOP_RETRY: /* lets go of the file now, so the save can have it */
      focus_part(s, (int)(part_of(s, ID_SAVE) - s->parts));
      playback_clear();
      save();
      break;
    case ID_CANCEL:
    case ID_CLOSE: form_cancel(); break;
    case ID_SEARCH_MB: search_mb(); break;
    case ID_ART_COVER:
    case ID_ART_CHANGE: pick_art(); break;
    case ID_ART_AUTO:
      free(g_form.new_art);
      g_form.new_art = NULL;
      g_form.reset_art = TRUE;
      set_message(s, mc_wstr(MC_STR_EDIT_ART_AUTOMATIC_NOTE), FALSE, FALSE);
      form_update();
      break;
    case ID_NO_SCROBBLE:
    case ID_MIX:
      p->checked = !p->checked;
      uia_toggled(&s->ui, ui_node_of(s, (int)(p - s->parts)), p->checked);
      InvalidateRect(s->wnd, &p->rc, FALSE);
      break;
  }
}

/* The checks in a card: two for an album, a track's one. Returns the y under it. */
static int layout_checks(sheet *s, int x, int y, int w) {
  int pad = scale(s, CARD_PAD), size = scale(s, CHECK_SIZE), gap = scale(s, CHECK_GAP);
  int row_gap = scale(s, CHECK_ROW_GAP);
  int ids[] = {ID_NO_SCROBBLE, ID_MIX};
  y += pad / 2;
  for (int i = g_form.kind == KIND_TRACK; i < 2; i++) {
    part *p = part_of(s, ids[i]);
    int text_x = x + pad + size + gap, text_w = x + w - pad - text_x;
    int note_h = text_height(s->font, mc_wstr(p->note), text_w), h = scale(s, 6) * 2 + size + note_h;
    SetRect(&p->rc, x + pad / 2, y, x + w - pad / 2, y + h);
    SetRect(&p->label_rc, text_x, y + scale(s, 6), x + w - pad, y + scale(s, 6) + size);
    SetRect(&p->note_rc, text_x, p->label_rc.bottom, x + w - pad, p->label_rc.bottom + note_h);
    y += h + row_gap;
  }
  return y + pad / 2 - row_gap;
}

static void form_layout(void) {
  sheet *s = &g_form.s;
  s->w = scale(s, EDIT_W);
  int x = scale(s, PAD), col = s->w - 2 * x, gap = scale(s, COL_GAP), row = scale(s, ROW_GAP);
  int y = layout_heading(s);
  if (g_form.kind == KIND_ALBUM) {
    /* The cover and its button, and beside them the rows they're as tall as. */
    int art = scale(s, ART_SIZE), side = x + art + scale(s, ART_GAP), rest = x + col - side;
    int half = (rest - gap) / 2, art_top = y;
    y = layout_field(s, part_of(s, F_TITLE), side, y, rest) + row;
    y = layout_field(s, part_of(s, F_ARTIST), side, y, rest) + row;
    layout_field(s, part_of(s, F_YEAR), side, y, half);
    y = layout_field(s, part_of(s, F_ORIGINAL_YEAR), side + half + gap, y, rest - half - gap);
    SetRect(&part_of(s, ID_ART_COVER)->rc, x, art_top, x + art, art_top + art);
    SetRect(&part_of(s, ID_ART_CHANGE)->rc, x, y - scale(s, BUTTON_H), x + art, y);
    part *automatic = part_of(s, ID_ART_AUTO);
    if (automatic) {
      y += scale(s, BUTTON_GAP) + scale(s, BUTTON_H);
      SetRect(&automatic->rc, x, y - scale(s, BUTTON_H), x + art, y);
    }
    y += row;
    part *search = part_of(s, ID_SEARCH_MB);
    int button = button_width(s, search);
    int under = layout_field(s, part_of(s, F_MB), x, y, col - button - scale(s, BUTTON_GAP));
    SetRect(&search->rc, x + col - button, under - scale(s, BUTTON_H), x + col, under);
    y = under + row + scale(s, 6);
    int top = y;
    y = layout_checks(s, x, y, col);
    SetRect(&g_form.checks_card, x, top, x + col, y);
  } else if (g_form.kind == KIND_ARTIST) {
    y = layout_field(s, part_of(s, F_NAME), x, y, col) + row;
    y = layout_field(s, part_of(s, F_SORT_NAME), x, y, col);
  } else {
    int third = (col - 2 * gap) / 3;
    y = layout_field(s, part_of(s, F_TITLE), x, y, col) + row;
    y = layout_field(s, part_of(s, F_ARTIST), x, y, col) + row;
    layout_field(s, part_of(s, F_TRACK_NO), x, y, third);
    layout_field(s, part_of(s, F_DISC_NO), x + third + gap, y, third);
    y = layout_field(s, part_of(s, F_YEAR), x + 2 * (third + gap), y, col - 2 * (third + gap)) + row;
    y = layout_field(s, part_of(s, F_GENRE), x, y, col);
    if (g_form.track_mix) {
      int top = y += row + scale(s, 6);
      y = layout_checks(s, x, y, col);
      SetRect(&g_form.checks_card, x, top, x + col, y);
    }
  }
  y += row;
  SetRect(&s->message_rc, x, y, x + col, y + MESSAGE_LINES * s->line_h);
  y = s->message_rc.bottom + row;
  static const int ids[] = {ID_STOP_RETRY, ID_SAVE, ID_CANCEL};
  SetRectEmpty(&part_of(s, ID_STOP_RETRY)->rc);
  layout_buttons(s, y, g_form.in_use ? ids : ids + 1, g_form.in_use ? 3 : 2);
  s->h = y + scale(s, BUTTON_H) + scale(s, BOTTOM);
}

/* The cover, or a place for one. A file over the window fills it with what a drop would do, and
 * a ring runs inside the window's edge, as the whole window takes it: that's solid, with the
 * text boxes on surfaces of their own, so it reads the same over Mica in dark and light. */
static void paint_cover(canvas *c) {
  sheet *s = &g_form.s;
  const part *p = part_of(s, ID_ART_COVER);
  int radius = scale(s, RADIUS);
  UINT centred = k_para | DT_CENTER;
  if (g_form.dragging) {
    RECT all = {0, 0, s->w, s->h};
    InflateRect(&all, -scale(s, DROP_INSET), -scale(s, DROP_INSET));
    canvas_frame(c, &all, scale(s, CARD_RADIUS), scale(s, 2), theme.accent, 255);
    canvas_fill(c, &p->rc, radius, theme.accent, 255);
    RECT text = p->rc;
    InflateRect(&text, -scale(s, 8), 0);
    text.top += (p->rc.bottom - p->rc.top - text_height(s->font, mc_wstr(MC_STR_EDIT_ART_DROP), text.right - text.left)) / 2;
    canvas_text(c, mc_wstr(MC_STR_EDIT_ART_DROP), -1, &text, centred, s->font, theme.on_accent);
    return;
  }
  if (g_form.art_px) {
    canvas_draw_image(c, &p->rc, g_form.art_px, g_form.art_px_size, g_form.art_px_size);
  } else {
    if (theme.high_contrast) canvas_fill(c, &p->rc, radius, theme.bg, 255);
    else canvas_fill(c, &p->rc, radius, ink(), theme.dark ? 15 : 6);
    RECT text = p->rc;
    text.top += (p->rc.bottom - p->rc.top - s->line_h) / 2;
    canvas_text(c, mc_wstr(MC_STR_EDIT_ART_NONE), -1, &text, centred, s->font, theme.text2);
  }
  int i = (int)(p - s->parts);
  COLORREF edge = state_of(s, i) != STATE_NORMAL ? theme.accent : theme.high_contrast ? theme.text : theme.line;
  canvas_frame(c, &p->rc, radius, 1, edge, 255);
}

static void form_paint(canvas *c) {
  sheet *s = &g_form.s;
  if (!IsRectEmpty(&g_form.checks_card)) paint_card(c, &g_form.checks_card, scale(s, CARD_RADIUS));
  if (g_form.kind == KIND_ALBUM) paint_cover(c);
  paint_message(s, c, &s->message_rc, FALSE);
}

static const sheet_ops k_form_ops = {
  .layout = form_layout,
  .activate = form_activate,
  .enter = save,
  .cancel = form_cancel,
  .paint = form_paint,
  .drop = take_art,
};

static BOOL form_ui_node(int i, uia_node *n) {
  return ui_node(&g_form.s, i, n);
}

static int form_ui_focus(void) {
  return ui_focus(&g_form.s);
}

static int form_ui_at(POINT pt) {
  return ui_at(&g_form.s, pt);
}

static void form_ui_invoke(int i) {
  ui_invoke(&g_form.s, i);
}

static void form_ui_set_focus(int i) {
  ui_set_focus(&g_form.s, i);
}

static const uia_ops k_form_ui = {
  .node = form_ui_node,
  .focus = form_ui_focus,
  .at = form_ui_at,
  .invoke = form_ui_invoke,
  .set_focus = form_ui_set_focus,
};

static void add_field(int field, enum mc_str_id label, DWORD style, int limit, enum mc_str_id cue) {
  sheet *s = &g_form.s;
  sheet_add_edit(s, add_part(s, PART_FIELD, field, label, NULL), style, limit, cue);
}

static void add_check(int id, enum mc_str_id label, enum mc_str_id note, const wchar_t *automation_id, int checked) {
  part *p = add_part(&g_form.s, PART_CHECK, id, label, automation_id);
  p->note = note;
  p->checked = checked != 0;
}

static void build_form(void) {
  enum { YEAR_DIGITS = 4, NUMBER_DIGITS = 3 };
  sheet *s = &g_form.s;
  if (g_form.kind == KIND_ALBUM) {
    add_field(F_TITLE, MC_STR_TITLE, 0, 0, 0);
    add_field(F_ARTIST, MC_STR_EDIT_ALBUM_ARTIST, 0, 0, 0);
    add_field(F_YEAR, MC_STR_COL_YEAR, ES_NUMBER, YEAR_DIGITS, 0);
    add_field(F_ORIGINAL_YEAR, MC_STR_EDIT_ORIGINAL_YEAR, ES_NUMBER, YEAR_DIGITS, 0);
    add_part(s, PART_COVER, ID_ART_COVER, g_form.art ? MC_STR_EDIT_ART_COVER : MC_STR_EDIT_ART_NONE_NAME,
             L"AlbumCover")->note = MC_STR_EDIT_ART_HELP;
    add_part(s, PART_BUTTON, ID_ART_CHANGE, MC_STR_EDIT_ART_CHANGE, L"ChangeCover");
    if (g_form.saved_flags.art_chosen)
      add_part(s, PART_BUTTON, ID_ART_AUTO, MC_STR_EDIT_ART_AUTOMATIC, L"AutomaticCover");
    add_field(F_MB, MC_STR_EDIT_MB_RELEASE, ES_READONLY, 0, MC_STR_EDIT_MB_NONE);
    add_part(s, PART_BUTTON, ID_SEARCH_MB, MC_STR_EDIT_MB_SEARCH, L"SearchMusicBrainz");
    add_check(ID_NO_SCROBBLE, MC_STR_EDIT_NO_SCROBBLE, MC_STR_EDIT_NO_SCROBBLE_TEXT, L"NoScrobble",
              g_form.saved_flags.no_scrobble);
    add_check(ID_MIX, MC_STR_EDIT_MIX, MC_STR_EDIT_MIX_TEXT, L"ContinuousMix", g_form.saved_flags.mix);
  } else if (g_form.kind == KIND_ARTIST) {
    add_field(F_NAME, MC_STR_NAME, 0, 0, 0);
    add_field(F_SORT_NAME, MC_STR_EDIT_SORT_NAME, 0, 0, 0);
  } else {
    add_field(F_TITLE, MC_STR_TITLE, 0, 0, 0);
    add_field(F_ARTIST, MC_STR_ARTIST, 0, 0, MC_STR_EDIT_TRACK_ARTIST_CUE);
    add_field(F_TRACK_NO, MC_STR_EDIT_TRACK_NUMBER, ES_NUMBER, NUMBER_DIGITS, 0);
    add_field(F_DISC_NO, MC_STR_EDIT_DISC_NUMBER, ES_NUMBER, NUMBER_DIGITS, 0);
    add_field(F_YEAR, MC_STR_COL_YEAR, ES_NUMBER, YEAR_DIGITS, 0);
    add_field(F_GENRE, MC_STR_EDIT_GENRE, 0, 0, 0);
    if (g_form.track_mix)
      add_check(ID_MIX, MC_STR_EDIT_MIX, MC_STR_EDIT_MIX_TRACK_TEXT, L"ContinuousMix",
                g_form.saved_flags.mix);
  }
  add_part(s, PART_BUTTON, ID_STOP_RETRY, MC_STR_EDIT_STOP_RETRY, L"StopAndRetry");
  add_part(s, PART_ACCENT, ID_SAVE, MC_STR_SAVE, L"Save");
  add_part(s, PART_BUTTON, ID_CANCEL, MC_STR_CANCEL, L"Cancel");
  add_part(s, PART_CLOSE, ID_CLOSE, MC_STR_CLOSE, L"Close");
}

static const enum mc_str_id k_titles[] = {
  [KIND_ALBUM] = MC_STR_EDIT_ALBUM_TITLE,
  [KIND_ARTIST] = MC_STR_EDIT_ARTIST_TITLE,
  [KIND_TRACK] = MC_STR_EDIT_TRACK_TITLE,
};

static BOOL edit_dialog(HWND owner, mc_library *lib, edit_kind kind, const char *uid) {
  if (!lib || !uid || g_form.s.wnd) return FALSE;
  memset(&g_form, 0, sizeof g_form);
  sheet *s = &g_form.s;
  g_form.kind = kind;
  g_form.lib = lib;
  snprintf(g_form.uid, sizeof g_form.uid, "%s", uid);
  if (!sheet_create(s, owner, k_titles[kind], &k_form_ops, &k_form_ui)) return FALSE;

  mc_status st = load();
  g_form.loaded = st == MC_OK;
  /* OLE's drag and drop, for the state while a file's over the window; WM_DROPFILES without. */
  BOOL ole = FALSE;
  if (kind == KIND_ALBUM && g_form.loaded) {
    load_art();
    DragAcceptFiles(s->wnd, TRUE);
    ole = SUCCEEDED(OleInitialize(NULL));
    if (ole) RegisterDragDrop(s->wnd, &k_drop_target);
  }
  build_form();
  for (int i = 0; i < NUM_FIELDS; i++) {
    part *p = part_of(s, i);
    if (p) set_field_text(p, g_form.saved[i]);
  }
  /* A failure leaves an empty form with only Cancel to press, and says why. */
  form_layout();
  form_update();
  if (g_form.loaded) {
    s->focus = 0;
  } else {
    wchar_t text[512];
    if (st == MC_ERR_NOT_FOUND) wcscpy(text, mc_wstr(MC_STR_EDIT_GONE));
    else mc_wformat(text, 512, mc_str(MC_STR_EDIT_LOAD_FAILED), MC_S(mc_status_str(st)));
    set_message(s, text, TRUE, FALSE);
    s->focus = (int)(part_of(s, ID_CANCEL) - s->parts);
  }
  sheet_run(s);

  if (ole) OleUninitialize();
  /* The views key what they've decoded on the folder, and that's the same. */
  if (g_form.art_saved) {
    now_playing_art_changed();
    track_list_art_changed();
    artist_view_art_changed();
  }
  BOOL changed = g_form.changed;
  free_saved();
  free(g_form.new_art);
  if (g_form.art) DeleteObject(g_form.art);
  if (g_form.wic) IWICImagingFactory_Release(g_form.wic);
  sheet_free(s);
  memset(&g_form, 0, sizeof g_form);
  return changed;
}

BOOL edit_dialog_album(HWND owner, mc_library *lib, const char *uid) {
  return edit_dialog(owner, lib, KIND_ALBUM, uid);
}

BOOL edit_dialog_artist(HWND owner, mc_library *lib, const char *uid) {
  return edit_dialog(owner, lib, KIND_ARTIST, uid);
}

BOOL edit_dialog_track(HWND owner, mc_library *lib, const char *uid) {
  return edit_dialog(owner, lib, KIND_TRACK, uid);
}
