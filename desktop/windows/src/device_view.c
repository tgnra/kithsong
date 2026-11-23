#include "device_view.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windowsx.h>

#include "canvas.h"
#include "firewall.h"
#include "firewall_banner.h"
#include "issue_dialog.h"
#include "prompt_dialog.h"
#include "scroller.h"
#include "theme.h"
#include "wstr.h"

#define MAX_ROWS 24 /* devices listed, members and nearby ones together */

enum {
  /* The page, above the panels. */
  ID_DEV_SYNC_NOW = 400,
  ID_DEV_STATUS,
  ID_DEV_PROGRESS,
  ID_DEV_MESSAGE,
  ID_DEV_NOTICE,
  ID_DEV_ISSUES, /* Details: the issue dialog for this PC */
  /* This PC's panel. */
  ID_DEV_TITLE,
  ID_DEV_FILES,
  ID_DEV_NOTE,
  ID_DEV_ARCHIVE_LABEL,
  ID_DEV_ARCHIVE,
  ID_DEV_ARCHIVE_TEXT,
  ID_DEV_HEADING,
  ID_DEV_SYNC_ALL,
  ID_DEV_SYNC_ALL_LABEL,
  ID_DEV_TAB, /* four of them */
  ID_DEV_LIST = ID_DEV_TAB + 4,
  ID_DEV_SELECT_ALL,
  ID_DEV_SELECT_NONE,
  ID_DEV_COUNT,
  ID_DEV_QUALITY,
  ID_DEV_FORMAT_LABEL,
  ID_DEV_FORMAT,
  ID_DEV_BUDGET,
  ID_DEV_BUDGET_LABEL,
  ID_DEV_BUDGET_SIZE,
  ID_DEV_BUDGET_UNIT,
  ID_DEV_ESTIMATE,
  /* The devices' panel. */
  ID_DEV_DEVICES,
  ID_DEV_UNPAIRED,
  ID_DEV_NONE,
  ID_DEV_ADD,
  ID_DEV_LEAVE,
  ID_DEV_FIREWALL, /* and its two buttons */
  ID_DEV_FIREWALL_LAST = ID_DEV_FIREWALL + 2,
  ID_DEV_ROW, /* MAX_ROWS of them */
  ID_DEV_ROW_LAST = ID_DEV_ROW + MAX_ROWS - 1,
  /* Timers on the parent, run by their TIMERPROCs rather than WM_TIMER. */
  ID_DEV_APPLY_TIMER = 470,
  ID_DEV_REFRESH_TIMER,
  ID_DEV_ESTIMATE_TIMER,
  ID_DEV_FINISHED_TIMER,
};

/* Posted by the list to itself after its theme changes, to put our checkboxes back. */
#define WM_RECHECK (WM_APP + 1)
/* Posted to the estimate line by the estimate's thread; lParam is its estimate_job. */
#define WM_ESTIMATE_DONE (WM_APP + 2)
/* Likewise, by Select All's thread; lParam is its select_job. */
#define WM_SELECT_DONE (WM_APP + 3)

#define APPLY_DELAY_MS 400
#define ESTIMATE_DELAY_MS 250 /* a read of every track: once the edits pause */
#define FINISHED_MS 5000 /* a finished sync's full bar stays this long */
#define PROGRESS_MAX 1000
#define DEFAULT_BUDGET_GB 32
#define AUTO_ITEM 1 /* the format list's item data for Auto; the others hold their kbps, 0 originals */
#define GIB ((int64_t)1024 * 1024 * 1024)
#define MAX_BUSY 16
#define REVEAL_MARGIN 40 /* at 96 DPI: what's kept in view around a control tabbed to */

enum { TAB_ALBUMS, TAB_ARTISTS, TAB_PLAYLISTS, TAB_GROUPS, NUM_TABS };
static const mc_profile_item_kind k_tab_kind[NUM_TABS] = {MC_PROFILE_ALBUM, MC_PROFILE_ARTIST,
                                                          MC_PROFILE_PLAYLIST, MC_PROFILE_ALBUM_GROUP};
static const enum mc_str_id k_tab_name[NUM_TABS] = {MC_STR_SECTION_ALBUMS, MC_STR_SECTION_ARTISTS,
                                                    MC_STR_SECTION_PLAYLISTS, MC_STR_SECTION_ALBUM_GROUPS};
/* A selection's count of each tab's items, "3 albums". */
static const enum mc_str_id k_tab_count[NUM_TABS] = {MC_STR_COUNT_ALBUMS, MC_STR_COUNT_ARTISTS,
                                                     MC_STR_COUNT_PLAYLISTS, MC_STR_COUNT_ALBUM_GROUPS};

/* Which fields of the profile have edits waiting to be applied. */
enum { PEND_FORMAT = 1, PEND_BUDGET = 2, PEND_SYNC_ALL = 4 };

typedef struct toggle {
  mc_profile_item_kind kind;
  char uid[MC_UID_STRLEN];
  int on;
} toggle;

typedef char uid_str[MC_UID_STRLEN];

/* A sorted set of uids, for the items selected of one kind. */
typedef struct uid_set {
  uid_str *uids;
  int count, cap;
} uid_set;

/* One of the two panels: a window that clips what scrolls in it, with the scrollbar in its
 * right margin, as the settings page does. */
typedef struct panel {
  HWND view;    /* clips the content, and paints the scrollbar */
  HWND content; /* as tall as the controls need, moved up and down in view; they're its children */
  scroller scroll;
  canvas canvas;
  BOOL tracking; /* the mouse's leaving view */
} panel;

/* A row of the devices' panel: another member, or a device nearby that isn't paired. */
typedef struct dev_row {
  char uid[MC_UID_STRLEN];
  BOOL nearby;
  mc_device_kind kind;
  BOOL warning; /* something's wrong with it: its details say what */
  wchar_t name[MC_SYNC_DEVICE_NAME_MAX + 1];
  wchar_t detail[256]; /* what it syncs; for a nearby device, why it can't pair, or "" */
} dev_row;

/* How a member's last session here went, for its details, when it didn't simply succeed. */
typedef struct session_result {
  char uid[MC_UID_STRLEN];
  wchar_t text[256];
} session_result;

static struct {
  HWND parent; /* the main window */
  device_view_host host;
  HWND page;   /* the notices and the strip, then the panels */
  panel left, right;
  HWND status, progress, sync_now, message, notice, issues;
  HWND title, files, note, archive_label, archive_box, archive_text, heading, sync_all, sync_all_label,
    tabs[NUM_TABS], select_all, select_none, list, count, quality, format_label, format, combo_list, budget,
    budget_label, budget_size, budget_unit, estimate;
  HWND devices, dev_btns[MAX_ROWS], unpaired, none, add, leave; /* dev_btns: devs[], as buttons */
  HWND hot; /* the owner-drawn button (a tab or a row) under the mouse, or NULL */
  HFONT font, title_font, heading_font, icon_font, small_icon_font;
  HIMAGELIST checks; /* the list's checkboxes; the list owns them once set */
  UINT dpi;
  BOOL shown;
  BOOL refresh_later; /* a refresh came while the format drop-down was open */
  BOOL measuring;     /* layout_left() is only measuring: nothing moves */
  int x, y, w, h;     /* the last layout */
  int line_y, line_w, divider_x, divider_bottom; /* the page's lines, in it; line_y 0 for none */

  mc_sync *sync;
  mc_library *lib;
  char uid[MC_UID_STRLEN]; /* this PC's */

  /* This PC, from mc_sync_list_devices(). found: it's in a group. */
  BOOL found, removed;
  wchar_t name[MC_SYNC_DEVICE_NAME_MAX + 1];
  wchar_t others[512]; /* the other members' names, for Leave group */
  wchar_t files_text[256]; /* its own report of its files, or "" */
  mc_sync_lib_state lib_state;
  int chosen; /* its user has chosen what it syncs */
  wchar_t warning[1024]; /* mc_sync_device_warning(), or "" */
  wchar_t hold_note[256]; /* mc_sync_device_note(): its next automatic sync, or "" */
  wchar_t activity[256]; /* what sync last did here: mc_sync_activity_text(), or "" */
  wchar_t outlook[256];  /* what the next sync would change: mc_sync_outlook_text(), or "" */
  BOOL has_issues; /* the problems hold issues about it */
  BOOL missing_shown; /* the message says the last session left tracks missing */

  /* The other members, online ones first, then the nearby devices. */
  dev_row devs[MAX_ROWS];
  int ndevs, nmembers;
  session_result results[MAX_ROWS];
  int nresults;

  /* Its profile, with the pending edits applied; NULL if it couldn't be read. */
  mc_sync_profile *profile;
  mc_status profile_status;
  uid_set sel[NUM_TABS];
  uid_set removed_items[NUM_TABS]; /* removed from the device: Select All leaves them out */
  HANDLE select_thread; /* Select All or Select None, storing the rows */

  /* The list's rows: the current tab's items, in the library's order. */
  int tab, rows_tab;
  uid_str *rows;
  int nrows;
  mc_status rows_status;
  BOOL filling; /* ignore the list's and edits' notifications while we set them */

  /* Edits not applied yet. */
  struct {
    unsigned mask;
    mc_transcode_opts format;
    int budget_enabled;
    int64_t budget_bytes;
    int sync_all;
    toggle *toggles;
    int ntoggles, cap;
  } pending;

  /* Sessions in progress, with any member. */
  uid_str busy[MAX_BUSY];
  int nbusy;
  mc_sync_phase phase;
  int64_t sent, received;
  int files_done, files_total; /* the files this PC is fetching */
  int sends_done, sends_total; /* the files the other device is fetching from it */
  int64_t bytes_received, bytes_sent;
  char pace[96]; /* speed and time left, UTF-8: mc_sync_pace_text() */
  /* The last session ended and none is running: the bar shows full, with finished_text,
   * for FINISHED_MS. */
  BOOL finished;
  wchar_t finished_text[256];

  /* The profile's size estimate, worked out on a thread of its own: one at a time. */
  struct {
    HANDLE thread; /* the one running, or NULL */
    unsigned seq;  /* of the last one started */
    BOOL again;    /* the profile changed while it ran */
    BOOL over;     /* the line shows that it doesn't fit */
    HANDLE archive;       /* the archive confirmation's, or NULL */
    unsigned archive_seq; /* of the last of those started */
  } est;
  HWND confirm_dlg;          /* the archive confirmation, while it's open */
  wchar_t confirm_text[512]; /* its text, which it shows from here */
} g_dv;

static void layout_again(void);
static void estimate_request(void);

/* Sync is on. */
static BOOL active(void) {
  return g_dv.sync != NULL;
}

/* This PC is in a group, and its profile can be shown. */
static BOOL member(void) {
  return g_dv.sync && g_dv.found && !g_dv.removed;
}

/* --- Text helpers --- */

static void widen(const char *s, wchar_t *out, int cap) {
  if (!s || !MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap)) out[0] = L'\0';
  out[cap - 1] = L'\0';
}

/* w as UTF-8, for mc_wformat()'s arguments, in one of four static buffers used in turn: valid
 * until the fourth call after. UI thread only. */
static const char *u8(const wchar_t *w) {
  static char bufs[4][512];
  static int next;
  char *b = bufs[next++ & 3];
  if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 512, NULL, NULL)) b[0] = '\0';
  return b;
}

static int64_t unix_ms_now(void) {
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  ULARGE_INTEGER t = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
  return (int64_t)((t.QuadPart - 116444736000000000ULL) / 10000);
}

/* "just now", "3 minutes ago", "yesterday", "on 12/03/2026". */
static void fmt_ago(int64_t ms, wchar_t *out, int cap) {
  int64_t min = (unix_ms_now() - ms) / 60000;
  if (min < 1) {
    swprintf(out, (size_t)cap, L"%ls", mc_wstr(MC_STR_SYNCED_JUST_NOW));
  } else if (min < 60) {
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_AGO_MINUTES, (long)min), MC_D((int)min));
  } else if (min < 24 * 60) {
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_AGO_HOURS, (long)(min / 60)), MC_D((int)(min / 60)));
  } else if (min < 2 * 24 * 60) {
    swprintf(out, (size_t)cap, L"%ls", mc_wstr(MC_STR_AGO_YESTERDAY));
  } else if (min < 7 * 24 * 60) {
    int days = (int)(min / (24 * 60));
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_AGO_DAYS, days), MC_D(days));
  } else {
    ULARGE_INTEGER t = {.QuadPart = (uint64_t)ms * 10000 + 116444736000000000ULL};
    FILETIME ft = {t.LowPart, t.HighPart}, local;
    SYSTEMTIME st;
    wchar_t date[64] = L"";
    if (FileTimeToLocalFileTime(&ft, &local) && FileTimeToSystemTime(&local, &st))
      GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, 64, NULL);
    mc_wformat(out, (size_t)cap, mc_str(MC_STR_AGO_ON_DATE), MC_S(u8(date)));
  }
}

