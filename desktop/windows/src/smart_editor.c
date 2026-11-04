#define _CRT_RAND_S /* rand_s(), for a shuffled list's seed */
#define COBJMACROS
#include "smart_editor.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <oleacc.h>
#include <uxtheme.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <yyjson.h>

#include "canvas.h"
#include "resource.h"
#include "theme.h"
#include "wstr.h"

#define WM_APP_PREVIEW (WM_APP + 40) /* lParam: result *, from the preview thread */
#define WM_APP_REVEAL (WM_APP + 41)  /* to the panel, wParam: a child that got the focus */
#define WM_APP_REBUILD (WM_APP + 42) /* to the dialog: make the shown page's controls again */

enum { TIMER_PREVIEW = 1, PREVIEW_DELAY_MS = 250, PROBLEM_ROWS = 4 };

/* Caps that keep the arrays fixed. The core allows 32 levels of nesting, the list's own
 * group being the first. */
enum { MAX_FIELDS = 32, MAX_SORTS = 8, MAX_VALUE = 256, MAX_NAME = 255, MAX_DEPTH = 31, PRESET_COUNT = 5 };

enum { MODE_ALL, MODE_ANY, MODE_NONE };
enum { UNIT_DAYS, UNIT_WEEKS, UNIT_MONTHS };
enum { PROBLEM_NONE, PROBLEM_VALUE, PROBLEM_NUMBER, PROBLEM_DURATION, PROBLEM_CHOICE };
enum { CHOICE_NONE = -1, CHOICE_EXTRA = -2 };

/* Where the focus goes once a page's controls are made again. */
enum { FOCUS_NONE, FOCUS_NAME, FOCUS_FIRST, FOCUS_FIELD, FOCUS_OP, FOCUS_VALUE, FOCUS_CARD, FOCUS_ADD_CONDITION,
       FOCUS_SORT, FOCUS_SORT_ADD, FOCUS_PRESET };

/* Layout, in dialog units. */
enum {
  MARGIN = 7,
  GAP = 4,     /* between controls on a row */
  ROW = 14,    /* a row's controls */
  ROW_GAP = 4, /* between rows */
  HINT_H = 9,  /* a line under a condition */
  CARD_H = 24, /* a group's or ready-made rule's button: two lines */
  FIELD_W = 76,
  OP_W = 88,
  NUM_W = 40, /* a number beside a unit, or each end of "between" */
  AND_W = 16,
  UNIT_W = 50,
  COUNT_W = 30,
  REMOVE_W = 14,
  MODE_W = 110,
  SORT_W = 130,
  BUTTON_W = 54,
  CRUMB_MAX = 120,  /* a group's name in the path, at most */
  LEFT_MIN = 330,   /* the conditions' panel, at its narrowest */
  PREVIEW_MIN = 150, /* the preview beside it, at its narrowest; narrower windows put it below */
  MIN_W = 354,
  MIN_H = 314,
};

/* The formats a condition offers: the importer's file extensions (tagger.c), which is what
 * files.format holds. A rule naming another one keeps it as an extra entry. */
static const struct {
  const char *value;
  const wchar_t *label;
} k_formats[] = {
  {"flac", L"FLAC"}, {"mp3", L"MP3"}, {"opus", L"Opus"}, {"wav", L"WAV"}, {"aiff", L"AIFF"}, {"aif", L"AIF"},
};
#define FORMAT_COUNT ((int)(sizeof k_formats / sizeof k_formats[0]))

/* --- The rule, as the editor holds it --- */

/* A group of conditions, or a condition. Rows, and the counts from the thread, refer to one
 * by its id, so a count for a node that's since gone finds nothing. */
typedef struct node node;
struct node {
  UINT id;
  node *parent; /* NULL for the list's own group */
  BOOL is_group;
  /* A group */
  int mode;
  char *name; /* NULL or "" if it has none */
  node **kids;
  int nkids, cap;
  char *desc; /* mc_smart_describe() of it alone, as last worked out; NULL if none */
  BOOL bad;   /* something in it has a problem */
  /* A condition */
  int field, op;        /* into ed.fields, and that field's ops */
  char *value, *value2; /* UTF-8, as typed: a text, a number or a length; value2 the high end of between */
  int unit;
  long long date; /* before and after: local midnight, Unix ms */
  int choice;     /* Yes (1) or No (0); a playlist, group or format by index, or CHOICE_EXTRA */
  char *extra;    /* the choice's extra entry: a deleted playlist's or group's uid, or another format */
  int problem;
  /* What it matches on its own, as last counted, and the rule that was counted */
  int count;
  char *counted;
};

/* A row on the shown page, for a node. A condition has the value controls its field and
 * operator need, and NULL for the rest. */
typedef struct row {
  UINT id;
  HWND field, op, value, and_label, value2, unit, date, choice; /* a condition's */
  HWND card;                                                    /* a group's: opens its page */
  HWND count, remove, hint;
} row;

typedef struct sort_row {
  HWND label, box, remove; /* box's item data: sortable field index * 2 + desc */
} sort_row;

/* The thread's work: the rule without its limit, what the shown page previews, and the rule
 * for each row's count. */
typedef struct job {
  UINT gen;
  mc_smart_subject subject;
  char *list_rule;
  char *preview_rule; /* the list's rule on its own page, else the shown group's; may be NULL */
  BOOL preview_is_list;
  int n;
  UINT *ids;
  char **rules;
} job;

typedef struct result {
  UINT gen;
  int total; /* the list's, without its limit; -1 if it couldn't be worked out */
  BOOL preview_ok;
  mc_track_list *tracks;
  mc_album_list *albums;
  int n;
  UINT *ids;
  char **rules;
  int *counts; /* -1 where a count wasn't made */
} result;

typedef struct worker {
  CRITICAL_SECTION lock;
  CONDITION_VARIABLE wake;
  job *job; /* the next one; a newer one replaces it */
  BOOL quit;
  HWND dlg;
  char *dir; /* the library's, for the thread's own connection */
  HANDLE thread;
} worker;

typedef struct preset {
  enum mc_str_id name;
  const char *rule;
} preset;

/* The open editor. Modal, so there's one at a time. */
static struct {
  HWND dlg, panel, tip;
  HWND back, crumbs[MAX_DEPTH + 1], seps[MAX_DEPTH];
  int ncrumbs;
  HWND preview_label, preview_note, list;
  HWND problems_title, problem_lines[PROBLEM_ROWS], problem_show[PROBLEM_ROWS], problems_more;
  UINT problem_ids[PROBLEM_ROWS];
  RECT problems_box; /* the frame around the problems, in the dialog's client coordinates */
  RECT card;         /* the content area, solid, around the page, preview and problems; likewise */
  RECT divider;      /* the line between the page and the preview, if it's shown; likewise */
  HWND status;
  BOOL mica;         /* this window has Mica (theme.mica is the main window's) */
  MARGINS extended;  /* the frame as last extended */
  canvas surface;    /* the dialog's own painting */
  HFONT font, bold_font, icon_font;
  HFONT own_font; /* the font for a DPI moved to, which ed.font then is; NULL for the dialog's */
  UINT dpi;
  int base_x, base_y; /* the dialog's base units, which dx() and dy() scale by */
  IAccPropServices *acc;
  HHOOK hook;
  HWND hot; /* the owner-drawn button under the mouse */

  /* The shown page's controls, children of the panel, made again for each page. */
  HWND name_label, name, mode, placeholder, add_condition, add_group;
  HWND order_heading, sort_add, shuffled, shuffled_label, limited, limited_label, limit_box, limit_unit;
  sort_row sort_rows[MAX_SORTS];
  HWND read_only_note, read_only_text;
  HWND preset_heading, presets[PRESET_COUNT];
  row *rows;
  int nrows;

  mc_library *lib;
  mc_smart_subject subject;
  BOOL creating, read_only;
  BOOL choosing; /* a new list's ready-made rules are shown */
  BOOL changed;  /* the library changed */
  char uid[MC_UID_STRLEN];
  char *name_text; /* the list's name as typed, trimmed (UTF-8) */
  char *old_name, *old_rule;
  const preset *presets_of;

  mc_smart_field fields[MAX_FIELDS];
  int nfields;
  int sortable[MAX_FIELDS]; /* the sortable fields, into fields */
  int nsortable;
  mc_playlist_list *playlists;
  mc_album_group_list *groups;

  node *root;
  UINT next_id;
  int sorts[MAX_SORTS]; /* sortable index * 2 + desc */
  int nsorts;
  BOOL random, limited_on;
  long long seed;
  int limit;

  /* The groups opened, the list's first; the shown page is the last. */
  UINT stack[MAX_DEPTH + 1];
  int scrolls[MAX_DEPTH + 1];
  int depth;
  int scroll, content_h; /* the panel's, in pixels */
  BOOL wide;             /* the preview is beside the panel */
  BOOL rebuild_posted;
  int focus_what, focus_index;
  UINT focus_id;

  node **bad; /* the conditions with problems, in the rule's order */
  int nbad, bad_cap;
  worker *w;
  UINT gen;
  BOOL counted; /* a result has come back for the rule as it stands */
  int total;
  BOOL building; /* controls being set up: their notifications aren't edits */
  mc_track_list *tracks;
  mc_album_list *albums;
  int shown; /* rows in the preview */
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

static char *dup(const char *s) {
  return _strdup(s ? s : "");
}

/* s without surrounding spaces, malloc'd. */
static char *trimmed(const char *s) {
  if (!s) s = "";
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) n--;
  char *out = malloc(n + 1);
  if (out) {
    memcpy(out, s, n);
    out[n] = '\0';
  }
  return out;
}

static BOOL blank(const char *s) {
  char *t = trimmed(s);
  BOOL b = !t || !*t;
  free(t);
  return b;
}

/* A window's text as UTF-8 (malloc'd). */
static char *text_of(HWND w) {
  wchar_t buf[MAX_VALUE + 1];
  GetWindowTextW(w, buf, MAX_VALUE + 1);
  return wstr_to_utf8(buf);
}

static void set_text_utf8(HWND w, const char *s) {
  wchar_t *t = wstr_dup_from_utf8(s ? s : "");
  SetWindowTextW(w, t ? t : L"");
  free(t);
}

