#define _CRT_RAND_S /* rand_s(), for a shuffled list's seed */
#define COBJMACROS
#include "smart_editor.h"

#include <commctrl.h>
#include <oleacc.h>
#include <uxtheme.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <yyjson.h>

#include "resource.h"
#include "theme.h"
#include "wstr.h"

#define WM_APP_PREVIEW (WM_APP + 40) /* lParam: preview_result *, from the preview thread */
#define WM_APP_REVEAL (WM_APP + 41)  /* to the panel, wParam: a child that got the focus */

enum { TIMER_PREVIEW = 1, PREVIEW_DELAY_MS = 250 };

/* Caps that keep the arrays fixed; the + buttons grey out at them. */
enum { MAX_FIELDS = 32, MAX_ITEMS = 32, MAX_KIDS = 32, MAX_SORTS = 8, MAX_VALUE = 256, MAX_NAME = 255 };

enum { MODE_ALL, MODE_ANY, MODE_NONE };
enum { UNIT_DAYS, UNIT_WEEKS, UNIT_MONTHS };

/* Layout, in dialog units. */
enum {
  MARGIN = 7,
  GAP = 4,      /* between controls on a row */
  ROW = 14,     /* a row's controls */
  ROW_GAP = 4,  /* between rows */
  HINT_H = 9,   /* a line under a condition */
  RAIL_X = 3,   /* the top level's rail, from the panel's left */
  DEPTH0_X = 9, /* the top level's rows */
  INDENT = 12,  /* a group's rows, further in; its rail is half way */
  FIELD_W = 76,
  OP_W = 88,
  NUM_W = 40,  /* a number beside a unit, or each end of "between" */
  AND_W = 16,
  UNIT_W = 50,
  COUNT_W = 38,
  REMOVE_W = 14,
  MODE_W = 40,
  SORT_W = 130,
  BUTTON_W = 54,
  LEFT_MIN = 330,     /* the conditions' panel, at its narrowest */
  PREVIEW_MIN = 150,  /* the preview beside it, at its narrowest; narrower windows put it below */
  SUMMARY_H = 20,     /* the sentence: two lines */
  MIN_W = 340,
  MIN_H = 280,
};

/* The formats a condition offers: the importer's file extensions (tagger.c), which is what
 * files.format holds. A rule naming another one keeps it as an extra entry. */
static const struct {
  const char *value;
  const wchar_t *label;
} k_formats[] = {
  {"flac", L"FLAC"}, {"mp3", L"MP3"},     {"m4a", L"M4A (AAC, ALAC)"}, {"aac", L"AAC"},  {"opus", L"Opus"},
  {"ogg", L"Ogg"},   {"wav", L"WAV"},     {"aiff", L"AIFF"},           {"aif", L"AIF"},  {"wv", L"WavPack"},
  {"ape", L"APE"},   {"wma", L"WMA"},     {"dsf", L"DSF"},             {"mp4", L"MP4"},  {"oga", L"OGA"},
  {"mpc", L"MPC"},
};
#define FORMAT_COUNT ((int)(sizeof k_formats / sizeof k_formats[0]))

/* A condition: its controls hold its values, read back whenever the rule is rebuilt. */
typedef struct cond {
  int field; /* into ed.fields */
  int op;    /* into ed.fields[field].ops */
  HWND field_box, op_box, value, and_label, value2, unit, date, choice, count, remove, hint;
  char *extra;   /* the choice's extra entry: a deleted playlist's or group's uid, or another format */
  BOOL invalid;  /* its value is missing or can't be read */
} cond;

/* A top-level entry: a condition, or a group of them. */
typedef struct item {
  cond *c; /* NULL for a group */
  int mode;
  cond *kids[MAX_KIDS];
  int nkids;
  HWND label, mode_box, of_label, count, remove, add;
} item;

typedef struct sort_key {
  HWND label, box, remove; /* box's item data: sortable field index * 2 + desc */
} sort_key;

/* The preview thread's work: the rule without its limit, and for an all-of rule the rules of
 * its first top-level conditions (1, 1..2, ...), whose matches it counts. */
typedef struct preview_job {
  UINT gen;
  mc_smart_subject subject;
  char *rule;
  char **prefixes;
  int nprefixes;
} preview_job;

typedef struct preview_result {
  UINT gen;
  BOOL ok;
  mc_track_list *tracks;
  mc_album_list *albums;
  int *counts; /* nprefixes of them; -1 where a count failed */
  int ncounts;
} preview_result;

typedef struct worker {
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE wake;
  preview_job *job; /* the next one; a newer one replaces it */
  BOOL quit;
  HWND dlg;
  char *dir; /* the library's, for the thread's own connection */
  HANDLE thread;
} worker;

/* The open editor. Modal, so there's one at a time. */
static struct {
  HWND dlg, panel, tip;
  HWND name_label, name, preview_label, list, summary, counts, read_only_text;
  HWND match_label, match_box, match_of, add_cond, add_group;
  HWND sort_add, sorted, sorted_label, shuffled, shuffled_label, limit_check, limit_label, limit, limit_unit;
  HFONT font, icon_font;
  HFONT own_font; /* the font for a DPI moved to, which ed.font then is; NULL for the dialog's */
  UINT dpi;
  int base_x, base_y; /* the dialog's base units, which dx() and dy() scale by */
  IAccPropServices *acc;

  mc_library *lib;
  mc_smart_subject subject;
  BOOL creating;
  char uid[MC_UID_STRLEN]; /* editing: the list's */
  wchar_t *old_name;
  char *old_rule;
  BOOL read_only;
  BOOL changed;

  mc_smart_field fields[MAX_FIELDS];
  int nfields;
  int sortable[MAX_FIELDS]; /* the sortable fields, into fields */
  int nsortable;
  mc_playlist_list *playlists;
  mc_album_group_list *groups;

  int mode;
  item *items[MAX_ITEMS];
  int nitems;
  sort_key sorts[MAX_SORTS];
  int nsorts;
  long long seed;
  BOOL has_seed;
  BOOL limit_invalid;

  int scroll, content_h; /* the panel's, in pixels */
  BOOL wide;             /* the preview is beside the panel */
  struct rail {
    RECT r;
    int level;
  } rails[MAX_ITEMS + 1];
  int nrails;

  char *rule; /* as it stands; NULL while it's incomplete */
  worker *w;
  UINT gen;
  BOOL counted; /* a preview has come back for the rule as it stands */
  mc_track_list *tracks;
  mc_album_list *albums;
  int total, shown;
  int counts_of[MAX_ITEMS];
  int ncounts;
} ed;

/* --- Little helpers --- */

/* Dialog units, as MapDialogRect() has them, but from base units of our own: the dialog
 * manager's stay those it started with, as the editor rescales itself for a new DPI. */
static int dx(int n) {
  return MulDiv(n, ed.base_x, 4);
}

static int dy(int n) {
  return MulDiv(n, ed.base_y, 8);
}

static wchar_t *widen(const char *u) {
  if (!u) return NULL;
  int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  if (w) MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n);
  return w;
}

/* A window's text, without surrounding spaces, as UTF-8 (malloc'd; NULL if out of memory). */
static char *text_of(HWND w) {
  wchar_t buf[MAX_VALUE + 1];
  GetWindowTextW(w, buf, MAX_VALUE + 1);
  wchar_t *start = buf, *end = buf + wcslen(buf);
  while (*start && iswspace(*start)) start++;
  while (end > start && iswspace(end[-1])) end--;
  *end = L'\0';
  return wstr_to_utf8(start);
}

/* A whole number of at most 12 digits, ignoring spaces around it. */
static BOOL parse_int(HWND w, long long *out) {
  char *s = text_of(w);
  BOOL ok = s && *s && strlen(s) <= 12;
  long long n = 0;
  for (const char *p = s; ok && *p; p++) {
    ok = *p >= '0' && *p <= '9';
    n = n * 10 + (*p - '0');
  }
  free(s);
  if (ok) *out = n;
  return ok;
}

/* A length: m:ss or h:mm:ss, or a bare number of minutes. */
static BOOL parse_duration(HWND w, long long *out) {
  char *s = text_of(w);
  long long parts[3] = {0};
  int n = 0, digits = 0;
  BOOL ok = s && *s;
  for (const char *p = s; ok && *p; p++) {
    if (*p == ':') {
      ok = digits && ++n < 3;
      digits = 0;
    } else {
      ok = *p >= '0' && *p <= '9' && digits < 6;
      if (ok) parts[n] = parts[n] * 10 + (*p - '0'), digits++;
    }
  }
  ok = ok && digits;
  free(s);
  if (!ok) return FALSE;
  if (n == 0) *out = parts[0] * 60;
  else if (n == 1 && parts[1] < 60) *out = parts[0] * 60 + parts[1];
  else if (n == 2 && parts[1] < 60 && parts[2] < 60) *out = parts[0] * 3600 + parts[1] * 60 + parts[2];
  else return FALSE;
  return TRUE;
}

static void format_duration(long long s, wchar_t *out, size_t n) {
  if (s >= 3600) swprintf(out, n, L"%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
  else swprintf(out, n, L"%lld:%02lld", s / 60, s % 60);
}

#define EPOCH_DIFF_MS 11644473600000LL /* 1601 to 1970 */

/* A day in local time, at its midnight, as Unix ms. */
static long long date_to_ms(const SYSTEMTIME *day) {
  SYSTEMTIME local = *day, utc;
  local.wHour = local.wMinute = local.wSecond = local.wMilliseconds = 0;
  FILETIME ft;
  if (!TzSpecificLocalTimeToSystemTime(NULL, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return 0;
  ULARGE_INTEGER t = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
  return (long long)(t.QuadPart / 10000) - EPOCH_DIFF_MS;
}

static BOOL ms_to_date(long long ms, SYSTEMTIME *day) {
  if (ms < -EPOCH_DIFF_MS) return FALSE;
  ULARGE_INTEGER t = {.QuadPart = (ULONGLONG)(ms + EPOCH_DIFF_MS) * 10000};
  FILETIME ft = {t.LowPart, t.HighPart};
  SYSTEMTIME utc;
  return FileTimeToSystemTime(&ft, &utc) && SystemTimeToTzSpecificLocalTime(NULL, &utc, day);
}

/* A name for screen readers, where a control has no label of its own. */
static void set_name(HWND w, const wchar_t *name) {
  if (!ed.acc)
    CoCreateInstance(&CLSID_AccPropServices, NULL, CLSCTX_INPROC_SERVER, &IID_IAccPropServices, (void **)&ed.acc);
  if (ed.acc && w) IAccPropServices_SetHwndPropStr(ed.acc, w, OBJID_CLIENT, CHILDID_SELF, PROPID_ACC_NAME, name);
}

static void clear_name(HWND w) {
  MSAAPROPID props[] = {PROPID_ACC_NAME};
  if (ed.acc && w) IAccPropServices_ClearHwndProps(ed.acc, w, OBJID_CLIENT, CHILDID_SELF, props, 1);
}

static void destroy(HWND *w) {
  if (!*w) return;
  if (ed.tip) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = ed.panel, .uId = (UINT_PTR)*w};
    SendMessageW(ed.tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
  }
  clear_name(*w);
  DestroyWindow(*w);
  *w = NULL;
}

static const mc_smart_field *field_of(const cond *c) {
  return &ed.fields[c->field];
}

static const char *op_of(const cond *c) {
  return field_of(c)->ops[c->op];
}

static BOOL type_is(const cond *c, const char *type) {
  return !strcmp(field_of(c)->type, type);
}

static BOOL op_is(const cond *c, const char *op) {
  return !strcmp(op_of(c), op);
}

static int find_field(const char *name) {
  for (int i = 0; name && i < ed.nfields; i++)
    if (!strcmp(ed.fields[i].name, name)) return i;
  return -1;
}

static int find_op(int field, const char *op) {
  for (int i = 0; op && ed.fields[field].ops[i]; i++)
    if (!strcmp(ed.fields[field].ops[i], op)) return i;
  return -1;
}

/* A new condition's operator for its field's type: the one most rules want. */
static int default_op(int field) {
  const char *type = ed.fields[field].type;
  const char *op = !strcmp(type, "text")       ? "contains"
                   : !strcmp(type, "int")      ? "="
                   : !strcmp(type, "duration") ? ">"
                   : !strcmp(type, "date")     ? "within"
                                               : "is";
  int i = find_op(field, op);
  return i >= 0 ? i : 0;
}

/* The field's name inside a sentence, for the text fields' hint. */
static enum mc_str_id in_text_id(const char *field) {
  static const struct {
    const char *name;
    enum mc_str_id id;
  } k[] = {
    {"title", MC_STR_SMART_FIELD_TITLE_IN_TEXT},   {"artist", MC_STR_SMART_FIELD_ARTIST_IN_TEXT},
    {"album_artist", MC_STR_SMART_FIELD_ALBUM_ARTIST_IN_TEXT}, {"album", MC_STR_SMART_FIELD_ALBUM_IN_TEXT},
    {"genre", MC_STR_SMART_FIELD_GENRE_IN_TEXT},
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(k[i].name, field)) return k[i].id;
  return MC_STR_COUNT;
}

/* --- Controls --- */

static LRESULT CALLBACK child_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

/* A control in the panel, hidden until the layout places it. */
static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style) {
  HWND w = CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, ed.panel, NULL, GetModuleHandleW(NULL), NULL);
  if (!w) return NULL;
  SendMessageW(w, WM_SETFONT, (WPARAM)ed.font, FALSE);
  SetWindowSubclass(w, child_proc, 0, 0);
  if (!_wcsicmp(cls, WC_COMBOBOXW)) SendMessageW(w, CB_SETITEMHEIGHT, (WPARAM)-1, dy(ROW) - 6);
  if (!_wcsicmp(cls, WC_EDITW)) theme_center_edit(w);
  theme_apply_control(w);
  return w;
}