static BOOL is_checked(HWND box) {
  return SendMessageW(box, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void set_checked(HWND box, BOOL on) {
  SendMessageW(box, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void set_message(const wchar_t *text) {
  SetWindowTextW(g_dv.message, text ? text : L"");
  g_dv.missing_shown = FALSE; /* SYNC_EV_DONE sets it again after */
}

/* --- Selection sets --- */

static int uid_cmp(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static BOOL set_has(const uid_set *s, const char *uid) {
  return s->count && bsearch(uid, s->uids, (size_t)s->count, sizeof s->uids[0], uid_cmp) != NULL;
}

static void set_put(uid_set *s, const char *uid, BOOL on) {
  int lo = 0, hi = s->count;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    if (strcmp(s->uids[mid], uid) < 0) lo = mid + 1;
    else hi = mid;
  }
  BOOL there = lo < s->count && !strcmp(s->uids[lo], uid);
  if (on && !there) {
    if (s->count == s->cap) {
      int cap = s->cap ? 2 * s->cap : 64;
      uid_str *u = realloc(s->uids, (size_t)cap * sizeof *u);
      if (!u) return;
      s->uids = u;
      s->cap = cap;
    }
    memmove(s->uids + lo + 1, s->uids + lo, (size_t)(s->count - lo) * sizeof s->uids[0]);
    memcpy(s->uids[lo], uid, MC_UID_STRLEN);
    s->uids[lo][MC_UID_STRLEN - 1] = '\0';
    s->count++;
  } else if (!on && there) {
    memmove(s->uids + lo, s->uids + lo + 1, (size_t)(s->count - lo - 1) * sizeof s->uids[0]);
    s->count--;
  }
}

static int tab_of_kind(mc_profile_item_kind kind) {
  for (int i = 0; i < NUM_TABS; i++)
    if (k_tab_kind[i] == kind) return i;
  return -1;
}

static void build_sel(void) {
  for (int i = 0; i < NUM_TABS; i++) g_dv.sel[i].count = g_dv.removed_items[i].count = 0;
  if (!g_dv.profile) return;
  for (int i = 0; i < g_dv.profile->count; i++) {
    int t = tab_of_kind(g_dv.profile->items[i].kind);
    if (t >= 0) set_put(&g_dv.sel[t], g_dv.profile->items[i].ref_uid, TRUE);
  }
  for (int i = 0; i < g_dv.profile->removed_count; i++) {
    int t = tab_of_kind(g_dv.profile->removed[i].kind);
    if (t >= 0) set_put(&g_dv.removed_items[t], g_dv.profile->removed[i].ref_uid, TRUE);
  }
}

/* Whether the current list's row for uid is ticked: Sync everything has all but what was
 * removed, a selection what it has. */
static BOOL row_ticked(int tab, const char *uid) {
  if (g_dv.profile && g_dv.profile->sync_all) return !set_has(&g_dv.removed_items[tab], uid);
  return set_has(&g_dv.sel[tab], uid);
}

/* --- Pending edits --- */

static BOOL pending_any(void) {
  return g_dv.pending.mask || g_dv.pending.ntoggles;
}

static void pending_clear(void) {
  g_dv.pending.mask = 0;
  g_dv.pending.ntoggles = 0;
}

static void pending_toggle(mc_profile_item_kind kind, const char *uid, int on) {
  for (int i = 0; i < g_dv.pending.ntoggles; i++) {
    toggle *t = &g_dv.pending.toggles[i];
    if (t->kind == kind && !strcmp(t->uid, uid)) {
      t->on = on;
      return;
    }
  }
  if (g_dv.pending.ntoggles == g_dv.pending.cap) {
    int cap = g_dv.pending.cap ? 2 * g_dv.pending.cap : 16;
    toggle *t = realloc(g_dv.pending.toggles, (size_t)cap * sizeof *t);
    if (!t) return;
    g_dv.pending.toggles = t;
    g_dv.pending.cap = cap;
  }
  toggle *t = &g_dv.pending.toggles[g_dv.pending.ntoggles++];
  t->kind = kind;
  memcpy(t->uid, uid, MC_UID_STRLEN);
  t->uid[MC_UID_STRLEN - 1] = '\0';
  t->on = on;
}

/* Applies the pending edits to p, read just before. */
static mc_status apply_pending(mc_sync_profile *p) {
  if (g_dv.pending.mask & PEND_FORMAT) p->format = g_dv.pending.format;
  if (g_dv.pending.mask & PEND_BUDGET) {
    p->budget_enabled = g_dv.pending.budget_enabled;
    p->budget_bytes = g_dv.pending.budget_bytes;
  }
  if (g_dv.pending.mask & PEND_SYNC_ALL) p->sync_all = g_dv.pending.sync_all;
  if (!g_dv.pending.ntoggles) return MC_OK;

  /* The items, less every toggled one, then the toggled-on ones. */
  mc_profile_item *items = malloc(((size_t)p->count + (size_t)g_dv.pending.ntoggles) * sizeof *items);
  if (!items) return MC_ERR_NOMEM;
  int n = 0;
  for (int i = 0; i < p->count; i++) {
    BOOL toggled = FALSE;
    for (int j = 0; j < g_dv.pending.ntoggles && !toggled; j++)
      toggled = g_dv.pending.toggles[j].kind == p->items[i].kind &&
                !strcmp(g_dv.pending.toggles[j].uid, p->items[i].ref_uid);
    if (!toggled) items[n++] = p->items[i];
  }
  for (int j = 0; j < g_dv.pending.ntoggles; j++) {
    const toggle *t = &g_dv.pending.toggles[j];
    if (!t->on) continue;
    items[n].kind = t->kind;
    memcpy(items[n].ref_uid, t->uid, MC_UID_STRLEN);
    n++;
  }
  free(p->items);
  p->items = items;
  p->count = n;
  return MC_OK;
}

/* --- Loading --- */

static void free_profile(void) {
  mc_sync_profile_free(g_dv.profile);
  g_dv.profile = NULL;
}

/* A device's own report of its files: "1,204 of 1,210 files OK · 6 missing · 12 GB"; "" if it
 * hasn't written one. */
static void files_text(const mc_sync_device *d, wchar_t *text, int cap) {
  text[0] = L'\0';
  if (!d->has_status) return;
  wchar_t ok[32], expected[32], n[32], size[32];
  const wchar_t *sep = mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR);
  mc_wformat(text, (size_t)cap, mc_str_plural(MC_STR_FILES_OK, (long)d->expected),
             MC_S(u8(wstr_count(ok, 32, d->ok))), MC_S(u8(wstr_count(expected, 32, d->expected))));
  size_t used = wcslen(text), len = (size_t)cap;
  if (d->missing > 0) {
    used += (size_t)swprintf(text + used, len - used, L"%ls", sep);
    mc_wformat(text + used, len - used, mc_str(MC_STR_FILES_MISSING), MC_S(u8(wstr_count(n, 32, d->missing))));
    used = wcslen(text);
  }
  if (d->corrupt > 0) {
    used += (size_t)swprintf(text + used, len - used, L"%ls", sep);
    mc_wformat(text + used, len - used, mc_str(MC_STR_FILES_DAMAGED), MC_S(u8(wstr_count(n, 32, d->corrupt))));
    used = wcslen(text);
  }
  swprintf(text + used, len - used, L"%ls%ls", sep, wstr_size(size, 32, d->bytes));
}

/* This PC's own row. */
static void load_self(const mc_sync_device *d) {
  char text[1024];
  g_dv.found = TRUE;
  g_dv.removed = d->removed;
  files_text(d, g_dv.files_text, sizeof g_dv.files_text / sizeof *g_dv.files_text);
  if (mc_sync_device_warning(g_dv.sync, d, 0, text, sizeof text))
    wstr_from_utf8(g_dv.warning, sizeof g_dv.warning / sizeof *g_dv.warning, text);
  if (mc_sync_device_note(d, 0, 1, text, sizeof text))
    wstr_from_utf8(g_dv.hold_note, sizeof g_dv.hold_note / sizeof *g_dv.hold_note, text);
}

static dev_row *add_row(const char *uid, const char *name, mc_device_kind kind) {
  if (g_dv.ndevs == MAX_ROWS) return NULL;
  dev_row *r = &g_dv.devs[g_dv.ndevs++];
  memset(r, 0, sizeof *r);
  snprintf(r->uid, sizeof r->uid, "%s", uid);
  widen(name && *name ? name : mc_str(MC_STR_DEVICE_UNNAMED), r->name, MC_SYNC_DEVICE_NAME_MAX + 1);
  r->kind = kind;
  return r;
}

/* Another member, with what its profile syncs in a sentence. */
static void add_member(const mc_sync_device *d) {
  dev_row *r = add_row(d->uid, d->name, d->kind);
  if (!r) return;
  g_dv.nmembers++;
  char text[1024] = "";
  r->warning = mc_sync_device_warning(g_dv.sync, d, 0, text, sizeof text) > 0;
  mc_sync_profile *p = NULL;
  text[0] = '\0';
  if (mc_sync_get_profile(g_dv.sync, d->uid, &p) == MC_OK) mc_sync_profile_summary(p, text, sizeof text);
  mc_sync_profile_free(p);
  wstr_from_utf8(r->detail, sizeof r->detail / sizeof *r->detail, text);
}

/* A device nearby, with why pairing with it would be refused, if it would. */
static void add_nearby(const mc_sync_nearby *n) {
  dev_row *r = add_row(n->uid, n->name, n->kind);
  if (!r) return;
  r->nearby = TRUE;
  char text[512];
  if (n->version.outdated != MC_SYNC_VERSION_OK) {
    mc_sync_version_text(n->name, &n->version, text, sizeof text);
    wstr_from_utf8(r->detail, sizeof r->detail / sizeof *r->detail, text);
  } else if (n->in_other_group) {
    swprintf(r->detail, sizeof r->detail / sizeof *r->detail, L"%ls", mc_wstr(MC_STR_DEVICE_OTHER_GROUP_TIP));
  }
}

/* Reads the devices: this PC's row, the other members and the devices nearby. */
static void load_devices(void) {
  g_dv.found = g_dv.removed = g_dv.has_issues = FALSE;
  g_dv.others[0] = g_dv.files_text[0] = g_dv.warning[0] = g_dv.hold_note[0] = L'\0';
  g_dv.activity[0] = g_dv.outlook[0] = L'\0';
  g_dv.lib_state = MC_SYNC_LIBRARY_GROUP;
  g_dv.chosen = TRUE;
  g_dv.ndevs = g_dv.nmembers = 0;
  if (!g_dv.sync) return;
  char text[512];
  if (mc_sync_get_device_name(g_dv.sync, text, sizeof text) == MC_OK) widen(text, g_dv.name, MC_SYNC_DEVICE_NAME_MAX + 1);
  else g_dv.name[0] = L'\0';

  mc_sync_device_list *list = NULL;
  if (mc_sync_list_devices(g_dv.sync, &list) == MC_OK && list) {
    size_t used = 0, cap = sizeof g_dv.others / sizeof g_dv.others[0];
    for (int pass = 0; pass < 2; pass++)
      for (int i = 0; i < list->count; i++) {
        const mc_sync_device *d = &list->items[i];
        if (d->is_self) {
          if (pass == 0) load_self(d);
          continue;
        }
        if (d->removed || (pass == 0) != (d->online != 0)) continue;
        add_member(d);
        if (d->name && used + 4 < cap) {
          wchar_t name[MC_SYNC_DEVICE_NAME_MAX + 1];
          widen(d->name, name, MC_SYNC_DEVICE_NAME_MAX + 1);
          int n = swprintf(g_dv.others + used, cap - used, L"%ls%ls",
                           used ? mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR) : L"", name);
          if (n > 0) used += (size_t)n;
        }
      }
    mc_sync_device_list_free(list);
  }
  mc_sync_nearby_list *nearby = NULL;
  if (mc_sync_list_nearby(g_dv.sync, &nearby) == MC_OK && nearby) {
    for (int i = 0; i < nearby->count; i++) add_nearby(&nearby->items[i]);
    mc_sync_nearby_list_free(nearby);
  }

  mc_sync_outlook o;
  if (mc_sync_get_outlook(g_dv.sync, &o) == MC_OK) {
    g_dv.chosen = o.chosen;
    text[0] = '\0';
    mc_sync_outlook_text(&o, text, sizeof text);
    wstr_from_utf8(g_dv.outlook, sizeof g_dv.outlook / sizeof *g_dv.outlook, text);
  }
  mc_sync_activity a;
  if (mc_sync_get_last_activity(g_dv.sync, &a) == MC_OK) {
    text[0] = '\0';
    mc_sync_activity_text(&a, text, sizeof text);
    wstr_from_utf8(g_dv.activity, sizeof g_dv.activity / sizeof *g_dv.activity, text);
  }
  mc_sync_lib_state state;
  if (mc_sync_library_state(g_dv.sync, &state) == MC_OK) g_dv.lib_state = state;
  g_dv.has_issues = g_dv.found && issue_dialog_has_issues(g_dv.sync, g_dv.uid, NULL);
}

/* Reads the profile afresh and lays the pending edits over it. */
static void load_profile(void) {
  free_profile();
  g_dv.profile_status = MC_ERR_NOT_FOUND;
  if (member()) {
    g_dv.profile_status = mc_sync_get_profile(g_dv.sync, g_dv.uid, &g_dv.profile);
    if (g_dv.profile_status == MC_OK) g_dv.profile_status = apply_pending(g_dv.profile);
    if (g_dv.profile_status != MC_OK) free_profile();
  }
  build_sel();
  estimate_request();
}

/* An archive's profile is fixed. */
static BOOL editable(void) {
  return g_dv.profile && !g_dv.profile->archive;
}

static BOOL syncing(void) {
  return g_dv.nbusy > 0;
}

/* Where the bar stands: a step for each phase, and the files phase by files fetched and
 * sent. */
static int progress_pos(void) {
  switch (g_dv.phase) {
    case MC_SYNC_PHASE_CONNECTING: return 0;
    case MC_SYNC_PHASE_GROUP: return PROGRESS_MAX / 20;
    case MC_SYNC_PHASE_LIBRARY: return PROGRESS_MAX * 3 / 20;
    case MC_SYNC_PHASE_FILES: {
      int base = PROGRESS_MAX * 3 / 10;
      int total = g_dv.files_total + g_dv.sends_total;
      if (total <= 0) return base;
      int done = (g_dv.files_done < g_dv.files_total ? g_dv.files_done : g_dv.files_total) +
                 (g_dv.sends_done < g_dv.sends_total ? g_dv.sends_done : g_dv.sends_total);
      return base + (int)((int64_t)(PROGRESS_MAX - base) * done / total);
    }
    default: return PROGRESS_MAX;
  }
}

static void CALLBACK finished_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time);

/* Starts or ends the finished state; started, it ends by itself after FINISHED_MS. */
static void set_finished(BOOL on) {
  g_dv.finished = on;
  if (on) SetTimer(g_dv.parent, ID_DEV_FINISHED_TIMER, FINISHED_MS, finished_timer);
  else KillTimer(g_dv.parent, ID_DEV_FINISHED_TIMER);
}

/* --- Controls from state --- */

/* While syncing, Sync now reads Cancel sync: set only on a change, not at every progress. */
static void set_sync_button(void) {
  const wchar_t *want = mc_wstr(syncing() ? MC_STR_CANCEL_SYNC : MC_STR_SYNC_NOW);
  wchar_t have[64];
  GetWindowTextW(g_dv.sync_now, have, 64);
  if (wcscmp(have, want)) SetWindowTextW(g_dv.sync_now, want);
}

/* The strip's line: how the sessions are going while they run, then for a moment how they
 * went; else what sync last did here or, before it has done anything, what the next sync
 * would change, with when this PC next syncs by itself. */
static void update_strip(void) {
  wchar_t text[256] = L"";
  static const enum mc_str_id phases[] = {
    [MC_SYNC_PHASE_CONNECTING] = MC_STR_PHASE_CONNECTING,
    [MC_SYNC_PHASE_GROUP] = MC_STR_PHASE_GROUP,
    [MC_SYNC_PHASE_LIBRARY] = MC_STR_PHASE_LIBRARY,
    [MC_SYNC_PHASE_FILES] = MC_STR_PHASE_FILES,
    [MC_SYNC_PHASE_DONE] = MC_STR_PHASE_DONE,
  };
  /* The other device's turn: it's fetching from this PC, or it's all there is. */
  BOOL sending = g_dv.sends_total > 0 && (g_dv.sends_done < g_dv.sends_total || g_dv.files_total <= 0);
  if (syncing()) {
    BOOL known = (unsigned)g_dv.phase <= (unsigned)MC_SYNC_PHASE_DONE;
    const wchar_t *phase = mc_wstr(known ? phases[g_dv.phase] : MC_STR_SYNCING_PROGRESS);
    if (g_dv.phase == MC_SYNC_PHASE_CONNECTING) {
      swprintf(text, 256, L"%ls", phase);
    } else if (g_dv.phase == MC_SYNC_PHASE_FILES && sending) {
      wchar_t done[32], total[32], size[32];
      mc_wformat(text, 256, mc_str_plural(MC_STR_PROGRESS_FILES_SENT, (long)g_dv.sends_total), MC_S(u8(phase)),
                 MC_S(u8(wstr_count(done, 32, g_dv.sends_done))), MC_S(u8(wstr_count(total, 32, g_dv.sends_total))),
                 MC_S(u8(wstr_size(size, 32, g_dv.bytes_sent))));
    } else if (g_dv.phase == MC_SYNC_PHASE_FILES && g_dv.files_total > 0) {
      wchar_t done[32], total[32], size[32];
      mc_wformat(text, 256, mc_str_plural(MC_STR_PROGRESS_FILES, (long)g_dv.files_total), MC_S(u8(phase)),
                 MC_S(u8(wstr_count(done, 32, g_dv.files_done))), MC_S(u8(wstr_count(total, 32, g_dv.files_total))),
                 MC_S(u8(wstr_size(size, 32, g_dv.bytes_received))));
    } else if (g_dv.phase == MC_SYNC_PHASE_FILES) { /* fetching nothing; maybe serving */
      wchar_t received[32], sent[32];
      mc_wformat(text, 256, mc_str(MC_STR_PROGRESS_SIZES), MC_S(u8(phase)),
                 MC_S(u8(wstr_size(received, 32, g_dv.bytes_received))),
                 MC_S(u8(wstr_size(sent, 32, g_dv.bytes_sent))));
    } else {
      wchar_t sent[32], received[32];
      mc_wformat(text, 256, mc_str(MC_STR_PROGRESS_ROWS), MC_S(u8(phase)),
                 MC_S(u8(wstr_count(sent, 32, g_dv.sent))), MC_S(u8(wstr_count(received, 32, g_dv.received))));
    }
    /* Of several sessions, whichever spoke last would have its speed shown: none is better. */
    if (g_dv.phase == MC_SYNC_PHASE_FILES && g_dv.nbusy == 1 && g_dv.pace[0]) {
      wchar_t pace[96];
      size_t used = wcslen(text);
      swprintf(text + used, 256 - used, L"%ls%ls", mc_wstr(MC_STR_SEPARATOR_DOT),
               wstr_from_utf8(pace, 96, g_dv.pace));
    }
    SendMessageW(g_dv.progress, PBM_SETPOS, (WPARAM)progress_pos(), 0);
  } else if (g_dv.finished) {
    swprintf(text, 256, L"%ls", g_dv.finished_text);
    SendMessageW(g_dv.progress, PBM_SETPOS, PROGRESS_MAX, 0);
  } else if (!active()) {
    swprintf(text, 256, L"%ls", mc_wstr(MC_STR_SYNC_IS_OFF));
  } else if (member()) {
    const wchar_t *last = g_dv.activity[0] ? g_dv.activity : g_dv.outlook;
    if (last[0] && g_dv.hold_note[0]) swprintf(text, 256, L"%ls%ls%ls", last, mc_wstr(MC_STR_SEPARATOR_DOT), g_dv.hold_note);
    else swprintf(text, 256, L"%ls", last[0] ? last : g_dv.hold_note);
  }
  SetWindowTextW(g_dv.status, text);
  set_sync_button();
  EnableWindow(g_dv.sync_now, member());
}

/* The page's notice: what's wrong here, or what keeps this PC from syncing music. */
static void update_notice(void) {
  const wchar_t *text = L"";
  if (member() && g_dv.warning[0]) text = g_dv.warning; /* unreachable, low on space */
  else if (g_dv.lib_state == MC_SYNC_LIBRARY_OTHER) text = mc_wstr(MC_STR_WINDOWS_PC_SYNC_OTHER_LIBRARY);
  else if (g_dv.lib_state == MC_SYNC_LIBRARY_UNKNOWN) text = mc_wstr(MC_STR_SYNC_UNKNOWN_LIBRARY);
  else if (member() && !g_dv.chosen) text = mc_wstr(MC_STR_SYNC_CHOOSE_FIRST);
  SetWindowTextW(g_dv.notice, text);
}

static void set_check(int row, BOOL on) {
  ListView_SetItemState(g_dv.list, row, INDEXTOSTATEIMAGEMASK(on ? 2 : 1), LVIS_STATEIMAGEMASK);
}

/* Select All and Select None, each greyed out while pressing it would change nothing. Keeps
 * the keyboard's place when the one with the focus goes grey. */
static void update_select_buttons(void) {
  BOOL can_all = FALSE, can_none = FALSE;
  if (editable() && g_dv.rows_tab >= 0 && !g_dv.profile->sync_all) {
    const uid_set *sel = &g_dv.sel[g_dv.rows_tab], *removed = &g_dv.removed_items[g_dv.rows_tab];
    for (int i = 0; i < g_dv.nrows && !(can_all && can_none); i++) {
      if (set_has(sel, g_dv.rows[i])) can_none = TRUE;
      else if (!set_has(removed, g_dv.rows[i])) can_all = TRUE;
    }
  }
  HWND focus = GetFocus();
  EnableWindow(g_dv.select_all, can_all);
  EnableWindow(g_dv.select_none, can_none);
  if ((focus == g_dv.select_all && !can_all) || (focus == g_dv.select_none && !can_none))
    SetFocus(can_all ? g_dv.select_all : can_none ? g_dv.select_none : g_dv.list);
}

/* The list's checkboxes from the selection. */
static void update_checks(void) {
  if (g_dv.rows_tab < 0) return;
  g_dv.filling = TRUE;
  SendMessageW(g_dv.list, WM_SETREDRAW, FALSE, 0);
  for (int i = 0; i < g_dv.nrows; i++) set_check(i, row_ticked(g_dv.rows_tab, g_dv.rows[i]));
  SendMessageW(g_dv.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_dv.list, NULL, TRUE);
  g_dv.filling = FALSE;
  update_select_buttons();
}

static void update_count(void) {
  char counts[256] = "";
  size_t used = 0;
  for (int i = 0; i < NUM_TABS && used < sizeof counts; i++) {
    int n = g_dv.sel[i].count;
    if (!n) continue;
    if (used) used += (size_t)snprintf(counts + used, sizeof counts - used, "%s", mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
    if (used < sizeof counts)
      used += (size_t)mc_format(counts + used, sizeof counts - used, mc_str_plural(k_tab_count[i], n), MC_D(n));
  }
  wchar_t text[256];
  if (counts[0]) mc_wformat(text, 256, mc_str(MC_STR_COUNT_SELECTED), MC_S(counts));
  else swprintf(text, 256, L"%ls", mc_wstr(MC_STR_NOTHING_SELECTED));
  SetWindowTextW(g_dv.count, text);
}

/* The devices' rows from devs. Each button's text is what screen readers read: the name and
 * what it says under it. */
static void update_rows(void) {
  for (int i = 0; i < g_dv.ndevs; i++) {
    const dev_row *r = &g_dv.devs[i];
    wchar_t text[512], have[512];
    if (r->detail[0]) swprintf(text, 512, L"%ls%ls%ls", r->name, mc_wstr(MC_STR_SEPARATOR_DOT), r->detail);
    else swprintf(text, 512, L"%ls", r->name);
    GetWindowTextW(g_dv.dev_btns[i], have, 512);
    if (wcscmp(have, text)) SetWindowTextW(g_dv.dev_btns[i], text);
    InvalidateRect(g_dv.dev_btns[i], NULL, FALSE); /* its marks may have changed */
  }
  wchar_t unpaired[64];
  mc_wformat(unpaired, 64, mc_str(MC_STR_DEVICES_UNPAIRED), MC_D(g_dv.ndevs - g_dv.nmembers));
  SetWindowTextW(g_dv.unpaired, unpaired);
}

/* What comes from the devices: this PC's name and files, the strip, the notice and the rows. */
static void update_devices(void) {
  wchar_t title[MC_SYNC_DEVICE_NAME_MAX + 32];
  swprintf(title, MC_SYNC_DEVICE_NAME_MAX + 32, L"%ls%ls", g_dv.name, mc_wstr(MC_STR_WINDOWS_THIS_PC_SUFFIX));
  SetWindowTextW(g_dv.title, title);
  SetWindowTextW(g_dv.files, g_dv.files_text);
  update_strip();
  update_notice();
  update_rows();
}

/* Every control's text, value and enabled state from the loaded devices and profile. The
 * budget's edit is left alone while it has the focus, unless force. */
static void update_controls(BOOL force) {
  g_dv.filling = TRUE;
  update_devices();

  BOOL edit = editable();
  const mc_sync_profile *p = g_dv.profile;
  set_checked(g_dv.archive_box, p && p->archive);
  EnableWindow(g_dv.archive_box, p != NULL);

  int sel = -1, n = (int)SendMessageW(g_dv.format, CB_GETCOUNT, 0, 0);
  for (int i = 0; p && i < n; i++) {
    int kbps = (int)SendMessageW(g_dv.format, CB_GETITEMDATA, (WPARAM)i, 0);
    if (p->format.format == MC_FORMAT_AUTO       ? kbps == AUTO_ITEM
        : p->format.format == MC_FORMAT_ORIGINAL ? kbps == 0
                                                 : kbps == p->format.bitrate_kbps)
      sel = i;
  }
  SendMessageW(g_dv.format, CB_SETCURSEL, (WPARAM)sel, 0);
  EnableWindow(g_dv.format, edit);

  set_checked(g_dv.budget, p && p->budget_enabled);
  EnableWindow(g_dv.budget, edit);
  if (force || GetFocus() != g_dv.budget_size) {
    int64_t gb = p && p->budget_bytes > 0 ? (p->budget_bytes + GIB / 2) / GIB : DEFAULT_BUDGET_GB;
    wchar_t text[32];
    swprintf(text, 32, L"%d", (int)(gb > 0 ? gb : 1));
    SetWindowTextW(g_dv.budget_size, text);
  }
  EnableWindow(g_dv.budget_size, edit && p->budget_enabled);

  set_checked(g_dv.sync_all, !p || p->sync_all);
  EnableWindow(g_dv.sync_all, edit);
  for (int i = 0; i < NUM_TABS; i++) EnableWindow(g_dv.tabs[i], edit);
  EnableWindow(g_dv.list, edit);
  update_select_buttons();

  /* Outside a group the note stands in for the profile; in one it's under What to sync. */
  wchar_t note[256] = L"";
  if (active() && !g_dv.found) swprintf(note, 256, L"%ls", mc_wstr(MC_STR_DEVICE_NOT_IN_GROUP));
  else if (active() && g_dv.removed) swprintf(note, 256, L"%ls", mc_wstr(MC_STR_DEVICE_REMOVED));
  else if (p && p->archive) swprintf(note, 256, L"%ls", mc_wstr(MC_STR_DEVICE_ARCHIVE_NOTE));
  else if (!p && member())
    mc_wformat(note, 256, mc_str(MC_STR_PROFILE_READ_FAILED), MC_S(mc_status_str(g_dv.profile_status)));
  SetWindowTextW(g_dv.note, note);

  update_count();
  g_dv.filling = FALSE;
  /* The labels' colours follow their controls'. */
  HWND labels[] = {g_dv.archive_label, g_dv.archive_text, g_dv.format_label, g_dv.budget_label, g_dv.budget_unit,
                   g_dv.sync_all_label};
  for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++) InvalidateRect(labels[i], NULL, TRUE);
  for (int i = 0; i < NUM_TABS; i++) InvalidateRect(g_dv.tabs[i], NULL, FALSE);
}

/* The library's items for one tab; one of them is set. */
typedef struct tab_items {
  mc_album_list *albums;
  mc_artist_list *artists;
  mc_playlist_list *playlists;
  mc_album_group_list *groups;
} tab_items;

static mc_status list_items(int tab, tab_items *out, int *n) {
  *out = (tab_items){0};
  *n = 0;
  if (!g_dv.lib) return MC_ERR_INVALID_ARG;
  mc_status st;
  switch (tab) {
    case TAB_ALBUMS:
      st = mc_library_list_albums(g_dv.lib, &out->albums);
      if (st == MC_OK && out->albums) *n = out->albums->count;
      break;
    case TAB_ARTISTS:
      st = mc_library_list_artists(g_dv.lib, &out->artists);
      if (st == MC_OK && out->artists) *n = out->artists->count;
      break;
    case TAB_PLAYLISTS:
      st = mc_library_list_playlists(g_dv.lib, 0, &out->playlists);
      if (st == MC_OK && out->playlists) *n = out->playlists->count;
      break;
    default:
      st = mc_library_list_album_groups(g_dv.lib, 0, &out->groups);
      if (st == MC_OK && out->groups) *n = out->groups->count;
      break;
  }
  return st;
}

static void free_items(tab_items *items) {
  mc_album_list_free(items->albums);
  mc_artist_list_free(items->artists);
  mc_playlist_list_free(items->playlists);
  mc_album_group_list_free(items->groups);
  *items = (tab_items){0};
}

/* Item i's uid, and its row's text: "Artist - Title (year)" or a name, and a count, or that
 * it was removed from the device. */
static const char *item_row(const tab_items *items, int i, wchar_t *text, int cap, wchar_t *detail,
                            int detail_cap) {
  int count = 0;
  const char *uid;
  if (items->albums) {
    const mc_album_info *al = &items->albums->items[i];
    uid = al->uid;
    if (al->artist) mc_wformat(text, (size_t)cap, mc_str(MC_STR_TRACK_BYLINE), MC_S(al->artist), MC_S(al->title));
    else widen(al->title, text, cap);
    if (al->year > 0) {
      size_t len = wcslen(text);
      mc_wformat(text + len, (size_t)cap - len, mc_str(MC_STR_ALBUM_YEAR_SUFFIX), MC_D(al->year));
    }
  } else if (items->artists) {
    uid = items->artists->items[i].uid;
    widen(items->artists->items[i].name, text, cap);
    count = items->artists->items[i].album_count;
  } else if (items->playlists) {
    uid = items->playlists->items[i].uid;
    widen(items->playlists->items[i].name, text, cap);
  } else {
    uid = items->groups->items[i].uid;
    widen(items->groups->items[i].name, text, cap);
    count = items->groups->items[i].album_count;
  }
  detail[0] = L'\0';
  if (set_has(&g_dv.removed_items[g_dv.tab], uid))
    swprintf(detail, (size_t)detail_cap, L"%ls", mc_wstr(MC_STR_PROFILE_ITEM_REMOVED));
  else if (count > 0)
    mc_wformat(detail, (size_t)detail_cap, mc_str_plural(MC_STR_COUNT_ALBUMS, count), MC_D(count));
  return uid;
}

/* Fills the list with the current tab's items. For a reload of the same tab (keep), the rows
 * are updated in place if the items are the same ones, else rebuilt at the same scroll
 * position. */
static void fill_list(BOOL keep) {
  keep = keep && g_dv.rows_tab == g_dv.tab;
  tab_items items;
  int n;
  g_dv.rows_status = list_items(g_dv.tab, &items, &n);
  uid_str *rows = n ? malloc((size_t)n * sizeof *rows) : NULL;
  if (!rows) n = 0;
  wchar_t text[512], detail[64];
  for (int i = 0; i < n; i++) {
    memcpy(rows[i], item_row(&items, i, text, 512, detail, 64), MC_UID_STRLEN);
    rows[i][MC_UID_STRLEN - 1] = '\0';
  }
  BOOL same = keep && n == g_dv.nrows && (!n || !memcmp(rows, g_dv.rows, (size_t)n * sizeof *rows));
  int top = keep ? ListView_GetTopIndex(g_dv.list) : 0;

  g_dv.filling = TRUE;
  SendMessageW(g_dv.list, WM_SETREDRAW, FALSE, 0);
  if (!same) {
    ListView_DeleteAllItems(g_dv.list);
    SendMessageW(g_dv.list, LVM_SETITEMCOUNT, (WPARAM)n, 0);
  }
  for (int i = 0; i < n; i++) {
    item_row(&items, i, text, 512, detail, 64);
    BOOL ticked = row_ticked(g_dv.tab, rows[i]);
    if (same) {
      LVITEMW item = {.iSubItem = 0, .pszText = text};
      SendMessageW(g_dv.list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&item);
      set_check(i, ticked);
    } else {
      LVITEMW item = {.mask = LVIF_TEXT | LVIF_STATE, .iItem = i, .pszText = text,
                      .state = INDEXTOSTATEIMAGEMASK(ticked ? 2 : 1),
                      .stateMask = LVIS_STATEIMAGEMASK};
      SendMessageW(g_dv.list, LVM_INSERTITEMW, 0, (LPARAM)&item);
      set_check(i, ticked); /* a checkbox list may insert it unchecked anyway */
    }
    LVITEMW sub = {.iSubItem = 1, .pszText = detail};
    SendMessageW(g_dv.list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&sub);
  }
  free_items(&items);
  free(g_dv.rows);
  g_dv.rows = rows;
  g_dv.nrows = n;
  g_dv.rows_tab = g_dv.tab;

  if (!same && top > 0 && top < n) {
    RECT rc;
    if (ListView_GetItemRect(g_dv.list, 0, &rc, LVIR_BOUNDS))
      ListView_Scroll(g_dv.list, 0, top * (rc.bottom - rc.top));
  }
  SendMessageW(g_dv.list, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g_dv.list, NULL, TRUE);
  g_dv.filling = FALSE;
  update_select_buttons();
}

/* Reloads everything, keeping the pending edits. keep: it's the same sync, so the list keeps
 * its place and a focused edit its text. */
static void reload(BOOL keep) {
  load_devices();
  load_profile();
  update_controls(!keep);
  fill_list(keep);
  layout_again();
}

/* --- Size estimate ---
 * mc_sync_estimate_profile() reads every track (about half a second for 50,000), so it runs
 * on a thread, for the profile as shown (pending edits included), once the edits pause.
 * Like sync's own callbacks, the thread only calls in while the mc_sync exists: it is waited
 * for before the page lets go of it (device_view_open() with another, or NULL before sync
 * stops). */

typedef struct estimate_job {
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  mc_sync_profile profile; /* a copy: no name, its own items */
  unsigned seq;
  HWND notify;
  mc_status status;
  mc_sync_estimate est;
  BOOL confirm; /* for the archive confirmation, rather than the estimate line */
} estimate_job;

static void estimate_job_free(estimate_job *job) {
  if (!job) return;
  free(job->profile.items);
  free(job);
}

static DWORD WINAPI estimate_main(void *arg) {
  estimate_job *job = arg;
  job->status = mc_sync_estimate_profile(job->sync, job->uid, &job->profile, &job->est);
  if (!PostMessageW(job->notify, WM_ESTIMATE_DONE, 0, (LPARAM)job)) estimate_job_free(job);
  return 0;
}

static void set_estimate(const wchar_t *text, BOOL over) {
  g_dv.est.over = over;
  SetWindowTextW(g_dv.estimate, text ? text : L"");
  InvalidateRect(g_dv.estimate, NULL, TRUE);
  layout_again();
}

/* The estimate line: the size and, for Auto, what the budget picked; or how much too big. */
static void show_estimate(const estimate_job *job) {
  const mc_sync_estimate *e = &job->est;
  wchar_t text[512] = L"", a[32], b[32];
  BOOL over = job->status == MC_OK && e->over_budget;
  if (over) {
    /* Free space, when it is less than the budget, is what it had to fit in. */
    int64_t budget = job->profile.budget_bytes;
    BOOL by_free = e->free_bytes >= 0 && (budget <= 0 || e->limit_bytes < budget);
    mc_wformat(text, 256, mc_str(by_free ? MC_STR_ESTIMATE_OVER_FREE : MC_STR_ESTIMATE_OVER),
               MC_S(u8(wstr_size(a, 32, e->shortfall_bytes))), MC_S(u8(wstr_size(b, 32, e->limit_bytes))));
  } else if (job->status == MC_OK) {
    enum mc_str_id id = MC_STR_ESTIMATE_SIZE;
    if (job->profile.format.format == MC_FORMAT_AUTO && !e->archive)
      id = e->kbps ? MC_STR_ESTIMATE_AUTO_OPUS : MC_STR_ESTIMATE_AUTO_ORIGINAL;
    mc_wformat(text, 256, mc_str_plural(id, e->tracks), MC_S(u8(wstr_size(a, 32, e->total_bytes))),
               MC_S(u8(wstr_count(b, 32, e->tracks))), MC_D(e->kbps));
  } else if (job->status != MC_ERR_LIBRARY_MISMATCH && job->status != MC_ERR_NOT_FOUND) {
    /* Another library open, or the device gone: the page says so already. */
    mc_wformat(text, 256, mc_str(MC_STR_ESTIMATE_FAILED), MC_S(mc_status_str(job->status)));
  }
  if (job->status == MC_OK && e->imports_dropped > 0) {
    size_t len = wcslen(text);
    swprintf(text + len, 512 - len, L"\n");
    len = wcslen(text);
    mc_wformat(text + len, 512 - len, mc_str_plural(MC_STR_WINDOWS_IMPORTS_DROPPED_PC, e->imports_dropped),
               MC_D(e->imports_dropped));
  }
  set_estimate(text, over);
}

/* A job for p on the shown device; NULL if out of memory. */
static estimate_job *estimate_job_new(const mc_sync_profile *p) {
  estimate_job *job = calloc(1, sizeof *job);
  if (!job) return NULL;
  job->sync = g_dv.sync;
  memcpy(job->uid, g_dv.uid, MC_UID_STRLEN);
  job->profile = *p;
  job->profile.name = NULL;
  job->profile.items = NULL;
  job->profile.removed = NULL;
  job->profile.removed_count = 0;
  if (p->count > 0) {
    job->profile.items = malloc((size_t)p->count * sizeof *p->items);
    if (!job->profile.items) {
      free(job);
      return NULL;
    }
    memcpy(job->profile.items, p->items, (size_t)p->count * sizeof *p->items);
  }
  job->notify = g_dv.estimate;
  return job;
}

static void estimate_start(void) {
  if (g_dv.est.thread) {
    g_dv.est.again = TRUE;
    return;
  }
  g_dv.est.again = FALSE;
  const mc_sync_profile *p = g_dv.profile;
  if (!g_dv.sync || !p || !g_dv.found || g_dv.removed) {
    set_estimate(NULL, FALSE);
    return;
  }
  estimate_job *job = estimate_job_new(p);
  if (!job) return;
  job->seq = ++g_dv.est.seq;
  g_dv.est.thread = CreateThread(NULL, 0, estimate_main, job, 0, NULL);
  if (!g_dv.est.thread) estimate_job_free(job);
}

static void CALLBACK estimate_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)msg, (void)time;
  KillTimer(wnd, id);
  estimate_start();
}