/* A whole number of at most 12 digits, ignoring spaces around it. */
static BOOL parse_int(const char *text, long long *out) {
  char *s = trimmed(text);
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
static BOOL parse_duration(const char *text, long long *out) {
  char *s = trimmed(text);
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

static char *format_duration(long long s) {
  char buf[64];
  if (s >= 3600) snprintf(buf, sizeof buf, "%lld:%02lld:%02lld", s / 3600, s / 60 % 60, s % 60);
  else snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
  return dup(buf);
}

static char *format_int(long long n) {
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", n);
  return dup(buf);
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

static long long today(void) {
  SYSTEMTIME now;
  GetLocalTime(&now);
  return date_to_ms(&now);
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

static void add_tip(HWND w) {
  TTTOOLINFOW ti = {.cbSize = sizeof ti, .uFlags = TTF_IDISHWND | TTF_SUBCLASS, .hwnd = GetParent(w),
                    .uId = (UINT_PTR)w, .lpszText = LPSTR_TEXTCALLBACKW};
  if (ed.tip && w) SendMessageW(ed.tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void destroy(HWND w) {
  if (!w) return;
  if (ed.tip) {
    TTTOOLINFOW ti = {.cbSize = sizeof ti, .hwnd = GetParent(w), .uId = (UINT_PTR)w};
    SendMessageW(ed.tip, TTM_DELTOOLW, 0, (LPARAM)&ti);
  }
  clear_name(w);
  if (ed.hot == w) ed.hot = NULL;
  DestroyWindow(w);
}

static COLORREF error_color(void) {
  return theme.high_contrast ? theme.text : theme.dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
}

/* --- Fields and operators --- */

static const mc_smart_field *field_of(const node *c) {
  return &ed.fields[c->field];
}

static const char *op_of(const node *c) {
  return field_of(c)->ops[c->op];
}

static BOOL type_is(const node *c, const char *type) {
  return !strcmp(field_of(c)->type, type);
}

static BOOL op_is(const node *c, const char *op) {
  return !strcmp(op_of(c), op);
}

static BOOL takes_no_value(const node *c) {
  return op_is(c, "empty") || op_is(c, "not_empty");
}

static BOOL is_choice(const node *c) {
  return type_is(c, "bool") || type_is(c, "playlist") || type_is(c, "group") || type_is(c, "format");
}

static BOOL is_days(const node *c) {
  return type_is(c, "date") && (op_is(c, "within") || op_is(c, "not_within"));
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

/* An operator's label. A date's empty and not_empty read as never and any time: "Last played
 * is never". */
static const char *op_label(const char *type, const char *op) {
  if (!strcmp(type, "date") && !strcmp(op, "empty")) return mc_str(MC_STR_SMART_OP_NEVER);
  if (!strcmp(type, "date") && !strcmp(op, "not_empty")) return mc_str(MC_STR_SMART_OP_EVER);
  const char *label = mc_smart_op_label(type, op);
  return label ? label : op;
}

/* The field's name inside a sentence, for a negative text condition's hint. */
static enum mc_str_id in_text_id(const char *field) {
  static const struct {
    const char *name;
    enum mc_str_id id;
  } k[] = {
    {"title", MC_STR_SMART_FIELD_TITLE_IN_TEXT}, {"artist", MC_STR_SMART_FIELD_ARTIST_IN_TEXT},
    {"album_artist", MC_STR_SMART_FIELD_ALBUM_ARTIST_IN_TEXT}, {"album", MC_STR_SMART_FIELD_ALBUM_IN_TEXT},
    {"genre", MC_STR_SMART_FIELD_GENRE_IN_TEXT},
  };
  for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
    if (!strcmp(k[i].name, field)) return k[i].id;
  return MC_STR_COUNT;
}

/* --- Nodes --- */

static node *node_new(node *parent, BOOL is_group) {
  node *n = calloc(1, sizeof *n);
  if (!n) return NULL;
  if (parent && parent->nkids == parent->cap) {
    int cap = parent->cap ? parent->cap * 2 : 8;
    node **kids = realloc(parent->kids, (size_t)cap * sizeof *kids);
    if (!kids) {
      free(n);
      return NULL;
    }
    parent->kids = kids;
    parent->cap = cap;
  }
  n->id = ++ed.next_id;
  n->parent = parent;
  n->is_group = is_group;
  n->count = -1;
  n->choice = CHOICE_NONE;
  if (parent) parent->kids[parent->nkids++] = n;
  return n;
}

static void node_free(node *n) {
  if (!n) return;
  for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
  free(n->kids);
  free(n->name);
  free(n->desc);
  free(n->value);
  free(n->value2);
  free(n->extra);
  free(n->counted);
  free(n);
}

static node *find_in(node *g, UINT id) {
  if (!g || g->id == id) return g;
  for (int i = 0; i < g->nkids; i++) {
    node *n = find_in(g->kids[i], id);
    if (n) return n;
  }
  return NULL;
}

static node *find(UINT id) {
  return id ? find_in(ed.root, id) : NULL;
}

static BOOL named(const node *g) {
  return g->name && *g->name;
}

static int depth_of(const node *g) {
  int d = 0;
  for (; g && g->parent; g = g->parent) d++;
  return d;
}

/* The first choice for a condition's picker, or CHOICE_NONE if it has nothing to pick. */
static int first_choice(const node *c) {
  if (type_is(c, "bool")) return 1;
  if (type_is(c, "playlist")) return ed.playlists && ed.playlists->count ? 0 : CHOICE_NONE;
  if (type_is(c, "group")) return ed.groups && ed.groups->count ? 0 : CHOICE_NONE;
  return 0;
}

/* Values that suit c's field, after it's changed to another type. */
static void reset_values(node *c) {
  free(c->value);
  free(c->value2);
  free(c->extra);
  c->value = dup(type_is(c, "date") ? "30" : "");
  c->value2 = dup("");
  c->extra = NULL;
  c->unit = UNIT_DAYS;
  c->date = today();
  c->choice = first_choice(c);
}

/* A new condition: Genre for tracks, which most rules start from, else Artist. */
static node *cond_new(node *parent) {
  node *c = node_new(parent, FALSE);
  if (!c) return NULL;
  int f = find_field(ed.subject == MC_SMART_TRACKS ? "genre" : "artist");
  c->field = f >= 0 ? f : 0;
  c->op = default_op(c->field);
  reset_values(c);
  return c;
}

/* --- The rule as JSON --- */

/* c's value is missing or can't be read: why, or PROBLEM_NONE. */
static int cond_problem(const node *c) {
  if (takes_no_value(c)) return PROBLEM_NONE;
  long long n;
  if (type_is(c, "text")) return blank(c->value) ? PROBLEM_VALUE : PROBLEM_NONE;
  if (type_is(c, "int") || type_is(c, "duration")) {
    BOOL dur = type_is(c, "duration");
    const char *vals[2] = {c->value, op_is(c, "between") ? c->value2 : NULL};
    for (int i = 0; i < 2; i++) {
      if (i && !vals[i]) break;
      if (blank(vals[i])) return PROBLEM_VALUE;
      if (!(dur ? parse_duration(vals[i], &n) : parse_int(vals[i], &n))) return dur ? PROBLEM_DURATION : PROBLEM_NUMBER;
    }
    return PROBLEM_NONE;
  }
  if (is_days(c)) {
    if (blank(c->value)) return PROBLEM_VALUE;
    static const int k_unit_days[] = {1, 7, 30};
    return parse_int(c->value, &n) && n * k_unit_days[c->unit] <= 1000000 ? PROBLEM_NONE : PROBLEM_NUMBER;
  }
  if (type_is(c, "date") || type_is(c, "bool")) return PROBLEM_NONE;
  return c->choice == CHOICE_NONE ? PROBLEM_CHOICE : PROBLEM_NONE;
}

/* c's condition, or NULL if it has a problem. */
static yyjson_mut_val *cond_json(yyjson_mut_doc *d, const node *c) {
  if (cond_problem(c)) return NULL;
  yyjson_mut_val *v = NULL;
  long long n = 0, m = 0;
  if (takes_no_value(c)) {
  } else if (type_is(c, "text")) {
    char *s = trimmed(c->value);
    v = yyjson_mut_strcpy(d, s ? s : "");
    free(s);
  } else if (type_is(c, "int") || type_is(c, "duration")) {
    BOOL dur = type_is(c, "duration");
    if (dur) parse_duration(c->value, &n);
    else parse_int(c->value, &n);
    if (op_is(c, "between")) {
      if (dur) parse_duration(c->value2, &m);
      else parse_int(c->value2, &m);
      v = yyjson_mut_arr(d);
      yyjson_mut_arr_add_sint(d, v, n);
      yyjson_mut_arr_add_sint(d, v, m);
    } else {
      v = yyjson_mut_sint(d, n);
    }
  } else if (is_days(c)) {
    static const int k_unit_days[] = {1, 7, 30};
    parse_int(c->value, &n);
    v = yyjson_mut_obj(d);
    yyjson_mut_obj_add_sint(d, v, "days", n * k_unit_days[c->unit]);
  } else if (type_is(c, "date")) {
    v = yyjson_mut_sint(d, c->date);
  } else if (type_is(c, "bool")) {
    v = yyjson_mut_bool(d, c->choice != 0);
  } else {
    int i = c->choice;
    const char *s = i == CHOICE_EXTRA        ? c->extra
                    : i < 0                  ? NULL
                    : type_is(c, "playlist") ? (ed.playlists && i < ed.playlists->count ? ed.playlists->items[i].uid : NULL)
                    : type_is(c, "group")    ? (ed.groups && i < ed.groups->count ? ed.groups->items[i].uid : NULL)
                    : i < FORMAT_COUNT       ? k_formats[i].value
                                             : NULL;
    if (!s) return NULL;
    v = yyjson_mut_strcpy(d, s);
  }
  yyjson_mut_val *obj = yyjson_mut_obj(d);
  yyjson_mut_obj_add_str(d, obj, "field", field_of(c)->name);
  yyjson_mut_obj_add_str(d, obj, "op", op_of(c));
  if (v) yyjson_mut_obj_add_val(d, obj, "value", v);
  return obj;
}

static yyjson_mut_val *node_json(yyjson_mut_doc *d, const node *n, BOOL lenient, BOOL *ok);

/* {"name", "all": [...]}, {"name", "any": [...]} or, for none, {"name", "not": {"any": [...]}}.
 * Lenient leaves out what has a problem, and empty groups, as for a sentence; otherwise a
 * problem anywhere clears *ok. */
static yyjson_mut_val *group_json(yyjson_mut_doc *d, const node *g, BOOL lenient, BOOL *ok) {
  yyjson_mut_val *list = yyjson_mut_arr(d);
  for (int i = 0; i < g->nkids; i++) {
    const node *k = g->kids[i];
    if (lenient && k->is_group && !k->nkids) continue;
    yyjson_mut_val *v = node_json(d, k, lenient, ok);
    if (v) yyjson_mut_arr_append(list, v);
  }
  yyjson_mut_val *obj = yyjson_mut_obj(d);
  if (named(g) && g->parent) yyjson_mut_obj_add_strcpy(d, obj, "name", g->name);
  if (g->mode != MODE_NONE) {
    yyjson_mut_obj_add_val(d, obj, g->mode == MODE_ALL ? "all" : "any", list);
    return obj;
  }
  yyjson_mut_val *inner = yyjson_mut_obj(d);
  yyjson_mut_obj_add_val(d, inner, "any", list);
  yyjson_mut_obj_add_val(d, obj, "not", inner);
  return obj;
}

static yyjson_mut_val *node_json(yyjson_mut_doc *d, const node *n, BOOL lenient, BOOL *ok) {
  if (n->is_group) return group_json(d, n, lenient, ok);
  yyjson_mut_val *v = cond_json(d, n);
  if (!v && !lenient) *ok = FALSE;
  return v;
}

/* A document with {"v": 1} at its root, which *root is. */
static yyjson_mut_doc *rule_doc(yyjson_mut_val **root) {
  yyjson_mut_doc *d = yyjson_mut_doc_new(NULL);
  if (!d) return NULL;
  *root = yyjson_mut_obj(d);
  yyjson_mut_doc_set_root(d, *root);
  yyjson_mut_obj_add_int(d, *root, "v", 1);
  return d;
}

static char *write_doc(yyjson_mut_doc *d, BOOL ok) {
  char *json = ok && d ? yyjson_mut_write(d, 0, NULL) : NULL;
  yyjson_mut_doc_free(d);
  return json;
}

/* {"v":1,"match":<n>} as written (free it), or NULL if strict and n has a problem. */
static char *match_json(const node *n, BOOL lenient) {
  yyjson_mut_val *root;
  yyjson_mut_doc *d = rule_doc(&root);
  if (!d) return NULL;
  BOOL ok = TRUE;
  yyjson_mut_val *v = node_json(d, n, lenient, &ok);
  if (v) yyjson_mut_obj_add_val(d, root, "match", v);
  return write_doc(d, ok && v);
}

/* The whole rule as it stands (free it), or NULL if anything in it has a problem. */
static char *rule_json(BOOL with_limit) {
  yyjson_mut_val *root;
  yyjson_mut_doc *d = rule_doc(&root);
  if (!d) return NULL;
  BOOL ok = TRUE;
  if (ed.root->nkids) yyjson_mut_obj_add_val(d, root, "match", group_json(d, ed.root, FALSE, &ok));
  yyjson_mut_val *order = yyjson_mut_arr(d);
  for (int i = 0; i < ed.nsorts; i++) {
    int k = ed.sorts[i];
    yyjson_mut_val *key = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, key, "field", ed.fields[ed.sortable[k / 2]].name);
    yyjson_mut_obj_add_str(d, key, "dir", k % 2 ? "desc" : "asc");
    yyjson_mut_arr_append(order, key);
  }
  if (ed.random) {
    yyjson_mut_val *key = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, key, "field", "random");
    yyjson_mut_obj_add_sint(d, key, "seed", ed.seed);
    yyjson_mut_arr_append(order, key);
  }
  if (yyjson_mut_arr_size(order)) yyjson_mut_obj_add_val(d, root, "order", order);
  if (with_limit && ed.limited_on && ed.limit > 0) yyjson_mut_obj_add_sint(d, root, "limit", ed.limit);
  return write_doc(d, ok);
}

/* What the core says rule is (malloc'd), or NULL. */
static char *describe(const char *rule) {
  char *sentence = NULL, *out = NULL;
  if (rule && mc_smart_describe(ed.lib, ed.subject, rule, &sentence) == MC_OK) out = dup(sentence);
  mc_free(sentence);
  return out;
}

/* --- Reading a rule into the editor --- */

static long long json_int(yyjson_val *v) {
  return yyjson_is_sint(v) ? yyjson_get_sint(v) : yyjson_is_uint(v) ? (long long)yyjson_get_uint(v) : 0;
}

/* A "not" around a condition, as the negative operator that means the same: missing values
 * count as false either way, so only the ops the core writes as another's negation map. */
static const char *negated_op(const char *op) {
  static const char *const k[][2] = {{"is", "is_not"}, {"contains", "not_contains"}, {"empty", "not_empty"},
                                     {"=", "!="},      {"within", "not_within"}};
  for (size_t i = 0; op && i < sizeof k / sizeof k[0]; i++) {
    if (!strcmp(op, k[i][0])) return k[i][1];
    if (!strcmp(op, k[i][1])) return k[i][0];
  }
  return NULL;
}

/* A condition (or a "not" around one, if negated) into a new node of parent's; FALSE if the
 * editor can't show it. The rule has been validated, so values have the right shapes. */
static BOOL load_cond(node *parent, yyjson_val *leaf, BOOL negated) {
  int field = find_field(yyjson_get_str(yyjson_obj_get(leaf, "field")));
  if (field < 0) return FALSE;
  const char *type = ed.fields[field].type, *op = yyjson_get_str(yyjson_obj_get(leaf, "op"));
  yyjson_val *value = yyjson_obj_get(leaf, "value");
  BOOL yes = yyjson_get_bool(value);
  if (negated && !strcmp(type, "bool")) yes = !yes;
  else if (negated) op = negated_op(op);
  int op_i = find_op(field, op);
  if (op_i < 0) return FALSE;
  node *c = node_new(parent, FALSE);
  if (!c) return FALSE;
  c->field = field;
  c->op = op_i;
  reset_values(c);
  if (!strcmp(type, "text") && yyjson_is_str(value)) {
    free(c->value);
    c->value = dup(yyjson_get_str(value));
  } else if (!strcmp(type, "int") || !strcmp(type, "duration")) {
    BOOL dur = !strcmp(type, "duration");
    yyjson_val *vals[2] = {value, NULL};
    if (yyjson_is_arr(value)) vals[0] = yyjson_arr_get(value, 0), vals[1] = yyjson_arr_get(value, 1);
    char **to[2] = {&c->value, &c->value2};
    for (int i = 0; i < 2; i++) {
      if (!vals[i]) continue;
      long long n = json_int(vals[i]);
      if (n < 0) return FALSE; /* the boxes take digits only */
      free(*to[i]);
      *to[i] = dur ? format_duration(n) : format_int(n);
    }
  } else if (!strcmp(type, "date") && yyjson_is_obj(value)) {
    long long days = json_int(yyjson_obj_get(value, "days"));
    BOOL weeks = days >= 14 && days % 7 == 0; /* as mc_smart_describe() says it */
    free(c->value);
    c->value = format_int(weeks ? days / 7 : days);
    c->unit = weeks ? UNIT_WEEKS : UNIT_DAYS;
  } else if (!strcmp(type, "date") && value) {
    SYSTEMTIME day;
    c->date = json_int(value);
    if (!ms_to_date(c->date, &day)) return FALSE; /* the picker can't show it */
  } else if (!strcmp(type, "bool")) {
    c->choice = yes ? 1 : 0;
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
    if (found < 0) c->extra = dup(s);
    c->choice = found >= 0 ? found : CHOICE_EXTRA;
  }
  return TRUE;
}

/* {"all": [...]}, {"any": [...]} or {"not": {"any": [...]}}, each maybe with a "name": its
 * mode, list and name. FALSE for anything else. */
static BOOL group_of(yyjson_val *v, int *mode, yyjson_val **list, const char **name) {
  *name = yyjson_get_str(yyjson_obj_get(v, "name"));
  yyjson_val *not = yyjson_obj_get(v, "not");
  if (not) {
    *list = yyjson_obj_get(not, "any");
    *mode = MODE_NONE;
    return *list != NULL;
  }
  if ((*list = yyjson_obj_get(v, "all"))) *mode = MODE_ALL;
  else if ((*list = yyjson_obj_get(v, "any"))) *mode = MODE_ANY;
  return *list != NULL;
}

/* v into a new node of parent's. FALSE if the editor can't show it. */
static BOOL load_node(node *parent, yyjson_val *v) {
  int mode;
  yyjson_val *list;
  const char *name;
  if (group_of(v, &mode, &list, &name)) {
    node *g = node_new(parent, TRUE);
    if (!g || depth_of(g) > MAX_DEPTH) return FALSE;
    g->mode = mode;
    g->name = name ? dup(name) : NULL;
    size_t i, max;
    yyjson_val *k;
    yyjson_arr_foreach(list, i, max, k) if (!load_node(g, k)) return FALSE;
    return TRUE;
  }
  yyjson_val *not = yyjson_obj_get(v, "not");
  if (not && (yyjson_obj_get(not, "all") || yyjson_obj_get(not, "any") || yyjson_obj_get(not, "not"))) return FALSE;
  return load_cond(parent, not ? not : v, not != NULL);
}

/* rule into ed.root and the order. FALSE if the editor can't show it: it's read-only then. */
static BOOL load_rule(const char *rule) {
  if (!rule || mc_smart_validate(ed.subject, rule) != MC_OK) return FALSE;
  yyjson_doc *doc = yyjson_read(rule, strlen(rule), 0);
  yyjson_val *root = yyjson_doc_get_root(doc), *match = yyjson_obj_get(root, "match");
  BOOL ok = doc != NULL && json_int(yyjson_obj_get(root, "v")) == 1;
  int mode;
  yyjson_val *list = NULL;
  const char *name;
  size_t i, max;
  if (ok && match && group_of(match, &mode, &list, &name)) {
    ed.root->mode = mode;
    yyjson_val *k;
    yyjson_arr_foreach(list, i, max, k) if (ok) ok = load_node(ed.root, k);
  } else if (ok && match) { /* one condition on its own */
    ok = load_node(ed.root, match);
  }
  yyjson_val *key;
  yyjson_arr_foreach(yyjson_obj_get(root, "order"), i, max, key) {
    if (!ok) break;
    const char *field = yyjson_get_str(yyjson_obj_get(key, "field"));
    if (field && !strcmp(field, "random")) {
      ed.random = TRUE;
      ed.seed = json_int(yyjson_obj_get(key, "seed"));
      continue;
    }
    int f = find_field(field), s = -1;
    for (int j = 0; j < ed.nsortable && f >= 0; j++)
      if (ed.sortable[j] == f) s = j;
    yyjson_val *dir = yyjson_obj_get(key, "dir");
    ok = s >= 0 && ed.nsorts < MAX_SORTS;
    if (ok) ed.sorts[ed.nsorts++] = s * 2 + (dir && yyjson_equals_str(dir, "desc") ? 1 : 0);
  }
  yyjson_val *limit = yyjson_obj_get(root, "limit");
  if (ok && limit) {
    long long n = json_int(limit);
    ed.limited_on = TRUE;
    ed.limit = n < 1 ? 1 : n > INT_MAX ? INT_MAX : (int)n;
  }
  yyjson_doc_free(doc);
  return ok;
}

/* --- Problems --- */

static const char *problem_text(int problem) {
  switch (problem) {
    case PROBLEM_NUMBER: return mc_str(MC_STR_SMART_PROBLEM_NUMBER);
    case PROBLEM_DURATION: return mc_str(MC_STR_SMART_PROBLEM_DURATION);
    case PROBLEM_CHOICE: return mc_str(MC_STR_SMART_PROBLEM_CHOICE);
    default: return mc_str(MC_STR_SMART_PROBLEM_VALUE);
  }
}

static void add_bad(node *c) {
  if (ed.nbad == ed.bad_cap) {
    int cap = ed.bad_cap ? ed.bad_cap * 2 : 16;
    node **bad = realloc(ed.bad, (size_t)cap * sizeof *bad);
    if (!bad) return;
    ed.bad = bad;
    ed.bad_cap = cap;
  }
  ed.bad[ed.nbad++] = c;
}

/* Every condition's problem and every group's bad, with the conditions that have one added to
 * ed.bad in order. TRUE if g holds any. */
static BOOL check_group(node *g) {
  g->bad = FALSE;
  for (int i = 0; i < g->nkids; i++) {
    node *k = g->kids[i];
    if (k->is_group) {
      g->bad |= check_group(k);
    } else if ((k->problem = cond_problem(k))) {
      add_bad(k);
      g->bad = TRUE;
    }
  }
  return g->bad;
}

/* What a group is called in the path, the problems and its row: its name, else its sentence,
 * else "Group". UTF-8, static or the node's own. */
static const char *group_title(const node *g) {
  if (!g->parent) return ed.name_text && *ed.name_text ? ed.name_text : mc_str(MC_STR_UNTITLED);
  if (named(g)) return g->name;
  return g->desc ? g->desc : mc_str(MC_STR_SMART_GROUP_UNTITLED);
}

/* "Fresh stuff › Artist: needs a value", into buf. */
static void problem_line(const node *c, wchar_t *buf, size_t n) {
  const node *chain[MAX_DEPTH + 1];
  int nchain = 0;
  for (const node *g = c->parent; g && g->parent && nchain <= MAX_DEPTH; g = g->parent) chain[nchain++] = g;
  size_t cap = 64;
  for (int i = 0; i < nchain; i++) cap += strlen(group_title(chain[i])) + 16;
  char *where = malloc(cap);
  if (!where) {
    buf[0] = L'\0';
    return;
  }
  where[0] = '\0';
  for (int i = nchain - 1; i >= 0; i--) {
    strcat(where, group_title(chain[i]));
    strcat(where, mc_str(MC_STR_SMART_PATH_SEPARATOR));
  }
  const char *field = mc_smart_field_label(field_of(c)->name);
  char *all = malloc(strlen(where) + strlen(field ? field : "") + 1);
  if (all) {
    strcpy(all, where);
    strcat(all, field ? field : "");
    mc_wformat(buf, n, mc_str(MC_STR_SMART_PROBLEM), MC_S(all), MC_S(problem_text(c->problem)));
  } else {
    buf[0] = L'\0';
  }
  free(all);
  free(where);
}

/* --- Pages --- */

static node *page_group(void) {
  return ed.choosing ? NULL : find(ed.stack[ed.depth]);
}

static row *row_of(UINT id) {
  for (int i = 0; i < ed.nrows; i++)
    if (ed.rows[i].id == id) return &ed.rows[i];
  return NULL;
}

static row *row_of_window(HWND w) {
  for (int i = 0; w && i < ed.nrows; i++) {
    row *r = &ed.rows[i];
    if (w == r->field || w == r->op || w == r->value || w == r->value2 || w == r->unit || w == r->date ||
        w == r->choice || w == r->card || w == r->remove)
      return r;
  }
  return NULL;
}

/* The count beside a row: what n matches on its own, if it's been counted as n now stands. */
static void set_count(HWND label, node *n) {
  char *now = NULL;
  if (!(n->is_group && !n->nkids)) now = match_json(n, FALSE);
  BOOL shown = now && n->counted && n->count >= 0 && !strcmp(now, n->counted);
  wchar_t text[32] = L"", name[128] = L"";
  if (shown) {
    swprintf(text, 32, L"%d", n->count);
    mc_wformat(name, 128,
               mc_str_plural(ed.subject == MC_SMART_TRACKS ? MC_STR_SMART_TRACKS_MATCH : MC_STR_SMART_ALBUMS_MATCH, n->count),
               MC_D(n->count));
  }
  free(now);
  SetWindowTextW(label, text);
  set_name(label, name);
}

/* A condition row's line beneath: its problem, else that it depends on this device, else that
 * a negative text condition takes in what has no value. Into buf; FALSE if there's none. */
static BOOL cond_hint(const node *c, wchar_t *buf, size_t n) {
  if (c->problem) {
    wstr_from_utf8(buf, n, problem_text(c->problem));
  } else if (!strcmp(field_of(c)->name, "on_device") || !strcmp(field_of(c)->name, "format")) {
    wcsncpy(buf, mc_wstr(MC_STR_SMART_HINT_DEVICE), n - 1);
    buf[n - 1] = L'\0';
  } else if (in_text_id(field_of(c)->name) != MC_STR_COUNT && (op_is(c, "is_not") || op_is(c, "not_contains"))) {
    mc_wformat(buf, n,
               mc_str(ed.subject == MC_SMART_TRACKS ? MC_STR_SMART_HINT_MISSING_TRACKS : MC_STR_SMART_HINT_MISSING_ALBUMS),
               MC_S(mc_str(in_text_id(field_of(c)->name))));
  } else {
    buf[0] = L'\0';
    return FALSE;
  }
  return TRUE;
}

static void layout_panel(void);

/* Brings the shown page's rows, and its name's cue, up to date with the rule. */
static void update_page(void) {
  node *g = page_group();
  if (!g) return;
  for (int i = 0; i < ed.nrows; i++) {
    row *r = &ed.rows[i];
    node *n = find(r->id);
    if (!n) continue;
    if (n->is_group) {
      wchar_t *title = wstr_dup_from_utf8(group_title(n));
      SetWindowTextW(r->card, title ? title : L""); /* its name for screen readers too */
      free(title);
      InvalidateRect(r->card, NULL, FALSE); /* its count, drawn with it */
    } else {
      wchar_t hint[256];
      BOOL shown = cond_hint(n, hint, 256);
      SetWindowTextW(r->hint, shown ? hint : L"");
      set_count(r->count, n);
    }
  }
  if (g->parent && ed.name) {
    wchar_t *cue = wstr_dup_from_utf8(g->desc ? g->desc : "");
    SendMessageW(ed.name, EM_SETCUEBANNER, TRUE, (LPARAM)(cue ? cue : L""));
    free(cue);
  }
  layout_panel();
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
  if (!_wcsicmp(cls, WC_EDITW) && !(style & ES_MULTILINE)) theme_center_edit(w);
  theme_apply_control(w);
  return w;
}

static HWND make_label(const wchar_t *text) {
  return make(WC_STATICW, text, SS_NOPREFIX | SS_CENTERIMAGE);
}

static HWND make_heading(const wchar_t *text) {
  HWND w = make_label(text);
  if (w) SendMessageW(w, WM_SETFONT, (WPARAM)ed.bold_font, FALSE);
  return w;
}

static HWND make_combo(const wchar_t *name) {
  HWND w = make(WC_COMBOBOXW, NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
  set_name(w, name);
  return w;
}

static HWND make_edit(const char *text, const wchar_t *name, BOOL digits) {
  wchar_t *t = wstr_dup_from_utf8(text ? text : "");
  HWND w = make(WC_EDITW, t, WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL | (digits ? ES_NUMBER : 0));
  free(t);
  SendMessageW(w, EM_LIMITTEXT, MAX_VALUE, 0);
  if (name) set_name(w, name);
  return w;
}

static HWND make_button(enum mc_str_id label) {
  return make(WC_BUTTONW, mc_wstr_mnemonic(label), BS_PUSHBUTTON | WS_TABSTOP);
}

/* The ✕ on a condition, group or sort order: the icon font's Cancel, named for screen readers. */
static HWND make_remove(enum mc_str_id name) {
  HWND w = make(WC_BUTTONW, L"\xE711", BS_PUSHBUTTON | WS_TABSTOP);
  SendMessageW(w, WM_SETFONT, (WPARAM)ed.icon_font, FALSE);
  set_name(w, mc_wstr(name));
  add_tip(w);
  return w;
}

/* A group's or a ready-made rule's button, drawn here: a title and a line beneath. */
static HWND make_card(const wchar_t *title) {
  HWND w = make(WC_BUTTONW, title, BS_OWNERDRAW | WS_TABSTOP);
  add_tip(w);
  return w;
}

static void combo_add(HWND box, const wchar_t *text, LPARAM data) {
  int i = (int)SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)text);
  if (i >= 0) SendMessageW(box, CB_SETITEMDATA, i, data);
}

static void combo_add_utf8(HWND box, const char *text, LPARAM data) {
  wchar_t *w = wstr_dup_from_utf8(text ? text : "");
  if (w) combo_add(box, w, data);
  free(w);
}

static LPARAM combo_data(HWND box) {
  int i = (int)SendMessageW(box, CB_GETCURSEL, 0, 0);
  return i >= 0 ? SendMessageW(box, CB_GETITEMDATA, i, 0) : CB_ERR;
}

static void combo_select_data(HWND box, LPARAM data) {
  int n = (int)SendMessageW(box, CB_GETCOUNT, 0, 0);
  for (int i = 0; i < n; i++)
    if (SendMessageW(box, CB_GETITEMDATA, i, 0) == data) {
      SendMessageW(box, CB_SETCURSEL, i, 0);
      return;
    }
}

/* The picker of Yes and No, the playlists, the album groups or the formats, with c->extra as
 * one more if it's set. Item data: the choice. */
static HWND choice_box(node *c) {
  HWND box = make_combo(mc_wstr(MC_STR_SMART_VALUE_NAME));
  if (type_is(c, "bool")) {
    combo_add(box, mc_wstr(MC_STR_SMART_VALUE_YES), 1);
    combo_add(box, mc_wstr(MC_STR_SMART_VALUE_NO), 0);
  } else if (type_is(c, "playlist")) {
    for (int i = 0; ed.playlists && i < ed.playlists->count; i++) combo_add_utf8(box, ed.playlists->items[i].name, i);
  } else if (type_is(c, "group")) {
    for (int i = 0; ed.groups && i < ed.groups->count; i++) combo_add_utf8(box, ed.groups->items[i].name, i);
  } else {
    for (int i = 0; i < FORMAT_COUNT; i++) combo_add(box, k_formats[i].label, i);
  }
  if (c->extra && type_is(c, "format")) {
    wchar_t *w = wstr_dup_from_utf8(c->extra);
    for (wchar_t *p = w; p && *p; p++) *p = towupper(*p);
    if (w) combo_add(box, w, CHOICE_EXTRA);
    free(w);
  } else if (c->extra) {
    combo_add(box, mc_wstr(MC_STR_SMART_DELETED_MEMBER), CHOICE_EXTRA);
  }
  combo_select_data(box, c->choice);
  return box;
}

/*  [Field v] [op v] [value        ]  412  x
 *  Needs a value                               <- its problem, or a hint */
static void cond_row(row *r, node *c) {
  r->field = make_combo(mc_wstr(MC_STR_SMART_FIELD_NAME));
  for (int i = 0; i < ed.nfields; i++) combo_add_utf8(r->field, mc_smart_field_label(ed.fields[i].name), i);
  SendMessageW(r->field, CB_SETCURSEL, c->field, 0);
  r->op = make_combo(mc_wstr(MC_STR_SMART_OP_NAME));
  const mc_smart_field *f = field_of(c);
  for (int i = 0; f->ops[i]; i++) combo_add_utf8(r->op, op_label(f->type, f->ops[i]), i);
  SendMessageW(r->op, CB_SETCURSEL, c->op, 0);
  BOOL number = type_is(c, "int") || type_is(c, "duration");
  if (takes_no_value(c)) {
  } else if (is_choice(c)) {
    r->choice = choice_box(c);
  } else if (number) {
    r->value = make_edit(c->value, mc_wstr(MC_STR_SMART_VALUE_NAME), type_is(c, "int"));
    if (op_is(c, "between")) {
      r->and_label = make_label(mc_wstr(MC_STR_SMART_BETWEEN_AND));
      r->value2 = make_edit(c->value2, mc_wstr(MC_STR_SMART_VALUE_HIGH_NAME), type_is(c, "int"));
    }
    if (type_is(c, "duration")) {
      SendMessageW(r->value, EM_SETCUEBANNER, FALSE, (LPARAM)mc_wstr(MC_STR_SMART_DURATION_CUE));
      if (r->value2) SendMessageW(r->value2, EM_SETCUEBANNER, FALSE, (LPARAM)mc_wstr(MC_STR_SMART_DURATION_CUE));
    }
  } else if (is_days(c)) {
    r->value = make_edit(c->value, mc_wstr(MC_STR_SMART_VALUE_NAME), TRUE);
    r->unit = make_combo(mc_wstr(MC_STR_SMART_UNIT_NAME));
    combo_add(r->unit, mc_wstr(MC_STR_SMART_UNIT_DAYS), UNIT_DAYS);
    combo_add(r->unit, mc_wstr(MC_STR_SMART_UNIT_WEEKS), UNIT_WEEKS);
    combo_add(r->unit, mc_wstr(MC_STR_SMART_UNIT_MONTHS), UNIT_MONTHS);
    combo_select_data(r->unit, c->unit);
  } else if (type_is(c, "date")) {
    r->date = make(DATETIMEPICK_CLASSW, NULL, WS_TABSTOP | DTS_SHORTDATEFORMAT);
    set_name(r->date, mc_wstr(MC_STR_SMART_VALUE_NAME));
    SYSTEMTIME day;
    if (ms_to_date(c->date, &day)) DateTime_SetSystemtime(r->date, GDT_VALID, &day);
  } else {
    r->value = make_edit(c->value, mc_wstr(MC_STR_SMART_VALUE_NAME), FALSE);
  }
  r->count = make(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE | SS_RIGHT);
  r->remove = make_remove(MC_STR_SMART_REMOVE_CONDITION);
  r->hint = make(WC_STATICW, NULL, SS_NOPREFIX | SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS);
}

/* The first thing to type in on a condition's row: its value, or its field. */
static HWND row_focus(const row *r) {
  if (r->card) return r->card;
  return r->value ? r->value : r->choice ? r->choice : r->date ? r->date : r->field;
}

/* A sort order picker: every sortable field both ways, in mc_smart_order_label()'s words. */
static void sort_row_make(int i) {
  sort_row *s = &ed.sort_rows[i];
  s->label = make_label(mc_wstr(i ? MC_STR_SMART_SORT_THEN : MC_STR_SMART_SORT_BY));
  s->box = make_combo(mc_wstr(MC_STR_SMART_SORT_NAME));
  for (int f = 0; f < ed.nsortable; f++)
    for (int d = 0; d < 2; d++) {
      const char *label = mc_smart_order_label(ed.fields[ed.sortable[f]].name, d);
      if (label) combo_add_utf8(s->box, label, f * 2 + d);
    }
  combo_select_data(s->box, ed.sorts[i]);
  s->remove = make_remove(MC_STR_SMART_REMOVE_SORT);
}

/* --- Ready-made rules --- */

static const preset k_track_presets[PRESET_COUNT] = {
  {MC_STR_SMART_PRESET_RECENT,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}]},"
   "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}"},
  {MC_STR_SMART_PRESET_LEAST_PLAYED,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]},"
   "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"last_played\",\"dir\":\"asc\"}],\"limit\":100}"},
  {MC_STR_SMART_PRESET_NOT_PLAYED,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":180}}]},"
   "\"order\":[{\"field\":\"last_played\",\"dir\":\"asc\"}]}"},
  {MC_STR_SMART_PRESET_FLAC,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true},"
   "{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}]}}"},
  {MC_STR_SMART_PRESET_BLANK, "{\"v\":1}"},
};

