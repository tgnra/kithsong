#include "device_view.h"

#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "canvas.h"
#include "theme.h"
#include "wstr.h"

enum {
  ID_DEV_ICON = 400,
  ID_DEV_TITLE,
  ID_DEV_SYNC_NOW,
  ID_DEV_REMOVE,
  ID_DEV_STATUS,
  ID_DEV_FILES,
  ID_DEV_PROGRESS,
  ID_DEV_PROGRESS_TEXT,
  ID_DEV_MESSAGE,
  ID_DEV_NOTICE,
  ID_DEV_NAME_LABEL,
  ID_DEV_NAME,
  ID_DEV_FORMAT_LABEL,
  ID_DEV_FORMAT,
  ID_DEV_BUDGET,
  ID_DEV_BUDGET_LABEL,
  ID_DEV_BUDGET_SIZE,
  ID_DEV_BUDGET_UNIT,
  ID_DEV_HEADING,
  ID_DEV_NOTE,
  ID_DEV_SYNC_ALL,
  ID_DEV_SYNC_ALL_LABEL,
  ID_DEV_TAB, /* four of them */
  ID_DEV_LIST = ID_DEV_TAB + 4,
  ID_DEV_COUNT,
  /* Timers on the parent, run by their TIMERPROCs rather than WM_TIMER. */
  ID_DEV_APPLY_TIMER = 440,
  ID_DEV_TICK_TIMER,
  ID_DEV_REFRESH_TIMER,
};

/* Posted by the list to itself after its theme changes, to put our checkboxes back. */
#define WM_RECHECK (WM_APP + 1)

#define APPLY_DELAY_MS 400
#define TICK_MS 30000 /* "synced 3 minutes ago" moves on */
#define DEFAULT_BUDGET_GB 32
#define AUTO_ITEM 1 /* the format list's item data for Auto; the others hold their kbps, 0 originals */
#define GIB ((int64_t)1024 * 1024 * 1024)
#define MAX_BUSY 16

enum { TAB_ALBUMS, TAB_ARTISTS, TAB_PLAYLISTS, TAB_GROUPS, NUM_TABS };
static const mc_profile_item_kind k_tab_kind[NUM_TABS] = {MC_PROFILE_ALBUM, MC_PROFILE_ARTIST,
                                                          MC_PROFILE_PLAYLIST, MC_PROFILE_ALBUM_GROUP};
static const enum mc_str_id k_tab_name[NUM_TABS] = {MC_STR_SECTION_ALBUMS, MC_STR_SECTION_ARTISTS,
                                                    MC_STR_SECTION_PLAYLISTS, MC_STR_WINDOWS_SECTION_ALBUM_GROUPS};
/* A selection's count of each tab's items, "3 albums". */
static const enum mc_str_id k_tab_count[NUM_TABS] = {MC_STR_WINDOWS_COUNT_ALBUMS, MC_STR_WINDOWS_COUNT_ARTISTS,
                                                     MC_STR_WINDOWS_COUNT_PLAYLISTS, MC_STR_WINDOWS_COUNT_ALBUM_GROUPS};

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

static struct {
  HWND parent;
  device_view_host host;
  HWND icon, title, sync_now, remove, status, files, progress, progress_text, message, notice;
  HWND name_label, name_edit, format_label, format, budget, budget_label, budget_size, budget_unit;
  HWND heading, note, sync_all, sync_all_label, tabs[NUM_TABS], list, count, combo_list;
  HFONT font, title_font, heading_font, icon_font;
  HIMAGELIST checks; /* the list's checkboxes; the list owns them once set */
  UINT dpi;
  BOOL shown;
  BOOL refresh_later; /* a refresh came while the format drop-down was open */
  int x, y, w, h; /* the last layout */

  mc_sync *sync;
  mc_library *lib;
  char uid[MC_UID_STRLEN];

  /* The device, from mc_sync_list_devices(). */
  BOOL found, is_self;
  wchar_t name[MC_SYNC_DEVICE_NAME_MAX + 1];
  wchar_t others[512]; /* the other members' names, for Leave group */
  mc_device_kind kind;
  int online, archive, removed, has_status;
  int64_t last_sync_ms, expected, ok, missing, corrupt, bytes;
  mc_sync_lib_state lib_state;

  /* Its profile, with the pending edits applied; NULL if it couldn't be read. */
  mc_sync_profile *profile;
  mc_status profile_status;
  uid_set sel[NUM_TABS];

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

  /* Sessions in progress with this device (with any member, on this PC's own page). */
  uid_str busy[MAX_BUSY];
  int nbusy;
  mc_sync_phase phase;
  int64_t sent, received;

  int hot_tab; /* under the mouse, or -1 */
} g_dv = {.hot_tab = -1};

static void layout_again(void);

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

/* n with the user's digit grouping: 1,204. */
static wchar_t *fmt_count(int64_t n, wchar_t *out, int cap) {
  wchar_t digits[32], *p = digits + 31;
  uint64_t v = n < 0 ? (uint64_t)-n : (uint64_t)n;
  *p = L'\0';
  do *--p = (wchar_t)(L'0' + v % 10);
  while ((v /= 10) && p > digits + 1);
  if (n < 0) *--p = L'-';
  NUMBERFMTW fmt = {.NumDigits = 0, .Grouping = 3, .lpDecimalSep = L".", .lpThousandSep = L","};
  wchar_t sep[8];
  if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_STHOUSAND, sep, 8)) fmt.lpThousandSep = sep;
  if (!GetNumberFormatEx(LOCALE_NAME_USER_DEFAULT, 0, p, &fmt, out, cap)) wcsncpy(out, p, (size_t)cap - 1);
  out[cap - 1] = L'\0';
  return out;
}

static wchar_t *fmt_size(int64_t bytes, wchar_t *out, int cap) {
  static const enum mc_str_id units[] = {MC_STR_COUNT, MC_STR_WINDOWS_SIZE_KB, MC_STR_WINDOWS_SIZE_MB,
                                         MC_STR_WINDOWS_SIZE_GB, MC_STR_WINDOWS_SIZE_TB};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 4) v /= 1024.0, u++;
  if (u == 0) {
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_WINDOWS_SIZE_BYTES, (long)bytes), MC_D((int)bytes));
  } else {
    char num[32];
    snprintf(num, sizeof num, v < 100.0 ? "%.1f" : "%.0f", v);
    mc_wformat(out, (size_t)cap, mc_str(units[u]), MC_S(num));
  }
  return out;
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
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_WINDOWS_AGO_MINUTES, (long)min), MC_D((int)min));
  } else if (min < 24 * 60) {
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_WINDOWS_AGO_HOURS, (long)(min / 60)), MC_D((int)(min / 60)));
  } else if (min < 2 * 24 * 60) {
    swprintf(out, (size_t)cap, L"%ls", mc_wstr(MC_STR_WINDOWS_AGO_YESTERDAY));
  } else if (min < 7 * 24 * 60) {
    int days = (int)(min / (24 * 60));
    mc_wformat(out, (size_t)cap, mc_str_plural(MC_STR_WINDOWS_AGO_DAYS, days), MC_D(days));
  } else {
    ULARGE_INTEGER t = {.QuadPart = (uint64_t)ms * 10000 + 116444736000000000ULL};
    FILETIME ft = {t.LowPart, t.HighPart}, local;
    SYSTEMTIME st;
    wchar_t date[64] = L"";
    if (FileTimeToLocalFileTime(&ft, &local) && FileTimeToSystemTime(&local, &st))
      GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, NULL, date, 64, NULL);
    mc_wformat(out, (size_t)cap, mc_str(MC_STR_WINDOWS_AGO_ON_DATE), MC_S(u8(date)));
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
  for (int i = 0; i < NUM_TABS; i++) g_dv.sel[i].count = 0;
  if (!g_dv.profile) return;
  for (int i = 0; i < g_dv.profile->count; i++) {
    int t = tab_of_kind(g_dv.profile->items[i].kind);
    if (t >= 0) set_put(&g_dv.sel[t], g_dv.profile->items[i].ref_uid, TRUE);
  }
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