/* Works the estimate out again shortly, for the profile as it is then. */
static void estimate_request(void) {
  if (g_dv.parent) SetTimer(g_dv.parent, ID_DEV_ESTIMATE_TIMER, ESTIMATE_DELAY_MS, estimate_timer);
}

static void confirm_archive_estimated(const estimate_job *job);

static void estimate_done(estimate_job *job) {
  if (job->confirm) { /* the archive confirmation's; an older one's thread is gone already */
    if (job->seq == g_dv.est.archive_seq && g_dv.est.archive) {
      WaitForSingleObject(g_dv.est.archive, INFINITE); /* it has posted: about to return */
      CloseHandle(g_dv.est.archive);
      g_dv.est.archive = NULL;
    }
    confirm_archive_estimated(job);
    estimate_job_free(job);
    return;
  }
  if (g_dv.est.thread) {
    WaitForSingleObject(g_dv.est.thread, INFINITE); /* it has posted: about to return */
    CloseHandle(g_dv.est.thread);
    g_dv.est.thread = NULL;
  }
  if (job->seq == g_dv.est.seq && job->sync == g_dv.sync && !strcmp(job->uid, g_dv.uid)) show_estimate(job);
  estimate_job_free(job);
  if (g_dv.est.again) estimate_start();
}

/* --- Select All and Select None --- */