static HWND make_label(const wchar_t *text) {
  return make(WC_STATICW, text, SS_NOPREFIX | SS_CENTERIMAGE);
}

static HWND make_combo(const wchar_t *name) {
  HWND w = make(WC_COMBOBOXW, NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
  set_name(w, name);
  return w;
}

static void add_tip(HWND w) {
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_IDISHWND | TTF_SUBCLASS, .hwnd = ed.panel,
                    .uId = (UINT_PTR)w, .lpszText = LPSTR_TEXTCALLBACKW};
  if (ed.tip && w) SendMessageW(ed.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

/* The ✕ on a condition, group or sort order: the icon font's Cancel, named for screen readers. */
static HWND make_remove(enum mc_str_id name) {
  HWND w = make(WC_BUTTONW, L"\xE711", BS_PUSHBUTTON | WS_TABSTOP);
  SendMessageW(w, WM_SETFONT, (WPARAM)ed.icon_font, FALSE);
  set_name(w, mc_wstr(name));
  add_tip(w);
  return w;
}

static void combo_add(HWND box, const wchar_t *text, LPARAM data) {
  int i = (int)SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)text);
  if (i >= 0) SendMessageW(box, CB_SETITEMDATA, i, data);
}

static void combo_add_utf8(HWND box, const char *text, LPARAM data) {
  wchar_t *w = widen(text ? text : "");
  if (w) combo_add(box, w, data);
  free(w);
}

static LPARAM combo_data(HWND box) {
  int i = (int)SendMessageW(box, CB_GETCURSEL, 0, 0);
  return i >= 0 ? SendMessageW(box, CB_GETITEMDATA, i, 0) : -1;
}

static void combo_select_data(HWND box, LPARAM data) {
  int n = (int)SendMessageW(box, CB_GETCOUNT, 0, 0);
  for (int i = 0; i < n; i++)
    if (SendMessageW(box, CB_GETITEMDATA, i, 0) == data) {
      SendMessageW(box, CB_SETCURSEL, i, 0);
      return;
    }
}

static void fill_match_box(HWND box) {
  /* Not sorted: CB_ADDSTRING keeps the order, as the combo has no CBS_SORT. */
  combo_add(box, mc_wstr(MC_STR_WINDOWS_SMART_MATCH_ALL), MODE_ALL);
  combo_add(box, mc_wstr(MC_STR_WINDOWS_SMART_MATCH_ANY), MODE_ANY);
  combo_add(box, mc_wstr(MC_STR_WINDOWS_SMART_MATCH_NONE), MODE_NONE);
}

/* The op picker for c's field, with c->op selected. */
static void fill_ops(cond *c) {
  SendMessageW(c->op_box, CB_RESETCONTENT, 0, 0);
  const mc_smart_field *f = field_of(c);
  for (int i = 0; f->ops[i]; i++) combo_add_utf8(c->op_box, mc_smart_op_label(f->type, f->ops[i]), i);
  SendMessageW(c->op_box, CB_SETCURSEL, c->op, 0);
}

/* The value picker for c's field's type: Yes and No, the playlists, the album groups or the
 * formats, with c->extra as one more if it's set. Item data: an index, or -2 for extra. */
static void fill_choice(cond *c) {
  SendMessageW(c->choice, CB_RESETCONTENT, 0, 0);
  if (type_is(c, "bool")) {
    combo_add(c->choice, mc_wstr(MC_STR_SMART_VALUE_YES), 1);
    combo_add(c->choice, mc_wstr(MC_STR_SMART_VALUE_NO), 0);
  } else if (type_is(c, "playlist")) {
    for (int i = 0; ed.playlists && i < ed.playlists->count; i++)
      combo_add_utf8(c->choice, ed.playlists->items[i].name, i);
  } else if (type_is(c, "group")) {
    for (int i = 0; ed.groups && i < ed.groups->count; i++) combo_add_utf8(c->choice, ed.groups->items[i].name, i);
  } else if (type_is(c, "format")) {
    for (int i = 0; i < FORMAT_COUNT; i++) combo_add(c->choice, k_formats[i].label, i);
  }
  if (c->extra && type_is(c, "format")) {
    wchar_t *w = widen(c->extra);
    for (wchar_t *p = w; p && *p; p++) *p = towupper(*p);
    if (w) combo_add(c->choice, w, -2);
    free(w);
  } else if (c->extra) {
    combo_add(c->choice, mc_wstr(MC_STR_WINDOWS_SMART_DELETED_MEMBER), -2);
  }
  SendMessageW(c->choice, CB_SETCURSEL, 0, 0);
}

/* The value box takes digits only for a number, and shows "m:ss" when empty for a length. */
static void style_value(cond *c) {
  BOOL digits = type_is(c, "int") || type_is(c, "date");
  HWND boxes[] = {c->value, c->value2};
  for (int i = 0; i < 2; i++) {
    LONG style = GetWindowLongW(boxes[i], GWL_STYLE);
    SetWindowLongW(boxes[i], GWL_STYLE, digits ? style | ES_NUMBER : style & ~ES_NUMBER);
    SendMessageW(boxes[i], EM_SETCUEBANNER, FALSE,
                 (LPARAM)(type_is(c, "duration") ? mc_wstr(MC_STR_WINDOWS_SMART_DURATION_CUE) : L""));
  }
}

static cond *new_cond(int field) {
  cond *c = calloc(1, sizeof *c);
  if (!c) return NULL;
  c->field = field;
  c->op = default_op(field);
  c->field_box = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_FIELD_NAME));
  for (int i = 0; i < ed.nfields; i++) combo_add_utf8(c->field_box, mc_smart_field_label(ed.fields[i].name), i);
  SendMessageW(c->field_box, CB_SETCURSEL, field, 0);
  c->op_box = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_OP_NAME));
  add_tip(c->op_box);
  c->value = make(WC_EDITW, NULL, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL);
  SendMessageW(c->value, EM_LIMITTEXT, MAX_VALUE, 0);
  set_name(c->value, mc_wstr(MC_STR_WINDOWS_SMART_VALUE_NAME));
  c->and_label = make_label(mc_wstr(MC_STR_WINDOWS_SMART_BETWEEN_AND));
  c->value2 = make(WC_EDITW, NULL, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL);
  SendMessageW(c->value2, EM_LIMITTEXT, MAX_VALUE, 0);
  set_name(c->value2, mc_wstr(MC_STR_WINDOWS_SMART_VALUE_HIGH_NAME));
  c->unit = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_UNIT_NAME));
  combo_add(c->unit, mc_wstr(MC_STR_WINDOWS_SMART_UNIT_DAYS), UNIT_DAYS);
  combo_add(c->unit, mc_wstr(MC_STR_WINDOWS_SMART_UNIT_WEEKS), UNIT_WEEKS);
  combo_add(c->unit, mc_wstr(MC_STR_WINDOWS_SMART_UNIT_MONTHS), UNIT_MONTHS);
  SendMessageW(c->unit, CB_SETCURSEL, UNIT_DAYS, 0);
  c->date = make(DATETIMEPICK_CLASSW, NULL, WS_TABSTOP | DTS_SHORTDATEFORMAT);
  set_name(c->date, mc_wstr(MC_STR_WINDOWS_SMART_VALUE_NAME));
  c->choice = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_VALUE_NAME));
  c->count = make(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE | SS_RIGHT);
  c->remove = make_remove(MC_STR_WINDOWS_SMART_REMOVE_CONDITION);
  c->hint = make(WC_STATICW, NULL, SS_NOPREFIX | SS_LEFTNOWORDWRAP);
  fill_ops(c);
  fill_choice(c);
  style_value(c);
  if (type_is(c, "date")) SetWindowTextW(c->value, L"30");
  return c;
}

static void free_cond(cond *c) {
  if (!c) return;
  HWND *ws[] = {&c->field_box, &c->op_box, &c->value, &c->and_label, &c->value2, &c->unit,
                &c->date,      &c->choice, &c->count, &c->remove,    &c->hint};
  for (size_t i = 0; i < sizeof ws / sizeof ws[0]; i++) destroy(ws[i]);
  free(c->extra);
  free(c);
}

static item *new_group(int mode) {
  item *it = calloc(1, sizeof *it);
  if (!it) return NULL;
  it->mode = mode;
  it->label = make_label(mc_wstr(MC_STR_WINDOWS_SMART_GROUP_MATCHING));
  it->mode_box = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_GROUP_MODE_NAME));
  fill_match_box(it->mode_box);
  combo_select_data(it->mode_box, mode);
  it->of_label = make_label(mc_wstr(MC_STR_WINDOWS_SMART_OF_THESE));
  it->count = make(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE | SS_RIGHT);
  it->remove = make_remove(MC_STR_WINDOWS_SMART_REMOVE_GROUP);
  it->add = make(WC_BUTTONW, mc_wstr(MC_STR_WINDOWS_SMART_ADD_CONDITION), BS_PUSHBUTTON | WS_TABSTOP);
  return it;
}

static item *new_item(cond *c) {
  item *it = c ? calloc(1, sizeof *it) : NULL;
  if (it) it->c = c;
  else free_cond(c);
  return it;
}

static void free_item(item *it) {
  if (!it) return;
  free_cond(it->c);
  for (int i = 0; i < it->nkids; i++) free_cond(it->kids[i]);
  HWND *ws[] = {&it->label, &it->mode_box, &it->of_label, &it->count, &it->remove, &it->add};
  for (size_t i = 0; i < sizeof ws / sizeof ws[0]; i++) destroy(ws[i]);
  free(it);
}

/* A sort order picker: every sortable field both ways, in mc_smart_order_label()'s words. */
static sort_key *add_sort(int sortable_index, BOOL desc) {
  if (ed.nsorts >= MAX_SORTS) return NULL;
  sort_key *k = &ed.sorts[ed.nsorts++];
  k->label = make_label(mc_wstr(ed.nsorts == 1 ? MC_STR_WINDOWS_SMART_SORT_BY : MC_STR_WINDOWS_SMART_SORT_THEN));
  k->box = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_SORT_NAME));
  for (int i = 0; i < ed.nsortable; i++)
    for (int d = 0; d < 2; d++) {
      const char *label = mc_smart_order_label(ed.fields[ed.sortable[i]].name, d);
      if (label) combo_add_utf8(k->box, label, i * 2 + d);
    }
  combo_select_data(k->box, sortable_index * 2 + (desc ? 1 : 0));
  k->remove = make_remove(MC_STR_WINDOWS_SMART_REMOVE_SORT);
  return k;
}

static void remove_sort(int i) {
  destroy(&ed.sorts[i].label);
  destroy(&ed.sorts[i].box);
  destroy(&ed.sorts[i].remove);
  memmove(&ed.sorts[i], &ed.sorts[i + 1], (size_t)(ed.nsorts - i - 1) * sizeof ed.sorts[0]);
  ed.nsorts--;
  /* The first one says "Sort by", the rest "then". */
  if (ed.nsorts) SetWindowTextW(ed.sorts[0].label, mc_wstr(MC_STR_WINDOWS_SMART_SORT_BY));
}

/* --- The rule as JSON --- */