/* Reads the device's row from the device list. */
static void load_device(void) {
  g_dv.found = FALSE;
  g_dv.others[0] = L'\0';
  g_dv.lib_state = MC_SYNC_LIBRARY_GROUP;
  if (!g_dv.sync) return;
  g_dv.is_self = !strcmp(g_dv.uid, mc_sync_device_uid(g_dv.sync));
  mc_sync_device_list *list = NULL;
  if (mc_sync_list_devices(g_dv.sync, &list) == MC_OK && list) {
    size_t used = 0, cap = sizeof g_dv.others / sizeof g_dv.others[0];
    for (int i = 0; i < list->count; i++) {
      const mc_sync_device *d = &list->items[i];
      if (!strcmp(d->uid, g_dv.uid)) {
        g_dv.found = TRUE;
        widen(d->name, g_dv.name, MC_SYNC_DEVICE_NAME_MAX + 1);
        g_dv.kind = d->kind;
        g_dv.online = d->online;
        g_dv.archive = d->archive;
        g_dv.removed = d->removed;
        g_dv.last_sync_ms = d->last_sync_ms;
        g_dv.has_status = d->has_status;
        g_dv.expected = d->expected;
        g_dv.ok = d->ok;
        g_dv.missing = d->missing;
        g_dv.corrupt = d->corrupt;
        g_dv.bytes = d->bytes;
      } else if (!d->is_self && !d->removed && d->name && used + 4 < cap) {
        wchar_t name[MC_SYNC_DEVICE_NAME_MAX + 1];
        widen(d->name, name, MC_SYNC_DEVICE_NAME_MAX + 1);
        int n = swprintf(g_dv.others + used, cap - used, L"%ls%ls", used ? mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR) : L"",
                         name);
        if (n > 0) used += (size_t)n;
      }
    }
    mc_sync_device_list_free(list);
  }
  if (g_dv.is_self) g_dv.online = 1;
  mc_sync_lib_state state;
  if (mc_sync_library_state(g_dv.sync, &state) == MC_OK) g_dv.lib_state = state;
}

/* Reads the profile afresh and lays the pending edits over it. */
static void load_profile(void) {
  free_profile();
  g_dv.profile_status = MC_ERR_NOT_FOUND;
  if (g_dv.sync && g_dv.found && !g_dv.removed) {
    g_dv.profile_status = mc_sync_get_profile(g_dv.sync, g_dv.uid, &g_dv.profile);
    if (g_dv.profile_status == MC_OK) g_dv.profile_status = apply_pending(g_dv.profile);
    if (g_dv.profile_status != MC_OK) free_profile();
  }
  build_sel();
}

static BOOL editable(void) {
  return g_dv.profile && !g_dv.profile->archive;
}

static BOOL syncing(void) {
  return g_dv.nbusy > 0;
}

/* --- Controls from state --- */

static void update_status(void) {
  wchar_t text[256], ago[64];
  if (!g_dv.found) {
    swprintf(text, 256, L"%ls", mc_wstr(MC_STR_WINDOWS_DEVICE_NOT_IN_GROUP));
  } else if (g_dv.removed) {
    swprintf(text, 256, L"%ls", mc_wstr(MC_STR_WINDOWS_DEVICE_REMOVED));
  } else {
    const wchar_t *state;
    wchar_t buf[128];
    if (g_dv.last_sync_ms > 0) fmt_ago(g_dv.last_sync_ms, ago, 64);
    if (syncing()) state = mc_wstr(MC_STR_WINDOWS_SYNCING_PROGRESS);
    else if (g_dv.is_self) state = mc_wstr(MC_STR_WINDOWS_THIS_PC);
    else if (g_dv.online) {
      if (g_dv.last_sync_ms > 0) mc_wformat(buf, 128, mc_str(MC_STR_DEVICE_ONLINE_SYNCED), MC_S(u8(ago)));
      else swprintf(buf, 128, L"%ls", mc_wstr(MC_STR_DEVICE_ONLINE));
      state = buf;
    } else {
      if (g_dv.last_sync_ms > 0) mc_wformat(buf, 128, mc_str(MC_STR_DEVICE_OFFLINE_SYNCED), MC_S(u8(ago)));
      else swprintf(buf, 128, L"%ls", mc_wstr(MC_STR_DEVICE_NOT_SYNCED));
      state = buf;
    }
    swprintf(text, 256, L"%ls", state); /* an archive's note says it keeps the whole library */
  }
  SetWindowTextW(g_dv.status, text);

  text[0] = L'\0';
  if (g_dv.found && g_dv.has_status) {
    wchar_t ok[32], expected[32], n[32], size[32];
    const wchar_t *sep = mc_wstr(MC_STR_IMPORT_LIST_SEPARATOR);
    mc_wformat(text, 256, mc_str_plural(MC_STR_WINDOWS_FILES_OK, (long)g_dv.expected),
               MC_S(u8(fmt_count(g_dv.ok, ok, 32))), MC_S(u8(fmt_count(g_dv.expected, expected, 32))));
    size_t used = wcslen(text);
    if (g_dv.missing > 0) {
      used += (size_t)swprintf(text + used, 256 - used, L"%ls", sep);
      mc_wformat(text + used, 256 - used, mc_str(MC_STR_WINDOWS_FILES_MISSING),
                 MC_S(u8(fmt_count(g_dv.missing, n, 32))));
      used = wcslen(text);
    }
    if (g_dv.corrupt > 0) {
      used += (size_t)swprintf(text + used, 256 - used, L"%ls", sep);
      mc_wformat(text + used, 256 - used, mc_str(MC_STR_WINDOWS_FILES_DAMAGED),
                 MC_S(u8(fmt_count(g_dv.corrupt, n, 32))));
      used = wcslen(text);
    }
    swprintf(text + used, 256 - used, L"%ls%ls", sep, fmt_size(g_dv.bytes, size, 32));
  }
  SetWindowTextW(g_dv.files, text);

  static const enum mc_str_id phases[] = {
    [MC_SYNC_PHASE_CONNECTING] = MC_STR_WINDOWS_PHASE_CONNECTING,
    [MC_SYNC_PHASE_GROUP] = MC_STR_WINDOWS_PHASE_GROUP,
    [MC_SYNC_PHASE_LIBRARY] = MC_STR_WINDOWS_PHASE_LIBRARY,
    [MC_SYNC_PHASE_FILES] = MC_STR_WINDOWS_PHASE_FILES,
    [MC_SYNC_PHASE_DONE] = MC_STR_WINDOWS_PHASE_DONE,
  };
  text[0] = L'\0';
  if (syncing()) {
    BOOL known = (unsigned)g_dv.phase <= (unsigned)MC_SYNC_PHASE_DONE;
    const wchar_t *phase = mc_wstr(known ? phases[g_dv.phase] : MC_STR_WINDOWS_SYNCING_PROGRESS);
    if (g_dv.phase == MC_SYNC_PHASE_CONNECTING) {
      swprintf(text, 256, L"%ls", phase);
    } else {
      wchar_t sent[32], received[32];
      mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_PROGRESS_ROWS), MC_S(u8(phase)),
                 MC_S(u8(fmt_count(g_dv.sent, sent, 32))), MC_S(u8(fmt_count(g_dv.received, received, 32))));
    }
    SendMessageW(g_dv.progress, PBM_SETPOS, (WPARAM)g_dv.phase + 1, 0);
  }
  SetWindowTextW(g_dv.progress_text, text);

  if (g_dv.lib_state == MC_SYNC_LIBRARY_OTHER)
    SetWindowTextW(g_dv.notice, mc_wstr(MC_STR_WINDOWS_PC_SYNC_OTHER_LIBRARY));
  else if (g_dv.lib_state == MC_SYNC_LIBRARY_UNKNOWN)
    SetWindowTextW(g_dv.notice, mc_wstr(MC_STR_SYNC_UNKNOWN_LIBRARY));
  else SetWindowTextW(g_dv.notice, L"");

  EnableWindow(g_dv.sync_now, g_dv.found && !g_dv.removed && g_dv.online && !syncing());
  EnableWindow(g_dv.remove, g_dv.found && !g_dv.removed);
}