/* One press: a long list takes seconds to store, and the window has to stay alive for it. */
typedef struct select_job {
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  mc_profile_item_kind kind;
  char (*rows)[MC_UID_STRLEN];
  int count, on;
  mc_status status;
  HWND notify;
} select_job;

static void select_job_free(select_job *job) {
  if (!job) return;
  free(job->rows);
  free(job);
}

static DWORD WINAPI select_main(void *arg) {
  select_job *job = arg;
  job->status = mc_sync_select_items(job->sync, job->uid, job->kind, (const char(*)[MC_UID_STRLEN])job->rows,
                                     job->count, job->on);
  if (!PostMessageW(job->notify, WM_SELECT_DONE, 0, (LPARAM)job)) select_job_free(job);
  return 0;
}

/* Lets a press being stored finish: what's edited next is read after it. */
static void select_wait(void) {
  if (!g_dv.select_thread) return;
  WaitForSingleObject(g_dv.select_thread, INFINITE);
  CloseHandle(g_dv.select_thread);
  g_dv.select_thread = NULL;
}

/* Waits for the threads and drops their results, and anything queued: before sync goes. */
static void estimate_cancel(void) {
  KillTimer(g_dv.parent, ID_DEV_ESTIMATE_TIMER);
  select_wait(); /* it uses sync too */
  if (g_dv.est.thread) {
    WaitForSingleObject(g_dv.est.thread, INFINITE);
    CloseHandle(g_dv.est.thread);
    g_dv.est.thread = NULL;
  }
  if (g_dv.est.archive) {
    WaitForSingleObject(g_dv.est.archive, INFINITE);
    CloseHandle(g_dv.est.archive);
    g_dv.est.archive = NULL;
  }
  MSG msg;
  while (g_dv.estimate && PeekMessageW(&msg, g_dv.estimate, WM_ESTIMATE_DONE, WM_ESTIMATE_DONE, PM_REMOVE))
    estimate_job_free((estimate_job *)msg.lParam);
  while (g_dv.estimate && PeekMessageW(&msg, g_dv.estimate, WM_SELECT_DONE, WM_SELECT_DONE, PM_REMOVE))
    select_job_free((select_job *)msg.lParam);
  g_dv.est.again = FALSE;
}

static void show_stored(mc_status st);

static LRESULT CALLBACK estimate_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)id, (void)data;
  if (msg == WM_ESTIMATE_DONE) {
    estimate_done((estimate_job *)lp);
    return 0;
  }
  if (msg == WM_SELECT_DONE) {
    select_job *job = (select_job *)lp;
    select_wait();
    if (job->sync == g_dv.sync && !strcmp(job->uid, g_dv.uid)) show_stored(job->status);
    select_job_free(job);
    return 0;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* --- Applying edits --- */

/* Says how storing edits went, and shows what's stored now, which may include others' edits
 * made meanwhile. */
static void show_stored(mc_status st) {
  if (st == MC_ERR_LOCKED) {
    set_message(mc_wstr(MC_STR_PROFILE_LOCKED));
  } else if (st != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_PROFILE_SAVE_FAILED), MC_S(mc_status_str(st)));
    set_message(text);
  } else if (g_dv.sync) {
    mc_sync_notify_changed(g_dv.sync);
  }
  int removed =g_dv.rows_tab >= 0 ? g_dv.removed_items[g_dv.rows_tab].count : 0;
  load_profile();
  update_controls(FALSE);
  /* A row ticked by hand isn't removed any more, which its text says. */
  if (g_dv.rows_tab >= 0 && g_dv.removed_items[g_dv.rows_tab].count != removed) fill_list(TRUE);
  else update_checks();
  layout_again();
}

/* Applies the pending edits now: get, change, set. */
static void flush(void) {
  KillTimer(g_dv.parent, ID_DEV_APPLY_TIMER);
  select_wait();
  if (!pending_any() || !active()) {
    pending_clear();
    return;
  }
  mc_sync_profile *p = NULL;
  mc_status st;
  st = mc_sync_get_profile(g_dv.sync, g_dv.uid, &p);
  if (st == MC_OK) st = apply_pending(p);
  if (st == MC_OK) st = mc_sync_set_profile(g_dv.sync, g_dv.uid, p);
  mc_sync_profile_free(p);
  pending_clear();
  show_stored(st);
}

static void CALLBACK apply_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)wnd, (void)msg, (void)id, (void)time;
  flush();
}

static void schedule(void) {
  SetTimer(g_dv.parent, ID_DEV_APPLY_TIMER, APPLY_DELAY_MS, apply_timer);
  estimate_request(); /* for the edited profile, not waiting for it to be stored */
}

/* --- The group --- */

static void leave_group(void) {
  if (!member()) return;
  wchar_t text[1024];
  mc_wformat(text, 1024, mc_str(MC_STR_WINDOWS_LEAVE_GROUP_CONFIRM_OTHERS),
             MC_S(g_dv.others[0] ? u8(g_dv.others) : mc_str(MC_STR_OTHER_DEVICES)));
  if (!confirm_dialog(g_dv.parent, mc_wstr(MC_STR_LEAVE_GROUP), text, MB_ICONWARNING,
                      mc_wstr_mnemonic(MC_STR_LEAVE_GROUP), mc_wstr_mnemonic(MC_STR_CANCEL), TRUE) ||
      !member())
    return;
  flush();
  HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT)); /* blocks for a few seconds */
  mc_status st = mc_sync_leave_group(g_dv.sync);
  SetCursor(old);
  if (st == MC_OK) {
    if (g_dv.host.devices_changed) g_dv.host.devices_changed();
    return;
  }
  if (st == MC_ERR_LOCKED) swprintf(text, 1024, L"%ls", mc_wstr(MC_STR_WINDOWS_LEAVE_ONLY_ARCHIVE));
  else mc_wformat(text, 1024, mc_str(MC_STR_LEAVE_GROUP_FAILED), MC_S(mc_status_str(st)));
  set_message(text);
  layout_again();
}

/* Removes member uid, called name, once confirmed. */
static void remove_member(const char *uid, const wchar_t *name) {
  wchar_t text[1024], title[256];
  mc_wformat(title, 256, mc_str(MC_STR_REMOVE_DEVICE_TITLE), MC_S(u8(name)));
  swprintf(text, 1024, L"%ls\n\n%ls", title, mc_wstr(MC_STR_REMOVE_DEVICE_TEXT));
  if (!confirm_dialog(g_dv.parent, mc_wstr(MC_STR_REMOVE_FROM_GROUP), text, MB_ICONWARNING,
                      mc_wstr_mnemonic(MC_STR_REMOVE_FROM_GROUP), mc_wstr_mnemonic(MC_STR_CANCEL), TRUE) ||
      !g_dv.sync)
    return;
  mc_status st = mc_sync_remove_device(g_dv.sync, uid);
  if (st == MC_OK) {
    if (g_dv.host.devices_changed) g_dv.host.devices_changed();
    return;
  }
  if (st == MC_ERR_LOCKED) mc_wformat(text, 1024, mc_str(MC_STR_REMOVE_ONLY_ARCHIVE), MC_S(u8(name)));
  else mc_wformat(text, 1024, mc_str(MC_STR_REMOVE_DEVICE_FAILED), MC_S(mc_status_str(st)));
  set_message(text);
  layout_again();
}

/* How the last session with uid went, if it didn't simply succeed: text "" forgets it. */
static void set_result(const char *uid, const wchar_t *text) {
  int i = 0;
  while (i < g_dv.nresults && strcmp(g_dv.results[i].uid, uid)) i++;
  if (!text[0]) {
    if (i < g_dv.nresults) g_dv.results[i] = g_dv.results[--g_dv.nresults];
    return;
  }
  if (i == g_dv.nresults) {
    if (i == MAX_ROWS) return;
    g_dv.nresults++;
  }
  snprintf(g_dv.results[i].uid, sizeof g_dv.results[i].uid, "%s", uid);
  swprintf(g_dv.results[i].text, sizeof g_dv.results[i].text / sizeof *g_dv.results[i].text, L"%ls", text);
}

/* Adds line, which may be UTF-8 (utf8) or wide, as a line of text; nothing for "". */
static void add_line(wchar_t *text, size_t cap, const wchar_t *line, const char *utf8) {
  wchar_t wide[1024];
  if (utf8) line = wstr_from_utf8(wide, 1024, utf8);
  if (!line || !line[0]) return;
  size_t used = wcslen(text);
  if (used + 1 < cap) swprintf(text + used, cap - used, L"%ls%ls", used ? L"\n" : L"", line);
}

/* "Original files", "Opus 128 kbps" or "Auto", into buf. */
static const wchar_t *format_name(const mc_transcode_opts *f, wchar_t *buf, int cap) {
  if (f->format == MC_FORMAT_AUTO) return mc_wstr(MC_STR_FORMAT_AUTO);
  if (f->format == MC_FORMAT_ORIGINAL) return mc_wstr(MC_STR_FORMAT_ORIGINAL);
  mc_wformat(buf, (size_t)cap, mc_str(MC_STR_FORMAT_OPUS), MC_D(f->bitrate_kbps));
  return buf;
}

/* A member's details, as its page once showed them: how it's doing, its last session here,
 * its files and its profile, how the two devices reach each other and where it was seen. */
static void member_details(const mc_sync_device *d, wchar_t *text, size_t cap) {
  wchar_t line[1024], ago[64], value[256];
  char buf[1024];
  if (d->last_sync_ms > 0) fmt_ago(d->last_sync_ms, ago, 64);
  if (d->online && d->last_sync_ms > 0) mc_wformat(line, 1024, mc_str(MC_STR_DEVICE_ONLINE_SYNCED), MC_S(u8(ago)));
  else if (d->online) swprintf(line, 1024, L"%ls", mc_wstr(MC_STR_DEVICE_ONLINE));
  else if (d->last_sync_ms > 0) mc_wformat(line, 1024, mc_str(MC_STR_DEVICE_OFFLINE_SYNCED), MC_S(u8(ago)));
  else swprintf(line, 1024, L"%ls", mc_wstr(MC_STR_DEVICE_NOT_SYNCED));
  buf[0] = '\0';
  mc_sync_device_note(d, 0, 1, buf, sizeof buf); /* when it next syncs by itself, or what holds it back */
  if (buf[0]) {
    size_t used = wcslen(line);
    swprintf(line + used, 1024 - used, L"%ls%ls", mc_wstr(MC_STR_SEPARATOR_DOT), wstr_from_utf8(value, 256, buf));
  }
  add_line(text, cap, line, NULL);
  buf[0] = '\0';
  mc_sync_device_warning(g_dv.sync, d, 0, buf, sizeof buf); /* its version first */
  add_line(text, cap, NULL, buf);
  for (int i = 0; i < g_dv.nresults; i++)
    if (!strcmp(g_dv.results[i].uid, d->uid)) add_line(text, cap, g_dv.results[i].text, NULL);
  files_text(d, line, 1024);
  add_line(text, cap, line, NULL);

  mc_sync_profile *p = NULL;
  if (mc_sync_get_profile(g_dv.sync, d->uid, &p) == MC_OK) {
    add_line(text, cap, L" ", NULL); /* a blank line before the profile */
    buf[0] = '\0';
    mc_sync_profile_summary(p, buf, sizeof buf);
    add_line(text, cap, NULL, buf);
    if (p->archive) {
      add_line(text, cap, mc_wstr(MC_STR_DEVICE_ARCHIVE_NOTE), NULL);
    } else {
      mc_wformat(line, 1024, mc_str(MC_STR_WINDOWS_SYNC_DETAIL), MC_S(mc_str(MC_STR_SYNC_FORMAT)),
                 MC_S(u8(format_name(&p->format, value, 256))));
      add_line(text, cap, line, NULL);
      if (p->budget_enabled && p->budget_bytes > 0) {
        mc_wformat(line, 1024, mc_str(MC_STR_WINDOWS_SYNC_BUDGET), MC_S(u8(wstr_size(value, 256, p->budget_bytes))));
        add_line(text, cap, line, NULL);
      }
    }
    mc_sync_profile_free(p);
  }

  add_line(text, cap, L" ", NULL); /* and before how they connect */
  buf[0] = '\0';
  mc_sync_reach_text(g_dv.sync, d->uid, d->name, 0, buf, sizeof buf);
  add_line(text, cap, NULL, buf);
  if (d->address && *d->address) mc_wformat(value, 256, mc_str(MC_STR_DEVICE_ADDRESS_PORT), MC_S(d->address), MC_D(d->port));
  else swprintf(value, 256, L"%ls", mc_wstr(MC_STR_DEVICE_ADDRESS_UNKNOWN));
  mc_wformat(line, 1024, mc_str(MC_STR_WINDOWS_SYNC_DETAIL), MC_S(mc_str(MC_STR_DEVICE_ADDRESS)), MC_S(u8(value)));
  add_line(text, cap, line, NULL);
}

/* A member's row clicked: its details and profile, read-only, with Details for its issues,
 * and Remove from group unless it's an archive, which the group can't do without. */
static void show_member(const char *row_uid) {
  mc_sync *sync = g_dv.sync;
  mc_sync_device_list *list = NULL;
  if (!sync || mc_sync_list_devices(sync, &list) != MC_OK || !list) return;
  char uid[MC_UID_STRLEN]; /* the row's goes if the devices refresh while the dialog is open */
  snprintf(uid, sizeof uid, "%s", row_uid);
  wchar_t name[MC_SYNC_DEVICE_NAME_MAX + 1], text[4096] = L"";
  BOOL found = FALSE, archive = FALSE;
  for (int i = 0; i < list->count && !found; i++) {
    const mc_sync_device *d = &list->items[i];
    if (strcmp(d->uid, uid) || d->removed) continue;
    found = TRUE;
    archive = d->archive;
    widen(d->name && *d->name ? d->name : mc_str(MC_STR_DEVICE_UNNAMED), name, MC_SYNC_DEVICE_NAME_MAX + 1);
    member_details(d, text, sizeof text / sizeof *text);
  }
  mc_sync_device_list_free(list);
  if (!found) return;
  BOOL issues = issue_dialog_has_issues(sync, uid, NULL);

  const wchar_t *close = mc_wstr_mnemonic(MC_STR_CLOSE), *details = mc_wstr_mnemonic(MC_STR_SYNC_ISSUES_DETAILS);
  prompt_dialog ask = {.caption = mc_wstr(MC_STR_APP_NAME), .heading = name, .text = text};
  ask.ok = !archive ? mc_wstr_mnemonic(MC_STR_REMOVE_FROM_GROUP_BUTTON) : issues ? details : close;
  ask.other = !archive && issues ? details : NULL;
  ask.cancel = ask.ok == close ? NULL : close;
  ask.cancel_default = ask.cancel != NULL;
  INT_PTR answer = prompt_dialog_show(GetAncestor(g_dv.parent, GA_ROOT), &ask);
  if (sync != g_dv.sync) return; /* sync stopped meanwhile */
  if (answer == IDOK && !archive) {
    remove_member(uid, name);
  } else if ((answer == IDOK && issues) || answer == IDNO) {
    if (issue_dialog_show(GetAncestor(g_dv.parent, GA_ROOT), g_dv.lib, sync, uid) && g_dv.host.library_changed)
      g_dv.host.library_changed();
    device_view_issues_changed();
  }
}