/* c's condition, or NULL (marking c invalid) if its value is missing or can't be read. */
static yyjson_mut_val *cond_json(yyjson_mut_doc *d, cond *c) {
  const mc_smart_field *f = field_of(c);
  const char *op = op_of(c);
  yyjson_mut_val *v = NULL;
  BOOL ok = TRUE, none = op_is(c, "empty") || op_is(c, "not_empty");
  long long n, m;
  if (none) {
  } else if (type_is(c, "text")) {
    char *s = text_of(c->value);
    ok = s && *s;
    if (ok) v = yyjson_mut_strcpy(d, s);
    free(s);
  } else if (type_is(c, "int") || type_is(c, "duration")) {
    BOOL dur = type_is(c, "duration");
    ok = dur ? parse_duration(c->value, &n) : parse_int(c->value, &n);
    if (ok && op_is(c, "between")) {
      ok = dur ? parse_duration(c->value2, &m) : parse_int(c->value2, &m);
      if (ok) {
        v = yyjson_mut_arr(d);
        yyjson_mut_arr_add_sint(d, v, n);
        yyjson_mut_arr_add_sint(d, v, m);
      }
    } else if (ok) {
      v = yyjson_mut_sint(d, n);
    }
  } else if (type_is(c, "date")) {
    if (op_is(c, "within") || op_is(c, "not_within")) {
      static const int k_unit_days[] = {1, 7, 30};
      LPARAM unit = combo_data(c->unit);
      ok = parse_int(c->value, &n) && unit >= 0 && unit <= UNIT_MONTHS && n * k_unit_days[unit] <= 1000000;
      if (ok) {
        v = yyjson_mut_obj(d);
        yyjson_mut_obj_add_sint(d, v, "days", n * k_unit_days[unit]);
      }
    } else {
      SYSTEMTIME day;
      ok = DateTime_GetSystemtime(c->date, &day) == GDT_VALID;
      if (ok) v = yyjson_mut_sint(d, date_to_ms(&day));
    }
  } else if (type_is(c, "bool")) {
    v = yyjson_mut_bool(d, combo_data(c->choice) != 0);
  } else {
    LPARAM i = combo_data(c->choice);
    const char *s = i == -2                    ? c->extra
                    : i < 0                    ? NULL
                    : type_is(c, "playlist")   ? (ed.playlists && i < ed.playlists->count ? ed.playlists->items[i].uid : NULL)
                    : type_is(c, "group")      ? (ed.groups && i < ed.groups->count ? ed.groups->items[i].uid : NULL)
                    : i < FORMAT_COUNT         ? k_formats[i].value
                                               : NULL;
    ok = s != NULL;
    if (ok) v = yyjson_mut_strcpy(d, s);
  }
  c->invalid = !ok;
  if (!ok) return NULL;
  yyjson_mut_val *obj = yyjson_mut_obj(d);
  yyjson_mut_obj_add_str(d, obj, "field", f->name);
  yyjson_mut_obj_add_str(d, obj, "op", op);
  if (v) yyjson_mut_obj_add_val(d, obj, "value", v);
  return obj;
}

/* {"all": [...]}, {"any": [...]} or, for none, {"not": {"any": [...]}}. */
static yyjson_mut_val *mode_json(yyjson_mut_doc *d, int mode, yyjson_mut_val *list) {
  yyjson_mut_val *obj = yyjson_mut_obj(d);
  yyjson_mut_obj_add_val(d, obj, mode == MODE_ALL ? "all" : "any", list);
  if (mode != MODE_NONE) return obj;
  yyjson_mut_val *not = yyjson_mut_obj(d);
  yyjson_mut_obj_add_val(d, not, "not", obj);
  return not;
}

static yyjson_mut_val *item_json(yyjson_mut_doc *d, item *it, BOOL *ok) {
  if (it->c) {
    yyjson_mut_val *v = cond_json(d, it->c);
    *ok &= v != NULL;
    return v;
  }
  yyjson_mut_val *list = yyjson_mut_arr(d);
  for (int i = 0; i < it->nkids; i++) {
    yyjson_mut_val *v = cond_json(d, it->kids[i]);
    *ok &= v != NULL;
    if (v) yyjson_mut_arr_append(list, v);
  }
  return mode_json(d, it->mode, list);
}

/* The rule as it stands (malloc'd), or NULL if anything in it is missing. prefix >= 0 makes
 * instead the rule matching all of the first prefix + 1 top-level entries, unordered and
 * unlimited, for the counts beside them. Every condition's invalid flag is brought up to
 * date along the way (by a whole rule, prefix -1). */
static char *build(int prefix, BOOL with_limit) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  if (!d) return NULL;
  yyjson_mut_val *root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, root);
  yyjson_mut_obj_add_int(d, root, "v", 1);
  BOOL ok = TRUE;
  int n = prefix >= 0 ? prefix + 1 : ed.nitems;
  int mode = prefix >= 0 ? MODE_ALL : ed.mode;
  if (n || mode != MODE_ALL) {
    yyjson_mut_val *list = yyjson_mut_arr(d);
    for (int i = 0; i < n; i++) {
      yyjson_mut_val *v = item_json(d, ed.items[i], &ok);
      if (v) yyjson_mut_arr_append(list, v);
    }
    yyjson_mut_obj_add_val(d, root, "match", mode_json(d, mode, list));
  }
  if (prefix < 0) {
    yyjson_mut_val *order = yyjson_mut_arr(d);
    for (int i = 0; i < ed.nsorts; i++) {
      LPARAM k = combo_data(ed.sorts[i].box);
      if (k < 0 || k / 2 >= ed.nsortable) continue;
      yyjson_mut_val *key = yyjson_mut_obj(d);
      yyjson_mut_obj_add_str(d, key, "field", ed.fields[ed.sortable[k / 2]].name);
      yyjson_mut_obj_add_str(d, key, "dir", k % 2 ? "desc" : "asc");
      yyjson_mut_arr_append(order, key);
    }
    if (SendMessageW(ed.shuffled, BM_GETCHECK, 0, 0) == BST_CHECKED) {
      yyjson_mut_val *key = yyjson_mut_obj(d);
      yyjson_mut_obj_add_str(d, key, "field", "random");
      yyjson_mut_obj_add_sint(d, key, "seed", ed.seed);
      yyjson_mut_arr_append(order, key);
    }
    if (yyjson_mut_arr_size(order)) yyjson_mut_obj_add_val(d, root, "order", order);
    long long limit = 0;
    ed.limit_invalid = SendMessageW(ed.limit_check, BM_GETCHECK, 0, 0) == BST_CHECKED &&
                       (!parse_int(ed.limit, &limit) || limit < 1);
    ok &= !ed.limit_invalid;
    if (with_limit && limit > 0 && !ed.limit_invalid) yyjson_mut_obj_add_sint(d, root, "limit", limit);
  }
  char *json = ok ? yyjson_mut_write(d, 0, NULL) : NULL;
  yyjson_mut_doc_free(d);
  return json;
}

/* --- Reading a rule into the editor --- */

static long long json_int(yyjson_val *v) {
  return yyjson_is_sint(v) ? yyjson_get_sint(v) : yyjson_is_uint(v) ? (long long)yyjson_get_uint(v) : 0;
}

/* A "not" around a condition, as the negative operator that means the same: missing values
 * count as false either way, so only the ops the core writes as another's negation map. */
static const char *negated_op(const char *op) {
  static const char *const k[][2] = {{"is", "is_not"},         {"contains", "not_contains"}, {"empty", "not_empty"},
                                     {"=", "!="},              {"within", "not_within"}};
  for (size_t i = 0; op && i < sizeof k / sizeof k[0]; i++) {
    if (!strcmp(op, k[i][0])) return k[i][1];
    if (!strcmp(op, k[i][1])) return k[i][0];
  }
  return NULL;
}

/* A condition (or a "not" around one, if negated) into a new cond; NULL if the editor can't
 * show it. The rule has been validated, so values have the right shapes. */
static cond *load_cond(yyjson_val *leaf, BOOL negated) {
  int field = find_field(yyjson_get_str(yyjson_obj_get(leaf, "field")));
  if (field < 0) return NULL;
  const char *type = ed.fields[field].type, *op = yyjson_get_str(yyjson_obj_get(leaf, "op"));
  yyjson_val *value = yyjson_obj_get(leaf, "value");
  BOOL bool_value = yyjson_get_bool(value);
  if (negated && !strcmp(type, "bool")) bool_value = !bool_value;
  else if (negated) op = negated_op(op);
  int op_i = find_op(field, op);
  if (op_i < 0) return NULL;
  cond *c = new_cond(field);
  if (!c) return NULL;
  c->op = op_i;
  fill_ops(c);
  wchar_t buf[64];
  if (!strcmp(type, "text") && yyjson_is_str(value)) {
    wchar_t *w = widen(yyjson_get_str(value));
    SetWindowTextW(c->value, w ? w : L"");
    free(w);
  } else if (!strcmp(type, "int") || !strcmp(type, "duration")) {
    BOOL dur = !strcmp(type, "duration");
    yyjson_val *vals[2] = {value, NULL};
    if (yyjson_is_arr(value)) vals[0] = yyjson_arr_get(value, 0), vals[1] = yyjson_arr_get(value, 1);
    HWND boxes[2] = {c->value, c->value2};
    for (int i = 0; i < 2; i++) {
      if (!vals[i]) continue;
      long long n = json_int(vals[i]);
      if (n < 0) { /* the boxes take digits only */
        free_cond(c);
        return NULL;
      }
      if (dur) format_duration(n, buf, 64);
      else swprintf(buf, 64, L"%lld", n);
      SetWindowTextW(boxes[i], buf);
    }
  } else if (!strcmp(type, "date") && yyjson_is_obj(value)) {
    long long days = json_int(yyjson_obj_get(value, "days"));
    BOOL weeks = days >= 14 && days % 7 == 0; /* as mc_smart_describe() says it */
    swprintf(buf, 64, L"%lld", weeks ? days / 7 : days);
    SetWindowTextW(c->value, buf);
    SendMessageW(c->unit, CB_SETCURSEL, weeks ? UNIT_WEEKS : UNIT_DAYS, 0);
  } else if (!strcmp(type, "date") && value) {
    SYSTEMTIME day;
    if (!ms_to_date(json_int(value), &day)) {
      free_cond(c);
      return NULL;
    }
    DateTime_SetSystemtime(c->date, GDT_VALID, &day);
  } else if (!strcmp(type, "bool")) {
    combo_select_data(c->choice, bool_value ? 1 : 0);
  } else if (yyjson_is_str(value)) {
    const char *s = yyjson_get_str(value);
    int found = -1;
    if (!strcmp(type, "playlist"))
      for (int i = 0; ed.playlists && i < ed.playlists->count && found < 0; i++)
        if (!strcmp(ed.playlists->items[i].uid, s)) found = i;
    if (!strcmp(type, "group"))
      for (int i = 0; ed.groups && i < ed.groups->count && found < 0; i++)
        if (!strcmp(ed.groups->items[i].uid, s)) found = i;
    if (!strcmp(type, "format"))
      for (int i = 0; i < FORMAT_COUNT && found < 0; i++)
        if (!_stricmp(k_formats[i].value, s)) found = i;
    if (found < 0) {
      c->extra = _strdup(s);
      fill_choice(c);
    }
    combo_select_data(c->choice, found >= 0 ? found : -2);
  }
  style_value(c);
  return c;
}

/* A node that's a condition, or a "not" around one: into *out. FALSE if it's neither, or
 * can't be shown. */
static BOOL load_leaf(yyjson_val *node, cond **out) {
  *out = NULL;
  yyjson_val *not = yyjson_obj_get(node, "not");
  if (not && (yyjson_obj_get(not, "all") || yyjson_obj_get(not, "any") || yyjson_obj_get(not, "not"))) return FALSE;
  if (!not && (yyjson_obj_get(node, "all") || yyjson_obj_get(node, "any"))) return FALSE;
  return (*out = load_cond(not ? not : node, not != NULL)) != NULL;
}

/* {"all": [...]}, {"any": [...]} or {"not": {"any": [...]}}: its mode and list. FALSE for
 * anything else. */
static BOOL group_of(yyjson_val *node, int *mode, yyjson_val **list) {
  yyjson_val *not = yyjson_obj_get(node, "not");
  if (not) {
    *list = yyjson_obj_get(not, "any");
    *mode = MODE_NONE;
    return *list != NULL;
  }
  if ((*list = yyjson_obj_get(node, "all"))) *mode = MODE_ALL;
  else if ((*list = yyjson_obj_get(node, "any"))) *mode = MODE_ANY;
  return *list != NULL;
}