static const preset k_album_presets[PRESET_COUNT] = {
  {MC_STR_SMART_PRESET_RECENT_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"added\",\"op\":\"within\",\"value\":{\"days\":30}}]},"
   "\"order\":[{\"field\":\"added\",\"dir\":\"desc\"}]}"},
  {MC_STR_SMART_PRESET_LEAST_PLAYED_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true}]},"
   "\"order\":[{\"field\":\"plays\",\"dir\":\"asc\"},{\"field\":\"last_played\",\"dir\":\"asc\"}],\"limit\":20}"},
  {MC_STR_SMART_PRESET_NOT_PLAYED_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"last_played\",\"op\":\"not_within\",\"value\":{\"days\":180}}]},"
   "\"order\":[{\"field\":\"last_played\",\"dir\":\"asc\"}]}"},
  {MC_STR_SMART_PRESET_FLAC_ALBUMS,
   "{\"v\":1,\"match\":{\"all\":[{\"field\":\"on_device\",\"op\":\"is\",\"value\":true},"
   "{\"field\":\"format\",\"op\":\"is\",\"value\":\"flac\"}]}}"},
  {MC_STR_SMART_PRESET_BLANK, "{\"v\":1}"},
};

/* --- Making a page --- */

/* Every control of the shown page goes. */
static void clear_page(void) {
  HWND w;
  while ((w = GetWindow(ed.panel, GW_CHILD))) destroy(w);
  free(ed.rows);
  ed.rows = NULL;
  ed.nrows = 0;
  ed.name_label = ed.name = ed.mode = ed.placeholder = ed.add_condition = ed.add_group = NULL;
  ed.order_heading = ed.sort_add = ed.shuffled = ed.shuffled_label = ed.limited = ed.limited_label = NULL;
  ed.limit_box = ed.limit_unit = ed.read_only_note = ed.read_only_text = ed.preset_heading = NULL;
  memset(ed.sort_rows, 0, sizeof ed.sort_rows);
  memset(ed.presets, 0, sizeof ed.presets);
}

/* The ready-made rules' page. */
static void build_presets(void) {
  ed.preset_heading = make_heading(mc_wstr(MC_STR_SMART_PRESET_TITLE));
  for (int i = 0; i < PRESET_COUNT; i++) ed.presets[i] = make_card(mc_wstr(ed.presets_of[i].name));
}