/* --- Syncing --- */

static void sync_now(void) {
  if (!g_dv.sync) return;
  flush();
  /* Also chooses this PC's profile as it stands, if it wasn't. */
  mc_status st = mc_sync_now(g_dv.sync, NULL);
  wchar_t text[256] = L"";
  if (st != MC_OK) mc_wformat(text, 256, mc_str(MC_STR_SYNC_START_FAILED), MC_S(mc_status_str(st)));
  set_message(text);
  if (st == MC_OK) reload(TRUE); /* chosen now: the notice goes */
  layout_again();
}

/* Ends the sessions under way, which on_sync_done then reports as cancelled. */
static void cancel_sync(void) {
  if (g_dv.sync) mc_sync_cancel(g_dv.sync, NULL);
}

/* --- Archive --- */

static void set_archive(BOOL archive) {
  if (!g_dv.sync || !g_dv.found) return;
  flush();
  mc_status st = mc_sync_set_archive(g_dv.sync, g_dv.uid, archive);
  wchar_t text[256] = L"";
  if (st == MC_OK) mc_sync_notify_changed(g_dv.sync);
  else if (st == MC_ERR_LOCKED && !archive)
    swprintf(text, 256, L"%ls", mc_wstr(MC_STR_WINDOWS_PC_ARCHIVE_LOCKED));
  else mc_wformat(text, 256, mc_str(MC_STR_PROFILE_ARCHIVE_FAILED), MC_S(mc_status_str(st)));
  set_message(text);
  reload(TRUE);
}

/* The confirmation's text, with the whole library's size once it's known. */
static void confirm_archive_estimated(const estimate_job *job) {
  if (!g_dv.confirm_dlg || job->seq != g_dv.est.archive_seq || job->status != MC_OK) return;
  wchar_t size[32];
  mc_wformat(g_dv.confirm_text, 512, mc_str(MC_STR_MAKE_ARCHIVE_TEXT),
             MC_S(u8(wstr_size(size, 32, job->est.total_bytes))));
  prompt_dialog_set_text(g_dv.confirm_dlg, g_dv.confirm_text);
}

/* Asks first, saying how much the whole library comes to as originals, then makes it one. */
static void ask_make_archive(void) {
  if (!g_dv.sync || !g_dv.profile || g_dv.confirm_dlg) return;
  mc_sync *sync = g_dv.sync;
  char uid[MC_UID_STRLEN];
  memcpy(uid, g_dv.uid, MC_UID_STRLEN);
  wchar_t title[256];
  mc_wformat(title, 256, mc_str(MC_STR_MAKE_ARCHIVE_TITLE), MC_S(u8(g_dv.name)));
  swprintf(g_dv.confirm_text, 512, L"%ls", mc_wstr(MC_STR_MAKE_ARCHIVE_TEXT_UNKNOWN));

  /* The size: the profile as an archive, on a thread that posts to the estimate line as the
   * estimate's own does. One asked a moment ago finishes first. */
  if (g_dv.est.archive) {
    WaitForSingleObject(g_dv.est.archive, INFINITE);
    CloseHandle(g_dv.est.archive);
    g_dv.est.archive = NULL;
  }
  estimate_job *job = estimate_job_new(g_dv.profile);
  if (job) {
    job->profile.archive = 1;
    job->confirm = TRUE;
    job->seq = ++g_dv.est.archive_seq;
    g_dv.est.archive = CreateThread(NULL, 0, estimate_main, job, 0, NULL);
    if (!g_dv.est.archive) estimate_job_free(job);
  }

  prompt_dialog ask = {.caption = mc_wstr(MC_STR_APP_NAME), .icon = MB_ICONQUESTION, .heading = title,
                       .text = g_dv.confirm_text, .ok = mc_wstr_mnemonic(MC_STR_MAKE_ARCHIVE),
                       .cancel = mc_wstr_mnemonic(MC_STR_CANCEL), .shown = &g_dv.confirm_dlg};
  INT_PTR answer = prompt_dialog_show(g_dv.parent, &ask);
  /* Still the device it was asked about. */
  if (answer == IDOK && sync == g_dv.sync && !strcmp(uid, g_dv.uid)) set_archive(TRUE);
}

/* The box shows what is until it's confirmed and stored. Only a device sets its own role;
 * stepping down isn't asked. */
static void on_archive(void) {
  if (!g_dv.profile || !IsWindowEnabled(g_dv.archive_box)) return;
  if (!g_dv.profile->archive) ask_make_archive();
  else set_archive(FALSE);
}

/* --- Edits from the controls --- */

static int64_t budget_from_edit(void) {
  wchar_t buf[16];
  GetWindowTextW(g_dv.budget_size, buf, 16);
  long gb = wcstol(buf, NULL, 10);
  return gb > 0 ? (int64_t)gb * GIB : 0;
}

/* The format and the budget exclude each other (the budget picks the format, as Auto), so
 * an edit of either is pending for both. */
static void pend_format_budget(void) {
  g_dv.pending.format = g_dv.profile->format;
  g_dv.pending.budget_enabled = g_dv.profile->budget_enabled;
  g_dv.pending.budget_bytes = g_dv.profile->budget_bytes;
  g_dv.pending.mask |= PEND_FORMAT | PEND_BUDGET;
  update_controls(FALSE);
  schedule();
}

/* Auto turns the budget on; any other format turns it off. */
static void on_format(void) {
  if (!g_dv.profile) return;
  int i = (int)SendMessageW(g_dv.format, CB_GETCURSEL, 0, 0);
  if (i < 0) return;
  int kbps = (int)SendMessageW(g_dv.format, CB_GETITEMDATA, (WPARAM)i, 0);
  mc_sync_profile *p = g_dv.profile;
  p->format.format = kbps == AUTO_ITEM ? MC_FORMAT_AUTO : kbps ? MC_FORMAT_OPUS : MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = kbps == AUTO_ITEM ? 0 : kbps;
  p->budget_enabled = kbps == AUTO_ITEM;
  if (p->budget_enabled && p->budget_bytes <= 0) {
    int64_t bytes = budget_from_edit();
    p->budget_bytes = bytes ? bytes : DEFAULT_BUDGET_GB * GIB;
  }
  pend_format_budget();
}

/* A budget sets the format to Auto; none leaves it at originals if it was Auto. */
static void on_budget(void) {
  if (!g_dv.profile) return;
  int64_t bytes = budget_from_edit();
  if (!bytes) bytes = DEFAULT_BUDGET_GB * GIB;
  mc_sync_profile *p = g_dv.profile;
  p->budget_enabled = is_checked(g_dv.budget);
  p->budget_bytes = bytes;
  if (p->budget_enabled) p->format.format = MC_FORMAT_AUTO;
  else if (p->format.format == MC_FORMAT_AUTO) p->format.format = MC_FORMAT_ORIGINAL;
  if (p->format.format != MC_FORMAT_OPUS) p->format.bitrate_kbps = 0;
  pend_format_budget();
}

static void select_tab(int tab);

static void on_sync_all(void) {
  if (!g_dv.profile) return;
  g_dv.profile->sync_all = g_dv.pending.sync_all = is_checked(g_dv.sync_all);
  g_dv.pending.mask |= PEND_SYNC_ALL;
  /* Sync everything lists albums and artists: playlists and groups would select nothing more. */
  if (g_dv.profile->sync_all && g_dv.tab >= TAB_PLAYLISTS) select_tab(TAB_ALBUMS);
  update_checks();
  layout_again();
  schedule();
}

static void on_check(int row, BOOL on) {
  if (row < 0 || row >= g_dv.nrows || g_dv.rows_tab < 0) return;
  if (g_dv.profile && g_dv.profile->sync_all) {
    /* Sync everything keeps its exclusions apart from the selection, so a tick is one write. */
    uid_str uid;
    memcpy(uid, g_dv.rows[row], MC_UID_STRLEN);
    flush();
    show_stored(mc_sync_exclude_items(g_dv.sync, g_dv.uid, k_tab_kind[g_dv.rows_tab],
                                      (const char(*)[MC_UID_STRLEN])&uid, 1, !on));
    return;
  }
  set_put(&g_dv.sel[g_dv.rows_tab], g_dv.rows[row], on);
  pending_toggle(k_tab_kind[g_dv.rows_tab], g_dv.rows[row], on);
  update_count();
  update_select_buttons();
  schedule();
}

/* Select All and Select None: the rows shown, stored at once as one write rather than waiting
 * as a toggle each, which a long list couldn't afford, and on a thread of its own. Edits made
 * before them go first. A press while one is being stored does nothing. */
static void select_shown(BOOL on) {
  if (g_dv.rows_tab < 0 || !g_dv.nrows || !editable() || g_dv.select_thread) return;
  flush();
  select_job *job = calloc(1, sizeof *job);
  if (job) job->rows = malloc((size_t)g_dv.nrows * sizeof *job->rows);
  if (!job || !job->rows) {
    select_job_free(job);
    show_stored(MC_ERR_NOMEM);
    return;
  }
  memcpy(job->rows, g_dv.rows, (size_t)g_dv.nrows * sizeof *job->rows);
  job->sync = g_dv.sync;
  memcpy(job->uid, g_dv.uid, MC_UID_STRLEN);
  job->kind = k_tab_kind[g_dv.rows_tab];
  job->count = g_dv.nrows;
  job->on = on;
  job->notify = g_dv.estimate;
  g_dv.select_thread = CreateThread(NULL, 0, select_main, job, 0, NULL);
  if (!g_dv.select_thread) {
    select_job_free(job);
    show_stored(MC_ERR_NOMEM);
  }
}

static void select_tab(int tab) {
  if (tab == g_dv.tab) return;
  g_dv.tab = tab;
  fill_list(FALSE);
  for (int i = 0; i < NUM_TABS; i++) InvalidateRect(g_dv.tabs[i], NULL, FALSE);
}

/* --- Subclasses --- */

/* Enter in the budget's edit applies it straight away. */
static LRESULT CALLBACK edit_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        flush();
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r') return 0; /* no beep */
      break;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, edit_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* Our own checkbox images, drawn in the theme's colours: the stock ones stay light in dark
 * mode. Made again for each theme and DPI. */
static void set_check_images(void) {
  if (!g_dv.list) return;
  UINT dpi = g_dv.dpi ? g_dv.dpi : USER_DEFAULT_SCREEN_DPI;
  HIMAGELIST il = theme_checkboxes(MulDiv(16, (int)dpi, USER_DEFAULT_SCREEN_DPI));
  if (!il) return;
  HIMAGELIST old = ListView_SetImageList(g_dv.list, il, LVSIL_STATE);
  if (old && old == g_dv.checks) ImageList_Destroy(old);
  g_dv.checks = il;
}

/* A theme change makes the list remake the stock checkboxes, so ours go back on after it
 * (once the new colours are set too). */