/* rule into the editor's rows. FALSE if the editor can't show it: it's read-only then. */
static BOOL load_rule(const char *rule) {
  if (mc_smart_validate(ed.subject, rule) != MC_OK) return FALSE;
  yyjson_doc *doc = yyjson_read(rule, strlen(rule), 0);
  yyjson_val *root = yyjson_doc_get_root(doc), *match = yyjson_obj_get(root, "match");
  BOOL ok = doc != NULL;
  ed.mode = MODE_ALL;
  yyjson_val *list = NULL;
  cond *c;
  if (ok && match && !group_of(match, &ed.mode, &list)) { /* one condition on its own */
    ok = load_leaf(match, &c) && (ed.items[ed.nitems] = new_item(c)) != NULL;
    ed.nitems += ok;
  }
  size_t idx, max;
  yyjson_val *node;
  yyjson_arr_foreach(list, idx, max, node) {
    if (!ok) break;
    int mode;
    yyjson_val *kids;
    if (ed.nitems >= MAX_ITEMS) {
      ok = FALSE;
    } else if (group_of(node, &mode, &kids)) {
      item *g = new_group(mode);
      ed.items[ed.nitems++] = g;
      ok = g != NULL;
      size_t k, kmax;
      yyjson_val *kid;
      yyjson_arr_foreach(kids, k, kmax, kid) {
        if (!ok) break;
        ok = g->nkids < MAX_KIDS && load_leaf(kid, &g->kids[g->nkids]); /* deeper groups can't be shown */
        g->nkids += ok;
      }
    } else {
      ok = load_leaf(node, &c) && (ed.items[ed.nitems] = new_item(c)) != NULL;
      ed.nitems += ok;
    }
  }
  yyjson_val *key;
  yyjson_arr_foreach(yyjson_obj_get(root, "order"), idx, max, key) {
    if (!ok) break;
    const char *name = yyjson_get_str(yyjson_obj_get(key, "field"));
    if (name && !strcmp(name, "random")) {
      ed.seed = json_int(yyjson_obj_get(key, "seed"));
      ed.has_seed = TRUE;
      continue;
    }
    int f = find_field(name), s = -1;
    for (int i = 0; i < ed.nsortable && f >= 0; i++)
      if (ed.sortable[i] == f) s = i;
    yyjson_val *dir = yyjson_obj_get(key, "dir");
    ok = s >= 0 && add_sort(s, dir && yyjson_equals_str(dir, "desc")) != NULL;
  }
  SendMessageW(ed.has_seed ? ed.shuffled : ed.sorted, BM_SETCHECK, BST_CHECKED, 0);
  yyjson_val *limit = yyjson_obj_get(root, "limit");
  if (ok && limit) {
    wchar_t buf[32];
    swprintf(buf, 32, L"%lld", json_int(limit));
    SetWindowTextW(ed.limit, buf);
    SendMessageW(ed.limit_check, BM_SETCHECK, BST_CHECKED, 0);
  }
  yyjson_doc_free(doc);
  return ok;
}

/* --- Layout --- */

typedef struct placer {
  HDWP dwp;
  HWND after; /* z-order is the tab order, so each control goes after the one before */
} placer;