static void set_check(int row, BOOL on) {
  ListView_SetItemState(g_dv.list, row, INDEXTOSTATEIMAGEMASK(on ? 2 : 1), LVIS_STATEIMAGEMASK);
}

/* The list's checkboxes from the selection. */
static void update_checks(void) {
  if (g_dv.rows_tab < 0) return;
  g_dv.filling = TRUE;
  for (int i = 0; i < g_dv.nrows; i++) set_check(i, set_has(&g_dv.sel[g_dv.rows_tab], g_dv.rows[i]));
  g_dv.filling = FALSE;
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
  if (counts[0]) mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_COUNT_SELECTED), MC_S(counts));
  else swprintf(text, 256, L"%ls", mc_wstr(MC_STR_WINDOWS_NOTHING_SELECTED));
  SetWindowTextW(g_dv.count, text);
}

/* Every control's text, value and enabled state from the loaded device and profile. The edits
 * are left alone while they have the focus, unless force. */
static void update_controls(BOOL force) {
  g_dv.filling = TRUE;
  static const wchar_t glyphs[] = {[MC_DEVICE_DESKTOP] = 0xE7F4, [MC_DEVICE_PHONE] = 0xE8EA,
                                   [MC_DEVICE_WATCH] = 0xE916};
  wchar_t glyph[2] = {(unsigned)g_dv.kind <= (unsigned)MC_DEVICE_WATCH ? glyphs[g_dv.kind] : glyphs[0], 0};
  SetWindowTextW(g_dv.icon, glyph);
  SetWindowTextW(g_dv.title, !g_dv.sync      ? mc_wstr(MC_STR_WINDOWS_SYNC_IS_OFF)
                             : g_dv.found ? g_dv.name
                                          : mc_wstr(MC_STR_WINDOWS_DEVICE_UNKNOWN));
  SetWindowTextW(g_dv.remove, mc_wstr_mnemonic(g_dv.is_self ? MC_STR_WINDOWS_LEAVE_GROUP_BUTTON
                                                            : MC_STR_WINDOWS_REMOVE_FROM_GROUP_BUTTON));
  update_status();

  if (force || GetFocus() != g_dv.name_edit) SetWindowTextW(g_dv.name_edit, g_dv.found ? g_dv.name : L"");
  EnableWindow(g_dv.name_edit, g_dv.found && !g_dv.removed && (g_dv.is_self || g_dv.profile));

  BOOL edit = editable();
  const mc_sync_profile *p = g_dv.profile;
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

  wchar_t note[256] = L"";
  if (p && p->archive) swprintf(note, 256, L"%ls", mc_wstr(MC_STR_WINDOWS_DEVICE_ARCHIVE_NOTE));
  else if (!p && g_dv.found && !g_dv.removed)
    mc_wformat(note, 256, mc_str(MC_STR_WINDOWS_PROFILE_READ_FAILED), MC_S(mc_status_str(g_dv.profile_status)));
  SetWindowTextW(g_dv.note, note);

  update_count();
  g_dv.filling = FALSE;
  /* The labels' colours follow their controls'. */
  HWND labels[] = {g_dv.format_label, g_dv.budget_label, g_dv.budget_unit, g_dv.sync_all_label};
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
      st = mc_library_list_playlists(g_dv.lib, &out->playlists);
      if (st == MC_OK && out->playlists) *n = out->playlists->count;
      break;
    default:
      st = mc_library_list_album_groups(g_dv.lib, &out->groups);
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

/* Item i's uid, and its row's text: "Artist - Title (year)" or a name, and a count. */
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
      mc_wformat(text + len, (size_t)cap - len, mc_str(MC_STR_WINDOWS_ALBUM_YEAR_SUFFIX), MC_D(al->year));
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
  if (count > 0) mc_wformat(detail, (size_t)detail_cap, mc_str_plural(MC_STR_WINDOWS_COUNT_ALBUMS, count), MC_D(count));
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
  const uid_set *sel = &g_dv.sel[g_dv.tab];
  for (int i = 0; i < n; i++) {
    item_row(&items, i, text, 512, detail, 64);
    if (same) {
      LVITEMW item = {.iSubItem = 0, .pszText = text};
      SendMessageW(g_dv.list, LVM_SETITEMTEXTW, (WPARAM)i, (LPARAM)&item);
      set_check(i, set_has(sel, rows[i]));
    } else {
      LVITEMW item = {.mask = LVIF_TEXT | LVIF_STATE, .iItem = i, .pszText = text,
                      .state = INDEXTOSTATEIMAGEMASK(set_has(sel, rows[i]) ? 2 : 1),
                      .stateMask = LVIS_STATEIMAGEMASK};
      SendMessageW(g_dv.list, LVM_INSERTITEMW, 0, (LPARAM)&item);
      set_check(i, set_has(sel, rows[i])); /* a checkbox list may insert it unchecked anyway */
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
}

/* Reloads everything, keeping the pending edits. keep: it's the same device, so the list keeps
 * its place and focused edits their text. */
static void reload(BOOL keep) {
  load_device();
  load_profile();
  update_controls(!keep);
  fill_list(keep);
  layout_again();
}

/* --- Applying edits --- */

/* Applies the pending edits now: get, change, set. */
static void flush(void) {
  KillTimer(g_dv.parent, ID_DEV_APPLY_TIMER);
  if (!pending_any() || !g_dv.sync) {
    pending_clear();
    return;
  }
  mc_sync_profile *p = NULL;
  mc_status st = mc_sync_get_profile(g_dv.sync, g_dv.uid, &p);
  if (st == MC_OK) st = apply_pending(p);
  if (st == MC_OK) st = mc_sync_set_profile(g_dv.sync, g_dv.uid, p);
  mc_sync_profile_free(p);
  pending_clear();
  if (st == MC_ERR_LOCKED) {
    set_message(mc_wstr(MC_STR_WINDOWS_PROFILE_LOCKED));
  } else if (st != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_PROFILE_SAVE_FAILED), MC_S(mc_status_str(st)));
    set_message(text);
  }
  /* What's stored now, which may include others' edits made meanwhile. */
  load_profile();
  update_controls(FALSE);
  update_checks();
  layout_again();
}