/* The read-only page: the name, greyed out, and the rule as its sentence. */
static void build_read_only(void) {
  wchar_t label[64];
  ed.name_label = make(WC_STATICW, wstr_field_label(label, 64, MC_STR_NAME), SS_CENTERIMAGE);
  ed.name = make_edit(ed.name_text, NULL, FALSE);
  EnableWindow(ed.name, FALSE);
  ed.read_only_note = make(WC_STATICW, mc_wstr(MC_STR_SMART_READ_ONLY), SS_NOPREFIX);
  char *sentence = describe(ed.old_rule);
  const char *body = sentence ? sentence : ed.old_rule ? ed.old_rule : "";
  /* An edit control wants \r\n. */
  size_t n = strlen(body), extra = 0;
  for (size_t i = 0; i < n; i++) extra += body[i] == '\n';
  char *text = malloc(n + extra + 1);
  if (text) {
    char *o = text;
    for (size_t i = 0; i < n; i++) {
      if (body[i] == '\n') *o++ = '\r';
      *o++ = body[i];
    }
    *o = '\0';
  }
  wchar_t *w = wstr_dup_from_utf8(text ? text : "");
  ed.read_only_text = make(WC_EDITW, w ? w : L"", ES_MULTILINE | ES_READONLY | WS_TABSTOP | WS_BORDER);
  free(w);
  free(text);
  free(sentence);
}

/* g's page: its name, how its conditions combine, a row each and the buttons to add more; on
 * the list's own, the order too. */
static void build_group(node *g) {
  wchar_t label[64];
  ed.name_label =
    make(WC_STATICW, wstr_field_label(label, 64, g->parent ? MC_STR_SMART_GROUP_NAME : MC_STR_NAME), SS_CENTERIMAGE);
  ed.name = make_edit(g->parent ? g->name : ed.name_text, NULL, FALSE);
  SendMessageW(ed.name, EM_LIMITTEXT, MAX_NAME, 0);
  ed.mode = make_combo(mc_wstr(MC_STR_SMART_MODE_NAME));
  combo_add(ed.mode, mc_wstr(MC_STR_SMART_MODE_ALL), MODE_ALL);
  combo_add(ed.mode, mc_wstr(MC_STR_SMART_MODE_ANY), MODE_ANY);
  combo_add(ed.mode, mc_wstr(MC_STR_SMART_MODE_NONE), MODE_NONE);
  combo_select_data(ed.mode, g->mode);

  ed.rows = g->nkids ? calloc((size_t)g->nkids, sizeof *ed.rows) : NULL;
  for (int i = 0; ed.rows && i < g->nkids; i++) {
    node *n = g->kids[i];
    row *r = &ed.rows[ed.nrows++];
    r->id = n->id;
    if (n->is_group) {
      wchar_t *title = wstr_dup_from_utf8(group_title(n));
      r->card = make_card(title ? title : L"");
      free(title);
      r->remove = make_remove(MC_STR_SMART_REMOVE_GROUP);
    } else {
      cond_row(r, n);
    }
  }
  ed.placeholder = make(WC_STATICW, mc_wstr(g->parent ? MC_STR_SMART_GROUP_EMPTY_HINT : MC_STR_SMART_NO_CONDITIONS),
                        SS_NOPREFIX | SS_CENTERIMAGE);
  ed.add_condition = make_button(MC_STR_SMART_ADD_CONDITION_BUTTON);
  ed.add_group = make_button(MC_STR_SMART_ADD_GROUP_BUTTON);
  EnableWindow(ed.add_group, depth_of(g) < MAX_DEPTH);
  if (g->parent) return;

  ed.order_heading = make_heading(mc_wstr(MC_STR_SMART_ORDER_HEADING));
  for (int i = 0; i < ed.nsorts; i++) sort_row_make(i);
  ed.sort_add = make_button(MC_STR_SMART_ADD_SORT_BUTTON);
  EnableWindow(ed.sort_add, ed.nsorts < MAX_SORTS && ed.nsorts < ed.nsortable);
  /* No text of their own: a themed checkbox's text stays black in dark mode, so a label
   * beside each says it (and clicks it), as on the settings page. */
  ed.shuffled = make(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP);
  SendMessageW(ed.shuffled, BM_SETCHECK, ed.random ? BST_CHECKED : BST_UNCHECKED, 0);
  set_name(ed.shuffled, mc_wstr(MC_STR_SMART_SHUFFLED));
  ed.shuffled_label = make(WC_STATICW, mc_wstr(MC_STR_SMART_SHUFFLED), SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE);
  ed.limited = make(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP);
  SendMessageW(ed.limited, BM_SETCHECK, ed.limited_on ? BST_CHECKED : BST_UNCHECKED, 0);
  set_name(ed.limited, mc_wstr(MC_STR_SMART_LIMIT));
  ed.limited_label = make(WC_STATICW, mc_wstr(MC_STR_SMART_LIMIT), SS_NOPREFIX | SS_NOTIFY | SS_CENTERIMAGE);
  char *limit = format_int(ed.limit);
  ed.limit_box = make_edit(limit, mc_wstr(MC_STR_SMART_LIMIT_COUNT_NAME), TRUE);
  free(limit);
  SendMessageW(ed.limit_box, EM_LIMITTEXT, 6, 0);
  EnableWindow(ed.limit_box, ed.limited_on);
  ed.limit_unit = make_label(mc_wstr(ed.subject == MC_SMART_TRACKS ? MC_STR_SMART_LIMIT_TRACKS : MC_STR_SMART_LIMIT_ALBUMS));
}

static void update_path(void);
static void update_preview_heading(void);
static void submit(void);

/* Makes the shown page's controls again, from the rule, with the focus where ed.focus_what
 * says. */