static void put(placer *p, HWND w, int x, int y, int cx, int cy, BOOL shown) {
  if (!w) return;
  wchar_t cls[16]; /* a combo box's height is its drop-down's */
  if (GetClassNameW(w, cls, 16) && !_wcsicmp(cls, WC_COMBOBOXW)) cy = dy(ROW) + dy(120);
  if (p->dwp)
    p->dwp = DeferWindowPos(p->dwp, w, p->after, x, y - ed.scroll, cx, cy,
                            SWP_NOACTIVATE | (shown ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
  p->after = w;
}

static int text_width(HWND w) {
  wchar_t buf[128];
  int n = GetWindowTextW(w, buf, 128);
  HDC dc = GetDC(w);
  HGDIOBJ old = SelectObject(dc, ed.font);
  SIZE size = {0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(w, dc);
  return size.cx + dx(2);
}

/* What goes under a condition: what's wrong with it, else that it depends on this device. */
static const wchar_t *hint_text(const cond *c) {
  if (c->invalid) return mc_wstr(MC_STR_WINDOWS_SMART_NEEDS_VALUE);
  if (!strcmp(field_of(c)->name, "on_device") || !strcmp(field_of(c)->name, "format"))
    return mc_wstr(MC_STR_WINDOWS_SMART_HINT_DEVICE);
  return NULL;
}

/* One condition's row at x, from *y (moved past it), ending at right. */
static void place_cond(placer *p, cond *c, int x, int right, int *y, BOOL counted) {
  int h = dy(ROW), gap = dx(GAP);
  BOOL none = op_is(c, "empty") || op_is(c, "not_empty");
  BOOL number = type_is(c, "int") || type_is(c, "duration"), between = number && op_is(c, "between");
  BOOL date = type_is(c, "date"), days = date && (op_is(c, "within") || op_is(c, "not_within"));
  BOOL choice = type_is(c, "bool") || type_is(c, "playlist") || type_is(c, "group") || type_is(c, "format");
  BOOL edit = !none && !choice && !(date && !days);
  int remove_x = right - dx(REMOVE_W), count_x = remove_x - gap - dx(COUNT_W);
  int vx = x + dx(FIELD_W) + gap + dx(OP_W) + gap, vw = max(dx(NUM_W), count_x - gap - vx);
  int num_w = min(dx(NUM_W), vw);
  put(p, c->field_box, x, *y, dx(FIELD_W), h, TRUE);
  put(p, c->op_box, x + dx(FIELD_W) + gap, *y, dx(OP_W), h, TRUE);
  put(p, c->value, vx, *y, between || days ? num_w : vw, h, edit);
  put(p, c->and_label, vx + num_w + gap, *y, dx(AND_W), h, between);
  put(p, c->value2, vx + num_w + gap + dx(AND_W) + gap, *y, num_w, h, between);
  put(p, c->unit, vx + num_w + gap, *y, dx(UNIT_W), h, days);
  put(p, c->date, vx, *y, min(vw, dx(FIELD_W)), h, date && !days && !none);
  put(p, c->choice, vx, *y, vw, h, choice);
  put(p, c->count, count_x, *y, dx(COUNT_W), h, counted);
  put(p, c->remove, remove_x, *y, dx(REMOVE_W), h, TRUE);
  *y += h;
  const wchar_t *hint = hint_text(c);
  if (hint) SetWindowTextW(c->hint, hint);
  put(p, c->hint, vx, *y + dy(1), right - vx, dy(HINT_H) - dy(1), hint != NULL);
  if (hint) *y += dy(HINT_H);
  *y += dy(ROW_GAP);
}

static void add_rail(int left, int top, int bottom, int level) {
  if (ed.nrails >= (int)(sizeof ed.rails / sizeof ed.rails[0]) || bottom <= top) return;
  SetRect(&ed.rails[ed.nrails].r, left, top - ed.scroll, left + max(2, dx(2)), bottom - ed.scroll);
  ed.rails[ed.nrails++].level = level;
}

/* Every control in the panel, top to bottom, scrolled by ed.scroll. */
static void layout_panel(void) {
  if (!ed.panel || ed.read_only) return;
  RECT rc;
  GetClientRect(ed.panel, &rc);
  int right = rc.right - dx(2), h = dy(ROW), gap = dx(GAP);
  int x0 = dx(DEPTH0_X), x1 = x0 + dx(INDENT);
  BOOL counted = ed.mode == MODE_ALL && ed.nitems >= 2 && ed.ncounts == ed.nitems;
  int total = 0;
  for (int i = 0; i < ed.nitems; i++) total += ed.items[i]->c ? 12 : 8 + 12 * ed.items[i]->nkids;
  placer p = {BeginDeferWindowPos(40 + total + 3 * ed.nsorts), HWND_TOP};
  ed.nrails = 0;

  int y = dy(2);
  int x = dx(RAIL_X);
  put(&p, ed.match_label, x, y, text_width(ed.match_label), h, TRUE);
  x += text_width(ed.match_label) + gap;
  put(&p, ed.match_box, x, y, dx(MODE_W), h, TRUE);
  x += dx(MODE_W) + gap;
  put(&p, ed.match_of, x, y, text_width(ed.match_of), h, TRUE);
  y += h + dy(ROW_GAP);
  int rail_top = y;
  for (int i = 0; i < ed.nitems; i++) {
    item *it = ed.items[i];
    if (it->c) {
      place_cond(&p, it->c, x0, right, &y, counted);
      continue;
    }
    int gx = x0, remove_x = right - dx(REMOVE_W);
    put(&p, it->label, gx, y, text_width(it->label), h, TRUE);
    gx += text_width(it->label) + gap;
    put(&p, it->mode_box, gx, y, dx(MODE_W), h, TRUE);
    gx += dx(MODE_W) + gap;
    put(&p, it->of_label, gx, y, text_width(it->of_label), h, TRUE);
    put(&p, it->count, remove_x - gap - dx(COUNT_W), y, dx(COUNT_W), h, counted);
    put(&p, it->remove, remove_x, y, dx(REMOVE_W), h, TRUE);
    y += h + dy(ROW_GAP);
    int kids_top = y;
    for (int k = 0; k < it->nkids; k++) place_cond(&p, it->kids[k], x1, right, &y, FALSE);
    put(&p, it->add, x1, y, max(dx(BUTTON_W), text_width(it->add) + dx(8)), h, TRUE);
    EnableWindow(it->add, it->nkids < MAX_KIDS);
    y += h;
    add_rail(x0 + dx(INDENT) / 2 - dx(1), kids_top, y, 1);
    y += dy(ROW_GAP);
  }
  x = x0;
  put(&p, ed.add_cond, x, y, max(dx(BUTTON_W), text_width(ed.add_cond) + dx(8)), h, TRUE);
  x += max(dx(BUTTON_W), text_width(ed.add_cond) + dx(8)) + gap;
  put(&p, ed.add_group, x, y, max(dx(BUTTON_W), text_width(ed.add_group) + dx(8)), h, TRUE);
  EnableWindow(ed.add_cond, ed.nitems < MAX_ITEMS);
  EnableWindow(ed.add_group, ed.nitems < MAX_ITEMS);
  y += h;
  add_rail(dx(RAIL_X), rail_top, y, 0);
  y += dy(ROW_GAP) * 3;

  /* The order: a picker per sort order, then Sorted or Shuffled, then the limit. */
  int label_w = 0;
  for (int i = 0; i < ed.nsorts; i++) label_w = max(label_w, text_width(ed.sorts[i].label));
  for (int i = 0; i < ed.nsorts; i++) {
    sort_key *k = &ed.sorts[i];
    int sx = dx(RAIL_X);
    put(&p, k->label, sx, y, label_w, h, TRUE);
    sx += label_w + gap;
    int sort_w = min(dx(SORT_W), right - dx(REMOVE_W) - gap - sx);
    put(&p, k->box, sx, y, sort_w, h, TRUE);
    put(&p, k->remove, sx + sort_w + gap, y, dx(REMOVE_W), h, TRUE);
    y += h + dy(ROW_GAP);
  }
  put(&p, ed.sort_add, dx(RAIL_X), y, max(dx(BUTTON_W), text_width(ed.sort_add) + dx(8)), h, TRUE);
  EnableWindow(ed.sort_add, ed.nsorts < MAX_SORTS && ed.nsorts < ed.nsortable);
  y += h + dy(ROW_GAP);
  int box = GetSystemMetricsForDpi(SM_CXMENUCHECK, ed.dpi);
  x = dx(RAIL_X);
  put(&p, ed.sorted, x, y + (h - box) / 2, box, box, TRUE);
  put(&p, ed.shuffled, x + box + gap + text_width(ed.sorted_label) + dx(12), y + (h - box) / 2, box, box, TRUE);
  put(&p, ed.sorted_label, x + box + gap, y, text_width(ed.sorted_label), h, TRUE);
  put(&p, ed.shuffled_label, x + 2 * box + 2 * gap + text_width(ed.sorted_label) + dx(12), y,
      text_width(ed.shuffled_label), h, TRUE);
  y += h + dy(ROW_GAP);
  put(&p, ed.limit_check, x, y + (h - box) / 2, box, box, TRUE);
  x += box + gap;
  put(&p, ed.limit_label, x, y, text_width(ed.limit_label), h, TRUE);
  x += text_width(ed.limit_label) + gap;
  put(&p, ed.limit, x, y, dx(NUM_W), h, TRUE);
  EnableWindow(ed.limit, SendMessageW(ed.limit_check, BM_GETCHECK, 0, 0) == BST_CHECKED);
  x += dx(NUM_W) + gap;
  put(&p, ed.limit_unit, x, y, text_width(ed.limit_unit), h, TRUE);
  y += h + dy(4);

  if (p.dwp) EndDeferWindowPos(p.dwp);
  ed.content_h = y; /* put() takes content coordinates */
  if (ed.scroll > max(0, ed.content_h - rc.bottom)) { /* rows went: no space below the last */
    ed.scroll = max(0, ed.content_h - rc.bottom);
    layout_panel();
    return;
  }
  SCROLLINFO si = {.cbSize = sizeof si, .fMask = SIF_ALL | SIF_DISABLENOSCROLL, .nMin = 0,
                   .nMax = max(0, ed.content_h - 1), .nPage = (UINT)rc.bottom, .nPos = ed.scroll};
  SetScrollInfo(ed.panel, SB_VERT, &si, TRUE);
  InvalidateRect(ed.panel, NULL, TRUE);
}

/* Keeps the scroll position inside the content, and lays the panel out again. */
static void scroll_to(int pos) {
  RECT rc;
  GetClientRect(ed.panel, &rc);
  pos = max(0, min(pos, ed.content_h - rc.bottom));
  ed.scroll = pos;
  layout_panel();
}

/* Scrolls the panel just enough to show its child w. */
static void reveal(HWND w) {
  if (!w || GetParent(w) != ed.panel) return;
  RECT r, rc;
  GetWindowRect(w, &r);
  MapWindowPoints(NULL, ed.panel, (POINT *)&r, 2);
  GetClientRect(ed.panel, &rc);
  int h = min(r.bottom - r.top, dy(ROW) + dy(HINT_H)), pad = dy(ROW_GAP);
  if (r.top < 0) scroll_to(ed.scroll + r.top - pad);
  else if (r.top + h > rc.bottom) scroll_to(ed.scroll + r.top + h - rc.bottom + pad);
}

/* The dialog: the name at the top, the panel and the preview, the sentence and the buttons. */
static void layout_dialog(void) {
  RECT rc;
  GetClientRect(ed.dlg, &rc);
  int m = dx(MARGIN), h = dy(ROW);
  int name_label_w = text_width(ed.name_label);
  MoveWindow(ed.name_label, m, m, name_label_w, h, TRUE);
  MoveWindow(ed.name, m + name_label_w + dx(GAP), m, min(dx(200), rc.right - 2 * m - name_label_w - dx(GAP)), h, TRUE);
  int buttons_y = rc.bottom - m - dy(ROW + 1);
  HWND ok = GetDlgItem(ed.dlg, IDOK), cancel = GetDlgItem(ed.dlg, IDCANCEL);
  int bw = dx(BUTTON_W);
  MoveWindow(cancel, rc.right - m - bw, buttons_y, bw, dy(ROW + 1), TRUE);
  MoveWindow(ok, rc.right - m - 2 * bw - dx(GAP), buttons_y, bw, dy(ROW + 1), TRUE);
  MoveWindow(ed.counts, m, buttons_y, max(0, rc.right - 3 * m - 2 * bw - dx(GAP)), dy(ROW + 1), TRUE);
  int summary_y = buttons_y - dy(GAP) - dy(SUMMARY_H);
  MoveWindow(ed.summary, m, summary_y, rc.right - 2 * m, dy(SUMMARY_H), TRUE);

  int top = m + h + dy(6), bottom = summary_y - dy(6), avail_w = rc.right - 2 * m;
  ed.wide = avail_w >= dx(LEFT_MIN + PREVIEW_MIN + MARGIN);
  int label_h = dy(10);
  if (ed.wide) {
    int preview_w = max(dx(PREVIEW_MIN), (avail_w - m) * 3 / 8), left_w = avail_w - m - preview_w;
    MoveWindow(ed.panel, m, top, left_w, bottom - top, TRUE);
    MoveWindow(ed.read_only_text, m, top, left_w, bottom - top, TRUE);
    MoveWindow(ed.preview_label, m + left_w + m, top, preview_w, label_h, TRUE);
    MoveWindow(ed.list, m + left_w + m, top + label_h, preview_w, bottom - top - label_h, TRUE);
  } else {
    int panel_h = (bottom - top) * 3 / 5;
    MoveWindow(ed.panel, m, top, avail_w, panel_h, TRUE);
    MoveWindow(ed.read_only_text, m, top, avail_w, panel_h, TRUE);
    MoveWindow(ed.preview_label, m, top + panel_h + dy(4), avail_w, label_h, TRUE);
    MoveWindow(ed.list, m, top + panel_h + dy(4) + label_h, avail_w, max(0, bottom - top - panel_h - dy(4) - label_h), TRUE);
  }
  RECT lr;
  GetClientRect(ed.list, &lr);
  int list_w = lr.right - (GetWindowLongW(ed.list, GWL_STYLE) & WS_VSCROLL ? 0 : GetSystemMetricsForDpi(SM_CXVSCROLL, ed.dpi));
  int col = list_w / 2; /* with room for a scrollbar, so there's never a sideways one */
  ListView_SetColumnWidth(ed.list, 0, col);
  ListView_SetColumnWidth(ed.list, 1, list_w - col);
  scroll_to(ed.scroll);
}

/* --- Preview --- */

static void free_job(preview_job *j) {
  if (!j) return;
  free(j->rule);
  for (int i = 0; i < j->nprefixes; i++) free(j->prefixes[i]);
  free(j->prefixes);
  free(j);
}

static void free_result(preview_result *r) {
  if (!r) return;
  mc_track_list_free(r->tracks);
  mc_album_list_free(r->albums);
  free(r->counts);
  free(r);
}

/* How many a rule picks, or -1. */
static int count_of(mc_library *lib, mc_smart_subject subject, const char *rule) {
  int n;
  return mc_smart_count(lib, subject, rule, &n) == MC_OK ? n : -1;
}

static DWORD WINAPI worker_main(void *arg) {
  worker *w = arg;
  mc_library *lib = NULL;
  for (;;) {
    EnterCriticalSection(&w->lock);
    while (!w->job && !w->quit) SleepConditionVariableCS(&w->wake, &w->lock, INFINITE);
    preview_job *job = w->quit ? NULL : w->job;
    w->job = NULL;
    LeaveCriticalSection(&w->lock);
    if (!job) break;
    if (!lib && mc_library_open(w->dir, &lib) != MC_OK) lib = NULL;
    preview_result *r = calloc(1, sizeof *r);
    if (r && lib) {
      r->gen = job->gen;
      r->ok = job->subject == MC_SMART_TRACKS ? mc_smart_preview_tracks(lib, job->rule, &r->tracks) == MC_OK
                                              : mc_smart_preview_albums(lib, job->rule, &r->albums) == MC_OK;
      r->counts = job->nprefixes ? malloc((size_t)job->nprefixes * sizeof *r->counts) : NULL;
      for (int i = 0; r->counts && i < job->nprefixes; i++) {
        EnterCriticalSection(&w->lock);
        BOOL stale = w->job || w->quit; /* a newer rule is waiting: these counts are wasted */
        LeaveCriticalSection(&w->lock);
        r->counts[i] = stale ? -1 : count_of(lib, job->subject, job->prefixes[i]);
        r->ncounts++;
      }
    }
    free_job(job);
    if (r && !PostMessageW(w->dlg, WM_APP_PREVIEW, 0, (LPARAM)r)) free_result(r);
  }
  if (lib) mc_library_close(lib);
  return 0;
}

static void start_worker(void) {
  worker *w = calloc(1, sizeof *w);
  if (!w || !(w->dir = _strdup(mc_library_dir(ed.lib)))) {
    free(w);
    return;
  }
  InitializeCriticalSection(&w->lock);
  InitializeConditionVariable(&w->wake);
  w->dlg = ed.dlg;
  w->thread = CreateThread(NULL, 0, worker_main, w, 0, NULL);
  if (!w->thread) {
    DeleteCriticalSection(&w->lock);
    free(w->dir);
    free(w);
    return;
  }
  ed.w = w;
}

/* Waits for the thread to finish what it's doing, and frees what it sent that hasn't been
 * read. */
static void stop_worker(void) {
  worker *w = ed.w;
  if (!w) return;
  EnterCriticalSection(&w->lock);
  w->quit = TRUE;
  WakeConditionVariable(&w->wake);
  LeaveCriticalSection(&w->lock);
  WaitForSingleObject(w->thread, INFINITE);
  CloseHandle(w->thread);
  free_job(w->job);
  DeleteCriticalSection(&w->lock);
  free(w->dir);
  free(w);
  ed.w = NULL;
  MSG msg;
  while (PeekMessageW(&msg, ed.dlg, WM_APP_PREVIEW, WM_APP_PREVIEW, PM_REMOVE)) free_result((preview_result *)msg.lParam);
}

static void set_counts_text(void) {
  wchar_t text[256] = L"";
  if (ed.rule && !ed.counted) {
    wcscpy(text, mc_wstr(MC_STR_WINDOWS_SMART_COUNTING));
  } else if (ed.rule) {
    enum mc_str_id id = ed.subject == MC_SMART_TRACKS ? MC_STR_WINDOWS_SMART_TRACKS_MATCH : MC_STR_WINDOWS_SMART_ALBUMS_MATCH;
    char part[128], all[256];
    mc_format(part, sizeof part, mc_str_plural(id, ed.total), MC_D(ed.total));
    if (SendMessageW(ed.limit_check, BM_GETCHECK, 0, 0) == BST_CHECKED) {
      char in_list[128];
      mc_format(in_list, sizeof in_list, mc_str(MC_STR_WINDOWS_SMART_IN_LIST), MC_D(ed.shown));
      snprintf(all, sizeof all, "%s%s%s", part, mc_str(MC_STR_SEPARATOR_DOT), in_list);
    } else {
      snprintf(all, sizeof all, "%s", part);
    }
    wstr_from_utf8(text, 256, all);
  }
  SetWindowTextW(ed.counts, text);
}

/* The counts beside the top-level entries of an all-of rule. */
static void set_prefix_counts(void) {
  for (int i = 0; i < ed.nitems; i++) {
    HWND w = ed.items[i]->c ? ed.items[i]->c->count : ed.items[i]->count;
    wchar_t text[64] = L"", name[128] = L"";
    if (i < ed.ncounts && ed.counts_of[i] >= 0) {
      mc_wformat(text, 64, mc_str(MC_STR_WINDOWS_SMART_PREFIX_COUNT), MC_D(ed.counts_of[i]));
      mc_wformat(name, 128, mc_str_plural(MC_STR_WINDOWS_SMART_PREFIX_COUNT_NAME, ed.counts_of[i]),
                 MC_D(ed.counts_of[i]));
    }
    SetWindowTextW(w, text);
    set_name(w, name);
  }
}

static void on_preview(preview_result *r) {
  if (r->gen != ed.gen || !r->ok) {
    if (r->gen == ed.gen) { /* it failed: show nothing rather than an old answer */
      ed.counted = TRUE;
      ed.total = ed.shown = 0;
    }
    free_result(r);
    if (ed.counted) set_counts_text();
    return;
  }
  mc_track_list_free(ed.tracks);
  mc_album_list_free(ed.albums);
  ed.tracks = r->tracks;
  ed.albums = r->albums;
  r->tracks = NULL;
  r->albums = NULL;
  ed.total = ed.tracks ? ed.tracks->count : ed.albums ? ed.albums->count : 0;
  long long limit = 0;
  BOOL limited = SendMessageW(ed.limit_check, BM_GETCHECK, 0, 0) == BST_CHECKED && parse_int(ed.limit, &limit);
  ed.shown = limited && limit < ed.total ? (int)limit : ed.total;
  /* The last entry's count is the total: the rule without its limit. */
  ed.ncounts = 0;
  if (ed.mode == MODE_ALL && ed.nitems >= 2 && r->ncounts == ed.nitems - 1) {
    for (int i = 0; i < r->ncounts; i++) ed.counts_of[i] = r->counts[i];
    ed.counts_of[ed.nitems - 1] = ed.total;
    ed.ncounts = ed.nitems;
  }
  ed.counted = TRUE;
  free_result(r);
  ListView_SetItemCountEx(ed.list, ed.shown, 0);
  InvalidateRect(ed.list, NULL, TRUE);
  set_counts_text();
  set_prefix_counts();
  layout_panel();
}

/* Hands the rule as it stands to the thread. */
static void request_preview(void) {
  KillTimer(ed.dlg, TIMER_PREVIEW);
  if (!ed.w || !ed.rule) return;
  preview_job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->gen = ed.gen;
  j->subject = ed.subject;
  j->rule = build(-1, FALSE);
  if (!ed.read_only && ed.mode == MODE_ALL && ed.nitems >= 2) {
    j->prefixes = calloc((size_t)ed.nitems - 1, sizeof *j->prefixes);
    for (int i = 0; j->prefixes && i < ed.nitems - 1; i++)
      if ((j->prefixes[i] = build(i, FALSE))) j->nprefixes++;
  }
  if (ed.read_only) { /* a rule the editor can't show: previewed as it's saved, without its limit */
    free(j->rule);
    j->rule = NULL;
    yyjson_doc *saved = yyjson_read(ed.rule, strlen(ed.rule), 0);
    yyjson_mut_doc *d = saved ? yyjson_doc_mut_copy(saved, NULL) : NULL;
    if (d) yyjson_mut_obj_remove_key(yyjson_mut_doc_get_root(d), "limit");
    j->rule = d ? yyjson_mut_write(d, 0, NULL) : NULL;
    yyjson_mut_doc_free(d);
    yyjson_doc_free(saved);
  }
  if (!j->rule) {
    free_job(j);
    return;
  }
  EnterCriticalSection(&ed.w->lock);
  free_job(ed.w->job);
  ed.w->job = j;
  WakeConditionVariable(&ed.w->wake);
  LeaveCriticalSection(&ed.w->lock);
}

/* --- Changes --- */

static void update_ok(void) {
  wchar_t name[MAX_NAME + 1];
  GetWindowTextW(ed.name, name, MAX_NAME + 1);
  BOOL named = FALSE;
  for (const wchar_t *p = name; *p && !named; p++) named = !iswspace(*p);
  EnableWindow(GetDlgItem(ed.dlg, IDOK), !ed.read_only && named && ed.rule != NULL);
}

/* After any change to the rule: rebuilds it, the sentence and the hints, and asks for a new
 * preview a moment after the last change. */
static void rule_changed(void) {
  free(ed.rule);
  ed.rule = build(-1, TRUE);
  if (ed.rule && mc_smart_validate(ed.subject, ed.rule) != MC_OK) {
    free(ed.rule);
    ed.rule = NULL;
  }
  char *sentence = NULL;
  wchar_t *w = NULL;
  if (ed.rule && mc_smart_describe(ed.lib, ed.subject, ed.rule, &sentence) == MC_OK) w = widen(sentence);
  mc_free(sentence);
  SetWindowTextW(ed.summary, w ? w : L"");
  free(w);
  ed.gen++;
  ed.counted = FALSE;
  ed.ncounts = 0;
  set_prefix_counts();
  if (!ed.rule) {
    ListView_SetItemCountEx(ed.list, 0, 0);
    ed.shown = 0;
  }
  set_counts_text();
  update_ok();
  layout_panel();
  if (ed.rule) SetTimer(ed.dlg, TIMER_PREVIEW, PREVIEW_DELAY_MS, NULL);
}

static cond *find_cond(HWND w, item **parent, int *index) {
  for (int i = 0; i < ed.nitems; i++) {
    item *it = ed.items[i];
    cond *cs[MAX_KIDS + 1];
    int n = 0;
    if (it->c) cs[n++] = it->c;
    for (int k = 0; k < it->nkids; k++) cs[n++] = it->kids[k];
    for (int k = 0; k < n; k++) {
      cond *c = cs[k];
      if (w == c->field_box || w == c->op_box || w == c->value || w == c->value2 || w == c->unit || w == c->date ||
          w == c->choice || w == c->remove) {
        *parent = it;
        *index = it->c ? i : k;
        return c;
      }
    }
  }
  return NULL;
}

/* A new condition: Genre for tracks, which most rules start from, else the first field. */
static cond *blank_cond(void) {
  int f = find_field(ed.subject == MC_SMART_TRACKS ? "genre" : "artist");
  return new_cond(f >= 0 ? f : 0);
}

static void remove_item(int i) {
  free_item(ed.items[i]);
  memmove(&ed.items[i], &ed.items[i + 1], (size_t)(ed.nitems - i - 1) * sizeof ed.items[0]);
  ed.nitems--;
}

/* A control in the panel did something. */
static void on_panel_command(HWND w, int code) {
  item *it = NULL;
  int index = 0;
  cond *c = find_cond(w, &it, &index);
  if (c) {
    if (w == c->remove && code == BN_CLICKED) {
      if (it->c) {
        remove_item(index);
        SetFocus(ed.add_cond);
      } else {
        free_cond(c);
        memmove(&it->kids[index], &it->kids[index + 1], (size_t)(it->nkids - index - 1) * sizeof it->kids[0]);
        it->nkids--;
        SetFocus(it->add);
      }
      rule_changed();
    } else if (w == c->field_box && code == CBN_SELCHANGE) {
      int f = (int)combo_data(c->field_box);
      if (f < 0 || f == c->field) return;
      const char *was_type = field_of(c)->type, *was_op = op_of(c);
      BOOL same_type = !strcmp(was_type, ed.fields[f].type);
      c->field = f;
      int op = same_type ? find_op(f, was_op) : -1;
      c->op = op >= 0 ? op : default_op(f);
      if (!same_type) { /* "jazz" means nothing as a length */
        SetWindowTextW(c->value, type_is(c, "date") ? L"30" : L"");
        SetWindowTextW(c->value2, L"");
      }
      free(c->extra);
      c->extra = NULL;
      fill_ops(c);
      fill_choice(c);
      style_value(c);
      rule_changed();
    } else if (w == c->op_box && code == CBN_SELCHANGE) {
      int op = (int)combo_data(c->op_box);
      if (op >= 0) c->op = op;
      rule_changed();
    } else if (((w == c->value || w == c->value2) && code == EN_CHANGE) ||
               ((w == c->unit || w == c->choice) && code == CBN_SELCHANGE)) {
      rule_changed();
    }
    return;
  }
  for (int i = 0; i < ed.nitems; i++) {
    item *g = ed.items[i];
    if (g->c) continue;
    if (w == g->remove && code == BN_CLICKED) {
      remove_item(i);
      SetFocus(ed.add_group);
      rule_changed();
      return;
    }
    if (w == g->mode_box && code == CBN_SELCHANGE) {
      g->mode = (int)combo_data(g->mode_box);
      rule_changed();
      return;
    }
    if (w == g->add && code == BN_CLICKED && g->nkids < MAX_KIDS) {
      cond *k = blank_cond();
      if (!k) return;
      g->kids[g->nkids++] = k;
      rule_changed();
      SetFocus(k->field_box);
      return;
    }
  }
  for (int i = 0; i < ed.nsorts; i++) {
    if (w == ed.sorts[i].remove && code == BN_CLICKED) {
      remove_sort(i);
      SetFocus(ed.sort_add);
      rule_changed();
      return;
    }
    if (w == ed.sorts[i].box && code == CBN_SELCHANGE) {
      rule_changed();
      return;
    }
  }
  if (w == ed.match_box && code == CBN_SELCHANGE) {
    ed.mode = (int)combo_data(ed.match_box);
    rule_changed();
  } else if (w == ed.add_cond && code == BN_CLICKED && ed.nitems < MAX_ITEMS) {
    item *n = new_item(blank_cond());
    if (!n) return;
    ed.items[ed.nitems++] = n;
    rule_changed();
    SetFocus(n->c->field_box);
  } else if (w == ed.add_group && code == BN_CLICKED && ed.nitems < MAX_ITEMS) {
    item *g = new_group(MODE_ANY);
    cond *k = g ? blank_cond() : NULL;
    if (!k) {
      free_item(g);
      return;
    }
    g->kids[g->nkids++] = k;
    ed.items[ed.nitems++] = g;
    rule_changed();
    SetFocus(g->mode_box);
  } else if (w == ed.sort_add && code == BN_CLICKED) {
    /* The next sortable field not sorted on yet. */
    int next = 0;
    for (int s = 0; s < ed.nsortable; s++) {
      BOOL used = FALSE;
      for (int i = 0; i < ed.nsorts; i++) used |= combo_data(ed.sorts[i].box) / 2 == s;
      if (!used) {
        next = s;
        break;
      }
    }
    sort_key *k = add_sort(next, FALSE);
    if (!k) return;
    rule_changed();
    SetFocus(k->box);
  } else if ((w == ed.sorted || w == ed.shuffled || w == ed.limit_check) && code == BN_CLICKED) {
    rule_changed();
  } else if ((w == ed.sorted_label || w == ed.shuffled_label || w == ed.limit_label) && code == STN_CLICKED) {
    /* The labels stand in for the buttons' own text, which stays black in dark mode. */
    HWND button = w == ed.sorted_label ? ed.sorted : w == ed.shuffled_label ? ed.shuffled : ed.limit_check;
    SetFocus(button);
    SendMessageW(button, BM_CLICK, 0, 0);
  } else if (w == ed.limit && code == EN_CHANGE) {
    rule_changed();
  }
}

/* The tooltips' text: a remove button's name, or a negative text condition's warning. */
static void on_tip(NMTTDISPINFOW *info) {
  static wchar_t text[256];
  text[0] = L'\0';
  HWND w = (HWND)info->hdr.idFrom;
  item *it;
  int index;
  cond *c = find_cond(w, &it, &index);
  if (c && w == c->op_box) {
    enum mc_str_id field = in_text_id(field_of(c)->name);
    if (field != MC_STR_COUNT && (op_is(c, "is_not") || op_is(c, "not_contains")))
      mc_wformat(text, 256,
                 mc_str(ed.subject == MC_SMART_TRACKS ? MC_STR_WINDOWS_SMART_HINT_MISSING_TRACKS
                                                     : MC_STR_WINDOWS_SMART_HINT_MISSING_ALBUMS),
                 MC_S(mc_str(field)));
  } else if (c) {
    wcscpy(text, mc_wstr(MC_STR_WINDOWS_SMART_REMOVE_CONDITION));
  } else {
    for (int i = 0; i < ed.nitems; i++)
      if (w == ed.items[i]->remove) wcscpy(text, mc_wstr(MC_STR_WINDOWS_SMART_REMOVE_GROUP));
    for (int i = 0; i < ed.nsorts; i++)
      if (w == ed.sorts[i].remove) wcscpy(text, mc_wstr(MC_STR_WINDOWS_SMART_REMOVE_SORT));
  }
  info->lpszText = text;
}

/* The preview's rows, as the list view asks for them. */
static void on_get_item(NMLVDISPINFOW *info) {
  LVITEMW *lv = &info->item;
  if (!(lv->mask & LVIF_TEXT) || lv->iItem < 0 || lv->iItem >= ed.shown) return;
  const char *text = NULL;
  if (ed.tracks && lv->iItem < ed.tracks->count) {
    const mc_track_info *t = &ed.tracks->items[lv->iItem];
    text = lv->iSubItem ? t->artist : t->title;
  } else if (ed.albums && lv->iItem < ed.albums->count) {
    const mc_album_info *a = &ed.albums->items[lv->iItem];
    text = lv->iSubItem ? a->artist : a->title;
  }
  wstr_from_utf8(lv->pszText, (size_t)lv->cchTextMax, text ? text : "");
}

/* --- The panel --- */

static COLORREF error_color(void) {
  return theme.high_contrast ? theme.text : theme.dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
}

/* Colours for the controls in the panel and the dialog: hints and counts in secondary text,
 * a problem in red. */
static HBRUSH ctlcolor(UINT msg, HDC dc, HWND ctl) {
  HBRUSH brush = theme_ctlcolor(msg, dc);
  if (msg != WM_CTLCOLORSTATIC) return brush;
  if (ctl == ed.counts) SetTextColor(dc, theme.text2);
  for (int i = 0; i < ed.nitems; i++) {
    item *it = ed.items[i];
    if (ctl == it->count) SetTextColor(dc, theme.text2);
    cond *cs[MAX_KIDS + 1];
    int n = 0;
    if (it->c) cs[n++] = it->c;
    for (int k = 0; k < it->nkids; k++) cs[n++] = it->kids[k];
    for (int k = 0; k < n; k++) {
      if (ctl == cs[k]->count) SetTextColor(dc, theme.text2);
      if (ctl == cs[k]->hint) SetTextColor(dc, cs[k]->invalid ? error_color() : theme.text2);
    }
  }
  return brush;
}

/* A rail's colour: the accent for the top level, paler for a group's. */
static COLORREF rail_color(int level) {
  if (theme.high_contrast) return theme.text;
  if (!level) return theme.accent;
  COLORREF a = theme.accent, b = theme.bg;
  return RGB((GetRValue(a) * 3 + GetRValue(b) * 2) / 5, (GetGValue(a) * 3 + GetGValue(b) * 2) / 5,
             (GetBValue(a) * 3 + GetBValue(b) * 2) / 5);
}

static LRESULT CALLBACK panel_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
      if (lp) on_panel_command((HWND)lp, HIWORD(wp));
      return 0;
    case WM_NOTIFY: {
      NMHDR *hdr = (NMHDR *)lp;
      if (hdr->code == TTN_GETDISPINFOW) on_tip((NMTTDISPINFOW *)lp);
      else if (hdr->code == DTN_DATETIMECHANGE) rule_changed();
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return (LRESULT)ctlcolor(msg, (HDC)wp, (HWND)lp);
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(wnd, &rc);
      FillRect((HDC)wp, &rc, theme.bg_brush);
      for (int i = 0; i < ed.nrails; i++) {
        HBRUSH b = CreateSolidBrush(rail_color(ed.rails[i].level));
        FillRect((HDC)wp, &ed.rails[i].r, b);
        DeleteObject(b);
      }
      return 1;
    }
    case WM_VSCROLL: {
      SCROLLINFO si = {.cbSize = sizeof si, .fMask = SIF_ALL};
      GetScrollInfo(wnd, SB_VERT, &si);
      int pos = ed.scroll, line = dy(ROW + ROW_GAP);
      switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= line; break;
        case SB_LINEDOWN: pos += line; break;
        case SB_PAGEUP: pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        case SB_TOP: pos = 0; break;
        case SB_BOTTOM: pos = ed.content_h; break;
      }
      scroll_to(pos);
      return 0;
    }
    case WM_MOUSEWHEEL:
      scroll_to(ed.scroll - GET_WHEEL_DELTA_WPARAM(wp) * dy(ROW + ROW_GAP) * 3 / WHEEL_DELTA);
      return 0;
    case WM_SIZE:
      if (ed.dlg) layout_panel();
      return 0;
    case WM_APP_REVEAL:
      reveal((HWND)wp);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

/* Every control in the panel: when it gets the focus (by Tab, say), the panel scrolls to it;
 * and the mouse wheel over one scrolls the panel rather than changing a picker. */
static LRESULT CALLBACK child_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  if (msg == WM_SETFOCUS) PostMessageW(ed.panel, WM_APP_REVEAL, (WPARAM)wnd, 0);
  if (msg == WM_MOUSEWHEEL && GetFocus() != wnd) return SendMessageW(ed.panel, msg, wp, lp);
  if (msg == WM_NCDESTROY) RemoveWindowSubclass(wnd, child_proc, id);
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* --- The dialog --- */

static void apply_theme(void) {
  theme_apply_window(ed.dlg, FALSE);
  theme_apply_controls(ed.dlg);
  if (ed.panel) SetWindowTheme(ed.panel, theme.dark ? L"DarkMode_Explorer" : NULL, NULL);
  if (ed.list) theme_apply_control(ed.list);
}

static HWND dlg_child(const wchar_t *cls, const wchar_t *text, DWORD style, DWORD ex) {
  HWND w = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, ed.dlg, NULL,
                           GetModuleHandleW(NULL), NULL);
  if (w) SendMessageW(w, WM_SETFONT, (WPARAM)ed.font, FALSE);
  return w;
}

static HFONT make_icon_font(void) {
  return CreateFontW(-dy(7), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                     CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, theme_icon_face());
}

static void create_controls(void) {
  ed.font = (HFONT)SendMessageW(ed.dlg, WM_GETFONT, 0, 0);
  ed.dpi = GetDpiForWindow(ed.dlg);
  RECT units = {0, 0, 4, 8};
  MapDialogRect(ed.dlg, &units);
  ed.base_x = units.right;
  ed.base_y = units.bottom;
  ed.icon_font = make_icon_font();
  wchar_t label[64];
  ed.name_label = dlg_child(WC_STATICW, wstr_field_label(label, 64, MC_STR_NAME), SS_CENTERIMAGE, 0);
  ed.name = dlg_child(WC_EDITW, NULL, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL, 0);
  SendMessageW(ed.name, EM_LIMITTEXT, MAX_NAME, 0);
  theme_center_edit(ed.name);

  static BOOL registered;
  if (!registered) {
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = panel_proc, .hInstance = GetModuleHandleW(NULL),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_smart_rows"};
    registered = RegisterClassExW(&wc) != 0;
  }
  ed.panel = dlg_child(L"kithsong_smart_rows", NULL, WS_VSCROLL | WS_CLIPCHILDREN, WS_EX_CONTROLPARENT);
  ed.read_only_text = dlg_child(WC_EDITW, NULL, ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 0);
  ShowWindow(ed.read_only_text, SW_HIDE);
  ed.preview_label = dlg_child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SMART_PREVIEW), SS_NOPREFIX, 0);
  ed.list = dlg_child(WC_LISTVIEWW, NULL,
                      LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_BORDER |
                        WS_TABSTOP,
                      0);
  ListView_SetExtendedListViewStyle(ed.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW col = {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 100};
  col.pszText = (wchar_t *)mc_wstr(ed.subject == MC_SMART_TRACKS ? MC_STR_TITLE : MC_STR_ALBUM);
  ListView_InsertColumn(ed.list, 0, &col);
  col.pszText = (wchar_t *)mc_wstr(MC_STR_ARTIST);
  ListView_InsertColumn(ed.list, 1, &col);
  set_name(ed.list, mc_wstr(MC_STR_WINDOWS_SMART_PREVIEW));
  ed.summary = dlg_child(WC_STATICW, NULL, SS_NOPREFIX | SS_EDITCONTROL, 0);
  ed.counts = dlg_child(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE, 0);

  ed.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, ed.dlg, NULL,
                           GetModuleHandleW(NULL), NULL);
  SendMessageW(ed.tip, TTM_SETMAXTIPWIDTH, 0, dx(200));

  /* The panel's fixed controls, in the order they show. */
  ed.match_label = make_label(mc_wstr(ed.subject == MC_SMART_TRACKS ? MC_STR_WINDOWS_SMART_TRACKS_MATCHING
                                                                     : MC_STR_WINDOWS_SMART_ALBUMS_MATCHING));
  ed.match_box = make_combo(mc_wstr(MC_STR_WINDOWS_SMART_MATCH_NAME));
  fill_match_box(ed.match_box);
  ed.match_of = make_label(mc_wstr(MC_STR_WINDOWS_SMART_OF_THESE));
  ed.add_cond = make(WC_BUTTONW, mc_wstr(MC_STR_WINDOWS_SMART_ADD_CONDITION), BS_PUSHBUTTON | WS_TABSTOP);
  ed.add_group = make(WC_BUTTONW, mc_wstr(MC_STR_WINDOWS_SMART_ADD_GROUP), BS_PUSHBUTTON | WS_TABSTOP);
  ed.sort_add = make(WC_BUTTONW, mc_wstr(MC_STR_WINDOWS_SMART_ADD_SORT), BS_PUSHBUTTON | WS_TABSTOP);
  /* No text of their own: a themed radio button's or checkbox's text stays black in dark
   * mode, so a label beside each says it (and clicks it), as on the settings page. */
  ed.sorted = make(WC_BUTTONW, NULL, BS_AUTORADIOBUTTON | WS_TABSTOP | WS_GROUP);
  ed.shuffled = make(WC_BUTTONW, NULL, BS_AUTORADIOBUTTON);
  ed.sorted_label = make(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SMART_SORTED), SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE | WS_GROUP);
  ed.shuffled_label = make(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SMART_SHUFFLED), SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE);
  set_name(ed.sorted, mc_wstr(MC_STR_WINDOWS_SMART_SORTED));
  set_name(ed.shuffled, mc_wstr(MC_STR_WINDOWS_SMART_SHUFFLED));
  ed.limit_check = make(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP);
  ed.limit_label = make(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SMART_LIMIT), SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE);
  set_name(ed.limit_check, mc_wstr(MC_STR_WINDOWS_SMART_LIMIT));
  ed.limit = make(WC_EDITW, L"25", WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER);
  SendMessageW(ed.limit, EM_LIMITTEXT, 7, 0);
  set_name(ed.limit, mc_wstr(MC_STR_WINDOWS_SMART_LIMIT_COUNT_NAME));
  ed.limit_unit = make_label(mc_wstr(ed.subject == MC_SMART_TRACKS ? MC_STR_WINDOWS_SMART_LIMIT_TRACKS
                                                                   : MC_STR_WINDOWS_SMART_LIMIT_ALBUMS));

  /* Tab order: the name, the rows, the preview, the sentence, then OK and Cancel. */
  HWND order[] = {ed.name_label, ed.name, ed.panel, ed.read_only_text, ed.preview_label, ed.list, ed.summary, ed.counts,
                  GetDlgItem(ed.dlg, IDOK), GetDlgItem(ed.dlg, IDCANCEL)};
  for (size_t i = 1; i < sizeof order / sizeof order[0]; i++)
    SetWindowPos(order[i], order[i - 1], 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* The fields of ed.subject, and which of them can be sorted on. */
static void read_fields(void) {
  ed.nfields = ed.nsortable = 0;
  for (int i = 0; ed.nfields < MAX_FIELDS && mc_smart_field_get(ed.subject, i, &ed.fields[ed.nfields]); i++) {
    if (ed.fields[ed.nfields].sortable) ed.sortable[ed.nsortable++] = ed.nfields;
    ed.nfields++;
  }
}

/* For a rule the editor can't show: what it says, or its JSON if even that fails. */
static void show_read_only(const char *rule) {
  ed.read_only = TRUE;
  ShowWindow(ed.panel, SW_HIDE);
  ShowWindow(ed.read_only_text, SW_SHOW);
  char *sentence = NULL;
  BOOL described = mc_smart_describe(ed.lib, ed.subject, rule, &sentence) == MC_OK;
  /* An edit control wants \r\n. */
  const char *body = described ? sentence : rule;
  size_t n = strlen(body), extra = 0;
  for (size_t i = 0; i < n; i++) extra += body[i] == '\n';
  char *text = malloc(n + extra + strlen(mc_str(MC_STR_WINDOWS_SMART_READ_ONLY)) + 8);
  if (text) {
    char *o = text + sprintf(text, "%s\r\n\r\n", mc_str(MC_STR_WINDOWS_SMART_READ_ONLY));
    for (size_t i = 0; i < n; i++) {
      if (body[i] == '\n') *o++ = '\r';
      *o++ = body[i];
    }
    *o = '\0';
    wchar_t *w = widen(text);
    SetWindowTextW(ed.read_only_text, w ? w : L"");
    free(w);
  }
  free(text);
  mc_free(sentence);
  ShowWindow(GetDlgItem(ed.dlg, IDOK), SW_HIDE);
  SetDlgItemTextW(ed.dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CLOSE));
  SendMessageW(ed.dlg, DM_SETDEFID, IDCANCEL, 0);
  EnableWindow(ed.name, FALSE);
  SendMessageW(ed.limit_check, BM_SETCHECK, BST_UNCHECKED, 0); /* the counts are without the limit */
  /* It can still be previewed as it's saved, if this build can evaluate it. */
  if (mc_smart_validate(ed.subject, rule) == MC_OK) {
    ed.rule = _strdup(rule);
    ed.gen++;
    request_preview();
  }
  set_counts_text();
}