static void CALLBACK apply_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)wnd, (void)msg, (void)id, (void)time;
  flush();
}

static void schedule(void) {
  SetTimer(g_dv.parent, ID_DEV_APPLY_TIMER, APPLY_DELAY_MS, apply_timer);
}

static void CALLBACK tick_timer(HWND wnd, UINT msg, UINT_PTR id, DWORD time) {
  (void)wnd, (void)msg, (void)id, (void)time;
  if (g_dv.shown && g_dv.found) update_status();
}

static void apply_name(void) {
  if (!g_dv.sync || !g_dv.found || g_dv.removed) return;
  wchar_t buf[256], *s = buf;
  GetWindowTextW(g_dv.name_edit, buf, 256);
  while (*s == L' ' || *s == L'\t') s++;
  size_t len = wcslen(s);
  while (len && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = L'\0';
  if (!*s || !wcscmp(s, g_dv.name)) {
    SetWindowTextW(g_dv.name_edit, g_dv.name);
    return;
  }
  char utf8[4 * MC_SYNC_DEVICE_NAME_MAX + 4], stored[sizeof utf8];
  if (!WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, (int)sizeof utf8, NULL, NULL)) return;

  mc_status st;
  if (g_dv.is_self) {
    st = mc_sync_set_device_name(g_dv.sync, utf8);
  } else {
    mc_sync_profile *p = NULL;
    st = mc_sync_get_profile(g_dv.sync, g_dv.uid, &p);
    if (st == MC_OK) {
      free(p->name);
      p->name = _strdup(utf8);
      st = p->name ? mc_sync_set_profile(g_dv.sync, g_dv.uid, p) : MC_ERR_NOMEM;
    }
    mc_sync_profile_free(p);
  }
  if (st != MC_OK) {
    wchar_t text[256];
    mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_DEVICE_RENAME_FAILED), MC_S(mc_status_str(st)));
    set_message(text);
    SetWindowTextW(g_dv.name_edit, g_dv.name);
    layout_again();
    return;
  }
  /* The core may have cut it. */
  if (g_dv.is_self && mc_sync_get_device_name(g_dv.sync, stored, sizeof stored) == MC_OK) {
    widen(stored, g_dv.name, MC_SYNC_DEVICE_NAME_MAX + 1);
  } else {
    memcpy(stored, utf8, sizeof stored);
    wcsncpy(g_dv.name, s, MC_SYNC_DEVICE_NAME_MAX);
  }
  g_dv.name[MC_SYNC_DEVICE_NAME_MAX] = L'\0';
  SetWindowTextW(g_dv.title, g_dv.name);
  SetWindowTextW(g_dv.name_edit, g_dv.name);
  layout_again();
  /* Either may come back here to refresh the page. */
  device_view_host host = g_dv.host;
  if (g_dv.is_self && host.rename_self) host.rename_self(stored);
  if (host.devices_changed) host.devices_changed();
}

static void remove_device(void) {
  if (!g_dv.sync || !g_dv.found) return;
  wchar_t text[1024];
  const wchar_t *caption;
  if (g_dv.is_self) {
    caption = mc_wstr(MC_STR_LEAVE_GROUP);
    mc_wformat(text, 1024, mc_str(MC_STR_WINDOWS_LEAVE_GROUP_CONFIRM_OTHERS),
               MC_S(g_dv.others[0] ? u8(g_dv.others) : mc_str(MC_STR_WINDOWS_OTHER_DEVICES)));
  } else {
    caption = mc_wstr(MC_STR_REMOVE_FROM_GROUP);
    wchar_t title[256];
    mc_wformat(title, 256, mc_str(MC_STR_REMOVE_DEVICE_TITLE), MC_S(u8(g_dv.name)));
    swprintf(text, 1024, L"%ls\n\n%ls", title, mc_wstr(MC_STR_REMOVE_DEVICE_TEXT));
  }
  if (MessageBoxW(g_dv.parent, text, caption, MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return;
  flush();

  mc_status st;
  if (g_dv.is_self) {
    HCURSOR old = SetCursor(LoadCursorW(NULL, (LPCWSTR)IDC_WAIT)); /* blocks for a few seconds */
    st = mc_sync_leave_group(g_dv.sync);
    SetCursor(old);
  } else {
    st = mc_sync_remove_device(g_dv.sync, g_dv.uid);
  }
  if (st == MC_OK) {
    if (g_dv.host.devices_changed) g_dv.host.devices_changed();
    return;
  }
  if (st == MC_ERR_LOCKED && g_dv.is_self)
    swprintf(text, 1024, L"%ls", mc_wstr(MC_STR_WINDOWS_LEAVE_ONLY_ARCHIVE));
  else if (st == MC_ERR_LOCKED)
    mc_wformat(text, 1024, mc_str(MC_STR_WINDOWS_REMOVE_ONLY_ARCHIVE), MC_S(u8(g_dv.name)));
  else
    mc_wformat(text, 1024,
               mc_str(g_dv.is_self ? MC_STR_WINDOWS_LEAVE_GROUP_FAILED : MC_STR_WINDOWS_REMOVE_DEVICE_FAILED),
               MC_S(mc_status_str(st)));
  set_message(text);
  layout_again();
}

static void sync_now(void) {
  if (!g_dv.sync) return;
  mc_status st = mc_sync_now(g_dv.sync, g_dv.is_self ? NULL : g_dv.uid);
  wchar_t text[256] = L"";
  if (st != MC_OK) mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_SYNC_START_FAILED), MC_S(mc_status_str(st)));
  set_message(text);
  layout_again();
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

static void on_sync_all(void) {
  if (!g_dv.profile) return;
  g_dv.profile->sync_all = g_dv.pending.sync_all = is_checked(g_dv.sync_all);
  g_dv.pending.mask |= PEND_SYNC_ALL;
  layout_again();
  schedule();
}

static void on_check(int row, BOOL on) {
  if (row < 0 || row >= g_dv.nrows || g_dv.rows_tab < 0) return;
  set_put(&g_dv.sel[g_dv.rows_tab], g_dv.rows[row], on);
  pending_toggle(k_tab_kind[g_dv.rows_tab], g_dv.rows[row], on);
  update_count();
  schedule();
}

static void select_tab(int tab) {
  if (tab == g_dv.tab) return;
  g_dv.tab = tab;
  fill_list(FALSE);
  for (int i = 0; i < NUM_TABS; i++) InvalidateRect(g_dv.tabs[i], NULL, FALSE);
}

/* --- Subclasses --- */

/* Enter applies the name (or the budget straight away), Escape undoes the edit. */
static LRESULT CALLBACK edit_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  (void)data;
  switch (msg) {
    case WM_KEYDOWN:
      if (wp == VK_RETURN) {
        if (wnd == g_dv.name_edit) apply_name();
        else flush();
        return 0;
      }
      if (wp == VK_ESCAPE && wnd == g_dv.name_edit) {
        SetWindowTextW(wnd, g_dv.name);
        SendMessageW(wnd, EM_SETSEL, 0, -1);
        return 0;
      }
      break;
    case WM_CHAR:
      if (wp == L'\r' || wp == 27) return 0; /* no beep */
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

/* Hover for the segmented buttons, which owner-drawn buttons don't track themselves. */
static LRESULT CALLBACK tab_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data) {
  switch (msg) {
    case WM_MOUSEMOVE:
      if (g_dv.hot_tab != (int)data) {
        if (g_dv.hot_tab >= 0) InvalidateRect(g_dv.tabs[g_dv.hot_tab], NULL, FALSE);
        g_dv.hot_tab = (int)data;
        InvalidateRect(wnd, NULL, FALSE);
        TRACKMOUSEEVENT tme = {.cbSize = sizeof tme, .dwFlags = TME_LEAVE, .hwndTrack = wnd};
        TrackMouseEvent(&tme);
      }
      break;
    case WM_MOUSELEAVE:
      if (g_dv.hot_tab == (int)data) g_dv.hot_tab = -1;
      InvalidateRect(wnd, NULL, FALSE);
      break;
    case WM_ERASEBKGND:
      return 1;
    case WM_NCDESTROY:
      RemoveWindowSubclass(wnd, tab_proc, id);
      break;
  }
  return DefSubclassProc(wnd, msg, wp, lp);
}

/* --- Window plumbing --- */

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int id) {
  return CreateWindowExW(0, cls, text, WS_CHILD | style, 0, 0, 0, 0, g_dv.parent, (HMENU)(INT_PTR)id,
                         GetModuleHandleW(NULL), NULL);
}