static LRESULT CALLBACK list_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_THEMECHANGED:
      PostMessageW(wnd, WM_RECHECK, 0, 0);
      break;
    case WM_RECHECK:
      set_check_images();
      update_checks();
      InvalidateRect(wnd, NULL, TRUE);
      return 0;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, list_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* Hover for the owner-drawn buttons (the segmented ones and the devices' rows), which don't
 * track it themselves. */
static LRESULT CALLBACK hover_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_MOUSEMOVE:
      if (g_dv.hot != wnd) {
        if (g_dv.hot) InvalidateRect(g_dv.hot, NULL, FALSE);
        g_dv.hot = wnd;
        InvalidateRect(wnd, NULL, FALSE);
        TRACKMOUSEEVENT tme = {.cbSize = sizeof tme, .dwFlags = TME_LEAVE, .hwndTrack = wnd};
        TrackMouseEvent(&tme);
      }
      break;
    case WM_MOUSELEAVE:
      if (g_dv.hot == wnd) g_dv.hot = NULL;
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_NCDESTROY:
      if (g_dv.hot == wnd) g_dv.hot = NULL;
      RemoveWindowSubclass(wnd, hover_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* --- Window plumbing --- */

static int scale(int px) {
  return MulDiv(px, (int)(g_dv.dpi ? g_dv.dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

static int busy_index(const char *uid) {
  for (int i = 0; i < g_dv.nbusy; i++)
    if (!strcmp(g_dv.busy[i], uid)) return i;
  return -1;
}

static void left_scrolled(void) {
  SetWindowPos(g_dv.left.content, NULL, 0, -g_dv.left.scroll.pos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

static void right_scrolled(void) {
  SetWindowPos(g_dv.right.content, NULL, 0, -g_dv.right.scroll.pos, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/* The line under the strip and the one between the panels. */
static void paint_lines(HDC dc) {
  if (!g_dv.line_y) return;
  SetDCBrushColor(dc, theme.line);
  HBRUSH brush = GetStockObject(DC_BRUSH);
  RECT under = {0, g_dv.line_y, g_dv.line_w, g_dv.line_y + 1};
  RECT between = {g_dv.divider_x, g_dv.line_y + 1, g_dv.divider_x + 1, g_dv.divider_bottom};
  FillRect(dc, &under, brush);
  FillRect(dc, &between, brush);
}

/* The page and the panels' contents: their controls' messages go on to the main window, as
 * if it were their parent. */
static LRESULT CALLBACK forward_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_DRAWITEM:
    case WM_MEASUREITEM:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORSTATIC:
      return SendMessageW(g_dv.parent, msg, wp, lp);
    case WM_ERASEBKGND: /* also how themed controls draw their parent's background */
    case WM_PRINTCLIENT: {
      RECT client;
      GetClientRect(wnd, &client);
      FillRect((HDC)wp, &client, theme.bg_brush);
      if (wnd == g_dv.page) paint_lines((HDC)wp);
      return 1;
    }
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static panel *panel_of(HWND view) {
  return view == g_dv.left.view ? &g_dv.left : view == g_dv.right.view ? &g_dv.right : NULL;
}

/* A panel's view: its scrollbar. The wheel comes through device_view_translate(). */
static LRESULT CALLBACK panel_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  panel *p = panel_of(wnd);
  if (!p) return DefWindowProcW(wnd, msg, wp, lp); /* before CreateWindowExW returns */
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      RECT client;
      GetClientRect(wnd, &client);
      if (canvas_reserve(&p->canvas, client.right, client.bottom)) {
        canvas_clear(&p->canvas, &ps.rcPaint, theme.bg, 255);
        scroller_paint(&p->scroll, &p->canvas);
        canvas_blit(&p->canvas, dc, ps.rcPaint.left, ps.rcPaint.top, &ps.rcPaint);
      }
      EndPaint(wnd, &ps);
      return 0;
    }
    case WM_TIMER:
      scroller_timer(&p->scroll, wp);
      return 0;
    case WM_MOUSEMOVE:
      if (!p->tracking) {
        TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, wnd, 0};
        p->tracking = TrackMouseEvent(&tme);
      }
      scroller_mouse_move(&p->scroll, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_MOUSELEAVE:
      p->tracking = FALSE;
      scroller_mouse_leave(&p->scroll);
      return 0;
    case WM_LBUTTONDOWN:
      scroller_mouse_down(&p->scroll, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      return 0;
    case WM_LBUTTONUP:
      scroller_mouse_up(&p->scroll);
      return 0;
    case WM_CAPTURECHANGED:
      scroller_capture_changed(&p->scroll);
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}

static HWND child(HWND parent, const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

/* An edit as in Settings: a themed border, the text centred in it. */
static HWND edit_child(HWND parent, DWORD style, int id) {
  HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, NULL, WS_CHILD | WS_TABSTOP | style, 0, 0, 0, 0, parent,
                              (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
  theme_center_edit(edit);
  return edit;
}

static void firewall_changed(void) {
  if (firewall_banner_update()) layout_again();
}

void device_view_create(HWND parent, const device_view_host *host) {
  g_dv.parent = parent;
  if (host) g_dv.host = *host;
  g_dv.rows_tab = -1;
  HINSTANCE inst = GetModuleHandleW(NULL);
  WNDCLASSEXW wc = {.cbSize = sizeof wc, .lpfnWndProc = forward_proc, .hInstance = inst,
                    .hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), .lpszClassName = L"kithsong_sync_content"};
  RegisterClassExW(&wc);
  wc.lpfnWndProc = panel_proc;
  wc.lpszClassName = L"kithsong_sync_panel";
  RegisterClassExW(&wc);
  /* Tab goes through all of them (device_view_translate()), in the order they're made. */
  g_dv.page = CreateWindowExW(WS_EX_CONTROLPARENT, L"kithsong_sync_content", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0,
                              0, 0, parent, NULL, inst, NULL);
  HWND page = g_dv.page;
  DWORD label = SS_LEFT | SS_NOPREFIX, line = label | SS_ENDELLIPSIS;
  firewall_banner_create(page, ID_DEV_FIREWALL);
  firewall_set_listener(firewall_changed);
  g_dv.message = child(page, WC_STATICW, NULL, label, ID_DEV_MESSAGE);
  g_dv.notice = child(page, WC_STATICW, NULL, label, ID_DEV_NOTICE);
  g_dv.issues =
    child(page, WC_BUTTONW, mc_wstr_mnemonic(MC_STR_SYNC_ISSUES_DETAILS), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_ISSUES);
  g_dv.progress = child(page, PROGRESS_CLASSW, NULL, 0, ID_DEV_PROGRESS);
  SendMessageW(g_dv.progress, PBM_SETRANGE32, 0, PROGRESS_MAX);
  g_dv.status = child(page, WC_STATICW, NULL, line, ID_DEV_STATUS);
  g_dv.sync_now = child(page, WC_BUTTONW, mc_wstr(MC_STR_SYNC_NOW), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_SYNC_NOW);

  panel *panels[] = {&g_dv.left, &g_dv.right};
  for (int i = 0; i < 2; i++) {
    panel *p = panels[i];
    p->view = CreateWindowExW(WS_EX_CONTROLPARENT, L"kithsong_sync_panel", NULL, WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0,
                              0, page, NULL, inst, NULL);
    p->content = CreateWindowExW(WS_EX_CONTROLPARENT, L"kithsong_sync_content", NULL,
                                 WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0, p->view, NULL, inst, NULL);
    p->scroll.wnd = p->view;
  }
  g_dv.left.scroll.moved = left_scrolled;
  g_dv.right.scroll.moved = right_scrolled;

  /* This PC's panel. */
  HWND left = g_dv.left.content;
  g_dv.title = child(left, WC_STATICW, NULL, line, ID_DEV_TITLE);
  g_dv.files = child(left, WC_STATICW, NULL, line, ID_DEV_FILES);
  g_dv.archive_label = child(left, WC_STATICW, mc_wstr(MC_STR_DEVICE_ARCHIVE_TITLE), line, ID_DEV_ARCHIVE_LABEL);
  /* Not an auto checkbox: it shows the stored flag, which changes only once confirmed. */
  g_dv.archive_box = child(left, WC_BUTTONW, NULL, BS_CHECKBOX | WS_TABSTOP, ID_DEV_ARCHIVE);
  g_dv.archive_text =
    child(left, WC_STATICW, mc_wstr(MC_STR_DEVICE_ARCHIVE_TEXT), line | SS_NOTIFY, ID_DEV_ARCHIVE_TEXT);

  g_dv.heading = child(left, WC_STATICW, mc_wstr(MC_STR_WHAT_TO_SYNC), line, ID_DEV_HEADING);
  g_dv.note = child(left, WC_STATICW, NULL, label, ID_DEV_NOTE);
  /* Checkboxes have no text of their own: a themed checkbox's text stays black in dark
   * mode, so each has a label beside it, which toggles it too. */
  g_dv.sync_all = child(left, WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_DEV_SYNC_ALL);
  g_dv.sync_all_label =
    child(left, WC_STATICW, mc_wstr(MC_STR_SYNC_EVERYTHING), line | SS_NOTIFY, ID_DEV_SYNC_ALL_LABEL);
  for (int t = 0; t < NUM_TABS; t++) {
    g_dv.tabs[t] = child(left, WC_BUTTONW, mc_wstr(k_tab_name[t]), BS_OWNERDRAW | WS_TABSTOP, ID_DEV_TAB + t);
    SetWindowSubclass(g_dv.tabs[t], hover_proc, 0, 0);
  }
  g_dv.select_all =
    child(left, WC_BUTTONW, mc_wstr(MC_STR_SELECT_ALL), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_SELECT_ALL);
  g_dv.select_none =
    child(left, WC_BUTTONW, mc_wstr(MC_STR_SELECT_NONE), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_SELECT_NONE);
  g_dv.list = child(left, WC_LISTVIEWW, NULL,
                    LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP, ID_DEV_LIST);
  SetWindowSubclass(g_dv.list, list_proc, 0, 0);
  LVCOLUMNW col = {.mask = LVCF_WIDTH | LVCF_FMT, .fmt = LVCFMT_LEFT, .cx = 300};
  SendMessageW(g_dv.list, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
  col.fmt = LVCFMT_RIGHT;
  col.cx = 100;
  SendMessageW(g_dv.list, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
  theme_apply_control(g_dv.list);
  ListView_SetExtendedListViewStyleEx(g_dv.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER,
                                      LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
  set_check_images();
  g_dv.count = child(left, WC_STATICW, NULL, line, ID_DEV_COUNT);

  g_dv.quality = child(left, WC_STATICW, mc_wstr(MC_STR_WINDOWS_PC_SYNC_QUALITY), line, ID_DEV_QUALITY);
  g_dv.format_label = child(left, WC_STATICW, mc_wstr(MC_STR_SYNC_FORMAT), line, ID_DEV_FORMAT_LABEL);
  g_dv.format = child(left, WC_COMBOBOXW, NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_DEV_FORMAT);
  int i = (int)SendMessageW(g_dv.format, CB_ADDSTRING, 0, (LPARAM)mc_wstr(MC_STR_FORMAT_AUTO));
  SendMessageW(g_dv.format, CB_SETITEMDATA, (WPARAM)i, AUTO_ITEM);
  i = (int)SendMessageW(g_dv.format, CB_ADDSTRING, 0, (LPARAM)mc_wstr(MC_STR_FORMAT_ORIGINAL));
  SendMessageW(g_dv.format, CB_SETITEMDATA, (WPARAM)i, 0);
  for (int b = 0; b < MC_OPUS_BITRATE_COUNT; b++) {
    wchar_t text[32];
    mc_wformat(text, 32, mc_str(MC_STR_FORMAT_OPUS), MC_D(mc_opus_bitrates[b]));
    i = (int)SendMessageW(g_dv.format, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(g_dv.format, CB_SETITEMDATA, (WPARAM)i, (LPARAM)mc_opus_bitrates[b]);
  }
  COMBOBOXINFO info = {.cbSize = sizeof info};
  if (GetComboBoxInfo(g_dv.format, &info)) g_dv.combo_list = info.hwndList;
  g_dv.budget = child(left, WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_DEV_BUDGET);
  g_dv.budget_label = child(left, WC_STATICW, mc_wstr(MC_STR_BUDGET_LIMIT), line | SS_NOTIFY, ID_DEV_BUDGET_LABEL);
  g_dv.budget_size = edit_child(left, ES_NUMBER | ES_RIGHT, ID_DEV_BUDGET_SIZE);
  SendMessageW(g_dv.budget_size, EM_LIMITTEXT, 5, 0);
  SetWindowSubclass(g_dv.budget_size, edit_proc, 0, 0);
  g_dv.budget_unit = child(left, WC_STATICW, mc_wstr(MC_STR_BUDGET_UNIT), line, ID_DEV_BUDGET_UNIT);
  g_dv.estimate = child(left, WC_STATICW, NULL, label, ID_DEV_ESTIMATE);
  SetWindowSubclass(g_dv.estimate, estimate_proc, 0, 0); /* receives the estimate's thread's results */

  /* The devices' panel. */
  HWND right = g_dv.right.content;
  g_dv.devices = child(right, WC_STATICW, mc_wstr(MC_STR_DEVICES), line, ID_DEV_DEVICES);
  for (int r = 0; r < MAX_ROWS; r++) {
    g_dv.dev_btns[r] = child(right, WC_BUTTONW, NULL, BS_OWNERDRAW | WS_TABSTOP, ID_DEV_ROW + r);
    SetWindowSubclass(g_dv.dev_btns[r], hover_proc, 0, 0);
  }
  g_dv.unpaired = child(right, WC_STATICW, NULL, line, ID_DEV_UNPAIRED);
  g_dv.none = child(right, WC_STATICW, mc_wstr(MC_STR_DEVICES_NONE), label, ID_DEV_NONE);
  g_dv.add = child(right, WC_BUTTONW, mc_wstr(MC_STR_ADD_DEVICE_BY_ADDRESS), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_ADD);
  g_dv.leave =
    child(right, WC_BUTTONW, mc_wstr_mnemonic(MC_STR_LEAVE_GROUP_BUTTON), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_LEAVE);
}

void device_view_destroy(void) {
  firewall_set_listener(NULL);
  firewall_watch(g_dv.parent, FALSE);
  estimate_cancel();
  KillTimer(g_dv.parent, ID_DEV_APPLY_TIMER);
  KillTimer(g_dv.parent, ID_DEV_FINISHED_TIMER);
  KillTimer(g_dv.parent, ID_DEV_REFRESH_TIMER);
  pending_clear();
  free(g_dv.pending.toggles);
  g_dv.pending.toggles = NULL;
  g_dv.pending.cap = 0;
  free_profile();
  for (int i = 0; i < NUM_TABS; i++) {
    free(g_dv.sel[i].uids);
    g_dv.sel[i] = (uid_set){0};
    free(g_dv.removed_items[i].uids);
    g_dv.removed_items[i] = (uid_set){0};
  }
  free(g_dv.rows);
  g_dv.rows = NULL;
  g_dv.nrows = 0;
  g_dv.sync = NULL;
  g_dv.lib = NULL;
  canvas_free(&g_dv.left.canvas);
  canvas_free(&g_dv.right.canvas);
  HFONT fonts[] = {g_dv.title_font, g_dv.heading_font, g_dv.icon_font, g_dv.small_icon_font};
  for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
    if (fonts[i]) DeleteObject(fonts[i]);
  g_dv.title_font = g_dv.heading_font = g_dv.icon_font = g_dv.small_icon_font = NULL;
}

void device_view_set_font(HFONT font, UINT dpi) {
  g_dv.font = font;
  g_dv.dpi = dpi;
  NONCLIENTMETRICSW ncm = {.cbSize = sizeof ncm};
  SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
  LOGFONTW lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 20, 9); /* 9pt -> 20pt, Windows 11's title size */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_dv.title_font) DeleteObject(g_dv.title_font);
  g_dv.title_font = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_NORMAL;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy(lf.lfFaceName, theme_icon_face(), LF_FACESIZE - 1);
  lf.lfFaceName[LF_FACESIZE - 1] = L'\0';
  if (g_dv.icon_font) DeleteObject(g_dv.icon_font);
  g_dv.icon_font = CreateFontIndirectW(&lf);
  lf.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, 12, 9); /* the rows' marks */
  if (g_dv.small_icon_font) DeleteObject(g_dv.small_icon_font);
  g_dv.small_icon_font = CreateFontIndirectW(&lf);
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 14, 9); /* subtitle */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_dv.heading_font) DeleteObject(g_dv.heading_font);
  g_dv.heading_font = CreateFontIndirectW(&lf);

  HWND titles[] = {g_dv.title, g_dv.devices}, headings[] = {g_dv.heading, g_dv.quality, g_dv.unpaired};
  for (size_t i = 0; i < sizeof titles / sizeof titles[0]; i++)
    SendMessageW(titles[i], WM_SETFONT, (WPARAM)g_dv.title_font, TRUE);
  for (size_t i = 0; i < sizeof headings / sizeof headings[0]; i++)
    SendMessageW(headings[i], WM_SETFONT, (WPARAM)g_dv.heading_font, TRUE);
  HWND body[] = {g_dv.sync_now, g_dv.status, g_dv.message, g_dv.notice, g_dv.issues, g_dv.files,
                 g_dv.archive_label, g_dv.archive_text, g_dv.note, g_dv.sync_all_label, g_dv.select_all,
                 g_dv.select_none, g_dv.list, g_dv.count, g_dv.format_label, g_dv.format, g_dv.budget_label,
                 g_dv.budget_size, g_dv.budget_unit, g_dv.estimate, g_dv.none, g_dv.add, g_dv.leave};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
  for (int i = 0; i < NUM_TABS; i++) InvalidateRect(g_dv.tabs[i], NULL, FALSE);
  for (int i = 0; i < MAX_ROWS; i++) InvalidateRect(g_dv.dev_btns[i], NULL, FALSE);
  firewall_banner_set_font(font, dpi);
  set_check_images();
}

void device_view_theme_changed(void) {
  firewall_banner_theme_changed();
}

void device_view_show(BOOL show) {
  g_dv.shown = show;
  firewall_watch(g_dv.parent, show);
  if (!show) {
    ShowWindow(g_dv.page, SW_HIDE);
    return;
  }
  layout_again();
  /* A moment's sign that there's more below. */
  scroller_show(&g_dv.left.scroll);
  scroller_show(&g_dv.right.scroll);
}

static int text_width(HWND wnd, HFONT font) {
  wchar_t buf[256];
  int n = GetWindowTextW(wnd, buf, 256);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, font);
  SIZE size = {0, 0};
  GetTextExtentPoint32W(dc, buf, n, &size);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return size.cx;
}

static BOOL has_text(HWND wnd) {
  return GetWindowTextLengthW(wnd) > 0;
}

/* Moves wnd unless it's there already: the page is laid out again on every refresh, and a
 * move would close an open drop-down and redraw for nothing. h < 0 keeps its height (a combo
 * box's is its own) and gives it drop_h, the drop-down's. Where it does move, both where it
 * was and where it goes are repainted, so it leaves nothing behind. Nothing moves while
 * layout_left() is only measuring. */
static void move_to(HWND wnd, int x, int y, int w, int h, int drop_h) {
  if (g_dv.measuring) return;
  HWND parent = GetParent(wnd);
  RECT old, want = {x, y, x + w, y + h};
  GetWindowRect(wnd, &old);
  MapWindowPoints(NULL, parent, (POINT *)&old, 2);
  if (h < 0) want.bottom = y + (old.bottom - old.top);
  if (EqualRect(&old, &want)) return;
  MoveWindow(wnd, x, y, w, h >= 0 ? h : drop_h, FALSE);
  UINT flags = RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN;
  if (IsWindowVisible(wnd)) RedrawWindow(parent, &old, NULL, flags);
  GetWindowRect(wnd, &want);
  MapWindowPoints(NULL, parent, (POINT *)&want, 2);
  RedrawWindow(parent, &want, NULL, flags);
}

static void set_shown(HWND wnd, BOOL visible) {
  if (g_dv.measuring) return;
  if (!(GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) != !visible) ShowWindow(wnd, visible ? SW_SHOW : SW_HIDE);
}

static void place(HWND wnd, BOOL visible, int x, int y, int w, int h) {
  if (visible) move_to(wnd, x, y, w, h, 0);
  set_shown(wnd, visible);
}

static void set_column_width(int col, int w) {
  if (ListView_GetColumnWidth(g_dv.list, col) != w) ListView_SetColumnWidth(g_dv.list, col, w);
}

/* Lays This PC's panel out down its content, w wide, and returns how tall that is: its name,
 * its files, the archive switch, What to sync with the list list_h tall, then the format and
 * the budget with the estimate. Measuring, nothing moves. */
static int layout_left(int w, int list_h) {
  BOOL on = active(), dev = member();
  int line_h = canvas_text_height(g_dv.files, g_dv.font, scale(1000), 0);
  int top = 0, th = canvas_text_height(g_dv.title, g_dv.title_font, scale(1000), 0);
  place(g_dv.title, on, 0, top, w, th);
  if (on) top += th + scale(4);
  /* Outside a group the note stands in for everything below. */
  int note_y = top + scale(8);
  BOOL files = dev && has_text(g_dv.files);
  place(g_dv.files, files, 0, top, w, line_h);
  if (files) top += line_h + scale(2);
  if (dev) top += scale(16);

  /* Archive: the box, and what it means beside it, which toggles it too. */
  int label_w = max(text_width(g_dv.archive_label, g_dv.font), text_width(g_dv.format_label, g_dv.font)) + scale(16);
  int field_h = scale(30), edit_h = scale(28), box = scale(20), btn_h = scale(32), field_x = label_w;
  int archive_x = field_x + box + scale(8);
  place(g_dv.archive_label, dev, 0, top + (field_h - line_h) / 2, label_w, line_h);
  place(g_dv.archive_box, dev, field_x, top + (field_h - box) / 2, box, box);
  place(g_dv.archive_text, dev, archive_x, top + (field_h - line_h) / 2,
        max(min(text_width(g_dv.archive_text, g_dv.font) + scale(8), w - archive_x), scale(40)), line_h);
  if (dev) top += field_h + scale(24);

  /* What to sync. */
  int hh = canvas_text_height(g_dv.heading, g_dv.heading_font, w, 0);
  place(g_dv.heading, dev, 0, top, w, hh);
  if (dev) {
    top += hh + scale(8);
    note_y = top;
  }
  BOOL note = on && has_text(g_dv.note);
  int noteh = note ? canvas_text_height(g_dv.note, g_dv.font, w, 0) : 0;
  place(g_dv.note, note, 0, note_y, w, noteh);
  if (note) top = note_y + noteh + scale(8);
  int row_h = max(box, line_h);
  place(g_dv.sync_all, dev, 0, top + (row_h - box) / 2, box, box);
  place(g_dv.sync_all_label, dev, box + scale(8), top + (row_h - line_h) / 2,
        text_width(g_dv.sync_all_label, g_dv.font) + scale(8), line_h);
  if (dev) top += row_h + scale(12);

  /* Under Sync everything, what was removed shows unticked among the albums and artists. */
  BOOL pick = dev && g_dv.profile && !g_dv.profile->archive;
  BOOL choosing = pick && !g_dv.profile->sync_all;
  int tab_w = 0;
  for (int i = 0; i < NUM_TABS; i++) tab_w = max(tab_w, text_width(g_dv.tabs[i], g_dv.font) + scale(32));
  tab_w = min(tab_w, w / NUM_TABS);
  for (int i = 0; i < NUM_TABS; i++)
    place(g_dv.tabs[i], choosing || (pick && i < TAB_PLAYLISTS), i * tab_w, top, tab_w, btn_h);

  /* Select All and Select None at the tabs' right, or on a line of their own if there's no room. */
  int all_w = text_width(g_dv.select_all, g_dv.font) + scale(24);
  int none_w = text_width(g_dv.select_none, g_dv.font) + scale(24);
  if (choosing && NUM_TABS * tab_w + scale(16) + all_w + scale(8) + none_w > w) top += btn_h + scale(8);
  place(g_dv.select_none, choosing, w - none_w, top, none_w, btn_h);
  place(g_dv.select_all, choosing, w - none_w - scale(8) - all_w, top, all_w, btn_h);
  if (pick) top += btn_h + scale(8);

  place(g_dv.list, pick, 0, top, w, list_h);
  if (pick && !g_dv.measuring) {
    RECT rc;
    GetClientRect(g_dv.list, &rc);
    int detail_w = scale(110);
    set_column_width(1, detail_w);
    set_column_width(0, max(rc.right - detail_w, scale(80)));
  }
  if (pick) top += list_h + scale(8);
  place(g_dv.count, choosing, 0, top, w, line_h);
  if (choosing) top += line_h;
  if (dev) top += scale(32);

  /* Quality: the format and the budget, then the estimate. */
  int qh = canvas_text_height(g_dv.quality, g_dv.heading_font, w, 0);
  place(g_dv.quality, dev, 0, top, w, qh);
  if (dev) top += qh + scale(8);
  /* Combo boxes are left alone while they're dropped down. */
  int combo_w = scale(180);
  if (dev && !SendMessageW(g_dv.format, CB_GETDROPPEDSTATE, 0, 0)) {
    RECT rc;
    GetWindowRect(g_dv.format, &rc);
    int ch = rc.bottom - rc.top;
    move_to(g_dv.format, field_x, top + (ch > 0 && ch < field_h ? (field_h - ch) / 2 : 0), combo_w, -1, scale(300));
  }
  set_shown(g_dv.format, dev);
  place(g_dv.format_label, dev, 0, top + (field_h - line_h) / 2, label_w, line_h);

  int budget_label_w = text_width(g_dv.budget_label, g_dv.font) + scale(8);
  int unit_w = text_width(g_dv.budget_unit, g_dv.font) + scale(4), size_w = scale(64);
  int budget_w = box + scale(8) + budget_label_w + size_w + scale(8) + unit_w;
  int bx = field_x + combo_w + scale(24);
  if (bx + budget_w > w) { /* no room beside the format: on a line of its own */
    if (dev) top += field_h + scale(8);
    bx = field_x;
  }
  place(g_dv.budget, dev, bx, top + (field_h - box) / 2, box, box);
  bx += box + scale(8);
  place(g_dv.budget_label, dev, bx, top + (field_h - line_h) / 2, budget_label_w, line_h);
  bx += budget_label_w;
  place(g_dv.budget_size, dev, bx, top + (field_h - edit_h) / 2, size_w, edit_h);
  bx += size_w + scale(8);
  place(g_dv.budget_unit, dev, bx, top + (field_h - line_h) / 2, unit_w, line_h);
  if (dev) top += field_h;
  BOOL estimate = dev && has_text(g_dv.estimate);
  int eh = estimate ? canvas_text_height(g_dv.estimate, g_dv.font, max(w - field_x, scale(40)), 0) : 0;
  place(g_dv.estimate, estimate, field_x, top + scale(4), max(w - field_x, scale(40)), eh);
  if (estimate) top += eh + scale(4);
  return top + scale(24);
}

/* Lays the devices' panel out down its content, w wide, and returns how tall that is: the
 * members, the devices nearby under a heading of their own, Add device by address and Leave
 * group. */
static int layout_right(int w) {
  BOOL on = active(), dev = member();
  int top = 0, btn_h = scale(32), row_h = scale(56);
  int th = canvas_text_height(g_dv.devices, g_dv.title_font, scale(1000), 0);
  place(g_dv.devices, on, 0, top, w, th);
  if (on) top += th + scale(12);
  BOOL unpaired = on && g_dv.ndevs > g_dv.nmembers;
  int hh = canvas_text_height(g_dv.unpaired, g_dv.heading_font, w, 0);
  for (int i = 0; i < MAX_ROWS; i++) {
    BOOL shown = on && i < g_dv.ndevs;
    if (shown && i == g_dv.nmembers) {
      if (i) top += scale(16);
      place(g_dv.unpaired, TRUE, 0, top, w, hh);
      top += hh + scale(8);
    }
    place(g_dv.dev_btns[i], shown, 0, top, w, row_h);
    if (shown) top += row_h + scale(4);
  }
  if (!unpaired) set_shown(g_dv.unpaired, FALSE);
  BOOL none = on && !g_dv.ndevs;
  int nh = none ? canvas_text_height(g_dv.none, g_dv.font, w, 0) : 0;
  place(g_dv.none, none, 0, top, w, nh);
  if (none) top += nh + scale(4);
  int add_w = text_width(g_dv.add, g_dv.font) + scale(32);
  place(g_dv.add, on, 0, top + scale(12), add_w, btn_h);
  if (on) top += btn_h + scale(12);
  int leave_w = text_width(g_dv.leave, g_dv.font) + scale(32);
  place(g_dv.leave, dev, 0, top + scale(32), leave_w, btn_h);
  if (dev) top += btn_h + scale(32);
  return top + scale(24);
}

/* Puts p's view at x, y, w by h, with the content in it content_w wide and content_h tall. */
static void place_panel(panel *p, BOOL visible, int x, int y, int w, int h, int content_w, int content_h) {
  RECT view = {0, 0, w, h};
  move_to(p->view, x, y, w, h, 0);
  scroller_resize(&p->scroll, &view, content_h);
  SetWindowPos(p->content, NULL, 0, -p->scroll.pos, content_w, content_h, SWP_NOZORDER | SWP_NOACTIVATE);
  InvalidateRect(p->view, NULL, FALSE); /* the bar's size, or that it's gone */
  set_shown(p->view, visible);
}

void device_view_layout(int x, int y, int w, int h, UINT dpi) {
  g_dv.x = x, g_dv.y = y, g_dv.w = w, g_dv.h = h, g_dv.dpi = dpi;
  if (!g_dv.shown || !g_dv.font) return;
  /* The page leaves the main window the left margin and the band at the top, with the
   * content area's border and rounded corner, as the settings page does: Mica's frame reaches
   * down that far, and shows through what GDI draws. */
  int pad = scale(32), band = scale(CONTENT_RADIUS);
  int page_w = max(w - pad, 0), page_h = max(h - band, 0);
  move_to(g_dv.page, x + pad, y + band, page_w, page_h, 0);
  set_shown(g_dv.page, TRUE);
  int full = max(page_w - pad, scale(240)), top = scale(24) - band, btn_h = scale(32);
  int line_h = canvas_text_height(g_dv.status, g_dv.font, scale(1000), 0);
  BOOL on = active(), dev = member();

  /* The firewall banner, above everything: it's about every device in the group. */
  int firewall_h = firewall_banner_place(on, 0, top, full);
  if (firewall_h) top += firewall_h + scale(16);

  /* What went wrong, what keeps this PC from syncing, and Details for its issues (or the
   * missing tracks the last sync left). */
  BOOL message = on && has_text(g_dv.message);
  int mh = message ? canvas_text_height(g_dv.message, g_dv.font, full, 0) : 0;
  place(g_dv.message, message, 0, top, full, mh);
  if (message) top += mh + scale(10);
  BOOL notice = on && has_text(g_dv.notice);
  int nh = notice ? canvas_text_height(g_dv.notice, g_dv.font, full, 0) : 0;
  place(g_dv.notice, notice, 0, top, full, nh);
  if (notice) top += nh + scale(10);
  BOOL issues = dev && (g_dv.has_issues || g_dv.missing_shown);
  place(g_dv.issues, issues, 0, top, text_width(g_dv.issues, g_dv.font) + scale(32), btn_h);
  if (issues) top += btn_h + scale(16);

  /* The strip: what sync last did here, or how it's going, and Sync now. While syncing, the
   * bar comes in front of the line. */
  int sync_w = text_width(g_dv.sync_now, g_dv.font) + scale(32);
  place(g_dv.sync_now, on, full - sync_w, top, sync_w, btn_h);
  BOOL busy = dev && (syncing() || g_dv.finished);
  int bar_w = scale(120), status_x = busy ? bar_w + scale(12) : 0;
  place(g_dv.progress, busy, 0, top + (btn_h - scale(6)) / 2, bar_w, scale(6));
  place(g_dv.status, TRUE, status_x, top + (btn_h - line_h) / 2, max(full - sync_w - scale(16) - status_x, scale(40)),
        line_h);
  top += btn_h + scale(12);

  /* The panels, below a line: This PC's three fifths of the width, the devices' the rest,
   * with a line between. Each view reaches that line (or the window's edge), its scrollbar
   * in the margin past its content. */
  int gap = scale(48), left_w = (full - gap) * 3 / 5, right_x = left_w + gap;
  int lines[] = {on ? top : 0, full, left_w + gap / 2, page_h};
  if (lines[0] != g_dv.line_y || lines[1] != g_dv.line_w || lines[2] != g_dv.divider_x ||
      lines[3] != g_dv.divider_bottom) {
    g_dv.line_y = lines[0], g_dv.line_w = lines[1], g_dv.divider_x = lines[2], g_dv.divider_bottom = lines[3];
    InvalidateRect(g_dv.page, NULL, TRUE);
  }
  top += 1 + scale(16);
  int panel_h = max(page_h - top, 0), right_w = max(full - right_x, scale(120));
  g_dv.measuring = TRUE;
  int fixed = layout_left(left_w, 0);
  g_dv.measuring = FALSE;
  int left_h = layout_left(left_w, max(scale(160), panel_h - fixed));
  place_panel(&g_dv.left, on, 0, top, g_dv.divider_x, panel_h, left_w, left_h);
  place_panel(&g_dv.right, on, right_x, top, max(page_w - right_x, 0), panel_h, right_w, layout_right(right_w));
}

static void layout_again(void) {
  if (g_dv.shown) device_view_layout(g_dv.x, g_dv.y, g_dv.w, g_dv.h, g_dv.dpi);
}

/* Scrolls ctl's panel to bring it into view, with what's around it. */
static void reveal(HWND ctl) {
  panel *p = IsChild(g_dv.left.content, ctl) ? &g_dv.left : IsChild(g_dv.right.content, ctl) ? &g_dv.right : NULL;
  if (!p) return;
  RECT r;
  GetWindowRect(ctl, &r);
  MapWindowPoints(NULL, p->content, (POINT *)&r, 2);
  int margin = scale(REVEAL_MARGIN);
  scroller_reveal(&p->scroll, r.top - margin, r.bottom + margin);
}

/* Gives ctl the focus as Tab does in a dialog, which the main window isn't. */
static void tab_to(HWND ctl) {
  if (!ctl) return; /* nothing can take the focus */
  SendMessageW(g_dv.parent, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  SetFocus(ctl);
  if (SendMessageW(ctl, WM_GETDLGCODE, 0, 0) & DLGC_HASSETSEL) SendMessageW(ctl, EM_SETSEL, 0, -1);
  reveal(ctl);
}

void device_view_focus(void) {
  if (g_dv.shown) tab_to(GetNextDlgTabItem(g_dv.page, NULL, FALSE));
}

void device_view_focus_what_to_sync(void) {
  SetFocus(g_dv.sync_all);
  SendMessageW(g_dv.parent, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
  reveal(g_dv.heading);
}

BOOL device_view_translate(const MSG *msg) {
  if (!g_dv.shown || GetAncestor(msg->hwnd, GA_ROOT) != g_dv.parent || !IsWindowEnabled(g_dv.parent)) return FALSE;
  if (msg->message == WM_MOUSEMOVE) { /* over the controls, which the views don't hear of: their bars show */
    panel *p = IsChild(g_dv.left.view, msg->hwnd) ? &g_dv.left : IsChild(g_dv.right.view, msg->hwnd) ? &g_dv.right : NULL;
    if (p) {
      POINT pt = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
      MapWindowPoints(msg->hwnd, p->view, &pt, 1);
      scroller_mouse_move(&p->scroll, pt.x, pt.y);
    }
    return FALSE;
  }
  if (msg->message == WM_MOUSEWHEEL) { /* the panel under the mouse, but the list scrolls itself */
    POINT pt = {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
    HWND under = WindowFromPoint(pt);
    if (under == g_dv.list || IsChild(g_dv.list, under)) return FALSE;
    panel *panels[] = {&g_dv.left, &g_dv.right};
    for (int i = 0; i < 2; i++) {
      RECT r;
      GetWindowRect(panels[i]->view, &r);
      if (!IsWindowVisible(panels[i]->view) || !PtInRect(&r, pt)) continue;
      scroller_wheel(&panels[i]->scroll, GET_WHEEL_DELTA_WPARAM(msg->wParam));
      return TRUE;
    }
    return FALSE;
  }
  /* Tab from anywhere in the window, as on the settings page. */
  if (msg->message != WM_KEYDOWN || msg->wParam != VK_TAB || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
    return FALSE;
  tab_to(GetNextDlgTabItem(g_dv.page, IsChild(g_dv.page, msg->hwnd) ? msg->hwnd : NULL, GetKeyState(VK_SHIFT) < 0));
  return TRUE;
}

/* Opens the page on sync, or on none. */
void device_view_open(mc_sync *sync, mc_library *lib) {
  /* Edits still waiting go in while their sync is still there: opening with NULL comes
   * before it stops. */
  if (g_dv.sync && (sync == g_dv.sync || !sync)) flush();
  KillTimer(g_dv.parent, ID_DEV_APPLY_TIMER);
  pending_clear();
  BOOL same = sync && sync == g_dv.sync;
  if (sync != g_dv.sync) estimate_cancel(); /* it uses the old one */
  if (!same) set_estimate(NULL, FALSE);
  g_dv.sync = sync;
  g_dv.lib = lib;
  if (!same) {
    const char *self = sync ? mc_sync_device_uid(sync) : NULL;
    snprintf(g_dv.uid, sizeof g_dv.uid, "%s", self ? self : "");
    g_dv.nbusy = g_dv.nresults = 0;
    set_finished(FALSE);
    g_dv.tab = TAB_ALBUMS;
    set_message(NULL);
  }
  g_dv.rows_tab = -1; /* the library may have changed: reload */
  reload(FALSE);
  for (int i = 0; i < NUM_TABS; i++) InvalidateRect(g_dv.tabs[i], NULL, FALSE);
}

static void CALLBACK refresh_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)msg, (void)time;
  KillTimer(wnd, id);
  device_view_refresh();
}

static BOOL dropped_down(void) {
  return SendMessageW(g_dv.format, CB_GETDROPPEDSTATE, 0, 0) != 0;
}

void device_view_issues_changed(void) {
  if (!active() || dropped_down()) return; /* the drop-down's refresh reads them too */
  load_devices();
  update_devices();
  layout_again();
}

void device_view_refresh(void) {
  if (!active()) return;
  /* Not under an open drop-down, which would lose what's highlighted in it: once it
   * closes. */
  if (dropped_down()) {
    g_dv.refresh_later = TRUE;
    return;
  }
  g_dv.refresh_later = FALSE;
  reload(TRUE);
}

static void CALLBACK finished_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)wnd, (void)msg, (void)id, (void)time;
  set_finished(FALSE);
  update_strip();
  layout_again();
}

void device_view_on_sync_event(const sync_event *ev) {
  if (!member() || !ev->uid[0]) return;
  int i = busy_index(ev->uid);
  switch (ev->kind) {
    case SYNC_EV_PROGRESS:
      if (ev->phase == MC_SYNC_PHASE_DONE) { /* over; on_sync_done may follow, or not */
        if (i < 0) return;
        memmove(g_dv.busy + i, g_dv.busy + i + 1, (size_t)(g_dv.nbusy - i - 1) * sizeof g_dv.busy[0]);
        g_dv.nbusy--;
        /* One that got past connecting is reported by on_sync_done, which says how it went;
         * until then the bar shows full. */
        if (!syncing() && g_dv.phase != MC_SYNC_PHASE_CONNECTING) {
          swprintf(g_dv.finished_text, 256, L"%ls", mc_wstr(MC_STR_PHASE_DONE));
          set_finished(TRUE);
        }
        update_strip();
        update_rows(); /* its row's Sync mark goes */
        break;
      }
      if (i < 0 && g_dv.nbusy < MAX_BUSY) {
        memcpy(g_dv.busy[g_dv.nbusy], ev->uid, MC_UID_STRLEN);
        g_dv.busy[g_dv.nbusy++][MC_UID_STRLEN - 1] = '\0';
      }
      set_finished(FALSE);
      g_dv.phase = ev->phase;
      g_dv.sent = ev->rows_sent;
      g_dv.received = ev->rows_received;
      g_dv.files_done = ev->files_done;
      g_dv.files_total = ev->files_total;
      g_dv.sends_done = ev->sends_done;
      g_dv.sends_total = ev->sends_total;
      g_dv.bytes_received = ev->bytes_received;
      g_dv.bytes_sent = ev->bytes_sent;
      snprintf(g_dv.pace, sizeof g_dv.pace, "%s", ev->pace);
      if (i < 0) {
        set_message(NULL);
        update_rows(); /* its row gets the Sync mark */
      }
      update_strip();
      break;
    case SYNC_EV_DONE: {
      if (i >= 0) {
        memmove(g_dv.busy + i, g_dv.busy + i + 1, (size_t)(g_dv.nbusy - i - 1) * sizeof g_dv.busy[0]);
        g_dv.nbusy--;
      }
      wchar_t text[256] = L"";
      if (ev->cancelled || ev->peer_cancelled) {
        swprintf(text, 256, L"%ls", mc_wstr(MC_STR_SYNC_CANCELLED));
      } else if (ev->status != MC_OK) {
        if (ev->name[0])
          mc_wformat(text, 256, mc_str(MC_STR_SYNC_FAILED_WITH), MC_S(u8(ev->name)),
                     MC_S(mc_status_str(ev->status)));
        else mc_wformat(text, 256, mc_str(MC_STR_SYNC_FAILED), MC_S(mc_status_str(ev->status)));
      } else if (!ev->library_synced) {
        swprintf(text, 256, L"%ls", mc_wstr(MC_STR_SYNC_DEVICE_LIST_ONLY));
      } else if (ev->files_missing > 0) { /* stays, with Details beside it, till the next session */
        mc_sync_result r = {.device_uid = ev->uid, .status = ev->status, .library_synced = 1,
                            .files_missing = ev->files_missing, .files_corrupt = ev->files_corrupt};
        char missing[256];
        mc_sync_finished_text(&r, missing, sizeof missing);
        wstr_from_utf8(text, 256, missing);
      }
      set_message(text);
      set_result(ev->uid, text); /* for the member's details */
      g_dv.missing_shown = text[0] && ev->status == MC_OK && ev->library_synced && ev->files_missing > 0;
      /* The finished bar says what came in; a failure says so in the message instead. */
      if (ev->status != MC_OK) {
        set_finished(FALSE);
      } else if (g_dv.finished) {
        wchar_t n[32], size[32];
        if (ev->files_received > 0)
          mc_wformat(g_dv.finished_text, 256, mc_str_plural(MC_STR_SYNC_FINISHED_FILES, ev->files_received),
                     MC_S(u8(wstr_count(n, 32, ev->files_received))), MC_S(u8(wstr_size(size, 32, ev->bytes_received))));
        else swprintf(g_dv.finished_text, 256, L"%ls", mc_wstr(MC_STR_SYNC_FINISHED));
      }
      /* What sync last did, the devices' last sync, maybe a profile, have changed. */
      reload(TRUE);
      return;
    }
    default:
      return;
  }
  layout_again();
}

BOOL device_view_on_command(int id, int code) {
  if (firewall_banner_on_command(id, code)) return TRUE;
  switch (id) {
    case ID_DEV_SYNC_NOW:
      if (code == BN_CLICKED && syncing()) cancel_sync();
      else if (code == BN_CLICKED) sync_now();
      return TRUE;
    case ID_DEV_LEAVE:
      if (code == BN_CLICKED) leave_group();
      return TRUE;
    case ID_DEV_ADD:
      if (code == BN_CLICKED && g_dv.host.add_device) g_dv.host.add_device();
      return TRUE;
    case ID_DEV_ISSUES:
      if (code == BN_CLICKED && issue_dialog_show(GetAncestor(g_dv.parent, GA_ROOT), g_dv.lib, g_dv.sync, g_dv.uid) &&
          g_dv.host.library_changed)
        g_dv.host.library_changed();
      return TRUE;
    case ID_DEV_FORMAT:
      if (code == CBN_SELCHANGE && !g_dv.filling) on_format();
      /* After the CBN_SELCHANGE that may follow, so a choice isn't undone. */
      else if (code == CBN_CLOSEUP && g_dv.refresh_later) SetTimer(g_dv.parent, ID_DEV_REFRESH_TIMER, 1, refresh_timer);
      return TRUE;
    case ID_DEV_BUDGET_LABEL:
      if (code != STN_CLICKED || !IsWindowEnabled(g_dv.budget)) return TRUE;
      set_checked(g_dv.budget, !is_checked(g_dv.budget));
      on_budget();
      return TRUE;
    case ID_DEV_BUDGET:
      if (code == BN_CLICKED && !g_dv.filling) on_budget();
      return TRUE;
    case ID_DEV_BUDGET_SIZE:
      if (code == EN_CHANGE && !g_dv.filling && budget_from_edit() > 0) on_budget();
      else if (code == EN_KILLFOCUS && !budget_from_edit()) update_controls(TRUE); /* empty or 0: put it back */
      return TRUE;
    case ID_DEV_ARCHIVE:
    case ID_DEV_ARCHIVE_TEXT:
      if ((code == BN_CLICKED || code == STN_CLICKED) && !g_dv.filling) on_archive();
      return TRUE;
    case ID_DEV_SYNC_ALL_LABEL:
      if (code != STN_CLICKED || !IsWindowEnabled(g_dv.sync_all)) return TRUE;
      set_checked(g_dv.sync_all, !is_checked(g_dv.sync_all));
      on_sync_all();
      return TRUE;
    case ID_DEV_SYNC_ALL:
      if (code == BN_CLICKED && !g_dv.filling) on_sync_all();
      return TRUE;
    case ID_DEV_SELECT_ALL:
    case ID_DEV_SELECT_NONE:
      if (code == BN_CLICKED) select_shown(id == ID_DEV_SELECT_ALL);
      return TRUE;
    default:
      if (id >= ID_DEV_TAB && id < ID_DEV_TAB + NUM_TABS) {
        if (code == BN_CLICKED) select_tab(id - ID_DEV_TAB);
        return TRUE;
      }
      if (id >= ID_DEV_ROW && id <= ID_DEV_ROW_LAST) {
        const dev_row *r = id - ID_DEV_ROW < g_dv.ndevs ? &g_dv.devs[id - ID_DEV_ROW] : NULL;
        if (code != BN_CLICKED || !r) return TRUE;
        if (r->nearby && g_dv.host.pair) g_dv.host.pair(r->uid);
        else if (!r->nearby) show_member(r->uid);
        return TRUE;
      }
      return id >= ID_DEV_SYNC_NOW && id < ID_DEV_FIREWALL;
  }
}

BOOL device_view_on_notify(const NMHDR *nm, LRESULT *result) {
  if (!g_dv.list || nm->hwndFrom != g_dv.list) return FALSE;
  *result = 0;
  switch (nm->code) {
    case LVN_ITEMCHANGED: {
      const NMLISTVIEW *lv = (const NMLISTVIEW *)nm;
      UINT old_image = (lv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
      UINT new_image = (lv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
      if (g_dv.filling || !(lv->uChanged & LVIF_STATE) || !old_image || old_image == new_image) break;
      on_check(lv->iItem, new_image == 2);
      break;
    }
    case NM_CLICK: { /* a click on an item's text ticks it too */
      const NMITEMACTIVATE *ia = (const NMITEMACTIVATE *)nm;
      LVHITTESTINFO hit = {.pt = ia->ptAction};
      if (SendMessageW(g_dv.list, LVM_SUBITEMHITTEST, 0, (LPARAM)&hit) >= 0 && hit.iItem >= 0 &&
          (hit.flags & LVHT_ONITEM) && !(hit.flags & LVHT_ONITEMSTATEICON))
        ListView_SetCheckState(g_dv.list, hit.iItem, !ListView_GetCheckState(g_dv.list, hit.iItem));
      break;
    }
    case LVN_GETEMPTYMARKUP: {
      static const enum mc_str_id none[NUM_TABS] = {MC_STR_NO_ALBUMS, MC_STR_NO_ARTISTS,
                                                    MC_STR_NO_PLAYLISTS, MC_STR_NO_ALBUM_GROUPS};
      NMLVEMPTYMARKUP *em = (NMLVEMPTYMARKUP *)nm;
      em->dwFlags = EMF_CENTERED;
      if (g_dv.rows_status == MC_OK) swprintf(em->szMarkup, L_MAX_URL_LENGTH, L"%ls", mc_wstr(none[g_dv.tab]));
      else
        mc_wformat(em->szMarkup, L_MAX_URL_LENGTH, mc_str(MC_STR_LIST_FAILED),
                   MC_S(mc_status_str(g_dv.rows_status)));
      *result = TRUE;
      break;
    }
  }
  return TRUE;
}

/* A device's row: its kind's icon, its name and what it says under it, and at its end Sync
 * while a session with it runs, else Warning while something's wrong with it. */
static void draw_dev_row(canvas *c, const RECT *rc, int i, control_state state) {
  canvas_clear(c, rc, theme.bg, 255);
  if (i >= g_dv.ndevs) return;
  const dev_row *r = &g_dv.devs[i];
  COLORREF color = theme_draw_state(c, rc, scale(6), state);
  COLORREF dim = color == theme.text ? theme.text2 : color;
  static const wchar_t icons[] = {[MC_DEVICE_DESKTOP] = 0xE977, /* PC */
                                  [MC_DEVICE_PHONE] = 0xE8EA};  /* CellPhone */
  wchar_t icon = (unsigned)r->kind <= (unsigned)MC_DEVICE_PHONE ? icons[r->kind] : icons[0];
  UINT one = DT_SINGLELINE | DT_NOPREFIX;
  RECT at = {rc->left + scale(12), rc->top, rc->left + scale(44), rc->bottom};
  canvas_text(c, &icon, 1, &at, one | DT_CENTER | DT_VCENTER, g_dv.icon_font, color);
  int right = rc->right - scale(12);
  wchar_t mark = busy_index(r->uid) >= 0 ? 0xE895 : r->warning ? 0xE7BA : 0;
  if (mark) {
    RECT m = {right - scale(20), rc->top, right, rc->bottom};
    canvas_text(c, &mark, 1, &m, one | DT_CENTER | DT_VCENTER, g_dv.small_icon_font, dim);
    right = m.left - scale(8);
  }
  int x = rc->left + scale(56), mid = (rc->top + rc->bottom) / 2;
  if (!r->detail[0]) {
    RECT name = {x, rc->top, right, rc->bottom};
    canvas_text(c, r->name, -1, &name, one | DT_END_ELLIPSIS | DT_VCENTER, g_dv.font, color);
    return;
  }
  RECT name = {x, rc->top, right, mid}, detail = {x, mid, right, rc->bottom};
  canvas_text(c, r->name, -1, &name, one | DT_END_ELLIPSIS | DT_BOTTOM, g_dv.font, color);
  canvas_text(c, r->detail, -1, &detail, one | DT_END_ELLIPSIS | DT_TOP, g_dv.font, dim);
}

BOOL device_view_draw_item(const DRAWITEMSTRUCT *dis) {
  int t = (int)dis->CtlID - ID_DEV_TAB, row = (int)dis->CtlID - ID_DEV_ROW;
  BOOL tab = t >= 0 && t < NUM_TABS;
  if (!tab && (row < 0 || row >= MAX_ROWS)) return FALSE;
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas c = {0};
  if (!canvas_reserve(&c, rc.right, rc.bottom)) return TRUE;
  control_state state = dis->itemState & ODS_SELECTED ? STATE_PRESSED
                        : g_dv.hot == dis->hwndItem   ? STATE_HOT
                                                      : STATE_NORMAL;
  int radius = scale(tab ? 4 : 6);
  if (tab) {
    theme_draw_segment(&c, &rc, radius, mc_wstr(k_tab_name[t]), g_dv.font, t == 0, t == NUM_TABS - 1,
                       t == g_dv.tab, state);
    if (dis->itemState & ODS_DISABLED) canvas_fill(&c, &rc, 0, theme.bg, 128);
  } else {
    draw_dev_row(&c, &rc, row, state);
  }
  if ((dis->itemState & ODS_FOCUS) && !(dis->itemState & ODS_NOFOCUSRECT)) {
    RECT focus = rc;
    InflateRect(&focus, -2, -2);
    canvas_frame(&c, &focus, radius, 1, theme.text, 255);
  }
  canvas_blit(&c, dis->hDC, dis->rcItem.left, dis->rcItem.top, &rc);
  canvas_free(&c);
  return TRUE;
}

HBRUSH device_view_ctlcolor(UINT msg, HWND wnd, HDC dc) {
  if (!g_dv.parent || !wnd) return NULL;
  switch (msg) {
    case WM_CTLCOLORLISTBOX:
      if (wnd != g_dv.format && wnd != g_dv.combo_list) return NULL;
      SetTextColor(dc, theme.text);
      SetBkColor(dc, theme.field);
      return theme.field_brush;
    case WM_CTLCOLOREDIT:
      if (wnd != g_dv.budget_size && wnd != g_dv.format) return NULL;
      return theme_ctlcolor(msg, dc);
    case WM_CTLCOLORBTN: {
      HBRUSH firewall = firewall_banner_ctlcolor(wnd, dc);
      if (firewall) return firewall;
      if (wnd != g_dv.sync_now && wnd != g_dv.leave && wnd != g_dv.add && wnd != g_dv.budget &&
          wnd != g_dv.sync_all && wnd != g_dv.archive_box && wnd != g_dv.select_all && wnd != g_dv.select_none)
        return NULL;
      SetBkColor(dc, theme.bg);
      return theme.bg_brush;
    }
    case WM_CTLCOLORSTATIC:
      break;
    default:
      return NULL;
  }
  /* The disabled edit asks as a static. It loses the field colour too, as the disabled combo
   * box does, so it doesn't look editable. */
  if (wnd == g_dv.budget_size) {
    COLORREF bg = theme.dark || theme.high_contrast ? theme.bg : theme.frame;
    SetTextColor(dc, theme.text3);
    SetBkColor(dc, bg);
    SetDCBrushColor(dc, bg);
    return GetStockObject(DC_BRUSH);
  }
  COLORREF color;
  if (wnd == g_dv.title || wnd == g_dv.devices || wnd == g_dv.heading || wnd == g_dv.quality ||
      wnd == g_dv.unpaired || wnd == g_dv.message || wnd == g_dv.notice)
    color = theme.text;
  else if (wnd == g_dv.status || wnd == g_dv.files || wnd == g_dv.note || wnd == g_dv.count || wnd == g_dv.none)
    color = theme.text2;
  else if (wnd == g_dv.archive_label)
    color = IsWindowEnabled(g_dv.archive_box) ? theme.text : theme.text3;
  else if (wnd == g_dv.archive_text) /* disabled, it says why */
    color = IsWindowEnabled(g_dv.archive_box) ? theme.text : theme.text2;
  else if (wnd == g_dv.format_label)
    color = IsWindowEnabled(g_dv.format) ? theme.text : theme.text3;
  else if (wnd == g_dv.budget_label || wnd == g_dv.budget_unit)
    color = IsWindowEnabled(g_dv.budget) ? theme.text : theme.text3;
  else if (wnd == g_dv.sync_all_label)
    color = IsWindowEnabled(g_dv.sync_all) ? theme.text : theme.text3;
  else if (wnd == g_dv.budget || wnd == g_dv.sync_all || wnd == g_dv.archive_box)
    color = theme.text;
  else if (wnd == g_dv.estimate)
    /* Fluent's caution colours, for the line that says it doesn't fit. */
    color = !g_dv.est.over || theme.high_contrast ? (g_dv.est.over ? theme.text : theme.text2)
            : theme.dark                          ? RGB(0xFC, 0xE1, 0x00)
                                                  : RGB(0x9D, 0x5D, 0x00);
  else
    return NULL;
  SetTextColor(dc, color);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