static BOOL save(void) {
  wchar_t name[MAX_NAME + 1];
  GetWindowTextW(ed.name, name, MAX_NAME + 1);
  wchar_t *start = name, *end = name + wcslen(name);
  while (*start && iswspace(*start)) start++;
  while (end > start && iswspace(end[-1])) end--;
  *end = L'\0';
  char *name8 = wstr_to_utf8(start), *rule = build(-1, TRUE);
  BOOL ok = name8 && *name8 && rule && mc_smart_validate(ed.subject, rule) == MC_OK;
  enum mc_str_id failed = ed.creating ? MC_STR_WINDOWS_SMART_CREATE_FAILED : MC_STR_WINDOWS_SMART_SAVE_FAILED;
  if (ok && ed.creating) {
    ok = mc_smart_create(ed.lib, name8, ed.subject, rule, ed.uid) == MC_OK;
  } else if (ok) {
    if (!ed.old_name || wcscmp(start, ed.old_name)) {
      ok = mc_smart_rename(ed.lib, ed.uid, name8) == MC_OK;
      if (!ok) failed = MC_STR_WINDOWS_SMART_RENAME_FAILED;
      ed.changed |= ok;
    }
    if (ok && (!ed.old_rule || strcmp(rule, ed.old_rule))) {
      ok = mc_smart_set_rule(ed.lib, ed.uid, rule) == MC_OK;
      ed.changed |= ok;
    }
  }
  if (ok && ed.creating) ed.changed = TRUE;
  if (!ok) {
    wchar_t caption[128];
    GetWindowTextW(ed.dlg, caption, 128);
    MessageBoxW(ed.dlg, mc_wstr(failed), caption, MB_OK | MB_ICONERROR);
  }
  free(name8);
  free(rule);
  return ok;
}