static void build_page(void) {
  ed.rebuild_posted = FALSE;
  SendMessageW(ed.panel, WM_SETREDRAW, FALSE, 0);
  ed.building = TRUE;
  HWND focus = GetFocus();
  if (focus && IsChild(ed.panel, focus)) SetFocus(ed.panel); /* not into a control that's going */
  clear_page();
  node *g = page_group();
  if (ed.choosing) build_presets();
  else if (ed.read_only) build_read_only();
  else if (g) build_group(g);
  ed.building = FALSE;
  layout_panel();
  update_page();
  update_path();
  update_preview_heading();
  SendMessageW(ed.panel, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(ed.panel, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);

  row *r = ed.focus_id ? row_of(ed.focus_id) : NULL;
  HWND to = NULL;
  switch (ed.focus_what) {
    case FOCUS_NAME: to = ed.name; break;
    case FOCUS_FIRST: to = ed.nrows ? row_focus(&ed.rows[0]) : ed.add_condition; break;
    case FOCUS_FIELD: to = r ? r->field : NULL; break;
    case FOCUS_OP: to = r ? r->op : NULL; break;
    case FOCUS_VALUE:
    case FOCUS_CARD: to = r ? row_focus(r) : NULL; break;
    case FOCUS_ADD_CONDITION: to = ed.add_condition; break;
    case FOCUS_SORT: to = ed.focus_index >= 0 && ed.focus_index < ed.nsorts ? ed.sort_rows[ed.focus_index].box : ed.sort_add; break;
    case FOCUS_SORT_ADD: to = ed.sort_add; break;
    case FOCUS_PRESET: to = ed.presets[0]; break;
  }
  if (!to && ed.focus_what != FOCUS_NONE) to = ed.name ? ed.name : ed.presets[0];
  ed.focus_what = FOCUS_NONE;
  ed.focus_id = 0;
  if (to) {
    SetFocus(to);
    if (to == ed.name) SendMessageW(to, EM_SETSEL, 0, -1);
  }
}

/* A rebuild once the control that asked for it has finished with the message it's handling:
 * a combo box or button mustn't go while it's sending a notification. */
static void request_rebuild(int focus_what, UINT focus_id, int focus_index) {
  ed.focus_what = focus_what;
  ed.focus_id = focus_id;
  ed.focus_index = focus_index;
  if (!ed.rebuild_posted) ed.rebuild_posted = PostMessageW(ed.dlg, WM_APP_REBUILD, 0, 0);
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

static int text_width_font(HWND w, HFONT font) {
  wchar_t buf[256];
  int n = GetWindowTextW(w, buf, 256);
  HDC dc = GetDC(w);
  HGDIOBJ old = SelectObject(dc, font);
  SIZE size = {0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(w, dc);
  return size.cx + dx(2);
}

static int text_width(HWND w) {
  return text_width_font(w, (HFONT)SendMessageW(w, WM_GETFONT, 0, 0));
}

static int button_width(HWND w) {
  return max(dx(BUTTON_W), text_width(w) + dx(10));
}

/* One condition's row at x, from *y (moved past it), ending at right. */
static void place_cond(placer *p, row *r, node *c, int x, int right, int *y) {
  int h = dy(ROW), gap = dx(GAP);
  int remove_x = right - dx(REMOVE_W), count_x = remove_x - gap - dx(COUNT_W);
  int vx = x + dx(FIELD_W) + gap + dx(OP_W) + gap, vw = max(dx(NUM_W), count_x - gap - vx);
  BOOL pair = r->value2 || r->unit;
  int num_w = min(dx(NUM_W), r->value2 ? (vw - dx(AND_W) - 2 * gap) / 2 : r->unit ? vw - dx(UNIT_W) - gap : vw);
  num_w = max(num_w, dx(16));
  put(p, r->field, x, *y, dx(FIELD_W), h, TRUE);
  put(p, r->op, x + dx(FIELD_W) + gap, *y, dx(OP_W), h, TRUE);
  put(p, r->value, vx, *y, pair ? num_w : vw, h, TRUE);
  put(p, r->and_label, vx + num_w + gap, *y, dx(AND_W), h, TRUE);
  put(p, r->value2, vx + num_w + gap + dx(AND_W) + gap, *y, num_w, h, TRUE);
  put(p, r->unit, vx + num_w + gap, *y, dx(UNIT_W), h, TRUE);
  put(p, r->date, vx, *y, min(vw, dx(FIELD_W)), h, TRUE);
  put(p, r->choice, vx, *y, vw, h, TRUE);
  put(p, r->count, count_x, *y, dx(COUNT_W), h, TRUE);
  put(p, r->remove, remove_x, *y, dx(REMOVE_W), h, TRUE);
  *y += h;
  wchar_t hint[256];
  BOOL shown = cond_hint(c, hint, 256);
  put(p, r->hint, vx, *y + dy(1), right - vx, dy(HINT_H) - dy(1), shown);
  if (shown) *y += dy(HINT_H);
  *y += dy(ROW_GAP);
}

/* Every control in the panel, top to bottom, scrolled by ed.scroll. */
static void layout_panel(void) {
  if (!ed.panel || ed.building) return;
  RECT rc;
  GetClientRect(ed.panel, &rc);
  int right = rc.right - dx(2), h = dy(ROW), gap = dx(GAP), x = dx(2);
  placer p = {BeginDeferWindowPos(32 + 12 * ed.nrows + 3 * MAX_SORTS), HWND_TOP};
  int y = dy(2);

  if (ed.choosing) {
    put(&p, ed.preset_heading, x, y, right - x, h, TRUE);
    y += h + dy(ROW_GAP);
    for (int i = 0; i < PRESET_COUNT; i++) {
      put(&p, ed.presets[i], x, y, right - x, dy(CARD_H), TRUE);
      y += dy(CARD_H) + dy(ROW_GAP);
    }
  } else {
    int label_w = ed.name_label ? text_width(ed.name_label) : 0;
    put(&p, ed.name_label, x, y, label_w, h, TRUE);
    put(&p, ed.name, x + label_w + gap, y, max(dx(60), min(dx(200), right - x - label_w - gap)), h, TRUE);
    y += h + dy(ROW_GAP) * 2;
  }

  if (ed.read_only) {
    int note_h = canvas_text_height(ed.read_only_note, ed.font, right - x, DT_WORDBREAK);
    put(&p, ed.read_only_note, x, y, right - x, note_h, TRUE);
    y += note_h + dy(ROW_GAP) * 2;
    /* As tall as the sentence, with a line to spare. */
    int text_h = canvas_text_height(ed.read_only_text, ed.font, right - x - dx(8), DT_WORDBREAK | DT_EDITCONTROL) + dy(ROW);
    put(&p, ed.read_only_text, x, y, right - x, text_h, TRUE);
    y += text_h;
  } else if (!ed.choosing && page_group()) {
    node *g = page_group();
    put(&p, ed.mode, x, y, dx(MODE_W), h, TRUE);
    y += h + dy(ROW_GAP) * 2;
    for (int i = 0; i < ed.nrows; i++) {
      row *r = &ed.rows[i];
      node *n = find(r->id);
      if (!n) continue;
      if (r->card) {
        int remove_x = right - dx(REMOVE_W);
        put(&p, r->card, x, y, remove_x - gap - x, dy(CARD_H), TRUE);
        put(&p, r->remove, remove_x, y + (dy(CARD_H) - h) / 2, dx(REMOVE_W), h, TRUE);
        y += dy(CARD_H) + dy(ROW_GAP);
      } else {
        place_cond(&p, r, n, x, right, &y);
      }
    }
    put(&p, ed.placeholder, x, y, right - x, h, !g->nkids);
    if (!g->nkids) y += h + dy(ROW_GAP);
    int bx = x;
    put(&p, ed.add_condition, bx, y, button_width(ed.add_condition), h + dy(1), TRUE);
    bx += button_width(ed.add_condition) + gap;
    put(&p, ed.add_group, bx, y, button_width(ed.add_group), h + dy(1), TRUE);
    y += h + dy(1) + dy(ROW_GAP);

    if (ed.order_heading) {
      y += dy(ROW_GAP) * 2;
      put(&p, ed.order_heading, x, y, right - x, h, TRUE);
      y += h + dy(ROW_GAP);
      int lw = 0;
      for (int i = 0; i < ed.nsorts; i++) lw = max(lw, text_width(ed.sort_rows[i].label));
      for (int i = 0; i < ed.nsorts; i++) {
        sort_row *s = &ed.sort_rows[i];
        int sx = x;
        put(&p, s->label, sx, y, lw, h, TRUE);
        sx += lw + gap;
        int sort_w = min(dx(SORT_W), right - dx(REMOVE_W) - gap - sx);
        put(&p, s->box, sx, y, sort_w, h, TRUE);
        put(&p, s->remove, sx + sort_w + gap, y, dx(REMOVE_W), h, TRUE);
        y += h + dy(ROW_GAP);
      }
      put(&p, ed.sort_add, x, y, button_width(ed.sort_add), h + dy(1), TRUE);
      y += h + dy(1) + dy(ROW_GAP) * 2;
      int box = GetSystemMetricsForDpi(SM_CXMENUCHECK, ed.dpi);
      put(&p, ed.shuffled, x, y + (h - box) / 2, box, box, TRUE);
      put(&p, ed.shuffled_label, x + box + gap, y, text_width(ed.shuffled_label), h, TRUE);
      y += h + dy(ROW_GAP);
      int lx = x;
      put(&p, ed.limited, lx, y + (h - box) / 2, box, box, TRUE);
      lx += box + gap;
      put(&p, ed.limited_label, lx, y, text_width(ed.limited_label), h, TRUE);
      lx += text_width(ed.limited_label) + gap;
      put(&p, ed.limit_box, lx, y, dx(NUM_W), h, TRUE);
      lx += dx(NUM_W) + gap;
      put(&p, ed.limit_unit, lx, y, text_width(ed.limit_unit), h, TRUE);
      y += h;
    }
  }
  y += dy(4);
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
  if (!w || GetParent(w) != ed.panel || w == ed.read_only_text) return;
  RECT r, rc;
  GetWindowRect(w, &r);
  MapWindowPoints(NULL, ed.panel, (POINT *)&r, 2);
  GetClientRect(ed.panel, &rc);
  int h = min(r.bottom - r.top, dy(CARD_H)), pad = dy(ROW_GAP);
  if (r.top < 0) scroll_to(ed.scroll + r.top - pad);
  else if (r.top + h > rc.bottom) scroll_to(ed.scroll + r.top + h - rc.bottom + pad);
}

/* The path's height: a row, once a group is open. */
static int path_h(void) {
  return ed.depth > 0 ? dy(ROW + 2) + dy(GAP) : 0;
}

/* The problems' box's height, for n problems. */
static int problems_h(int n) {
  if (!n) return 0;
  int lines = 1 + min(n, PROBLEM_ROWS) + (n > PROBLEM_ROWS);
  return lines * dy(ROW) + dy(8);
}

static int card_radius(void) {
  return MulDiv(CONTENT_RADIUS, (int)ed.dpi, USER_DEFAULT_SCREEN_DPI);
}

/* The extended frame: everything around the card, and on into it far enough for its rounded
 * corners to show Mica. As in the main window, what's in that strip must be drawn with alpha,
 * which paint() and draw_item() do; the controls that GDI draws stay further in, as dx(MARGIN)
 * is more than the radius. */
static void extend_frame(void) {
  RECT rc;
  GetClientRect(ed.dlg, &rc);
  int r = card_radius();
  MARGINS m = {0};
  if (ed.mica && !IsRectEmpty(&ed.card))
    m = (MARGINS){ed.card.left + r, rc.right - ed.card.right + r, ed.card.top + r, rc.bottom - ed.card.bottom + r};
  if (!memcmp(&m, &ed.extended, sizeof m)) return;
  ed.extended = m;
  DwmExtendFrameIntoClientArea(ed.dlg, &m);
}

/* The dialog: the path at the top and the counts and the buttons at the bottom, on the frame,
 * and between them the card, with the panel and the preview, and the problems. */
static void layout_dialog(void) {
  RECT rc;
  GetClientRect(ed.dlg, &rc);
  int m = dx(MARGIN), h = dy(ROW), gap = dx(GAP);
  HDWP dwp = BeginDeferWindowPos(16 + 2 * MAX_DEPTH + 2 * PROBLEM_ROWS);
#define MOVE(w, x, y, cx, cy, shown)                                                                          \
  do {                                                                                                         \
    if ((w) && dwp)                                                                                            \
      dwp = DeferWindowPos(dwp, (w), NULL, (x), (y), max(0, (cx)), max(0, (cy)),                               \
                           SWP_NOZORDER | SWP_NOACTIVATE | ((shown) ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));      \
  } while (0)

  /* The path: Back, then a crumb per group, the shown one last. */
  int py = m, px = m + dy(ROW + 2) + gap;
  MOVE(ed.back, m, py, dy(ROW + 2), dy(ROW + 2), ed.depth > 0);
  for (int i = 0; i < ed.ncrumbs; i++) {
    if (i) {
      int sw = text_width(ed.seps[i - 1]);
      MOVE(ed.seps[i - 1], px, py, sw, dy(ROW + 2), ed.depth > 0);
      px += sw;
    }
    node *g = find(ed.stack[i]);
    int sign = g && g->bad ? dx(10) : 0; /* the warning sign before its name */
    int cw = min(dx(CRUMB_MAX), text_width_font(ed.crumbs[i], i == ed.ncrumbs - 1 ? ed.bold_font : ed.font) + dx(8) + sign);
    cw = max(0, min(cw, rc.right - m - px));
    MOVE(ed.crumbs[i], px, py, cw, dy(ROW + 2), ed.depth > 0);
    px += cw;
  }

  int bh = dy(ROW + 1), bw = dx(BUTTON_W);
  int buttons_y = rc.bottom - m - bh;
  HWND ok = GetDlgItem(ed.dlg, IDOK), cancel = GetDlgItem(ed.dlg, IDCANCEL);
  MOVE(cancel, rc.right - m - bw, buttons_y, bw, bh, TRUE);
  MOVE(ok, rc.right - m - 2 * bw - gap, buttons_y, bw, bh, IsWindowVisible(ok) || !ed.read_only);
  MOVE(ed.status, m, buttons_y, rc.right - 3 * m - 2 * bw - gap, bh, TRUE);

  /* The card, and inside it (by the same margin again) what it holds. */
  RECT card = {m, m + path_h(), rc.right - m, buttons_y - m};
  int left = card.left + m, right = card.right - m, inner_bottom = card.bottom - m;

  int np = ed.read_only || ed.choosing ? 0 : ed.nbad;
  int ph = problems_h(np), pt = inner_bottom - ph;
  RECT was_box = ed.problems_box;
  SetRect(&ed.problems_box, left, pt, right, pt + ph);
  int ly = pt + dy(4), lx = left + dx(6), lr = right - dx(6);
  MOVE(ed.problems_title, lx, ly, lr - lx, h, np > 0);
  ly += h;
  for (int i = 0; i < PROBLEM_ROWS; i++) {
    BOOL shown = i < np;
    int sw = button_width(ed.problem_show[i]);
    MOVE(ed.problem_lines[i], lx, ly, lr - lx - sw - gap, h, shown);
    MOVE(ed.problem_show[i], lr - sw, ly + dy(1) / 2, sw, h - dy(1), shown);
    if (shown) ly += h;
  }
  MOVE(ed.problems_more, lx, ly, lr - lx, h, np > PROBLEM_ROWS);

  int top = card.top + m, bottom = ph ? pt - dy(6) : inner_bottom, avail_w = right - left;
  BOOL preview = !ed.read_only && !ed.choosing;
  ed.wide = avail_w >= dx(LEFT_MIN + PREVIEW_MIN + MARGIN);
  int label_h = dy(10), note_h = GetWindowTextLengthW(ed.preview_note) ? dy(10) : 0;
  int line = max(1, MulDiv(1, (int)ed.dpi, USER_DEFAULT_SCREEN_DPI));
  RECT divider = {0};
  if (!preview) {
    MOVE(ed.panel, left, top, avail_w, bottom - top, TRUE);
    MOVE(ed.preview_label, 0, 0, 0, 0, FALSE);
    MOVE(ed.preview_note, 0, 0, 0, 0, FALSE);
    MOVE(ed.list, 0, 0, 0, 0, FALSE);
  } else if (ed.wide) {
    int preview_w = max(dx(PREVIEW_MIN), (avail_w - m) * 3 / 8), left_w = avail_w - m - preview_w;
    int lx2 = left + left_w + m;
    MOVE(ed.panel, left, top, left_w, bottom - top, TRUE);
    MOVE(ed.preview_label, lx2, top, preview_w, label_h, TRUE);
    MOVE(ed.preview_note, lx2, top + label_h, preview_w, note_h, note_h > 0);
    MOVE(ed.list, lx2, top + label_h + note_h, preview_w, bottom - top - label_h - note_h, TRUE);
    SetRect(&divider, lx2 - m / 2, top, lx2 - m / 2 + line, bottom);
  } else {
    int panel_h = (bottom - top) * 3 / 5, ly2 = top + panel_h + dy(6);
    MOVE(ed.panel, left, top, avail_w, panel_h, TRUE);
    MOVE(ed.preview_label, left, ly2, avail_w, label_h, TRUE);
    MOVE(ed.preview_note, left, ly2 + label_h, avail_w, note_h, note_h > 0);
    MOVE(ed.list, left, ly2 + label_h + note_h, avail_w, bottom - ly2 - label_h - note_h, TRUE);
    SetRect(&divider, left, top + panel_h + dy(3), right, top + panel_h + dy(3) + line);
  }
#undef MOVE
  if (dwp) EndDeferWindowPos(dwp);
  if (!EqualRect(&card, &ed.card) || !EqualRect(&divider, &ed.divider)) {
    ed.card = card;
    ed.divider = divider;
    extend_frame();
    InvalidateRect(ed.dlg, NULL, FALSE);
  }
  RECT lr2;
  GetClientRect(ed.list, &lr2);
  int list_w = lr2.right - (GetWindowLongW(ed.list, GWL_STYLE) & WS_VSCROLL ? 0 : GetSystemMetricsForDpi(SM_CXVSCROLL, ed.dpi));
  int col = list_w / 2; /* with room for a scrollbar, so there's never a sideways one */
  ListView_SetColumnWidth(ed.list, 0, col);
  ListView_SetColumnWidth(ed.list, 1, list_w - col);
  scroll_to(ed.scroll);
  if (!EqualRect(&was_box, &ed.problems_box)) { /* the frame moved */
    InflateRect(&was_box, 2, 2);
    InvalidateRect(ed.dlg, &was_box, TRUE);
    RECT now = ed.problems_box;
    InflateRect(&now, 2, 2);
    InvalidateRect(ed.dlg, &now, TRUE);
  }
}

/* --- The path at the top --- */

static LRESULT CALLBACK child_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data);

/* List name › Group › Subgroup, each but the last a button back to it, marked where something
 * in it has a problem. Shown once a group is open. */
static void update_path(void) {
  int focused = -1; /* a crumb with the focus keeps it, made again */
  for (int i = 0; i < ed.ncrumbs; i++)
    if (GetFocus() == ed.crumbs[i]) focused = i;
  if (focused >= 0) SetFocus(ed.back);
  for (int i = 0; i < ed.ncrumbs; i++) destroy(ed.crumbs[i]);
  for (int i = 0; i + 1 < ed.ncrumbs; i++) destroy(ed.seps[i]);
  ed.ncrumbs = 0;
  if (!ed.choosing && ed.depth > 0) {
    HWND after = ed.back;
    for (int i = 0; i <= ed.depth; i++) {
      node *g = find(ed.stack[i]);
      if (!g) break;
      if (i) {
        ed.seps[i - 1] = CreateWindowExW(0, WC_STATICW, mc_wstr(MC_STR_SMART_PATH_SEPARATOR), WS_CHILD | SS_OWNERDRAW,
                                         0, 0, 0, 0, ed.dlg, NULL, GetModuleHandleW(NULL), NULL);
        SendMessageW(ed.seps[i - 1], WM_SETFONT, (WPARAM)ed.font, FALSE);
      }
      wchar_t *title = wstr_dup_from_utf8(group_title(g));
      BOOL last = i == ed.depth;
      HWND w = CreateWindowExW(0, WC_BUTTONW, title ? title : L"", WS_CHILD | BS_OWNERDRAW | (last ? 0 : WS_TABSTOP),
                               0, 0, 0, 0, ed.dlg, NULL, GetModuleHandleW(NULL), NULL);
      free(title);
      SendMessageW(w, WM_SETFONT, (WPARAM)(last ? ed.bold_font : ed.font), FALSE);
      SetWindowSubclass(w, child_proc, 0, 0);
      SetWindowPos(w, after, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); /* tab order: after Back */
      after = w;
      add_tip(w);
      ed.crumbs[ed.ncrumbs++] = w;
    }
  }
  layout_dialog();
  if (focused >= 0 && focused + 1 < ed.ncrumbs) SetFocus(ed.crumbs[focused]);
}

/* --- The problems' box and the counts --- */

static void update_problems(void) {
  int n = ed.read_only || ed.choosing ? 0 : ed.nbad;
  wchar_t text[256];
  if (n) {
    mc_wformat(text, 256, mc_str_plural(MC_STR_SMART_PROBLEMS_TITLE, n), MC_D(n));
    SetWindowTextW(ed.problems_title, text);
  }
  for (int i = 0; i < PROBLEM_ROWS; i++) {
    ed.problem_ids[i] = i < n ? ed.bad[i]->id : 0;
    if (i < n) problem_line(ed.bad[i], text, 256);
    else text[0] = L'\0';
    SetWindowTextW(ed.problem_lines[i], text);
    set_name(ed.problem_show[i], text[0] ? text : mc_wstr(MC_STR_SMART_PROBLEM_SHOW));
  }
  text[0] = L'\0';
  if (n > PROBLEM_ROWS) mc_wformat(text, 256, mc_str_plural(MC_STR_SMART_MORE, n - PROBLEM_ROWS), MC_D(n - PROBLEM_ROWS));
  SetWindowTextW(ed.problems_more, text);
  layout_dialog();
}

/* "40 tracks match · 10 in the list", or Counting… while it's being worked out. */
static void update_status(void) {
  wchar_t text[256] = L"";
  if (ed.read_only || ed.choosing || ed.nbad) {
  } else if (!ed.counted) {
    wcscpy(text, mc_wstr(MC_STR_SMART_COUNTING));
  } else if (ed.total >= 0) {
    enum mc_str_id id = ed.subject == MC_SMART_TRACKS ? MC_STR_SMART_TRACKS_MATCH : MC_STR_SMART_ALBUMS_MATCH;
    char part[128], all[256];
    mc_format(part, sizeof part, mc_str_plural(id, ed.total), MC_D(ed.total));
    if (ed.limited_on) {
      char in_list[128];
      mc_format(in_list, sizeof in_list, mc_str(MC_STR_SMART_IN_LIST), MC_D(min(ed.limit, ed.total)));
      snprintf(all, sizeof all, "%s%s%s", part, mc_str(MC_STR_SEPARATOR_DOT), in_list);
    } else {
      snprintf(all, sizeof all, "%s", part);
    }
    wstr_from_utf8(text, 256, all);
  }
  SetWindowTextW(ed.status, text);
}

static void update_save(void) {
  EnableWindow(GetDlgItem(ed.dlg, IDOK),
               !ed.read_only && !ed.choosing && ed.name_text && *ed.name_text && !ed.nbad);
}

/* "Preview" over the list's preview, "What this group matches" over a group's, with how many
 * it matches beneath. */
static void update_preview_heading(void) {
  node *g = page_group();
  SetWindowTextW(ed.preview_label, mc_wstr(g && g->parent ? MC_STR_SMART_GROUP_PREVIEW : MC_STR_SMART_PREVIEW));
  set_name(ed.list, mc_wstr(g && g->parent ? MC_STR_SMART_GROUP_PREVIEW : MC_STR_SMART_PREVIEW));
}

/* --- The preview thread --- */

static void job_free(job *j) {
  if (!j) return;
  free(j->list_rule);
  free(j->preview_rule);
  for (int i = 0; i < j->n; i++) free(j->rules[i]);
  free(j->rules);
  free(j->ids);
  free(j);
}

static void result_free(result *r) {
  if (!r) return;
  mc_track_list_free(r->tracks);
  mc_album_list_free(r->albums);
  for (int i = 0; i < r->n; i++) free(r->rules[i]);
  free(r->rules);
  free(r->ids);
  free(r->counts);
  free(r);
}

/* How many a rule picks, or -1. */
static int count_of(mc_library *lib, mc_smart_subject subject, const char *rule) {
  int n;
  return rule && mc_smart_count(lib, subject, rule, &n) == MC_OK ? n : -1;
}

static DWORD WINAPI worker_main(void *arg) {
  worker *w = arg;
  mc_library *lib = NULL;
  for (;;) {
    EnterCriticalSection(&w->lock);
    while (!w->job && !w->quit) SleepConditionVariableCS(&w->wake, &w->lock, INFINITE);
    job *j = w->quit ? NULL : w->job;
    w->job = NULL;
    LeaveCriticalSection(&w->lock);
    if (!j) break;
    if (!lib && mc_library_open(w->dir, &lib) != MC_OK) lib = NULL;
    result *r = calloc(1, sizeof *r);
    if (r) {
      r->gen = j->gen;
      r->total = -1;
    }
    if (r && lib) {
      if (j->preview_rule)
        r->preview_ok = j->subject == MC_SMART_TRACKS ? mc_smart_preview_tracks(lib, j->preview_rule, &r->tracks) == MC_OK
                                                      : mc_smart_preview_albums(lib, j->preview_rule, &r->albums) == MC_OK;
      if (j->preview_is_list && r->preview_ok) r->total = r->tracks ? r->tracks->count : r->albums ? r->albums->count : 0;
      else r->total = count_of(lib, j->subject, j->list_rule);
      r->n = j->n;
      r->ids = j->ids;
      r->rules = j->rules;
      j->ids = NULL;
      j->rules = NULL;
      j->n = 0;
      r->counts = malloc((size_t)(r->n > 0 ? r->n : 1) * sizeof *r->counts);
      for (int i = 0; r->counts && i < r->n; i++) {
        EnterCriticalSection(&w->lock);
        BOOL stale = w->job || w->quit; /* a newer rule is waiting: these counts are wasted */
        LeaveCriticalSection(&w->lock);
        r->counts[i] = stale ? -1 : count_of(lib, j->subject, r->rules[i]);
      }
      if (!r->counts) r->n = 0; /* the rules are freed with r */
    }
    job_free(j);
    if (r && !PostMessageW(w->dlg, WM_APP_PREVIEW, 0, (LPARAM)r)) result_free(r);
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
  job_free(w->job);
  DeleteCriticalSection(&w->lock);
  free(w->dir);
  free(w);
  ed.w = NULL;
  MSG msg;
  while (PeekMessageW(&msg, ed.dlg, WM_APP_PREVIEW, WM_APP_PREVIEW, PM_REMOVE)) result_free((result *)msg.lParam);
}

/* The rule as it stands, and the shown page's rows, to the thread. */
static void submit(void) {
  KillTimer(ed.dlg, TIMER_PREVIEW);
  node *g = page_group();
  if (!ed.w || ed.read_only || !g) return;
  job *j = calloc(1, sizeof *j);
  if (!j) return;
  j->gen = ++ed.gen;
  j->subject = ed.subject;
  j->list_rule = rule_json(FALSE);
  j->preview_is_list = !g->parent;
  if (g->parent && g->nkids) j->preview_rule = match_json(g, FALSE);
  else if (!g->parent && j->list_rule) j->preview_rule = _strdup(j->list_rule);
  j->ids = calloc((size_t)ed.nrows + 1, sizeof *j->ids);
  j->rules = calloc((size_t)ed.nrows + 1, sizeof *j->rules);
  for (int i = 0; j->ids && j->rules && i < ed.nrows; i++) {
    node *n = find(ed.rows[i].id);
    if (!n || (n->is_group && !n->nkids)) continue;
    char *m = match_json(n, FALSE);
    if (!m) continue;
    j->ids[j->n] = n->id;
    j->rules[j->n++] = m;
  }
  ed.counted = FALSE;
  update_status();
  EnterCriticalSection(&ed.w->lock);
  job_free(ed.w->job);
  ed.w->job = j;
  WakeConditionVariable(&ed.w->wake);
  LeaveCriticalSection(&ed.w->lock);
}

static void set_preview(mc_track_list *tracks, mc_album_list *albums, int shown) {
  mc_track_list_free(ed.tracks);
  mc_album_list_free(ed.albums);
  ed.tracks = tracks;
  ed.albums = albums;
  ed.shown = shown;
  ListView_SetItemCountEx(ed.list, shown, 0);
  InvalidateRect(ed.list, NULL, TRUE);
}

static void on_result(result *r) {
  /* Counts keep, whenever they come: each is for the rule it was made from. */
  for (int i = 0; i < r->n; i++) {
    node *n = find(r->ids[i]);
    if (!n || r->counts[i] < 0) continue;
    free(n->counted);
    n->counted = r->rules[i];
    r->rules[i] = NULL;
    n->count = r->counts[i];
  }
  if (r->gen == ed.gen) {
    ed.counted = TRUE;
    ed.total = r->total;
    node *g = page_group();
    if (g) {
      int n = r->preview_ok ? (r->tracks ? r->tracks->count : r->albums ? r->albums->count : 0) : 0;
      if (!g->parent && ed.limited_on) n = min(n, ed.limit);
      set_preview(r->tracks, r->albums, n);
      r->tracks = NULL;
      r->albums = NULL;
      wchar_t note[128] = L"";
      if (g->parent && r->preview_ok)
        mc_wformat(note, 128,
                   mc_str_plural(ed.subject == MC_SMART_TRACKS ? MC_STR_SMART_GROUP_TRACKS : MC_STR_SMART_GROUP_ALBUMS, n),
                   MC_D(n));
      SetWindowTextW(ed.preview_note, note);
      layout_dialog();
    }
    update_status();
  }
  update_page();
  result_free(r);
}

/* --- Changes --- */

/* Whether g holds a condition without a problem: else its sentence would be "All tracks". */
static BOOL says_something(const node *g) {
  for (int i = 0; i < g->nkids; i++) {
    const node *k = g->kids[i];
    if (k->is_group ? says_something(k) : !cond_problem(k)) return TRUE;
  }
  return FALSE;
}

/* Every group's sentence, for its row, its page's cue, the path and the problems. */
static void describe_groups(node *g) {
  for (int i = 0; i < g->nkids; i++) {
    node *k = g->kids[i];
    if (!k->is_group) continue;
    describe_groups(k);
    free(k->desc);
    char *m = says_something(k) ? match_json(k, TRUE) : NULL;
    k->desc = m ? describe(m) : NULL;
    free(m);
  }
}

/* A quarter of a second after the last change: the sentences, then the counts and preview. */
static void on_timer(void) {
  KillTimer(ed.dlg, TIMER_PREVIEW);
  describe_groups(ed.root);
  update_page();
  update_path();
  update_problems();
  submit();
}

/* After any edit: the problems, Save, the shown page's marks, and soon the rest. */
static void changed(void) {
  if (ed.read_only || ed.choosing || ed.building) return;
  ed.nbad = 0;
  check_group(ed.root);
  update_page();
  update_problems();
  update_save();
  ed.counted = FALSE;
  update_status();
  for (int i = 0; i < ed.ncrumbs; i++) InvalidateRect(ed.crumbs[i], NULL, FALSE);
  SetTimer(ed.dlg, TIMER_PREVIEW, PREVIEW_DELAY_MS, NULL);
}

/* The shown page changed: its controls, the preview cleared until the thread has the new
 * one, and its counts. */
static void page_changed(int focus_what, UINT focus_id) {
  set_preview(NULL, NULL, 0);
  SetWindowTextW(ed.preview_note, L"");
  ed.scroll = ed.scrolls[ed.depth];
  request_rebuild(focus_what, focus_id, 0);
  if (!ed.read_only && !ed.choosing) SetTimer(ed.dlg, TIMER_PREVIEW, 1, NULL); /* once the page is made */
}

/* Opens g's page over the shown one, with the focus on its first condition (or Add
 * condition). */
static void push_group(node *g) {
  if (!g || ed.depth >= MAX_DEPTH) return;
  ed.scrolls[ed.depth] = ed.scroll;
  ed.stack[++ed.depth] = g->id;
  ed.scrolls[ed.depth] = 0;
  page_changed(FOCUS_FIRST, 0);
}

/* Back to the group at level depth; the focus on the row of the group left, if it's there. */
static void pop_to(int depth) {
  if (depth < 0 || depth >= ed.depth) return;
  UINT left = ed.stack[depth + 1];
  ed.depth = depth;
  page_changed(FOCUS_CARD, left);
}

/* Escape, Alt+Left, Back: back a group. FALSE if the list's own page is shown. */
static BOOL go_back(void) {
  if (ed.choosing || ed.read_only || ed.depth == 0) return FALSE;
  pop_to(ed.depth - 1);
  return TRUE;
}

/* Show on a problem: its group's page (and those above it), with the focus on its value. */
static void show_problem(UINT id) {
  node *c = find(id);
  if (!c) return;
  node *chain[MAX_DEPTH + 1];
  int n = 0;
  for (node *g = c->parent; g && n <= MAX_DEPTH; g = g->parent) chain[n++] = g;
  ed.depth = n - 1;
  for (int i = 0; i < n; i++) {
    ed.stack[i] = chain[n - 1 - i]->id;
    ed.scrolls[i] = 0;
  }
  page_changed(FOCUS_VALUE, id);
}

/* --- Edits --- */

static void remove_node(node *n) {
  node *g = n->parent;
  int at = 0;
  while (at < g->nkids && g->kids[at] != n) at++;
  if (at == g->nkids) return;
  memmove(&g->kids[at], &g->kids[at + 1], (size_t)(g->nkids - at - 1) * sizeof g->kids[0]);
  g->nkids--;
  node_free(n);
  /* The focus to the row after it, or before, or Add condition. */
  UINT next = g->nkids ? g->kids[at < g->nkids ? at : at - 1]->id : 0;
  request_rebuild(next ? FOCUS_VALUE : FOCUS_ADD_CONDITION, next, 0);
  changed();
}

static void on_field_changed(row *r, node *c) {
  int f = (int)combo_data(r->field);
  if (f < 0 || f >= ed.nfields || f == c->field) return;
  const char *was_type = field_of(c)->type, *was_op = op_of(c);
  BOOL same_type = !strcmp(was_type, ed.fields[f].type);
  c->field = f;
  int op = same_type ? find_op(c->field, was_op) : -1;
  c->op = op >= 0 ? op : default_op(c->field);
  if (!same_type) {
    reset_values(c); /* "jazz" means nothing as a length */
  } else if (is_choice(c)) {
    c->choice = first_choice(c);
    free(c->extra);
    c->extra = NULL;
  }
  request_rebuild(FOCUS_FIELD, c->id, 0);
  changed();
}

static void on_op_changed(row *r, node *c) {
  int op = (int)combo_data(r->op);
  if (op < 0 || op == c->op) return;
  BOOL had_value = !takes_no_value(c), was_days = is_days(c), was_between = op_is(c, "between");
  c->op = op;
  /* The value's controls are made again only when the operator wants others. */
  if (had_value != !takes_no_value(c) || was_days != is_days(c) || was_between != op_is(c, "between"))
    request_rebuild(FOCUS_OP, c->id, 0);
  changed();
}

/* A control in the panel did something. */
static void on_panel_command(HWND w, int code) {
  if (ed.building) return;
  row *r = row_of_window(w);
  node *n = r ? find(r->id) : NULL;
  if (r && n) {
    if (w == r->remove && code == BN_CLICKED) {
      remove_node(n);
    } else if (w == r->card && code == BN_CLICKED) {
      push_group(n);
    } else if (w == r->field && code == CBN_SELCHANGE) {
      on_field_changed(r, n);
    } else if (w == r->op && code == CBN_SELCHANGE) {
      on_op_changed(r, n);
    } else if ((w == r->value || w == r->value2) && code == EN_CHANGE) {
      char **to = w == r->value2 ? &n->value2 : &n->value;
      free(*to);
      *to = text_of(w);
      changed();
    } else if (w == r->unit && code == CBN_SELCHANGE) {
      LPARAM u = combo_data(r->unit);
      if (u >= UNIT_DAYS && u <= UNIT_MONTHS) n->unit = (int)u;
      changed();
    } else if (w == r->choice && code == CBN_SELCHANGE) {
      LPARAM ch = combo_data(r->choice);
      n->choice = ch == CB_ERR ? CHOICE_NONE : (int)ch;
      changed();
    }
    return;
  }
  for (int i = 0; i < ed.nsorts; i++) {
    sort_row *s = &ed.sort_rows[i];
    if (w == s->remove && code == BN_CLICKED) {
      memmove(&ed.sorts[i], &ed.sorts[i + 1], (size_t)(ed.nsorts - i - 1) * sizeof ed.sorts[0]);
      ed.nsorts--;
      request_rebuild(FOCUS_SORT_ADD, 0, 0);
      changed();
      return;
    }
    if (w == s->box && code == CBN_SELCHANGE) {
      LPARAM k = combo_data(s->box);
      if (k >= 0) ed.sorts[i] = (int)k;
      changed();
      return;
    }
  }
  for (int i = 0; i < PRESET_COUNT; i++)
    if (w == ed.presets[i] && code == BN_CLICKED) {
      /* The dialog starts it, once the button's done: it goes with the page. */
      PostMessageW(ed.dlg, WM_COMMAND, MAKEWPARAM(IDOK + 100 + i, BN_CLICKED), 0);
      return;
    }
  node *g = page_group();
  if (!g) return;
  if (w == ed.name && code == EN_CHANGE) {
    char *text = text_of(w), *t = trimmed(text);
    free(text);
    if (g->parent) {
      free(g->name);
      g->name = t;
    } else {
      free(ed.name_text);
      ed.name_text = t;
    }
    changed();
  } else if (w == ed.mode && code == CBN_SELCHANGE) {
    LPARAM m = combo_data(ed.mode);
    if (m >= MODE_ALL && m <= MODE_NONE) g->mode = (int)m;
    changed();
  } else if (w == ed.add_condition && code == BN_CLICKED) {
    node *c = cond_new(g);
    if (c) request_rebuild(FOCUS_FIELD, c->id, 0);
    changed();
  } else if (w == ed.add_group && code == BN_CLICKED && depth_of(g) < MAX_DEPTH) {
    node *k = node_new(g, TRUE);
    if (!k) return;
    k->mode = g->mode == MODE_ALL ? MODE_ANY : MODE_ALL; /* the other way round, else why a group */
    changed();
    push_group(k);
  } else if (w == ed.sort_add && code == BN_CLICKED && ed.nsorts < MAX_SORTS) {
    /* The next sortable field not sorted on yet. */
    int next = 0;
    for (int s = 0; s < ed.nsortable; s++) {
      BOOL used = FALSE;
      for (int i = 0; i < ed.nsorts; i++) used |= ed.sorts[i] / 2 == s;
      if (!used) {
        next = s;
        break;
      }
    }
    ed.sorts[ed.nsorts++] = next * 2;
    request_rebuild(FOCUS_SORT, 0, ed.nsorts - 1);
    changed();
  } else if ((w == ed.shuffled || w == ed.limited) && code == BN_CLICKED) {
    ed.random = SendMessageW(ed.shuffled, BM_GETCHECK, 0, 0) == BST_CHECKED;
    ed.limited_on = SendMessageW(ed.limited, BM_GETCHECK, 0, 0) == BST_CHECKED;
    EnableWindow(ed.limit_box, ed.limited_on);
    changed();
  } else if ((w == ed.shuffled_label || w == ed.limited_label) && code == STN_CLICKED) {
    /* The labels stand in for the checkboxes' own text, which stays black in dark mode. */
    HWND box = w == ed.shuffled_label ? ed.shuffled : ed.limited;
    SetFocus(box);
    SendMessageW(box, BM_CLICK, 0, 0);
  } else if (w == ed.limit_box && code == EN_CHANGE) {
    char *text = text_of(w);
    long long v = 0;
    if (parse_int(text, &v) && v >= 1) ed.limit = (int)v; /* else the last good one, shown again when it loses the focus */
    free(text);
    changed();
  } else if (w == ed.limit_box && code == EN_KILLFOCUS) {
    char *text = text_of(w), *good = format_int(ed.limit);
    if (strcmp(text ? text : "", good ? good : "")) set_text_utf8(w, good);
    free(text);
    free(good);
  }
}

/* The tooltips' text: a remove button's name, a group's Open group, a crumb's whole name, or
 * Back with its shortcut. */
static void on_tip(NMTTDISPINFOW *info) {
  static wchar_t text[512];
  text[0] = L'\0';
  HWND w = (HWND)info->hdr.idFrom;
  row *r = row_of_window(w);
  if (r && w == r->card) wcscpy(text, mc_wstr(MC_STR_SMART_OPEN_GROUP));
  else if (r && w == r->remove) wcscpy(text, mc_wstr(r->card ? MC_STR_SMART_REMOVE_GROUP : MC_STR_SMART_REMOVE_CONDITION));
  for (int i = 0; i < ed.nsorts; i++)
    if (w == ed.sort_rows[i].remove) wcscpy(text, mc_wstr(MC_STR_SMART_REMOVE_SORT));
  for (int i = 0; i < ed.ncrumbs; i++)
    if (w == ed.crumbs[i]) GetWindowTextW(w, text, 512);
  for (int i = 0; i < PRESET_COUNT; i++)
    if (w == ed.presets[i]) {
      char *sentence = ed.presets_of[i].name == MC_STR_SMART_PRESET_BLANK ? NULL : describe(ed.presets_of[i].rule);
      if (sentence) wstr_from_utf8(text, 512, sentence);
      free(sentence);
    }
  if (w == ed.back) wstr_tooltip(text, 512, MC_STR_BACK, MC_STR_WINDOWS_SHORTCUT_BACK);
  info->lpszText = text;
}

/* The preview's rows, as the list view asks for them. */
static void on_get_item(NMLVDISPINFOW *info) {
  LVITEMW *lv = &info->item;
  if (!(lv->mask & LVIF_TEXT) || lv->iItem < 0 || lv->iItem >= ed.shown) return;
  const char *text = NULL;
  if (ed.tracks && lv->iItem < ed.tracks->count) {
    const mc_track_info *t = &ed.tracks->items[lv->iItem];
    text = lv->iSubItem ? t->artist : t->title && *t->title ? t->title : mc_str(MC_STR_UNTITLED);
  } else if (ed.albums && lv->iItem < ed.albums->count) {
    const mc_album_info *a = &ed.albums->items[lv->iItem];
    text = lv->iSubItem ? a->artist : a->title && *a->title ? a->title : mc_str(MC_STR_UNTITLED);
  }
  wstr_from_utf8(lv->pszText, (size_t)lv->cchTextMax, text ? text : "");
}

/* --- Drawing --- */

/* Save and Cancel on the frame, as Windows 11 draws buttons over Mica: translucent, or in the
 * accent colour for the one Enter presses. Back is a bare icon with a fill on hover, like the
 * main window's playback buttons. */
static void draw_frame_button(canvas *c, const RECT *rc, const DRAWITEMSTRUCT *dis, control_state state, int radius) {
  HWND w = dis->hwndItem;
  BOOL disabled = (dis->itemState & ODS_DISABLED) != 0;
  BOOL primary = !disabled && (int)dis->CtlID == (ed.read_only ? IDCANCEL : IDOK);
  COLORREF fg;
  if (w == ed.back) {
    fg = theme_draw_state(c, rc, radius, state);
  } else if (theme.high_contrast) {
    fg = theme_draw_state(c, rc, radius, primary ? STATE_SELECTED : state);
    canvas_frame(c, rc, radius, 1, disabled ? theme.text3 : theme.text, 255);
  } else if (primary) {
    canvas_fill(c, rc, radius, theme.accent, state == STATE_PRESSED ? 204 : state == STATE_HOT ? 230 : 255);
    fg = theme.on_accent;
  } else {
    /* Windows 11's control fills and stroke, as white or black at a low opacity. */
    static const int fill[2][3] = {{179, 128, 77}, {15, 21, 8}}; /* light, dark: normal, hot, pressed */
    int i = disabled || state == STATE_NORMAL ? 0 : state == STATE_HOT ? 1 : 2;
    canvas_fill(c, rc, radius, RGB(255, 255, 255), disabled ? fill[theme.dark][2] : fill[theme.dark][i]);
    canvas_frame(c, rc, radius, 1, theme.dark ? RGB(255, 255, 255) : RGB(0, 0, 0), theme.dark ? 24 : 26);
    fg = state == STATE_PRESSED ? theme.text2 : theme.text;
  }
  if (disabled) fg = theme.text3;
  wchar_t title[64];
  GetWindowTextW(w, title, 64);
  UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE | (dis->itemState & ODS_NOACCEL ? DT_HIDEPREFIX : 0);
  canvas_text(c, title, -1, rc, format, (HFONT)SendMessageW(w, WM_GETFONT, 0, 0), fg);
}

/* What's drawn here: on the frame, so with alpha, the path's crumbs and separators, the counts
 * beside the buttons, and Back, Save and Cancel; on the card, a group's or ready-made rule's
 * button: its title, a line beneath (the group's sentence, or that it's empty; the rule's
 * sentence), and at the right a group's count, a warning sign if something in it needs fixing,
 * and an arrow. A crumb in the path: its name, linked but for the last, which is the page
 * shown. */
static void draw_item(const DRAWITEMSTRUCT *dis) {
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas c = {0};
  if (!canvas_reserve(&c, rc.right, rc.bottom)) return;
  HWND w = dis->hwndItem;
  BOOL on_frame = GetParent(w) == ed.dlg;
  if (on_frame) canvas_clear(&c, &rc, theme.frame, ed.mica ? 0 : 255);
  else canvas_clear(&c, &rc, theme.bg, 255);
  wchar_t title[512];
  GetWindowTextW(w, title, 512);
  if (dis->CtlType == ODT_STATIC) {
    canvas_text(&c, title, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                (HFONT)SendMessageW(w, WM_GETFONT, 0, 0), theme.text2);
    canvas_blit(&c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
    canvas_free(&c);
    return;
  }
  int crumb = -1;
  for (int i = 0; i < ed.ncrumbs; i++)
    if (w == ed.crumbs[i]) crumb = i;
  BOOL last = crumb >= 0 && crumb == ed.ncrumbs - 1;
  BOOL pressed = (dis->itemState & ODS_SELECTED) != 0;
  control_state state = last ? STATE_NORMAL : pressed ? STATE_PRESSED : ed.hot == w ? STATE_HOT : STATE_NORMAL;
  int radius = MulDiv(4, (int)ed.dpi, USER_DEFAULT_SCREEN_DPI);
  if (on_frame && crumb < 0) {
    draw_frame_button(&c, &rc, dis, state, radius);
  } else if (crumb >= 0) {
    theme_draw_state(&c, &rc, radius, state);
    RECT text = rc;
    InflateRect(&text, -dx(4), 0);
    node *g = find(ed.stack[crumb]);
    if (g && g->bad) { /* the warning sign first */
      canvas_text(&c, L"\xE7BA", 1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX, ed.icon_font,
                  error_color());
      text.left += dx(10);
    }
    canvas_text(&c, title, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS,
                last ? ed.bold_font : ed.font, last || theme.high_contrast ? theme.text : theme.accent);
  } else {
    canvas_fill(&c, &rc, radius, theme.field, 255);
    theme_draw_state(&c, &rc, radius, state);
    canvas_frame(&c, &rc, radius, 1, theme.line, 255);
    SetBkMode(c.dc, TRANSPARENT);
    HGDIOBJ old = SelectObject(c.dc, ed.font);
    RECT text = rc;
    InflateRect(&text, -dx(4), 0);
    row *r = row_of_window(w);
    node *g = r ? find(r->id) : NULL;
    const char *sub = NULL;
    char *preset_sub = NULL;
    for (int i = 0; i < PRESET_COUNT; i++)
      if (w == ed.presets[i] && ed.presets_of[i].name != MC_STR_SMART_PRESET_BLANK)
        sub = preset_sub = describe(ed.presets_of[i].rule);
    if (g) sub = !g->nkids ? mc_str(MC_STR_SMART_GROUP_EMPTY_HINT) : named(g) ? g->desc : NULL;
    /* At the right: the arrow, the warning sign and the count. */
    RECT right = text;
    UINT glyph = DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
    canvas_text(&c, L"\xE76C", 1, &right, glyph, ed.icon_font, theme.text2); /* ChevronRight */
    right.right -= dx(12);
    if (g && g->bad) {
      canvas_text(&c, L"\xE7BA", 1, &right, glyph, ed.icon_font, error_color()); /* Warning */
      right.right -= dx(12);
    }
    if (g) {
      char *now = g->nkids ? match_json(g, FALSE) : NULL;
      if (now && g->counted && g->count >= 0 && !strcmp(now, g->counted)) {
        wchar_t count[32];
        swprintf(count, 32, L"%d", g->count);
        SelectObject(c.dc, ed.font);
        SetTextColor(c.dc, theme.text2);
        DrawTextW(c.dc, count, -1, &right, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      free(now);
    }
    text.right = right.right - dx(COUNT_W);
    RECT top = text, bottom = text;
    if (sub) {
      top.bottom = rc.bottom / 2;
      top.top = dy(2);
    }
    bottom.top = rc.bottom / 2;
    bottom.bottom = rc.bottom - dy(2);
    SelectObject(c.dc, ed.bold_font);
    SetTextColor(c.dc, theme.text);
    DrawTextW(c.dc, title, -1, &top, DT_LEFT | (sub ? DT_BOTTOM : DT_VCENTER) | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (sub) {
      wchar_t *s = wstr_dup_from_utf8(sub);
      SelectObject(c.dc, ed.font);
      SetTextColor(c.dc, theme.text2);
      DrawTextW(c.dc, s ? s : L"", -1, &bottom, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
      free(s);
    }
    free(preset_sub);
    SelectObject(c.dc, old);
    canvas_opaque(&c, &rc);
  }
  if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
    RECT focus = rc;
    InflateRect(&focus, -1, -1);
    canvas_frame(&c, &focus, radius, 2, theme.text, 255);
  }
  canvas_blit(&c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
  canvas_free(&c);
}

/* Colours for the static controls: hints and counts in secondary text, a problem in red. */
static HBRUSH ctlcolor(UINT msg, HDC dc, HWND ctl) {
  HBRUSH brush = theme_ctlcolor(msg, dc);
  if (msg != WM_CTLCOLORSTATIC) return brush;
  if (ctl == ed.placeholder || ctl == ed.preview_note || ctl == ed.problems_more) SetTextColor(dc, theme.text2);
  if (ctl == ed.problems_title) SetTextColor(dc, error_color());
  for (int i = 0; i < ed.nrows; i++) {
    row *r = &ed.rows[i];
    if (ctl == r->count) SetTextColor(dc, theme.text2);
    if (ctl == r->hint) {
      node *n = find(r->id);
      SetTextColor(dc, n && n->problem ? error_color() : theme.text2);
    }
  }
  return brush;
}

static LRESULT CALLBACK panel_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
      if (lp) on_panel_command((HWND)lp, HIWORD(wp));
      return 0;
    case WM_NOTIFY: {
      NMHDR *hdr = (NMHDR *)lp;
      if (hdr->code == TTN_GETDISPINFOW) {
        on_tip((NMTTDISPINFOW *)lp);
      } else if (hdr->code == DTN_DATETIMECHANGE && !ed.building) {
        row *r = row_of_window(hdr->hwndFrom);
        node *n = r ? find(r->id) : NULL;
        SYSTEMTIME day;
        if (n && DateTime_GetSystemtime(r->date, &day) == GDT_VALID) {
          n->date = date_to_ms(&day);
          changed();
        }
      }
      return 0;
    }
    case WM_DRAWITEM:
      draw_item((const DRAWITEMSTRUCT *)lp);
      return TRUE;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return (LRESULT)ctlcolor(msg, (HDC)wp, (HWND)lp);
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(wnd, &rc);
      FillRect((HDC)wp, &rc, theme.bg_brush);
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

/* Every control in the panel, and the path's crumbs: when one gets the focus (by Tab, say),
 * the panel scrolls to it; the mouse wheel over one scrolls the panel rather than changing a
 * picker; and the owner-drawn buttons light up under the mouse. */
static LRESULT CALLBACK child_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_SETFOCUS:
      if (GetParent(wnd) == ed.panel) PostMessageW(ed.panel, WM_APP_REVEAL, (WPARAM)wnd, 0);
      break;
    case WM_MOUSEWHEEL:
      if (GetFocus() != wnd && GetParent(wnd) == ed.panel && wnd != ed.read_only_text)
        return SendMessageW(ed.panel, msg, wp, lp);
      break;
    case WM_MOUSEMOVE:
      if (ed.hot != wnd && (GetWindowLongW(wnd, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) {
        HWND was = ed.hot;
        ed.hot = wnd;
        TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, wnd, 0};
        TrackMouseEvent(&t);
        if (was) InvalidateRect(was, NULL, FALSE);
        InvalidateRect(wnd, NULL, FALSE);
      }
      break;
    case WM_MOUSELEAVE:
      if (ed.hot == wnd) {
        ed.hot = NULL;
        InvalidateRect(wnd, NULL, FALSE);
      }
      break;
    case WM_GETDLGCODE: /* Enter presses an owner-drawn button, as Space does, not Save */
      if ((GetWindowLongW(wnd, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW && lp &&
          ((MSG *)lp)->message == WM_KEYDOWN && ((MSG *)lp)->wParam == VK_RETURN)
        return DLGC_WANTMESSAGE | DefSubclassProc(wnd, msg, wp, lp);
      break;
    case WM_KEYDOWN:
      if (wp == VK_RETURN && (GetWindowLongW(wnd, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW) {
        SendMessageW(GetParent(wnd), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(wnd), BN_CLICKED), (LPARAM)wnd);
        return 0;
      }
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, child_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* Whether w is a picker whose drop-down (or calendar) is open, which Escape closes first. */
static BOOL dropped(HWND w) {
  wchar_t cls[32];
  if (!w || !GetClassNameW(w, cls, 32)) return FALSE;
  if (!_wcsicmp(cls, WC_COMBOBOXW)) return SendMessageW(w, CB_GETDROPPEDSTATE, 0, 0) != 0;
  if (!_wcsicmp(cls, DATETIMEPICK_CLASSW)) return DateTime_GetMonthCal(w) != NULL;
  return FALSE;
}

/* Escape, Alt+Left and the mouse's back button, wherever the focus is in the dialog: back a
 * group, before the dialog manager makes Escape a Cancel. */
static LRESULT CALLBACK filter_proc(int code, WPARAM wp, LPARAM lp) {
  MSG *m = (MSG *)lp;
  if (code == MSGF_DIALOGBOX && m && ed.dlg && (m->hwnd == ed.dlg || IsChild(ed.dlg, m->hwnd))) {
    BOOL escape = m->message == WM_KEYDOWN && m->wParam == VK_ESCAPE && !dropped(GetFocus());
    BOOL alt_left = m->message == WM_SYSKEYDOWN && m->wParam == VK_LEFT && (m->lParam & (1 << 29));
    BOOL x1 = m->message == WM_XBUTTONUP && GET_XBUTTON_WPARAM(m->wParam) == XBUTTON1;
    if ((escape || alt_left || x1) && go_back()) return 1;
    if (escape && m->hwnd == ed.read_only_text) { /* a multiline edit keeps Escape from the dialog manager */
      PostMessageW(ed.dlg, WM_COMMAND, IDCANCEL, 0);
      return 1;
    }
  }
  return CallNextHookEx(ed.hook, code, wp, lp);
}

/* --- The dialog --- */

static void apply_theme(void) {
  theme_apply_window(ed.dlg, FALSE);
  ed.mica = theme_apply_backdrop(ed.dlg);
  extend_frame();
  theme_apply_controls(ed.dlg);
  if (ed.panel) {
    SetWindowTheme(ed.panel, theme.dark ? L"DarkMode_Explorer" : NULL, NULL);
    theme_apply_controls(ed.panel);
  }
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

static HFONT make_bold_font(void) {
  LOGFONTW lf;
  if (!GetObjectW(ed.font, sizeof lf, &lf)) return NULL;
  lf.lfWeight = FW_SEMIBOLD;
  return CreateFontIndirectW(&lf);
}

static void create_controls(void) {
  ed.font = (HFONT)SendMessageW(ed.dlg, WM_GETFONT, 0, 0);
  ed.dpi = GetDpiForWindow(ed.dlg);
  RECT units = {0, 0, 4, 8};
  MapDialogRect(ed.dlg, &units);
  ed.base_x = units.right;
  ed.base_y = units.bottom;
  ed.icon_font = make_icon_font();
  ed.bold_font = make_bold_font();
  if (!ed.bold_font) ed.bold_font = ed.font;

  ed.tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, NULL, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                           CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, ed.dlg, NULL,
                           GetModuleHandleW(NULL), NULL);
  SendMessageW(ed.tip, TTM_SETMAXTIPWIDTH, 0, dx(240));

  ed.back = dlg_child(WC_BUTTONW, L"\xE72B", BS_OWNERDRAW | WS_TABSTOP, 0); /* Back */
  SendMessageW(ed.back, WM_SETFONT, (WPARAM)ed.icon_font, FALSE);
  set_name(ed.back, mc_wstr(MC_STR_BACK));
  add_tip(ed.back);
  ShowWindow(ed.back, SW_HIDE);
  /* These are owner-drawn, as Mica needs alpha, so they light up under the mouse as the crumbs
   * do. An owner-drawn button can't be a DEFPUSHBUTTON, so the dialog is told which Enter
   * presses. */
  SetWindowSubclass(ed.back, child_proc, 0, 0);
  SetWindowSubclass(GetDlgItem(ed.dlg, IDOK), child_proc, 0, 0);
  SetWindowSubclass(GetDlgItem(ed.dlg, IDCANCEL), child_proc, 0, 0);
  SendMessageW(ed.dlg, DM_SETDEFID, IDOK, 0);

  static BOOL registered;
  if (!registered) {
    WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = panel_proc, .hInstance = GetModuleHandleW(NULL),
                      .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_smart_rows"};
    registered = RegisterClassExW(&wc) != 0;
  }
  ed.panel = dlg_child(L"kithsong_smart_rows", NULL, WS_VSCROLL | WS_CLIPCHILDREN, WS_EX_CONTROLPARENT);
  set_name(ed.panel, mc_wstr(MC_STR_SMART_PATH_NAME));
  ed.preview_label = dlg_child(WC_STATICW, mc_wstr(MC_STR_SMART_PREVIEW), SS_NOPREFIX, 0);
  SendMessageW(ed.preview_label, WM_SETFONT, (WPARAM)ed.bold_font, FALSE);
  ed.preview_note = dlg_child(WC_STATICW, NULL, SS_NOPREFIX | SS_ENDELLIPSIS, 0);
  ed.list = dlg_child(WC_LISTVIEWW, NULL,
                      LVS_REPORT | LVS_OWNERDATA | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, 0);
  ListView_SetExtendedListViewStyle(ed.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  LVCOLUMNW col = {.mask = LVCF_TEXT | LVCF_WIDTH, .cx = 100};
  col.pszText = (wchar_t *)mc_wstr(ed.subject == MC_SMART_TRACKS ? MC_STR_TITLE : MC_STR_ALBUM);
  ListView_InsertColumn(ed.list, 0, &col);
  col.pszText = (wchar_t *)mc_wstr(MC_STR_ARTIST);
  ListView_InsertColumn(ed.list, 1, &col);
  set_name(ed.list, mc_wstr(MC_STR_SMART_PREVIEW));

  ed.problems_title = dlg_child(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE, 0);
  SendMessageW(ed.problems_title, WM_SETFONT, (WPARAM)ed.bold_font, FALSE);
  for (int i = 0; i < PROBLEM_ROWS; i++) {
    ed.problem_lines[i] = dlg_child(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE | SS_PATHELLIPSIS, 0);
    ed.problem_show[i] = dlg_child(WC_BUTTONW, mc_wstr(MC_STR_SMART_PROBLEM_SHOW), BS_PUSHBUTTON | WS_TABSTOP, 0);
  }
  ed.problems_more = dlg_child(WC_STATICW, NULL, SS_NOPREFIX | SS_CENTERIMAGE, 0);
  ed.status = dlg_child(WC_STATICW, NULL, SS_OWNERDRAW, 0);

  /* Tab order: Back and the path, the page, the preview, the problems, then Save and Cancel. */
  HWND order[3 + 2 * PROBLEM_ROWS + 7];
  int n = 0;
  order[n++] = ed.back;
  order[n++] = ed.panel;
  order[n++] = ed.preview_label;
  order[n++] = ed.preview_note;
  order[n++] = ed.list;
  order[n++] = ed.problems_title;
  for (int i = 0; i < PROBLEM_ROWS; i++) {
    order[n++] = ed.problem_lines[i];
    order[n++] = ed.problem_show[i];
  }
  order[n++] = ed.problems_more;
  order[n++] = ed.status;
  order[n++] = GetDlgItem(ed.dlg, IDOK);
  order[n++] = GetDlgItem(ed.dlg, IDCANCEL);
  SetWindowPos(order[0], HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  for (int i = 1; i < n; i++) SetWindowPos(order[i], order[i - 1], 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

/* The fields of ed.subject, and which of them can be sorted on. */
static void read_fields(void) {
  ed.nfields = ed.nsortable = 0;
  for (int i = 0; i < mc_smart_field_count(ed.subject) && ed.nfields < MAX_FIELDS; i++) {
    if (!mc_smart_field_get(ed.subject, i, &ed.fields[ed.nfields])) continue;
    if (ed.fields[ed.nfields].sortable && mc_smart_order_label(ed.fields[ed.nfields].name, 0))
      ed.sortable[ed.nsortable++] = ed.nfields;
    ed.nfields++;
  }
}

static void fail(enum mc_str_id text) {
  wchar_t caption[128];
  GetWindowTextW(ed.dlg, caption, 128);
  MessageBoxW(ed.dlg, mc_wstr(text), caption, MB_OK | MB_ICONERROR);
}

static BOOL save(void) {
  char *rule = rule_json(TRUE);
  BOOL ok = ed.name_text && *ed.name_text && rule && mc_smart_validate(ed.subject, rule) == MC_OK;
  enum mc_str_id failed = ed.creating ? MC_STR_SMART_CREATE_FAILED : MC_STR_SMART_SAVE_FAILED;
  if (ok && ed.creating) {
    ok = mc_smart_create(ed.lib, ed.name_text, ed.subject, rule, ed.uid) == MC_OK;
    ed.changed |= ok;
  } else if (ok) {
    if (!ed.old_name || strcmp(ed.name_text, ed.old_name)) {
      ok = mc_smart_rename(ed.lib, ed.uid, ed.name_text) == MC_OK;
      if (!ok) failed = MC_STR_SMART_RENAME_FAILED;
      ed.changed |= ok;
    }
    if (ok && (!ed.old_rule || strcmp(rule, ed.old_rule))) {
      ok = mc_smart_set_rule(ed.lib, ed.uid, rule) == MC_OK;
      ed.changed |= ok;
    }
  }
  free(rule);
  if (!ok) fail(failed);
  return ok;
}

/* Fills the editor from rule (or makes it read-only) and shows the list's page. */
static void start(const char *name, const char *rule) {
  ed.choosing = FALSE;
  free(ed.name_text);
  ed.name_text = trimmed(name);
  if (!load_rule(rule)) {
    node_free(ed.root); /* what was read before it failed */
    ed.root = node_new(NULL, TRUE);
    ed.nsorts = 0;
    ed.read_only = TRUE;
    ShowWindow(GetDlgItem(ed.dlg, IDOK), SW_HIDE);
    SetDlgItemTextW(ed.dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CLOSE));
    SendMessageW(ed.dlg, DM_SETDEFID, IDCANCEL, 0);
  }
  free(ed.old_rule);
  ed.old_rule = ed.creating ? NULL : ed.read_only ? dup(rule) : rule_json(TRUE); /* as written here */
  if (ed.creating && !ed.root->nkids) cond_new(ed.root); /* a blank rule: one condition to fill in */
  ed.depth = 0;
  ed.stack[0] = ed.root->id;
  ed.scroll = ed.scrolls[0] = 0;
  if (!ed.read_only) {
    describe_groups(ed.root);
    start_worker();
  }
  ed.focus_what = ed.read_only ? FOCUS_NONE : FOCUS_NAME;
  build_page();
  changed();
  update_save();
  update_status();
  update_problems();
  if (ed.read_only) SetFocus(GetDlgItem(ed.dlg, IDCANCEL));
  else submit();
}

typedef struct editor_args {
  const wchar_t *caption;
  const char *name, *rule; /* NULL rule: a new list, starting from the ready-made rules */
} editor_args;

/* Every control gets the font for the new DPI: the icon font's buttons their own, the bold
 * ones theirs, the rest the dialog's. */
typedef struct refont {
  HFONT old_icon, old_bold;
} refont;

static BOOL CALLBACK refont_child(HWND w, LPARAM lp) {
  const refont *r = (const refont *)lp;
  HFONT f = (HFONT)SendMessageW(w, WM_GETFONT, 0, 0);
  HFONT to = f && f == r->old_icon ? ed.icon_font : f && f == r->old_bold ? ed.bold_font : ed.font;
  SendMessageW(w, WM_SETFONT, (WPARAM)to, FALSE);
  wchar_t cls[32];
  if (GetClassNameW(w, cls, 32) && !_wcsicmp(cls, WC_COMBOBOXW)) SendMessageW(w, CB_SETITEMHEIGHT, (WPARAM)-1, dy(ROW) - 6);
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

  refont r = {ed.icon_font, ed.bold_font};
  HFONT old_font = ed.own_font, icon = make_icon_font();
  ed.font = ed.own_font = font;
  HFONT bold = make_bold_font();
  if (icon) ed.icon_font = icon;
  if (bold) ed.bold_font = bold;
  EnumChildWindows(ed.dlg, refont_child, (LPARAM)&r);
  if (icon && r.old_icon) DeleteObject(r.old_icon);
  if (bold && r.old_bold && r.old_bold != old_font && r.old_bold != (HFONT)SendMessageW(ed.dlg, WM_GETFONT, 0, 0))
    DeleteObject(r.old_bold);
  if (old_font) DeleteObject(old_font);
  if (ed.tip) SendMessageW(ed.tip, TTM_SETMAXTIPWIDTH, 0, dx(240));
  SetWindowPos(ed.dlg, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
               suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
  layout_dialog(); /* though the size may not have changed */
  RedrawWindow(ed.dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

/* The dialog's own surface, as the main window's is: the frame (transparent over Mica, otherwise
 * the frame colour) and on it the card, with the line beside the preview and the problems'
 * frame. */
static void paint(HWND dlg) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(dlg, &ps);
  RECT client;
  GetClientRect(dlg, &client);
  canvas *c = &ed.surface;
  if (canvas_reserve(c, client.right, client.bottom)) {
    int frame_alpha = ed.mica ? 0 : 255, radius = card_radius();
    int line = max(1, MulDiv(1, (int)ed.dpi, USER_DEFAULT_SCREEN_DPI));
    canvas_clear(c, &client, theme.frame, frame_alpha);
    canvas_clear(c, &ed.card, theme.bg, 255);
    canvas_frame(c, &ed.card, radius, line, theme.line, 255);
    canvas_round_corners(c, &ed.card, radius, CORNER_ALL, theme.frame, frame_alpha);
    canvas_clear(c, &ed.divider, theme.line, 255);
    if (!IsRectEmpty(&ed.problems_box)) canvas_frame(c, &ed.problems_box, radius, line, error_color(), 255);
    canvas_blit(c, dc, ps.rcPaint.left, ps.rcPaint.top, &ps.rcPaint);
  }
  EndPaint(dlg, &ps);
}

static INT_PTR CALLBACK editor_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_INITDIALOG: {
      const editor_args *a = (const editor_args *)lp;
      ed.dlg = dlg;
      SetDialogDpiChangeBehavior(dlg, DDC_DISABLE_ALL, DDC_DISABLE_ALL); /* on_dpi_changed() does it */
      SetWindowTextW(dlg, a->caption);
      SetDlgItemTextW(dlg, IDOK, mc_wstr_mnemonic(ed.creating ? MC_STR_CREATE : MC_STR_SAVE));
      SetDlgItemTextW(dlg, IDCANCEL, mc_wstr_mnemonic(MC_STR_CANCEL));
      read_fields();
      create_controls();
      ed.hook = SetWindowsHookExW(WH_MSGFILTER, filter_proc, NULL, GetCurrentThreadId());
      unsigned r = 0;
      rand_s(&r);
      ed.seed = (long long)(r & 0x7fffffff); /* should Shuffled be ticked */
      apply_theme();
      if (a->rule) {
        start(a->name, a->rule);
      } else {
        ed.choosing = TRUE;
        ed.focus_what = FOCUS_PRESET;
        build_page();
        update_save();
      }
      layout_dialog();
      return FALSE;
    }
    case WM_SIZE:
      layout_dialog();
      RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
      return TRUE;
    case WM_GETMINMAXINFO: {
      if (!ed.base_x) break;
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
      if (wp == TIMER_PREVIEW) on_timer();
      return TRUE;
    case WM_APP_PREVIEW:
      on_result((result *)lp);
      return TRUE;
    case WM_APP_REBUILD:
      build_page();
      return TRUE;
    case WM_ERASEBKGND: /* paint() fills it all, with the alpha a brush wouldn't give */
      SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
      return TRUE;
    case WM_PAINT:
      paint(dlg);
      return TRUE;
    case WM_DRAWITEM:
      draw_item((const DRAWITEMSTRUCT *)lp);
      return TRUE;
    case WM_NOTIFY: {
      NMHDR *hdr = (NMHDR *)lp;
      if (hdr->hwndFrom == ed.list && hdr->code == LVN_GETDISPINFOW) on_get_item((NMLVDISPINFOW *)lp);
      else if (hdr->code == TTN_GETDISPINFOW) on_tip((NMTTDISPINFOW *)lp);
      return TRUE;
    }
    case WM_COMMAND: {
      int id = LOWORD(wp);
      HWND from = (HWND)lp;
      if (id >= IDOK + 100 && id < IDOK + 100 + PRESET_COUNT && !from) { /* a ready-made rule, from the panel */
        const preset *p = &ed.presets_of[id - IDOK - 100];
        start(p->name == MC_STR_SMART_PRESET_BLANK ? "" : mc_str(p->name), p->rule);
        return TRUE;
      }
      if (from && from == ed.back) {
        go_back();
        return TRUE;
      }
      for (int i = 0; from && i + 1 < ed.ncrumbs; i++)
        if (from == ed.crumbs[i]) {
          pop_to(i);
          return TRUE;
        }
      for (int i = 0; from && i < PROBLEM_ROWS; i++)
        if (from == ed.problem_show[i]) {
          show_problem(ed.problem_ids[i]);
          return TRUE;
        }
      if (id == IDOK) {
        if (IsWindowEnabled(GetDlgItem(dlg, IDOK)) && !ed.read_only && !ed.choosing && save()) EndDialog(dlg, IDOK);
      } else if (id == IDCANCEL) { /* Cancel, closing the window, or Escape on the list's page (filter_proc()) */
        EndDialog(dlg, IDCANCEL);
      }
      return TRUE;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      return (INT_PTR)ctlcolor(msg, (HDC)wp, (HWND)lp);
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
    case WM_THEMECHANGED:
    case WM_DWMCOLORIZATIONCOLORCHANGED: /* the accent colour, which Save is filled with */
      if (theme_is_change(msg, wp, lp)) {
        theme_update();
        apply_theme();
        RedrawWindow(dlg, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      }
      break;
    case WM_DESTROY: {
      KillTimer(dlg, TIMER_PREVIEW);
      stop_worker();
      if (ed.hook) UnhookWindowsHookEx(ed.hook);
      ed.hook = NULL;
      ed.building = TRUE; /* no edits from controls going */
      clear_page();
      for (int i = 0; i < ed.ncrumbs; i++) destroy(ed.crumbs[i]);
      for (int i = 0; i + 1 < ed.ncrumbs; i++) destroy(ed.seps[i]);
      ed.ncrumbs = 0;
      if (ed.tip) DestroyWindow(ed.tip);
      ed.tip = NULL;
      HWND named[] = {ed.list, ed.back, ed.panel};
      for (size_t i = 0; i < sizeof named / sizeof named[0]; i++) clear_name(named[i]);
      for (int i = 0; i < PROBLEM_ROWS; i++) clear_name(ed.problem_show[i]);
      if (ed.icon_font) DeleteObject(ed.icon_font);
      if (ed.bold_font && ed.bold_font != ed.font) DeleteObject(ed.bold_font);
      if (ed.own_font) DeleteObject(ed.own_font);
      ed.icon_font = ed.bold_font = ed.own_font = NULL;
      canvas_free(&ed.surface);
      break;
    }
  }
  return FALSE;
}

/* Runs the editor; ed's library, subject, creating and uid are set. */
static BOOL run_editor(HWND owner, const wchar_t *caption, const char *name, const char *rule) {
  INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_DATE_CLASSES | ICC_LISTVIEW_CLASSES};
  InitCommonControlsEx(&icc);
  if (mc_library_list_playlists(ed.lib, 0, &ed.playlists) != MC_OK) ed.playlists = NULL;
  if (mc_library_list_album_groups(ed.lib, &ed.groups) != MC_OK) ed.groups = NULL;
  ed.presets_of = ed.subject == MC_SMART_TRACKS ? k_track_presets : k_album_presets;
  ed.limit = ed.subject == MC_SMART_TRACKS ? 25 : 10;
  ed.total = -1;
  ed.root = node_new(NULL, TRUE);
  editor_args a = {caption, name, rule};
  INT_PTR r = ed.root ? DialogBoxParamW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDD_SMART_EDITOR), owner, editor_proc,
                                        (LPARAM)&a)
                      : -1;
  BOOL changed = r == IDOK && ed.changed;
  mc_playlist_list_free(ed.playlists);
  mc_album_group_list_free(ed.groups);
  mc_track_list_free(ed.tracks);
  mc_album_list_free(ed.albums);
  node_free(ed.root);
  free(ed.bad);
  free(ed.name_text);
  free(ed.old_name);
  free(ed.old_rule);
  if (ed.acc) IAccPropServices_Release(ed.acc);
  return changed;
}

BOOL smart_new_dialog(HWND owner, mc_library *lib, mc_smart_subject subject, char out_uid[MC_UID_STRLEN]) {
  if (!lib) return FALSE;
  memset(&ed, 0, sizeof ed);
  ed.lib = lib;
  ed.subject = subject;
  ed.creating = TRUE;
  BOOL created = run_editor(
    owner, mc_wstr(subject == MC_SMART_TRACKS ? MC_STR_SMART_NEW_PLAYLIST_TITLE : MC_STR_SMART_NEW_GROUP_TITLE), NULL, NULL);
  if (created && out_uid) memcpy(out_uid, ed.uid, MC_UID_STRLEN);
  return created;
}

/* The smart list uid's name, subject and rule, from the listing. */
static BOOL find_smart(mc_library *lib, const char *uid, char **name, mc_smart_subject *subject, char **rule) {
  mc_smart_list *list = NULL;
  BOOL found = FALSE;
  if (lib && uid && mc_library_list_smart(lib, 0, &list) == MC_OK)
    for (int i = 0; i < list->count && !found; i++) {
      const mc_smart_info *s = &list->items[i];
      if (strcmp(s->uid, uid)) continue;
      found = TRUE;
      if (name) *name = dup(s->name);
      if (subject) *subject = s->subject;
      if (rule) *rule = dup(s->rule ? s->rule : "{\"v\":1}");
    }
  mc_smart_list_free(list);
  return found;
}

BOOL smart_edit_dialog(HWND owner, mc_library *lib, const char *uid) {
  char *name = NULL, *rule = NULL;
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
  ed.old_name = name; /* freed with the editor */
  BOOL changed = run_editor(
    owner, mc_wstr(subject == MC_SMART_TRACKS ? MC_STR_SMART_EDIT_PLAYLIST_TITLE : MC_STR_SMART_EDIT_GROUP_TITLE), name,
    rule);
  free(rule);
  return changed;
}

wchar_t *smart_describe(mc_library *lib, const char *uid) {
  char *rule = NULL, *sentence = NULL;
  mc_smart_subject subject;
  wchar_t *w = NULL;
  if (find_smart(lib, uid, NULL, &subject, &rule) && rule && mc_smart_describe(lib, subject, rule, &sentence) == MC_OK)
    w = wstr_dup_from_utf8(sentence);
  mc_free(sentence);
  free(rule);
  return w;
}