void device_view_create(HWND parent, const device_view_host *host) {
  g_dv.parent = parent;
  if (host) g_dv.host = *host;
  g_dv.rows_tab = -1;
  DWORD label = SS_LEFT | SS_NOPREFIX, line = label | SS_ENDELLIPSIS;
  g_dv.icon = child(WC_STATICW, NULL, SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX, ID_DEV_ICON);
  g_dv.title = child(WC_STATICW, NULL, line, ID_DEV_TITLE);
  g_dv.sync_now = child(WC_BUTTONW, mc_wstr(MC_STR_SYNC_NOW), BS_PUSHBUTTON | WS_TABSTOP, ID_DEV_SYNC_NOW);
  g_dv.remove = child(WC_BUTTONW, mc_wstr_mnemonic(MC_STR_WINDOWS_REMOVE_FROM_GROUP_BUTTON), BS_PUSHBUTTON | WS_TABSTOP,
                      ID_DEV_REMOVE);
  g_dv.status = child(WC_STATICW, NULL, line, ID_DEV_STATUS);
  g_dv.files = child(WC_STATICW, NULL, line, ID_DEV_FILES);
  g_dv.progress = child(PROGRESS_CLASSW, NULL, 0, ID_DEV_PROGRESS);
  SendMessageW(g_dv.progress, PBM_SETRANGE32, 0, MC_SYNC_PHASE_DONE + 1);
  g_dv.progress_text = child(WC_STATICW, NULL, line, ID_DEV_PROGRESS_TEXT);
  g_dv.message = child(WC_STATICW, NULL, label, ID_DEV_MESSAGE);
  g_dv.notice = child(WC_STATICW, NULL, label, ID_DEV_NOTICE);

  g_dv.name_label = child(WC_STATICW, mc_wstr(MC_STR_NAME), line, ID_DEV_NAME_LABEL);
  /* The edits look as in Settings: a themed border, the text centred in it. */
  g_dv.name_edit = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, NULL, WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                                   0, 0, parent, (HMENU)(INT_PTR)ID_DEV_NAME, GetModuleHandleW(NULL), NULL);
  theme_center_edit(g_dv.name_edit);
  SendMessageW(g_dv.name_edit, EM_LIMITTEXT, MC_SYNC_DEVICE_NAME_MAX, 0);
  SetWindowSubclass(g_dv.name_edit, edit_proc, 0, 0);
  g_dv.format_label = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SYNC_FORMAT), line, ID_DEV_FORMAT_LABEL);
  g_dv.format = child(WC_COMBOBOXW, NULL, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_DEV_FORMAT);
  int i = (int)SendMessageW(g_dv.format, CB_ADDSTRING, 0, (LPARAM)mc_wstr(MC_STR_WINDOWS_FORMAT_AUTO));
  SendMessageW(g_dv.format, CB_SETITEMDATA, (WPARAM)i, AUTO_ITEM);
  i = (int)SendMessageW(g_dv.format, CB_ADDSTRING, 0, (LPARAM)mc_wstr(MC_STR_WINDOWS_FORMAT_ORIGINAL));
  SendMessageW(g_dv.format, CB_SETITEMDATA, (WPARAM)i, 0);
  for (int b = 0; b < MC_OPUS_BITRATE_COUNT; b++) {
    wchar_t text[32];
    mc_wformat(text, 32, mc_str(MC_STR_WINDOWS_FORMAT_OPUS), MC_D(mc_opus_bitrates[b]));
    i = (int)SendMessageW(g_dv.format, CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(g_dv.format, CB_SETITEMDATA, (WPARAM)i, (LPARAM)mc_opus_bitrates[b]);
  }
  COMBOBOXINFO info = {.cbSize = sizeof info};
  if (GetComboBoxInfo(g_dv.format, &info)) g_dv.combo_list = info.hwndList;

  /* Checkboxes have no text of their own: a themed checkbox's text stays black in dark
   * mode, so each has a label beside it, which toggles it too. */
  g_dv.budget = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_DEV_BUDGET);
  g_dv.budget_label = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_BUDGET_LIMIT), line | SS_NOTIFY, ID_DEV_BUDGET_LABEL);
  g_dv.budget_size = CreateWindowExW(WS_EX_CLIENTEDGE, WC_EDITW, NULL, WS_CHILD | WS_TABSTOP | ES_NUMBER | ES_RIGHT,
                                     0, 0, 0, 0, parent, (HMENU)(INT_PTR)ID_DEV_BUDGET_SIZE, GetModuleHandleW(NULL),
                                     NULL);
  theme_center_edit(g_dv.budget_size);
  SendMessageW(g_dv.budget_size, EM_LIMITTEXT, 5, 0);
  SetWindowSubclass(g_dv.budget_size, edit_proc, 0, 0);
  g_dv.budget_unit = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_BUDGET_UNIT), line, ID_DEV_BUDGET_UNIT);

  g_dv.heading = child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_WHAT_TO_SYNC), line, ID_DEV_HEADING);
  g_dv.note = child(WC_STATICW, NULL, label, ID_DEV_NOTE);
  g_dv.sync_all = child(WC_BUTTONW, NULL, BS_AUTOCHECKBOX | WS_TABSTOP, ID_DEV_SYNC_ALL);
  g_dv.sync_all_label =
    child(WC_STATICW, mc_wstr(MC_STR_WINDOWS_SYNC_EVERYTHING), line | SS_NOTIFY, ID_DEV_SYNC_ALL_LABEL);
  for (int t = 0; t < NUM_TABS; t++) {
    g_dv.tabs[t] = child(WC_BUTTONW, mc_wstr(k_tab_name[t]), BS_OWNERDRAW | WS_TABSTOP, ID_DEV_TAB + t);
    SetWindowSubclass(g_dv.tabs[t], tab_proc, 0, (DWORD_PTR)t);
  }

  g_dv.list = child(WC_LISTVIEWW, NULL, LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | WS_BORDER | WS_TABSTOP,
                    ID_DEV_LIST);
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
  g_dv.count = child(WC_STATICW, NULL, line, ID_DEV_COUNT);

  SetTimer(parent, ID_DEV_TICK_TIMER, TICK_MS, tick_timer);
}