/* Every control, the panel's too, gets the font for the new DPI: the icon font's ✕ buttons
 * their own, the rest the dialog's. */
static BOOL CALLBACK refont_child(HWND w, LPARAM old_icon) {
  HFONT f = (HFONT)SendMessageW(w, WM_GETFONT, 0, 0);
  SendMessageW(w, WM_SETFONT, (WPARAM)(f && f == (HFONT)old_icon ? ed.icon_font : ed.font), FALSE);
  wchar_t cls[32];
  if (GetClassNameW(w, cls, 32) && !_wcsicmp(cls, WC_COMBOBOXW))
    SendMessageW(w, CB_SETITEMHEIGHT, (WPARAM)-1, dy(ROW) - 6);
  /* A centred edit's padding depends on its font. */
  SetWindowPos(w, NULL, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  return TRUE;
}

/* After a move to a monitor with another DPI (the dialog manager's own scaling is off, as it
 * would miss the controls made here): the font scaled to it, base units from that font as
 * Windows works them out (the average width of the letters, and the height), every control
 * refonted, and the dialog moved to the suggested rect. */
static void on_dpi_changed(UINT dpi, const RECT *suggested) {
  LOGFONTW lf;
  if (!dpi || dpi == ed.dpi || !GetObjectW(ed.font, sizeof lf, &lf)) return;
  lf.lfHeight = MulDiv(lf.lfHeight, (int)dpi, (int)ed.dpi);
  HFONT font = CreateFontIndirectW(&lf);
  if (!font) return;
  HDC dc = GetDC(ed.dlg);
  HGDIOBJ old = SelectObject(dc, font);
  TEXTMETRICW tm;
  SIZE size;
  static const wchar_t letters[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  GetTextMetricsW(dc, &tm);
  GetTextExtentPoint32W(dc, letters, 52, &size);
  SelectObject(dc, old);
  ReleaseDC(ed.dlg, dc);
  ed.base_x = (size.cx / 26 + 1) / 2;
  ed.base_y = tm.tmHeight;
  ed.dpi = dpi;

  HFONT old_font = ed.own_font, old_icon = ed.icon_font, icon = make_icon_font();
  ed.font = ed.own_font = font;
  if (icon) ed.icon_font = icon;
  EnumChildWindows(ed.dlg, refont_child, (LPARAM)old_icon);
  if (icon && old_icon) DeleteObject(old_icon);
  if (old_font) DeleteObject(old_font);
  if (ed.tip) SendMessageW(ed.tip, TTM_SETMAXTIPWIDTH, 0, dx(200));
  SetWindowPos(ed.dlg, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
  layout_dialog(); /* though the size may not have changed */
  RedrawWindow(ed.dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

static void free_rows(void) {
  for (int i = 0; i < ed.nitems; i++) free_item(ed.items[i]);
  ed.nitems = 0;
  while (ed.nsorts) remove_sort(ed.nsorts - 1);
}

typedef struct editor_args {
  const wchar_t *caption, *name;
  const char *rule;
} editor_args;

static INT_PTR CALLBACK editor_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      const editor_args *a = (const editor_args *)lp;
      ed.dlg = dlg;
      SetDialogDpiChangeBehavior(dlg, DDC_DISABLE_ALL, DDC_DISABLE_ALL); /* on_dpi_changed() does it */
      SetWindowTextW(dlg, a->caption);
      SetDlgItemTextW(dlg, IDOK, mc_wstr_mnemonic(ed.creating ? MC_STR_CREATE : MC_STR_OK));
      SetDlgItemTextW(dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CANCEL));
      read_fields();
      create_controls();
      SetWindowTextW(ed.name, a->name ? a->name : L"");
      start_worker();
      combo_select_data(ed.match_box, MODE_ALL);
      if (!load_rule(a->rule)) {
        free_rows();
        show_read_only(a->rule);
      } else {
        combo_select_data(ed.match_box, ed.mode);
        /* A blank rule starts with one condition to fill in. */
        if (ed.creating && !ed.nitems && (ed.items[0] = new_item(blank_cond()))) ed.nitems = 1;
        if (!ed.has_seed) { /* a new seed, should Shuffled be picked */
          unsigned r = 0;
          rand_s(&r);
          ed.seed = (long long)(r & 0x7fffffff);
        }
        rule_changed();
        free(ed.old_rule);
        ed.old_rule = ed.creating ? NULL : build(-1, TRUE); /* as this editor writes it, to see whether it changed */
        request_preview();
      }
      apply_theme();
      layout_dialog();
      update_ok();
      SetFocus(ed.read_only ? GetDlgItem(dlg, IDCANCEL) : ed.name);
      SendMessageW(ed.name, EM_SETSEL, 0, -1);
      return FALSE;
    }
    case WM_SIZE:
      layout_dialog();
      RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      return TRUE;
    case WM_GETMINMAXINFO: {
      MINMAXINFO *mm = (MINMAXINFO *)lp;
      RECT r = {0, 0, dx(MIN_W), dy(MIN_H)};
      AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(dlg, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(dlg, GWL_EXSTYLE));
      mm->ptMinTrackSize.x = r.right - r.left;
      mm->ptMinTrackSize.y = r.bottom - r.top;
      return TRUE;
    }
    case WM_DPICHANGED:
      on_dpi_changed(HIWORD(wp), (const RECT *)lp);
      return TRUE;
    case WM_TIMER:
      if (wp == TIMER_PREVIEW) request_preview();
      return TRUE;
    case WM_APP_PREVIEW:
      on_preview((preview_result *)lp);
      return TRUE;
    case WM_NOTIFY: {
      NMHDR *hdr = (NMHDR *)lp;
      if (hdr->hwndFrom == ed.list && hdr->code == LVN_GETDISPINFOW) on_get_item((NMLVDISPINFOW *)lp);
      return TRUE;
    }
    case WM_COMMAND:
      if ((HWND)lp == ed.name && HIWORD(wp) == EN_CHANGE) {
        update_ok();
      } else if (LOWORD(wp) == IDOK) {
        if (IsWindowEnabled(GetDlgItem(dlg, IDOK)) && !ed.read_only && save()) EndDialog(dlg, IDOK);
      } else if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
      }
      return TRUE;
    case WM_CTLCOLORSTATIC:
      if ((HWND)lp == ed.summary) {
        SetTextColor((HDC)wp, theme.text);
        SetBkColor((HDC)wp, theme.bg);
        return (INT_PTR)theme.bg_brush;
      }
      /* fall through */
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return (INT_PTR)ctlcolor(msg, (HDC)wp, (HWND)lp);
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
      if (theme_is_change(msg, wp, lp)) {
        theme_update();
        apply_theme();
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_DESTROY:
      KillTimer(dlg, TIMER_PREVIEW);
      stop_worker();
      free_rows();
      if (ed.tip) DestroyWindow(ed.tip);
      ed.tip = NULL;
      HWND named[] = {ed.list, ed.match_box, ed.sorted, ed.shuffled, ed.limit_check, ed.limit};
      for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) clear_name(named[i]);
      if (ed.icon_font) DeleteObject(ed.icon_font);
      if (ed.own_font) DeleteObject(ed.own_font);
      ed.icon_font = ed.own_font = NULL;
      break;
  }
  return FALSE;
}