void device_view_destroy(void) {
  KillTimer(g_dv.parent, ID_DEV_APPLY_TIMER);
  KillTimer(g_dv.parent, ID_DEV_TICK_TIMER);
  KillTimer(g_dv.parent, ID_DEV_REFRESH_TIMER);
  pending_clear();
  free(g_dv.pending.toggles);
  g_dv.pending.toggles = NULL;
  g_dv.pending.cap = 0;
  free_profile();
  for (int i = 0; i < NUM_TABS; i++) {
    free(g_dv.sel[i].uids);
    g_dv.sel[i] = (uid_set){0};
  }
  free(g_dv.rows);
  g_dv.rows = NULL;
  g_dv.nrows = 0;
  g_dv.sync = NULL;
  g_dv.lib = NULL;
  HFONT fonts[] = {g_dv.title_font, g_dv.heading_font, g_dv.icon_font};
  for (size_t i = 0; i < sizeof fonts / sizeof fonts[0]; i++)
    if (fonts[i]) DeleteObject(fonts[i]);
  g_dv.title_font = g_dv.heading_font = g_dv.icon_font = NULL;
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
  lf = ncm.lfMessageFont;
  lf.lfHeight = MulDiv(lf.lfHeight, 14, 9); /* subtitle */
  lf.lfWeight = FW_SEMIBOLD;
  if (g_dv.heading_font) DeleteObject(g_dv.heading_font);
  g_dv.heading_font = CreateFontIndirectW(&lf);

  SendMessageW(g_dv.title, WM_SETFONT, (WPARAM)g_dv.title_font, TRUE);
  SendMessageW(g_dv.icon, WM_SETFONT, (WPARAM)g_dv.icon_font, TRUE);
  SendMessageW(g_dv.heading, WM_SETFONT, (WPARAM)g_dv.heading_font, TRUE);
  HWND body[] = {g_dv.sync_now, g_dv.remove, g_dv.status, g_dv.files, g_dv.progress_text, g_dv.message,
                 g_dv.notice, g_dv.name_label, g_dv.name_edit, g_dv.format_label, g_dv.format, g_dv.budget_label,
                 g_dv.budget_size, g_dv.budget_unit, g_dv.note, g_dv.sync_all_label, g_dv.list, g_dv.count};
  for (size_t i = 0; i < sizeof body / sizeof body[0]; i++) SendMessageW(body[i], WM_SETFONT, (WPARAM)font, TRUE);
  for (int i = 0; i < NUM_TABS; i++) InvalidateRect(g_dv.tabs[i], NULL, FALSE);
  set_check_images();
}

static HWND *all_controls(size_t *n) {
  static HWND all[32];
  HWND list[] = {g_dv.icon, g_dv.title, g_dv.sync_now, g_dv.remove, g_dv.status, g_dv.files, g_dv.progress,
                 g_dv.progress_text, g_dv.message, g_dv.notice, g_dv.name_label, g_dv.name_edit, g_dv.format_label,
                 g_dv.format, g_dv.budget, g_dv.budget_label, g_dv.budget_size, g_dv.budget_unit, g_dv.heading,
                 g_dv.note, g_dv.sync_all, g_dv.sync_all_label, g_dv.tabs[0], g_dv.tabs[1], g_dv.tabs[2],
                 g_dv.tabs[3], g_dv.list, g_dv.count};
  memcpy(all, list, sizeof list);
  *n = sizeof list / sizeof list[0];
  return all;
}

void device_view_show(BOOL show) {
  g_dv.shown = show;
  if (!show) {
    size_t n;
    HWND *all = all_controls(&n);
    for (size_t i = 0; i < n; i++) ShowWindow(all[i], SW_HIDE);
  } else {
    if (g_dv.found) update_status(); /* the times have moved on */
    layout_again();
  }
}

/* The height wnd's text needs at width w, wrapped as its static control wraps it. */
static int text_height(HWND wnd, HFONT font, int w) {
  wchar_t buf[1024];
  int n = GetWindowTextW(wnd, buf, 1024);
  HDC dc = GetDC(wnd);
  HGDIOBJ old = SelectObject(dc, font);
  RECT rc = {0, 0, w, 0};
  DrawTextW(dc, n ? buf : L" ", n ? n : 1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
  SelectObject(dc, old);
  ReleaseDC(wnd, dc);
  return rc.bottom;
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
 * was and where it goes are repainted, so it leaves nothing behind. */
static void move_to(HWND wnd, int x, int y, int w, int h, int drop_h) {
  RECT old, want = {x, y, x + w, y + h};
  GetWindowRect(wnd, &old);
  MapWindowPoints(NULL, g_dv.parent, (POINT *)&old, 2);
  if (h < 0) want.bottom = y + (old.bottom - old.top);
  if (EqualRect(&old, &want)) return;
  MoveWindow(wnd, x, y, w, h >= 0 ? h : drop_h, FALSE);
  UINT flags = RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN;
  if (IsWindowVisible(wnd)) RedrawWindow(g_dv.parent, &old, NULL, flags);
  GetWindowRect(wnd, &want);
  MapWindowPoints(NULL, g_dv.parent, (POINT *)&want, 2);
  RedrawWindow(g_dv.parent, &want, NULL, flags);
}

static void set_shown(HWND wnd, BOOL visible) {
  if (!(GetWindowLongW(wnd, GWL_STYLE) & WS_VISIBLE) != !visible) ShowWindow(wnd, visible ? SW_SHOW : SW_HIDE);
}

static void place(HWND wnd, BOOL visible, int x, int y, int w, int h) {
  if (visible) move_to(wnd, x, y, w, h, 0);
  set_shown(wnd, visible);
}

static void set_column_width(int col, int w) {
  if (ListView_GetColumnWidth(g_dv.list, col) != w) ListView_SetColumnWidth(g_dv.list, col, w);
}

void device_view_layout(int x, int y, int w, int h, UINT dpi) {
  g_dv.x = x, g_dv.y = y, g_dv.w = w, g_dv.h = h, g_dv.dpi = dpi;
  if (!g_dv.shown || !g_dv.font) return;
#define S(px) MulDiv((px), (int)dpi, USER_DEFAULT_SCREEN_DPI)
  int pad = S(32), text_w = min(w - 2 * pad, S(720));
  int left = x + pad, top = y + S(24), line_h = text_height(g_dv.status, g_dv.font, S(1000));
  BOOL on = g_dv.sync != NULL, dev = on && g_dv.found;

  /* Header: icon, name, buttons. */
  int title_h = text_height(g_dv.title, g_dv.title_font, S(1000)), icon_w = S(40);
  int btn_h = S(32), sync_w = text_width(g_dv.sync_now, g_dv.font) + S(32);
  int remove_w = text_width(g_dv.remove, g_dv.font) + S(32);
  int head_h = max(title_h, btn_h), right = left + text_w;
  place(g_dv.icon, dev, left, top, icon_w, head_h);
  place(g_dv.remove, dev, right - remove_w, top + (head_h - btn_h) / 2, remove_w, btn_h);
  place(g_dv.sync_now, dev, right - remove_w - S(8) - sync_w, top + (head_h - btn_h) / 2, sync_w, btn_h);
  int title_x = dev ? left + icon_w + S(8) : left, title_r = dev ? right - remove_w - sync_w - S(24) : right;
  place(g_dv.title, TRUE, title_x, top + (head_h - title_h) / 2, max(title_r - title_x, S(40)), title_h);
  top += head_h + S(4);

  /* Status lines. While syncing, the progress takes the status line's place, so the page
   * doesn't move. */
  BOOL busy = dev && syncing();
  int bar_w = S(120);
  place(g_dv.status, on && !busy, left, top, text_w, line_h);
  place(g_dv.progress, busy, left, top + (line_h - S(6)) / 2, bar_w, S(6));
  place(g_dv.progress_text, busy, left + bar_w + S(12), top, text_w - bar_w - S(12), line_h);
  if (on) top += line_h + S(2);
  BOOL files = dev && has_text(g_dv.files);
  place(g_dv.files, files, left, top, text_w, line_h);
  if (files) top += line_h + S(2);
  BOOL message = dev && has_text(g_dv.message);
  int mh = message ? text_height(g_dv.message, g_dv.font, text_w) : 0;
  place(g_dv.message, message, left, top + S(4), text_w, mh);
  if (message) top += mh + S(6);
  BOOL notice = dev && has_text(g_dv.notice);
  int nh = notice ? text_height(g_dv.notice, g_dv.font, text_w) : 0;
  place(g_dv.notice, notice, left, top + S(4), text_w, nh);
  if (notice) top += nh + S(6);
  top += S(16);

  /* Name, format and budget. */
  int label_w = max(text_width(g_dv.name_label, g_dv.font), text_width(g_dv.format_label, g_dv.font)) + S(16);
  int field_h = S(30), edit_h = S(28), field_x = left + label_w;
  place(g_dv.name_label, dev, left, top + (field_h - line_h) / 2, label_w, line_h);
  place(g_dv.name_edit, dev, field_x, top + (field_h - edit_h) / 2, min(S(320), right - field_x), edit_h);
  if (dev) top += field_h + S(8);

  /* Left alone while it's dropped down. */
  int combo_w = S(180);
  if (dev && !SendMessageW(g_dv.format, CB_GETDROPPEDSTATE, 0, 0)) {
    RECT rc;
    GetWindowRect(g_dv.format, &rc);
    int ch = rc.bottom - rc.top;
    move_to(g_dv.format, field_x, top + (ch > 0 && ch < field_h ? (field_h - ch) / 2 : 0), combo_w, -1, S(300));
  }
  set_shown(g_dv.format, dev);
  place(g_dv.format_label, dev, left, top + (field_h - line_h) / 2, label_w, line_h);
  int box = S(20), budget_label_w = text_width(g_dv.budget_label, g_dv.font) + S(8);
  int unit_w = text_width(g_dv.budget_unit, g_dv.font) + S(4), size_w = S(64);
  int budget_w = box + S(8) + budget_label_w + size_w + S(8) + unit_w;
  int bx = field_x + combo_w + S(24);
  if (bx + budget_w > right) { /* no room beside the format: on a line of its own */
    top += field_h + S(8);
    bx = field_x;
  }
  place(g_dv.budget, dev, bx, top + (field_h - box) / 2, box, box);
  bx += box + S(8);
  place(g_dv.budget_label, dev, bx, top + (field_h - line_h) / 2, budget_label_w, line_h);
  bx += budget_label_w;
  place(g_dv.budget_size, dev, bx, top + (field_h - edit_h) / 2, size_w, edit_h);
  bx += size_w + S(8);
  place(g_dv.budget_unit, dev, bx, top + (field_h - line_h) / 2, unit_w, line_h);
  if (dev) top += field_h + S(24);

  /* What to sync. */
  int hh = text_height(g_dv.heading, g_dv.heading_font, text_w);
  place(g_dv.heading, dev, left, top, text_w, hh);
  if (dev) top += hh + S(8);
  BOOL note = dev && has_text(g_dv.note);
  int noteh = note ? text_height(g_dv.note, g_dv.font, text_w) : 0;
  place(g_dv.note, note, left, top, text_w, noteh);
  if (note) top += noteh + S(8);
  int row_h = max(box, line_h);
  place(g_dv.sync_all, dev, left, top + (row_h - box) / 2, box, box);
  place(g_dv.sync_all_label, dev, left + box + S(8), top + (row_h - line_h) / 2,
        text_width(g_dv.sync_all_label, g_dv.font) + S(8), line_h);
  if (dev) top += row_h + S(12);

  BOOL pick = dev && g_dv.profile && !g_dv.profile->sync_all;
  int tab_w = 0;
  for (int i = 0; i < NUM_TABS; i++) tab_w = max(tab_w, text_width(g_dv.tabs[i], g_dv.font) + S(32));
  tab_w = min(tab_w, text_w / NUM_TABS);
  for (int i = 0; i < NUM_TABS; i++) place(g_dv.tabs[i], pick, left + i * tab_w, top, tab_w, btn_h);
  if (pick) top += btn_h + S(8);

  int count_y = y + h - S(24) - line_h;
  place(g_dv.count, pick, left, count_y, text_w, line_h);
  int list_h = max(count_y - S(8) - top, S(80));
  place(g_dv.list, pick, left, top, text_w, list_h);
  if (pick) {
    RECT rc;
    GetClientRect(g_dv.list, &rc);
    int detail_w = S(110);
    set_column_width(1, detail_w);
    set_column_width(0, max(rc.right - detail_w, S(80)));
  }
#undef S
}

static void layout_again(void) {
  if (g_dv.shown) device_view_layout(g_dv.x, g_dv.y, g_dv.w, g_dv.h, g_dv.dpi);
}

void device_view_open(mc_sync *sync, mc_library *lib, const char *uid) {
  /* Edits still waiting go to the device they were made for, while its sync is still
   * there: opening with NULL comes before sync stops. */
  if (g_dv.sync && (sync == g_dv.sync || !sync)) {
    if (GetFocus() == g_dv.name_edit) apply_name();
    flush();
  }
  KillTimer(g_dv.parent, ID_DEV_APPLY_TIMER);
  pending_clear();
  BOOL same = sync && sync == g_dv.sync && uid && !strcmp(uid, g_dv.uid);
  g_dv.sync = sync;
  g_dv.lib = lib;
  if (!same) {
    snprintf(g_dv.uid, sizeof g_dv.uid, "%s", uid && sync ? uid : "");
    g_dv.nbusy = 0;
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

void device_view_refresh(void) {
  if (!g_dv.sync) return;
  /* Not under an open drop-down, which would lose what's highlighted in it: once it
   * closes. */
  if (SendMessageW(g_dv.format, CB_GETDROPPEDSTATE, 0, 0)) {
    g_dv.refresh_later = TRUE;
    return;
  }
  g_dv.refresh_later = FALSE;
  reload(TRUE);
}

static int busy_index(const char *uid) {
  for (int i = 0; i < g_dv.nbusy; i++)
    if (!strcmp(g_dv.busy[i], uid)) return i;
  return -1;
}

void device_view_on_sync_event(const sync_event *ev) {
  if (!g_dv.sync || !g_dv.found || !ev->uid[0]) return;
  if (!g_dv.is_self && strcmp(ev->uid, g_dv.uid)) return;
  int i = busy_index(ev->uid);
  switch (ev->kind) {
    case SYNC_EV_PEER:
      if (g_dv.is_self) return;
      g_dv.online = ev->online;
      update_status();
      break;
    case SYNC_EV_PROGRESS:
      if (ev->phase == MC_SYNC_PHASE_DONE) { /* over; on_sync_done may follow, or not */
        if (i < 0) return;
        memmove(g_dv.busy + i, g_dv.busy + i + 1, (size_t)(g_dv.nbusy - i - 1) * sizeof g_dv.busy[0]);
        g_dv.nbusy--;
        update_status();
        break;
      }
      if (i < 0 && g_dv.nbusy < MAX_BUSY) {
        memcpy(g_dv.busy[g_dv.nbusy], ev->uid, MC_UID_STRLEN);
        g_dv.busy[g_dv.nbusy++][MC_UID_STRLEN - 1] = '\0';
      }
      g_dv.phase = ev->phase;
      g_dv.sent = ev->rows_sent;
      g_dv.received = ev->rows_received;
      if (i < 0) set_message(NULL);
      update_status();
      break;
    case SYNC_EV_DONE: {
      if (i >= 0) {
        memmove(g_dv.busy + i, g_dv.busy + i + 1, (size_t)(g_dv.nbusy - i - 1) * sizeof g_dv.busy[0]);
        g_dv.nbusy--;
      }
      wchar_t text[256] = L"";
      if (ev->status != MC_OK) {
        if (g_dv.is_self && ev->name[0])
          mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_SYNC_FAILED_WITH), MC_S(u8(ev->name)),
                     MC_S(mc_status_str(ev->status)));
        else mc_wformat(text, 256, mc_str(MC_STR_WINDOWS_SYNC_FAILED), MC_S(mc_status_str(ev->status)));
      } else if (!ev->library_synced) {
        swprintf(text, 256, L"%ls", mc_wstr(MC_STR_WINDOWS_SYNC_DEVICE_LIST_ONLY));
      }
      set_message(text);
      /* The last sync time, its files, maybe its profile, have changed. */
      reload(TRUE);
      return;
    }
    default:
      return;
  }
  layout_again();
}

BOOL device_view_on_command(int id, int code) {
  switch (id) {
    case ID_DEV_SYNC_NOW:
      if (code == BN_CLICKED) sync_now();
      return TRUE;
    case ID_DEV_REMOVE:
      if (code == BN_CLICKED) remove_device();
      return TRUE;
    case ID_DEV_NAME:
      if (code == EN_KILLFOCUS) apply_name();
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
    case ID_DEV_SYNC_ALL_LABEL:
      if (code != STN_CLICKED || !IsWindowEnabled(g_dv.sync_all)) return TRUE;
      set_checked(g_dv.sync_all, !is_checked(g_dv.sync_all));
      on_sync_all();
      return TRUE;
    case ID_DEV_SYNC_ALL:
      if (code == BN_CLICKED && !g_dv.filling) on_sync_all();
      return TRUE;
    default:
      if (id >= ID_DEV_TAB && id < ID_DEV_TAB + NUM_TABS) {
        if (code == BN_CLICKED) select_tab(id - ID_DEV_TAB);
        return TRUE;
      }
      return id >= ID_DEV_ICON && id <= ID_DEV_COUNT;
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
      static const enum mc_str_id none[NUM_TABS] = {MC_STR_WINDOWS_NO_ALBUMS, MC_STR_WINDOWS_NO_ARTISTS,
                                                    MC_STR_WINDOWS_NO_PLAYLISTS, MC_STR_WINDOWS_NO_ALBUM_GROUPS};
      NMLVEMPTYMARKUP *em = (NMLVEMPTYMARKUP *)nm;
      em->dwFlags = EMF_CENTERED;
      if (g_dv.rows_status == MC_OK) swprintf(em->szMarkup, L_MAX_URL_LENGTH, L"%ls", mc_wstr(none[g_dv.tab]));
      else
        mc_wformat(em->szMarkup, L_MAX_URL_LENGTH, mc_str(MC_STR_WINDOWS_LIST_FAILED),
                   MC_S(mc_status_str(g_dv.rows_status)));
      *result = TRUE;
      break;
    }
  }
  return TRUE;
}

BOOL device_view_draw_item(const DRAWITEMSTRUCT *dis) {
  int t = (int)dis->CtlID - ID_DEV_TAB;
  if (t < 0 || t >= NUM_TABS) return FALSE;
  RECT rc = {0, 0, dis->rcItem.right - dis->rcItem.left, dis->rcItem.bottom - dis->rcItem.top};
  canvas c = {0};
  if (!canvas_reserve(&c, rc.right, rc.bottom)) return TRUE;
  control_state state = dis->itemState & ODS_SELECTED ? STATE_PRESSED : g_dv.hot_tab == t ? STATE_HOT : STATE_NORMAL;
  int radius = MulDiv(4, (int)g_dv.dpi, USER_DEFAULT_SCREEN_DPI);
  theme_draw_segment(&c, &rc, radius, mc_wstr(k_tab_name[t]), g_dv.font, t == 0, t == NUM_TABS - 1, t == g_dv.tab,
                     state);
  if (dis->itemState & ODS_DISABLED) canvas_fill(&c, &rc, 0, theme.bg, 128);
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
      if (wnd != g_dv.name_edit && wnd != g_dv.budget_size && wnd != g_dv.format) return NULL;
      return theme_ctlcolor(msg, dc);
    case WM_CTLCOLORBTN:
      if (wnd != g_dv.sync_now && wnd != g_dv.remove && wnd != g_dv.budget && wnd != g_dv.sync_all) return NULL;
      SetBkColor(dc, theme.bg);
      return theme.bg_brush;
    case WM_CTLCOLORSTATIC:
      break;
    default:
      return NULL;
  }
  /* Disabled edits ask as statics. They lose the field colour too, as the disabled combo box
   * does, so they don't look editable. */
  if (wnd == g_dv.name_edit || wnd == g_dv.budget_size) {
    COLORREF bg = theme.dark || theme.high_contrast ? theme.bg : theme.frame;
    SetTextColor(dc, theme.text3);
    SetBkColor(dc, bg);
    SetDCBrushColor(dc, bg);
    return GetStockObject(DC_BRUSH);
  }
  COLORREF color;
  if (wnd == g_dv.title || wnd == g_dv.icon || wnd == g_dv.heading || wnd == g_dv.message || wnd == g_dv.notice ||
      wnd == g_dv.name_label)
    color = theme.text;
  else if (wnd == g_dv.status || wnd == g_dv.files || wnd == g_dv.progress_text || wnd == g_dv.note ||
           wnd == g_dv.count)
    color = theme.text2;
  else if (wnd == g_dv.format_label)
    color = IsWindowEnabled(g_dv.format) ? theme.text : theme.text3;
  else if (wnd == g_dv.budget_label || wnd == g_dv.budget_unit)
    color = IsWindowEnabled(g_dv.budget) ? theme.text : theme.text3;
  else if (wnd == g_dv.sync_all_label)
    color = IsWindowEnabled(g_dv.sync_all) ? theme.text : theme.text3;
  else if (wnd == g_dv.budget || wnd == g_dv.sync_all)
    color = theme.text;
  else
    return NULL;
  SetTextColor(dc, color);
  SetBkColor(dc, theme.bg);
  return theme.bg_brush;
}