/* Runs the editor; ed's library, subject, creating and uid are set. */
static BOOL run_editor(HWND owner, const wchar_t *caption, const wchar_t *name, const char *rule) {
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_DATE_CLASSES | ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);
  if (mc_library_list_playlists(ed.lib, 0, &ed.playlists) != MC_OK) ed.playlists = NULL;
  if (mc_library_list_album_groups(ed.lib, &ed.groups) != MC_OK) ed.groups = NULL;
  editor_args a = {caption, name, rule};
  INT_PTR r = DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SMART_EDITOR), owner, editor_proc, (LPARAM)&a);
  BOOL changed = r == IDOK && ed.changed;
  mc_playlist_list_free(ed.playlists);
  mc_album_group_list_free(ed.groups);
  mc_track_list_free(ed.tracks);
  mc_album_list_free(ed.albums);
  free(ed.rule);
  free(ed.old_rule);
  if (ed.acc) IAccPropServices_Release(ed.acc);
  return changed;
}

/* --- Presets --- */

typedef struct preset {
  enum mc_str_id name;
  const char *rule;
} preset;

static const preset k_track_presets[] = {
  {MC_STR_WINDOWS_SMART_PRESET_RECENT,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}]},"
   "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}"},
  {MC_STR_WINDOWS_SMART_PRESET_LEAST_PLAYED,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]},"
   "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"last_played\",\"dir\":\"asc\"}],\"limit\":100}"},
  {MC_STR_WINDOWS_SMART_PRESET_NOT_PLAYED,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":180}}]},"
   "\"order\":[{\"field\":\"last_played\",\"dir\":\"asc\"}]}"},
  {MC_STR_WINDOWS_SMART_PRESET_FLAC,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true},"
   "{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}]}}"},
  {MC_STR_WINDOWS_SMART_PRESET_BLANK, "{\"v\":1}"},
};

static const preset k_album_presets[] = {
  {MC_STR_WINDOWS_SMART_PRESET_RECENT_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}]},"
   "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}"},
  {MC_STR_WINDOWS_SMART_PRESET_LEAST_PLAYED_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]},"
   "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"last_played\",\"dir\":\"asc\"}],\"limit\":20}"},
  {MC_STR_WINDOWS_SMART_PRESET_NOT_PLAYED_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":180}}]},"
   "\"order\":[{\"field\":\"last_played\",\"dir\":\"asc\"}]}"},
  {MC_STR_WINDOWS_SMART_PRESET_FLAC_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true},"
   "{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}]}}"},
  {MC_STR_WINDOWS_SMART_PRESET_BLANK, "{\"v\":1}"},
};
#define PRESET_COUNT 5

typedef struct preset_ask {
  const wchar_t *caption;
  const preset *presets;
  int chosen;
} preset_ask;

static INT_PTR CALLBACK preset_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  preset_ask *a = (preset_ask *)GetWindowLongPtrW(dlg, DWLP_USER);
  switch (msg) {
    case WM_INITDIALOG: {
      a = (preset_ask *)lp;
      SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)a);
      SetWindowTextW(dlg, a->caption);
      SetDlgItemTextW(dlg, IDC_SP_HEADING, mc_wstr(MC_STR_WINDOWS_SMART_PRESET_HEADING));
      SetDlgItemTextW(dlg, IDOK, mc_wstr_mnemonic(MC_STR_WINDOWS_NEXT));
      SetDlgItemTextW(dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CANCEL));
      HWND list = GetDlgItem(dlg, IDC_SP_LIST);
      for (int i = 0; i < PRESET_COUNT; i++) SendMessageW(list, LB_ADDSTRING, 0, (LPARAM)mc_wstr(a->presets[i].name));
      SendMessageW(list, LB_SETCURSEL, 0, 0);
      theme_apply_window(dlg, FALSE);
      theme_apply_controls(dlg);
      SetFocus(list);
      return FALSE;
    }
    case WM_COMMAND:
      if (LOWORD(wp) == IDC_SP_LIST && HIWORD(wp) == LBN_DBLCLK) wp = IDOK;
      if (LOWORD(wp) == IDOK) {
        a->chosen = (int)SendDlgItemMessageW(dlg, IDC_SP_LIST, LB_GETCURSEL, 0, 0);
        EndDialog(dlg, a->chosen >= 0 ? IDOK : IDCANCEL);
        return TRUE;
      }
      if (LOWORD(wp) == IDCANCEL) {
        EndDialog(dlg, IDCANCEL);
        return TRUE;
      }
      break;
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX:
      return (INT_PTR)theme_ctlcolor(msg, (HDC)wp);
  }
  return FALSE;
}

BOOL smart_new_dialog(HWND owner, mc_library *lib, mc_smart_subject subject, char out_uid[MC_UID_STRLEN]) {
  if (!lib) return FALSE;
  const wchar_t *caption = mc_wstr(subject == MC_SMART_TRACKS ? MC_STR_WINDOWS_SMART_NEW_PLAYLIST_TITLE
                                                             : MC_STR_WINDOWS_SMART_NEW_GROUP_TITLE);
  preset_ask pa = {caption, subject == MC_SMART_TRACKS ? k_track_presets : k_album_presets, -1};
  if (DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SMART_PRESET), owner, preset_proc, (LPARAM)&pa) !=
        IDOK ||
      pa.chosen < 0 || pa.chosen >= PRESET_COUNT)
    return FALSE;
  memset(&ed, 0, sizeof ed);
  ed.lib = lib;
  ed.subject = subject;
  ed.creating = TRUE;
  const preset *p = &pa.presets[pa.chosen];
  BOOL blank = p->name == MC_STR_WINDOWS_SMART_PRESET_BLANK;
  BOOL created = run_editor(owner, caption, blank ? L"" : mc_wstr(p->name), p->rule);
  if (created && out_uid) memcpy(out_uid, ed.uid, MC_UID_STRLEN);
  return created;
}

/* The smart list uid's name, subject and rule, from the listing. */
static BOOL find_smart(mc_library *lib, const char *uid, wchar_t **name, mc_smart_subject *subject, char **rule) {
  mc_smart_list *list = NULL;
  BOOL found = FALSE;
  if (lib && uid && mc_library_list_smart(lib, 0, &list) == MC_OK)
    for (int i = 0; i < list->count && !found; i++) {
      const mc_smart_info *s = &list->items[i];
      if (strcmp(s->uid, uid)) continue;
      found = TRUE;
      if (name) *name = widen(s->name ? s->name : "");
      if (subject) *subject = s->subject;
      if (rule) *rule = _strdup(s->rule ? s->rule : "");
    }
  mc_smart_list_free(list);
  return found;
}

BOOL smart_edit_dialog(HWND owner, mc_library *lib, const char *uid) {
  wchar_t *name = NULL;
  char *rule = NULL;
  mc_smart_subject subject;
  if (!find_smart(lib, uid, &name, &subject, &rule) || !rule) {
    free(name);
    free(rule);
    return FALSE;
  }
  memset(&ed, 0, sizeof ed);
  ed.lib = lib;
  ed.subject = subject;
  snprintf(ed.uid, sizeof ed.uid, "%s", uid);
  ed.old_name = name;
  BOOL changed = run_editor(owner,
                            mc_wstr(subject == MC_SMART_TRACKS ? MC_STR_WINDOWS_SMART_EDIT_PLAYLIST_TITLE
                                                               : MC_STR_WINDOWS_SMART_EDIT_GROUP_TITLE),
                            name, rule);
  free(name);
  free(rule);
  return changed;
}

wchar_t *smart_describe(mc_library *lib, const char *uid) {
  char *rule = NULL, *sentence = NULL;
  mc_smart_subject subject;
  wchar_t *w = NULL;
  if (find_smart(lib, uid, NULL, &subject, &rule) && rule &&
      mc_smart_describe(lib, subject, rule, &sentence) == MC_OK)
    w = widen(sentence);
  mc_free(sentence);
  free(rule);
  return w;
}
