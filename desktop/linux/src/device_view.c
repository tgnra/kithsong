#include "device_view.h"

#include <stdio.h>
#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "device_view.c needs libadwaita 1.5 or later"
#endif

#define APPLY_DELAY_MS 400
#define ESTIMATE_DELAY_MS 250 /* a read of every track: once the edits pause */
#define TICK_S 30             /* "synced 3 minutes ago" moves on */
#define FINISHED_MS 5000      /* a finished sync's full bar stays this long */
#define DEFAULT_BUDGET_GB 32
#define GIB ((int64_t)1024 * 1024 * 1024)
#define MAX_BUSY 16
#define MAX_ADDRESSES 8

/* The format row's items: Auto, Original files, then each of mc_opus_bitrates. */
enum { FORMAT_AUTO, FORMAT_ORIGINAL, FORMAT_FIRST_OPUS };

enum { TAB_ALBUMS, TAB_ARTISTS, TAB_PLAYLISTS, TAB_GROUPS, NUM_TABS };
static const mc_profile_item_kind k_tab_kind[NUM_TABS] = {MC_PROFILE_ALBUM, MC_PROFILE_ARTIST, MC_PROFILE_PLAYLIST,
                                                          MC_PROFILE_ALBUM_GROUP};
static const enum mc_str_id k_tab_name[NUM_TABS] = {MC_STR_SECTION_ALBUMS, MC_STR_SECTION_ARTISTS,
                                                    MC_STR_SECTION_PLAYLISTS, MC_STR_SECTION_ALBUM_GROUPS};
/* A selection's count of each tab's items, "3 albums". */
static const enum mc_str_id k_tab_count[NUM_TABS] = {MC_STR_COUNT_ALBUMS, MC_STR_COUNT_ARTISTS,
                                                     MC_STR_COUNT_PLAYLISTS, MC_STR_COUNT_ALBUM_GROUPS};
static const enum mc_str_id k_tab_none[NUM_TABS] = {MC_STR_NO_ALBUMS, MC_STR_NO_ARTISTS, MC_STR_NO_PLAYLISTS,
                                                    MC_STR_NO_ALBUM_GROUPS};

/* Which fields of the profile have edits waiting to be applied. */
enum { PEND_FORMAT = 1, PEND_BUDGET = 2, PEND_SYNC_ALL = 4 };

typedef struct toggle {
  mc_profile_item_kind kind;
  char uid[MC_UID_STRLEN];
  gboolean on;
} toggle;

/* --- A row of the selection list --- */

#define DV_TYPE_ITEM (dv_item_get_type())
G_DECLARE_FINAL_TYPE(DvItem, dv_item, DV, ITEM, GObject)

struct _DvItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *text, *detail;
};

G_DEFINE_FINAL_TYPE(DvItem, dv_item, G_TYPE_OBJECT)

static void dv_item_finalize(GObject *obj) {
  DvItem *self = DV_ITEM(obj);
  g_free(self->text);
  g_free(self->detail);
  G_OBJECT_CLASS(dv_item_parent_class)->finalize(obj);
}

static void dv_item_class_init(DvItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = dv_item_finalize;
}

static void dv_item_init(DvItem *self) {
  (void)self;
}

static DvItem *dv_item_new(const char *uid, const char *text, const char *detail) {
  DvItem *item = g_object_new(DV_TYPE_ITEM, NULL);
  g_strlcpy(item->uid, uid, sizeof item->uid);
  item->text = g_strdup(text);
  item->detail = g_strdup(detail);
  return item;
}

/* --- State --- */

static struct {
  device_view_callbacks cb;
  GtkWidget *root; /* NULL once destroyed */
  AdwToastOverlay *toasts;
  AdwBanner *banner;
  GtkWidget *icon, *title, *status_stack, *status, *progress_box, *progress, *progress_text, *message;
  GtkWidget *sync_now, *remove;
  GtkWidget *info_group, *files_row, *address_row, *reach_row, *reach_check;
  GtkWidget *profile_group, *name_row, *archive_row, *format_row, *budget_row, *budget_switch, *budget_spin;
  GtkWidget *what_group, *sync_all_row, *picker, *tabs[NUM_TABS], *list_stack, *empty, *count;
  GListStore *store;
  gboolean filling; /* ignore the controls' signals while we set them */

  mc_sync *sync;
  mc_library *lib;
  char uid[MC_UID_STRLEN];

  /* The device, from mc_sync_list_devices(). solo: this computer, not in a group, so found
   * with what it knows of itself. */
  gboolean found, is_self, solo;
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  char *others; /* the other members' names, for Leave group */
  char *address; /* where it was last seen, "addr, port n"; NULL if never */
  mc_device_kind kind;
  int online, archive, removed, has_status;
  int64_t last_sync_ms, expected, ok, missing, corrupt, bytes;
  mc_sync_lib_state lib_state;
  mc_sync_confirm_state confirm; /* files sync on their own only once confirmed */

  /* Its profile, with the pending edits applied; NULL if it couldn't be read. */
  mc_sync_profile *profile;
  mc_status profile_status;
  GHashTable *sel[NUM_TABS]; /* the selected uids of each kind */

  int tab, rows_tab; /* rows_tab: the tab the list holds, -1 for none */
  mc_status rows_status;

  /* Edits not applied yet. */
  struct {
    unsigned mask;
    mc_transcode_opts format;
    int budget_enabled;
    int64_t budget_bytes;
    int sync_all;
    GArray *toggles; /* of toggle */
  } pending;
  guint apply_source, estimate_source, tick_source, finished_source;

  /* Sessions in progress with this device (with any member, on this computer's own page). */
  char busy[MAX_BUSY][MC_UID_STRLEN];
  int nbusy;
  mc_sync_phase phase;
  int64_t sent, received;
  int files_done, files_total; /* the files this computer is fetching */
  int64_t bytes_received, bytes_sent;
  /* The last session ended and none is running: the bar shows full, with finished_text, for
   * FINISHED_MS. */
  gboolean finished;
  char finished_text[256];

  /* The profile's size estimate, worked out on a thread of its own: one at a time. */
  struct {
    GThread *thread; /* the one running, or NULL */
    unsigned seq;    /* of the last one started */
    gboolean again;  /* the profile changed while it ran */
    GThread *archive; /* the archive confirmation's, or NULL */
  } est;
} g_dv = {.rows_tab = -1};

static void estimate_request(void);
static void schedule(void);

/* --- Text helpers --- */

/* n with the locale's digit grouping: 1,204. */
static const char *fmt_count(int64_t n, char *out, size_t cap) {
  snprintf(out, cap, "%'lld", (long long)n);
  return out;
}

static const char *fmt_size(int64_t bytes, char *out, size_t cap) {
  static const enum mc_str_id units[] = {MC_STR_COUNT, MC_STR_SIZE_KB, MC_STR_SIZE_MB, MC_STR_SIZE_GB,
                                         MC_STR_SIZE_TB};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024.0 && u < 4) v /= 1024.0, u++;
  if (u == 0) {
    mc_format(out, cap, mc_str_plural(MC_STR_SIZE_BYTES, (long)bytes), MC_D(bytes));
  } else {
    char num[32];
    snprintf(num, sizeof num, v < 100.0 ? "%.1f" : "%.0f", v);
    mc_format(out, cap, mc_str(units[u]), MC_S(num));
  }
  return out;
}

/* "just now", "3 minutes ago", "yesterday", "on 12/03/2026". */
static const char *fmt_ago(int64_t ms, char *out, size_t cap) {
  int64_t min = (g_get_real_time() / 1000 - ms) / 60000;
  if (min < 1) {
    g_strlcpy(out, mc_str(MC_STR_SYNCED_JUST_NOW), cap);
  } else if (min < 60) {
    mc_format(out, cap, mc_str_plural(MC_STR_AGO_MINUTES, (long)min), MC_D(min));
  } else if (min < 24 * 60) {
    mc_format(out, cap, mc_str_plural(MC_STR_AGO_HOURS, (long)(min / 60)), MC_D(min / 60));
  } else if (min < 2 * 24 * 60) {
    g_strlcpy(out, mc_str(MC_STR_AGO_YESTERDAY), cap);
  } else if (min < 7 * 24 * 60) {
    int days = (int)(min / (24 * 60));
    mc_format(out, cap, mc_str_plural(MC_STR_AGO_DAYS, days), MC_D(days));
  } else {
    GDateTime *dt = g_date_time_new_from_unix_local(ms / 1000);
    char *date = dt ? g_date_time_format(dt, "%x") : NULL;
    mc_format(out, cap, mc_str(MC_STR_AGO_ON_DATE), MC_S(date ? date : ""));
    g_free(date);
    if (dt) g_date_time_unref(dt);
  }
  return out;
}

static void set_message(const char *text) {
  gtk_label_set_text(GTK_LABEL(g_dv.message), text ? text : "");
  gtk_widget_set_visible(g_dv.message, text && *text);
}

static void toast(const char *text) {
  if (!g_dv.toasts || !text || !*text) return;
  char *escaped = g_markup_escape_text(text, -1);
  AdwToast *t = adw_toast_new(escaped);
  adw_toast_set_timeout(t, 6);
  adw_toast_overlay_add_toast(g_dv.toasts, t);
  g_free(escaped);
}

/* A toast for a failed edit: "Couldn't save the changes (locked)." */
static void toast_status(enum mc_str_id id, mc_status st) {
  char text[256];
  mc_format(text, sizeof text, mc_str(id), MC_S(mc_status_str(st)));
  toast(text);
}

/* --- Selection sets --- */

static int tab_of_kind(mc_profile_item_kind kind) {
  for (int i = 0; i < NUM_TABS; i++)
    if (k_tab_kind[i] == kind) return i;
  return -1;
}

static void build_sel(void) {
  for (int i = 0; i < NUM_TABS; i++) g_hash_table_remove_all(g_dv.sel[i]);
  if (!g_dv.profile) return;
  for (int i = 0; i < g_dv.profile->count; i++) {
    int t = tab_of_kind(g_dv.profile->items[i].kind);
    if (t >= 0) g_hash_table_add(g_dv.sel[t], g_strdup(g_dv.profile->items[i].ref_uid));
  }
}

static gboolean sel_has(int tab, const char *uid) {
  return tab >= 0 && g_hash_table_contains(g_dv.sel[tab], uid);
}

/* --- Pending edits --- */

static gboolean pending_any(void) {
  return g_dv.pending.mask || g_dv.pending.toggles->len;
}

static void pending_clear(void) {
  g_dv.pending.mask = 0;
  g_array_set_size(g_dv.pending.toggles, 0);
}

static void pending_toggle(mc_profile_item_kind kind, const char *uid, gboolean on) {
  for (guint i = 0; i < g_dv.pending.toggles->len; i++) {
    toggle *t = &g_array_index(g_dv.pending.toggles, toggle, i);
    if (t->kind == kind && !strcmp(t->uid, uid)) {
      t->on = on;
      return;
    }
  }
  toggle t = {.kind = kind, .on = on};
  g_strlcpy(t.uid, uid, sizeof t.uid);
  g_array_append_val(g_dv.pending.toggles, t);
}

/* Applies the pending edits to p, read just before. */
static mc_status apply_pending(mc_sync_profile *p) {
  if (g_dv.pending.mask & PEND_FORMAT) p->format = g_dv.pending.format;
  if (g_dv.pending.mask & PEND_BUDGET) {
    p->budget_enabled = g_dv.pending.budget_enabled;
    p->budget_bytes = g_dv.pending.budget_bytes;
  }
  if (g_dv.pending.mask & PEND_SYNC_ALL) p->sync_all = g_dv.pending.sync_all;
  guint nt = g_dv.pending.toggles->len;
  if (!nt) return MC_OK;

  /* The items, less every toggled one, then the toggled-on ones. p's items were malloc'd by
   * the core and are freed with the profile, so the new array is malloc'd too. */
  mc_profile_item *items = malloc(((size_t)p->count + nt) * sizeof *items);
  if (!items) return MC_ERR_NOMEM;
  int n = 0;
  for (int i = 0; i < p->count; i++) {
    gboolean toggled = FALSE;
    for (guint j = 0; j < nt && !toggled; j++) {
      const toggle *t = &g_array_index(g_dv.pending.toggles, toggle, j);
      toggled = t->kind == p->items[i].kind && !strcmp(t->uid, p->items[i].ref_uid);
    }
    if (!toggled) items[n++] = p->items[i];
  }
  for (guint j = 0; j < nt; j++) {
    const toggle *t = &g_array_index(g_dv.pending.toggles, toggle, j);
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

/* "addr, port n", or for this computer its own addresses; g_free it. NULL if none. */
static char *self_address(void) {
  char addrs[MAX_ADDRESSES][MC_SYNC_ADDR_STRLEN];
  int n = mc_sync_local_addresses(addrs, MAX_ADDRESSES);
  if (n <= 0) return NULL;
  GString *list = g_string_new(NULL);
  for (int i = 0; i < n; i++) {
    if (i) g_string_append(list, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
    g_string_append(list, addrs[i]);
  }
  char *text = mc_format_alloc(mc_str(MC_STR_DEVICE_ADDRESS_PORT), MC_S(list->str), MC_D(mc_sync_port(g_dv.sync)));
  char *out = g_strdup(text);
  mc_free(text);
  g_string_free(list, TRUE);
  return out;
}

/* Reads the device's row from the device list. */
static void load_device(void) {
  g_dv.found = FALSE;
  g_clear_pointer(&g_dv.others, g_free);
  g_clear_pointer(&g_dv.address, g_free);
  g_dv.lib_state = MC_SYNC_LIBRARY_GROUP;
  if (!g_dv.sync) return;
  g_dv.is_self = !strcmp(g_dv.uid, mc_sync_device_uid(g_dv.sync));
  mc_sync_device_list *list = NULL;
  if (mc_sync_list_devices(g_dv.sync, &list) == MC_OK && list) {
    GString *others = g_string_new(NULL);
    for (int i = 0; i < list->count; i++) {
      const mc_sync_device *d = &list->items[i];
      if (!strcmp(d->uid, g_dv.uid)) {
        g_dv.found = TRUE;
        g_strlcpy(g_dv.name, d->name ? d->name : "", sizeof g_dv.name);
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
        g_dv.confirm = d->confirm;
        if (d->address && !d->is_self) {
          char *text = mc_format_alloc(mc_str(MC_STR_DEVICE_ADDRESS_PORT), MC_S(d->address), MC_D(d->port));
          g_dv.address = g_strdup(text);
          mc_free(text);
        }
      } else if (!d->is_self && !d->removed && d->name) {
        if (others->len) g_string_append(others, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
        g_string_append(others, d->name);
      }
    }
    g_dv.others = g_string_free(others, FALSE);
    mc_sync_device_list_free(list);
  }
  g_dv.solo = g_dv.is_self && !g_dv.found;
  if (g_dv.solo) {
    g_dv.found = TRUE;
    if (mc_sync_get_device_name(g_dv.sync, g_dv.name, sizeof g_dv.name) != MC_OK) g_dv.name[0] = '\0';
    g_dv.kind = MC_DEVICE_DESKTOP;
    g_dv.archive = g_dv.removed = g_dv.has_status = 0;
    g_dv.last_sync_ms = 0;
    g_dv.confirm = MC_SYNC_CONFIRMED; /* nothing to confirm */
  }
  if (g_dv.is_self) {
    g_dv.online = 1;
    g_dv.address = self_address();
  }
  mc_sync_lib_state state;
  if (mc_sync_library_state(g_dv.sync, &state) == MC_OK) g_dv.lib_state = state;
}

/* Reads the profile afresh and lays the pending edits over it. */
static void load_profile(void) {
  free_profile();
  g_dv.profile_status = MC_ERR_NOT_FOUND;
  if (g_dv.sync && g_dv.found && !g_dv.removed && !g_dv.solo) {
    g_dv.profile_status = mc_sync_get_profile(g_dv.sync, g_dv.uid, &g_dv.profile);
    if (g_dv.profile_status == MC_OK) g_dv.profile_status = apply_pending(g_dv.profile);
    if (g_dv.profile_status != MC_OK) free_profile();
  }
  build_sel();
  estimate_request();
}

static gboolean editable(void) {
  return g_dv.profile && !g_dv.profile->archive;
}

static gboolean syncing(void) {
  return g_dv.nbusy > 0;
}

/* Where the bar stands: a step for each phase, and the files phase by files fetched. */
static double progress_fraction(void) {
  switch (g_dv.phase) {
    case MC_SYNC_PHASE_CONNECTING: return 0;
    case MC_SYNC_PHASE_GROUP: return 0.05;
    case MC_SYNC_PHASE_LIBRARY: return 0.15;
    case MC_SYNC_PHASE_FILES: {
      if (g_dv.files_total <= 0) return 0.3;
      int done = MIN(g_dv.files_done, g_dv.files_total);
      return 0.3 + 0.7 * done / g_dv.files_total;
    }
    default: return 1;
  }
}

static gboolean on_finished_timeout(gpointer data);

/* Starts or ends the finished state; started, it ends by itself after FINISHED_MS. */
static void set_finished(gboolean on) {
  g_dv.finished = on;
  g_clear_handle_id(&g_dv.finished_source, g_source_remove);
  if (on) g_dv.finished_source = g_timeout_add(FINISHED_MS, on_finished_timeout, NULL);
}

/* --- Controls from state --- */

static void update_status(void) {
  if (!g_dv.root) return;
  char text[256] = "", ago[64];
  if (!g_dv.sync) {
    text[0] = '\0';
  } else if (!g_dv.found) {
    g_strlcpy(text, mc_str(MC_STR_DEVICE_NOT_IN_GROUP), sizeof text);
  } else if (g_dv.removed) {
    g_strlcpy(text, mc_str(MC_STR_DEVICE_REMOVED), sizeof text);
  } else {
    if (g_dv.last_sync_ms > 0) fmt_ago(g_dv.last_sync_ms, ago, sizeof ago);
    if (syncing()) g_strlcpy(text, mc_str(MC_STR_SYNCING_PROGRESS), sizeof text);
    else if (g_dv.solo) g_strlcpy(text, mc_str(MC_STR_LINUX_THIS_COMPUTER_NO_GROUP), sizeof text);
    else if (g_dv.is_self) g_strlcpy(text, mc_str(MC_STR_LINUX_THIS_COMPUTER), sizeof text);
    else if (g_dv.online && g_dv.last_sync_ms > 0) mc_format(text, sizeof text, mc_str(MC_STR_DEVICE_ONLINE_SYNCED), MC_S(ago));
    else if (g_dv.online) g_strlcpy(text, mc_str(MC_STR_DEVICE_ONLINE), sizeof text);
    else if (g_dv.last_sync_ms > 0) mc_format(text, sizeof text, mc_str(MC_STR_DEVICE_OFFLINE_SYNCED), MC_S(ago));
    else g_strlcpy(text, mc_str(MC_STR_DEVICE_NOT_SYNCED), sizeof text);
  }
  gtk_label_set_text(GTK_LABEL(g_dv.status), text);

  /* Files: its own report. */
  text[0] = '\0';
  if (g_dv.found && g_dv.has_status) {
    char ok[32], expected[32], n[32], size[32];
    const char *sep = mc_str(MC_STR_IMPORT_LIST_SEPARATOR);
    size_t used = mc_format(text, sizeof text, mc_str_plural(MC_STR_FILES_OK, (long)g_dv.expected),
                            MC_S(fmt_count(g_dv.ok, ok, sizeof ok)), MC_S(fmt_count(g_dv.expected, expected, sizeof expected)));
    if (g_dv.missing > 0 && used < sizeof text) {
      used += (size_t)g_snprintf(text + used, sizeof text - used, "%s", sep);
      if (used < sizeof text)
        used += mc_format(text + used, sizeof text - used, mc_str(MC_STR_FILES_MISSING),
                          MC_S(fmt_count(g_dv.missing, n, sizeof n)));
    }
    if (g_dv.corrupt > 0 && used < sizeof text) {
      used += (size_t)g_snprintf(text + used, sizeof text - used, "%s", sep);
      if (used < sizeof text)
        used += mc_format(text + used, sizeof text - used, mc_str(MC_STR_FILES_DAMAGED),
                          MC_S(fmt_count(g_dv.corrupt, n, sizeof n)));
    }
    if (used < sizeof text) g_snprintf(text + used, sizeof text - used, "%s%s", sep, fmt_size(g_dv.bytes, size, sizeof size));
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.files_row), text[0] ? text : mc_str(MC_STR_DEVICE_FILES_NONE));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.address_row),
                              g_dv.address ? g_dv.address : mc_str(MC_STR_DEVICE_ADDRESS_UNKNOWN));
  gtk_widget_set_visible(g_dv.files_row, !g_dv.solo);
  char *reach = g_dv.found ? device_view_reach_text(g_dv.sync, g_dv.uid, g_dv.name, FALSE) : NULL;
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.reach_row), reach ? reach : "");
  g_free(reach);
  gtk_widget_set_sensitive(g_dv.reach_check, g_dv.found && !g_dv.removed && g_dv.online);

  /* Progress takes the status line's place while syncing, and for a moment after. */
  static const enum mc_str_id phases[] = {
    [MC_SYNC_PHASE_CONNECTING] = MC_STR_PHASE_CONNECTING,
    [MC_SYNC_PHASE_GROUP] = MC_STR_PHASE_GROUP,
    [MC_SYNC_PHASE_LIBRARY] = MC_STR_PHASE_LIBRARY,
    [MC_SYNC_PHASE_FILES] = MC_STR_PHASE_FILES,
    [MC_SYNC_PHASE_DONE] = MC_STR_PHASE_DONE,
  };
  gboolean busy = g_dv.found && (syncing() || g_dv.finished);
  text[0] = '\0';
  if (syncing()) {
    gboolean known = (unsigned)g_dv.phase <= (unsigned)MC_SYNC_PHASE_DONE;
    const char *phase = mc_str(known ? phases[g_dv.phase] : MC_STR_SYNCING_PROGRESS);
    if (g_dv.phase == MC_SYNC_PHASE_CONNECTING) {
      g_strlcpy(text, phase, sizeof text);
    } else if (g_dv.phase == MC_SYNC_PHASE_FILES && g_dv.files_total > 0) {
      char done[32], total[32], size[32];
      mc_format(text, sizeof text, mc_str_plural(MC_STR_PROGRESS_FILES, (long)g_dv.files_total), MC_S(phase),
                MC_S(fmt_count(g_dv.files_done, done, sizeof done)), MC_S(fmt_count(g_dv.files_total, total, sizeof total)),
                MC_S(fmt_size(g_dv.bytes_received, size, sizeof size)));
    } else if (g_dv.phase == MC_SYNC_PHASE_FILES) { /* fetching nothing; maybe serving */
      char received[32], sent[32];
      mc_format(text, sizeof text, mc_str(MC_STR_PROGRESS_SIZES), MC_S(phase),
                MC_S(fmt_size(g_dv.bytes_received, received, sizeof received)),
                MC_S(fmt_size(g_dv.bytes_sent, sent, sizeof sent)));
    } else {
      char sent[32], received[32];
      mc_format(text, sizeof text, mc_str(MC_STR_PROGRESS_ROWS), MC_S(phase), MC_S(fmt_count(g_dv.sent, sent, sizeof sent)),
                MC_S(fmt_count(g_dv.received, received, sizeof received)));
    }
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_dv.progress), progress_fraction());
  } else if (g_dv.finished) {
    g_strlcpy(text, g_dv.finished_text, sizeof text);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(g_dv.progress), 1);
  }
  gtk_label_set_text(GTK_LABEL(g_dv.progress_text), text);
  gtk_stack_set_visible_child_name(GTK_STACK(g_dv.status_stack), busy ? "progress" : "status");

  /* The banner: why files aren't syncing, with Sync now where pressing it helps. */
  const char *notice = NULL;
  gboolean button = FALSE;
  if (g_dv.found && g_dv.lib_state == MC_SYNC_LIBRARY_OTHER) {
    notice = mc_str(MC_STR_LINUX_COMPUTER_SYNC_OTHER_LIBRARY);
  } else if (g_dv.found && g_dv.lib_state == MC_SYNC_LIBRARY_UNKNOWN) {
    notice = mc_str(MC_STR_SYNC_UNKNOWN_LIBRARY);
  } else if (g_dv.found && !g_dv.removed && g_dv.confirm == MC_SYNC_UNCONFIRMED) {
    notice = mc_str(MC_STR_SYNC_UNCONFIRMED);
    button = TRUE;
  } else if (g_dv.found && !g_dv.removed && g_dv.confirm == MC_SYNC_PROFILE_CHANGED) {
    notice = mc_str(MC_STR_SYNC_PAUSED_PROFILE_CHANGED);
    button = TRUE;
  }
  if (notice) adw_banner_set_title(g_dv.banner, notice);
  adw_banner_set_button_label(g_dv.banner, button ? mc_str(MC_STR_SYNC_NOW) : NULL);
  adw_banner_set_revealed(g_dv.banner, notice != NULL);

  /* While syncing, Sync now becomes Cancel sync; set only on a change, not every progress. */
  static int cancel_shown = -1;
  if (cancel_shown != syncing()) {
    cancel_shown = syncing();
    gtk_button_set_label(GTK_BUTTON(g_dv.sync_now), mc_str(cancel_shown ? MC_STR_CANCEL_SYNC : MC_STR_SYNC_NOW));
    if (cancel_shown) gtk_widget_remove_css_class(g_dv.sync_now, "suggested-action");
    else gtk_widget_add_css_class(g_dv.sync_now, "suggested-action");
  }
  gboolean can_sync = g_dv.found && !g_dv.removed && (syncing() || g_dv.online);
  gtk_widget_set_sensitive(g_dv.sync_now, can_sync);
  gtk_widget_set_visible(g_dv.sync_now, g_dv.found && !g_dv.solo);
  gtk_widget_set_sensitive(g_dv.remove, g_dv.found && !g_dv.removed);
  gtk_widget_set_visible(g_dv.remove, g_dv.found && !g_dv.solo);
}

static void update_count(void) {
  GString *counts = g_string_new(NULL);
  for (int i = 0; i < NUM_TABS; i++) {
    int n = (int)g_hash_table_size(g_dv.sel[i]);
    if (!n) continue;
    char one[64];
    if (counts->len) g_string_append(counts, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
    mc_format(one, sizeof one, mc_str_plural(k_tab_count[i], n), MC_D(n));
    g_string_append(counts, one);
  }
  if (counts->len) {
    char *text = mc_format_alloc(mc_str(MC_STR_COUNT_SELECTED), MC_S(counts->str));
    gtk_label_set_text(GTK_LABEL(g_dv.count), text);
    mc_free(text);
  } else {
    gtk_label_set_text(GTK_LABEL(g_dv.count), mc_str(MC_STR_NOTHING_SELECTED));
  }
  g_string_free(counts, TRUE);
}

/* Sets an entry row's text as the code's, not an edit: its apply button stays hidden. */
static void set_entry_text(GtkWidget *row, const char *text) {
  if (strcmp(gtk_editable_get_text(GTK_EDITABLE(row)), text)) gtk_editable_set_text(GTK_EDITABLE(row), text);
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), FALSE); /* forgets the change */
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(row), TRUE);
}

static gboolean has_focus_within(GtkWidget *widget) {
  GtkRoot *root = gtk_widget_get_root(widget);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  return focus && (focus == widget || gtk_widget_is_ancestor(focus, widget));
}

static guint format_index(const mc_sync_profile *p) {
  if (!p || p->format.format == MC_FORMAT_AUTO) return FORMAT_AUTO;
  if (p->format.format == MC_FORMAT_ORIGINAL) return FORMAT_ORIGINAL;
  for (int b = 0; b < MC_OPUS_BITRATE_COUNT; b++)
    if (mc_opus_bitrates[b] == p->format.bitrate_kbps) return FORMAT_FIRST_OPUS + (guint)b;
  return FORMAT_ORIGINAL;
}

static void set_group_description(GtkWidget *group, const char *text) {
  char *escaped = text && *text ? g_markup_escape_text(text, -1) : NULL;
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), escaped);
  g_free(escaped);
}

/* Every control's text, value and sensitivity from the loaded device and profile. The name is
 * left alone while it has the focus, unless force. */
static void update_controls(gboolean force) {
  if (!g_dv.root) return;
  g_dv.filling = TRUE;
  gboolean dev = g_dv.sync && g_dv.found;
  /* Not every icon theme has a watch. */
  const char *names[] = {g_dv.kind == MC_DEVICE_WATCH ? "smartwatch-symbolic" : g_dv.kind == MC_DEVICE_PHONE
                                                                                  ? "phone-symbolic"
                                                                                  : "computer-symbolic",
                         "phone-symbolic", NULL};
  GIcon *icon = g_themed_icon_new_from_names((char **)names, -1);
  gtk_image_set_from_gicon(GTK_IMAGE(g_dv.icon), icon);
  g_object_unref(icon);
  gtk_widget_set_visible(g_dv.icon, dev);
  gtk_label_set_text(GTK_LABEL(g_dv.title), !g_dv.sync ? mc_str(MC_STR_SYNC_IS_OFF)
                                            : g_dv.found ? g_dv.name
                                                         : mc_str(MC_STR_DEVICE_UNKNOWN));
  gtk_button_set_label(GTK_BUTTON(g_dv.remove),
                       mc_str(g_dv.is_self ? MC_STR_LEAVE_GROUP_BUTTON : MC_STR_REMOVE_FROM_GROUP_BUTTON));
  gtk_widget_set_tooltip_text(g_dv.sync_now, g_dv.is_self ? mc_str(MC_STR_LINUX_SYNC_EVERYONE_TOOLTIP) : NULL);
  update_status();

  gtk_widget_set_visible(g_dv.info_group, dev);
  gtk_widget_set_visible(g_dv.profile_group, dev && !g_dv.removed);
  gtk_widget_set_visible(g_dv.what_group, dev && !g_dv.removed && !g_dv.solo);
  /* Outside a group there's no profile, only the name. */
  gtk_widget_set_visible(g_dv.archive_row, !g_dv.solo);
  gtk_widget_set_visible(g_dv.format_row, !g_dv.solo);
  gtk_widget_set_visible(g_dv.budget_row, !g_dv.solo);

  if (force || !has_focus_within(g_dv.name_row)) set_entry_text(g_dv.name_row, g_dv.found ? g_dv.name : "");
  gtk_widget_set_sensitive(g_dv.name_row, g_dv.found && !g_dv.removed && (g_dv.is_self || g_dv.profile));

  const mc_sync_profile *p = g_dv.profile;
  gboolean edit = editable();
  adw_switch_row_set_active(ADW_SWITCH_ROW(g_dv.archive_row), p && p->archive);
  /* Only the device itself may stop being an archive. */
  gtk_widget_set_sensitive(g_dv.archive_row, p && (!p->archive || g_dv.is_self));
  if (p && p->archive && !g_dv.is_self) {
    char *text = mc_format_alloc(mc_str(MC_STR_PROFILE_ARCHIVE_OTHER), MC_S(g_dv.name));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.archive_row), text);
    mc_free(text);
  } else {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.archive_row), mc_str(MC_STR_DEVICE_ARCHIVE_TEXT));
  }

  adw_combo_row_set_selected(ADW_COMBO_ROW(g_dv.format_row), format_index(p));
  gtk_widget_set_sensitive(g_dv.format_row, edit);
  gtk_switch_set_active(GTK_SWITCH(g_dv.budget_switch), p && p->budget_enabled);
  gtk_widget_set_sensitive(g_dv.budget_row, edit);
  if (force || !has_focus_within(g_dv.budget_spin)) {
    int64_t gb = p && p->budget_bytes > 0 ? (p->budget_bytes + GIB / 2) / GIB : DEFAULT_BUDGET_GB;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_dv.budget_spin), (double)(gb > 0 ? gb : 1));
  }
  gtk_widget_set_sensitive(g_dv.budget_spin, edit && p->budget_enabled);

  adw_switch_row_set_active(ADW_SWITCH_ROW(g_dv.sync_all_row), !p || p->sync_all);
  gtk_widget_set_sensitive(g_dv.sync_all_row, edit);
  gtk_widget_set_visible(g_dv.picker, p && !p->sync_all);
  gtk_widget_set_sensitive(g_dv.picker, edit);

  char note[256] = "";
  if (p && p->archive) g_strlcpy(note, mc_str(MC_STR_DEVICE_ARCHIVE_NOTE), sizeof note);
  else if (!p && g_dv.found && !g_dv.removed)
    mc_format(note, sizeof note, mc_str(MC_STR_PROFILE_READ_FAILED), MC_S(mc_status_str(g_dv.profile_status)));
  set_group_description(g_dv.what_group, note);

  update_count();
  g_dv.filling = FALSE;
}

/* --- The selection list --- */

/* The list's rows: the current tab's items, "Artist – Title (year)" or a name, and a count.
 * A list view keeps its scroll position when the rows are replaced by as many. */
static void fill_list(void) {
  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
  mc_status st = MC_ERR_INVALID_ARG;
  char text[512], detail[64];
  if (g_dv.lib) {
    switch (g_dv.tab) {
      case TAB_ALBUMS: {
        mc_album_list *list = NULL;
        st = mc_library_list_albums(g_dv.lib, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++) {
          const mc_album_info *al = &list->items[i];
          size_t len = al->artist ? mc_format(text, sizeof text, mc_str(MC_STR_TRACK_BYLINE), MC_S(al->artist), MC_S(al->title))
                                  : g_strlcpy(text, al->title ? al->title : "", sizeof text);
          if (al->year > 0 && len < sizeof text)
            mc_format(text + len, sizeof text - len, mc_str(MC_STR_ALBUM_YEAR_SUFFIX), MC_D(al->year));
          g_ptr_array_add(items, dv_item_new(al->uid, text, NULL));
        }
        mc_album_list_free(list);
        break;
      }
      case TAB_ARTISTS: {
        mc_artist_list *list = NULL;
        st = mc_library_list_artists(g_dv.lib, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++) {
          int n = list->items[i].album_count;
          if (n > 0) mc_format(detail, sizeof detail, mc_str_plural(MC_STR_COUNT_ALBUMS, n), MC_D(n));
          g_ptr_array_add(items, dv_item_new(list->items[i].uid, list->items[i].name, n > 0 ? detail : NULL));
        }
        mc_artist_list_free(list);
        break;
      }
      case TAB_PLAYLISTS: {
        mc_playlist_list *list = NULL;
        st = mc_library_list_playlists(g_dv.lib, 0, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++)
          g_ptr_array_add(items, dv_item_new(list->items[i].uid, list->items[i].name, NULL));
        mc_playlist_list_free(list);
        break;
      }
      default: {
        mc_album_group_list *list = NULL;
        st = mc_library_list_album_groups(g_dv.lib, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++) {
          int n = list->items[i].album_count;
          if (n > 0) mc_format(detail, sizeof detail, mc_str_plural(MC_STR_COUNT_ALBUMS, n), MC_D(n));
          g_ptr_array_add(items, dv_item_new(list->items[i].uid, list->items[i].name, n > 0 ? detail : NULL));
        }
        mc_album_group_list_free(list);
        break;
      }
    }
  }
  g_dv.rows_status = st;
  g_dv.rows_tab = g_dv.tab; /* before the splice, which binds the rows */
  g_list_store_splice(g_dv.store, 0, g_list_model_get_n_items(G_LIST_MODEL(g_dv.store)), items->pdata, items->len);

  if (st == MC_OK) {
    gtk_label_set_text(GTK_LABEL(g_dv.empty), mc_str(k_tab_none[g_dv.tab]));
  } else {
    char msg[256];
    mc_format(msg, sizeof msg, mc_str(MC_STR_LIST_FAILED), MC_S(mc_status_str(st)));
    gtk_label_set_text(GTK_LABEL(g_dv.empty), msg);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(g_dv.list_stack), items->len ? "list" : "empty");
  g_ptr_array_unref(items);
}

static void on_item_toggled(GtkCheckButton *check, gpointer data) {
  (void)data;
  if (g_dv.filling || g_dv.rows_tab < 0) return;
  DvItem *item = g_object_get_data(G_OBJECT(check), "item");
  if (!item) return;
  gboolean on = gtk_check_button_get_active(check);
  if (on) g_hash_table_add(g_dv.sel[g_dv.rows_tab], g_strdup(item->uid));
  else g_hash_table_remove(g_dv.sel[g_dv.rows_tab], item->uid);
  pending_toggle(k_tab_kind[g_dv.rows_tab], item->uid, on);
  update_count();
  schedule();
}

/* Each row is a check button holding the item's text and detail, so a click anywhere on it
 * ticks it. */
static void on_item_setup(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data) {
  (void)factory, (void)data;
  GtkWidget *check = gtk_check_button_new();
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *text = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(text), 0);
  gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(text, TRUE);
  GtkWidget *detail = gtk_label_new(NULL);
  gtk_widget_add_css_class(detail, "dim-label");
  gtk_widget_add_css_class(detail, "numeric");
  gtk_box_append(GTK_BOX(box), text);
  gtk_box_append(GTK_BOX(box), detail);
  gtk_check_button_set_child(GTK_CHECK_BUTTON(check), box);
  gtk_widget_set_margin_start(check, 6);
  gtk_widget_set_margin_end(check, 6);
  g_signal_connect(check, "toggled", G_CALLBACK(on_item_toggled), NULL);
  gtk_list_item_set_child(li, check);
  gtk_list_item_set_activatable(li, FALSE);
}

static void on_item_bind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data) {
  (void)factory, (void)data;
  DvItem *item = gtk_list_item_get_item(li);
  GtkWidget *check = gtk_list_item_get_child(li);
  GtkWidget *box = gtk_check_button_get_child(GTK_CHECK_BUTTON(check));
  GtkWidget *text = gtk_widget_get_first_child(box), *detail = gtk_widget_get_next_sibling(text);
  gtk_label_set_text(GTK_LABEL(text), item->text ? item->text : "");
  gtk_label_set_text(GTK_LABEL(detail), item->detail ? item->detail : "");
  gtk_widget_set_visible(detail, item->detail != NULL);
  g_object_set_data(G_OBJECT(check), "item", item);
  gboolean was = g_dv.filling;
  g_dv.filling = TRUE;
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check), sel_has(g_dv.rows_tab, item->uid));
  g_dv.filling = was;
}

static void on_item_unbind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data) {
  (void)factory, (void)data;
  GtkWidget *check = gtk_list_item_get_child(li);
  if (check) g_object_set_data(G_OBJECT(check), "item", NULL);
}

/* Reloads everything, keeping the pending edits. keep: it's the same device, so a focused name
 * or budget keeps its text. */
static void reload(gboolean keep) {
  load_device();
  load_profile();
  update_controls(!keep);
  if (g_dv.profile && !g_dv.profile->sync_all) fill_list();
  else if (!keep) g_dv.rows_tab = -1; /* filled when the picker shows */
}

/* --- Size estimate ---
 * mc_sync_estimate_profile() reads every track (about half a second for 50,000), so it runs
 * on a thread, for the profile as shown (pending edits included), once the edits pause. Like
 * sync's own callbacks, the thread only calls in while the mc_sync exists: it is joined
 * before the page lets go of it (device_view_open() with another, or NULL before sync
 * stops). */

typedef struct estimate_job {
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  mc_sync_profile profile; /* a copy: no name, its own items */
  unsigned seq;
  GThread *thread;
  mc_status status;
  mc_sync_estimate est;
  /* For the archive confirmation, rather than the format row. */
  AdwAlertDialog *confirm;
} estimate_job;

static void estimate_job_free(estimate_job *job) {
  if (!job) return;
  g_free(job->profile.items);
  if (job->confirm) g_object_unref(job->confirm);
  g_free(job);
}

/* The estimate line: the size and, for Auto, what the budget picked; or how much too big. */
static void show_estimate(const estimate_job *job) {
  const mc_sync_estimate *e = &job->est;
  char text[256] = "", a[32], b[32];
  gboolean over = job->status == MC_OK && e->over_budget;
  if (over) {
    /* Free space, when it is less than the budget, is what it had to fit in. */
    int64_t budget = job->profile.budget_bytes;
    gboolean by_free = e->free_bytes >= 0 && (budget <= 0 || e->limit_bytes < budget);
    mc_format(text, sizeof text, mc_str(by_free ? MC_STR_ESTIMATE_OVER_FREE : MC_STR_ESTIMATE_OVER),
              MC_S(fmt_size(e->shortfall_bytes, a, sizeof a)), MC_S(fmt_size(e->limit_bytes, b, sizeof b)));
  } else if (job->status == MC_OK) {
    enum mc_str_id id = MC_STR_ESTIMATE_SIZE;
    if (job->profile.format.format == MC_FORMAT_AUTO && !e->archive)
      id = e->kbps ? MC_STR_ESTIMATE_AUTO_OPUS : MC_STR_ESTIMATE_AUTO_ORIGINAL;
    mc_format(text, sizeof text, mc_str_plural(id, e->tracks), MC_S(fmt_size(e->total_bytes, a, sizeof a)),
              MC_S(fmt_count(e->tracks, b, sizeof b)), MC_D(e->kbps));
  } else if (job->status != MC_ERR_LIBRARY_MISMATCH && job->status != MC_ERR_NOT_FOUND) {
    /* Another library open, or the device gone: the page says so already. */
    mc_format(text, sizeof text, mc_str(MC_STR_ESTIMATE_FAILED), MC_S(mc_status_str(job->status)));
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.format_row), text);
  if (over) gtk_widget_add_css_class(g_dv.format_row, "warning");
  else gtk_widget_remove_css_class(g_dv.format_row, "warning");
}

static void clear_estimate(void) {
  if (!g_dv.root) return;
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dv.format_row), "");
  gtk_widget_remove_css_class(g_dv.format_row, "warning");
}

static void estimate_start(void);
static void confirm_archive_estimated(estimate_job *job);

static gboolean estimate_done(gpointer data) {
  estimate_job *job = data;
  if (job->confirm) { /* the archive confirmation's */
    if (g_dv.est.archive == job->thread) {
      g_thread_join(job->thread);
      g_dv.est.archive = NULL;
      confirm_archive_estimated(job);
    } /* else cancelled: estimate_cancel() joined it already */
    estimate_job_free(job);
    return G_SOURCE_REMOVE;
  }
  if (g_dv.est.thread == job->thread) {
    g_thread_join(job->thread); /* it has posted: about to return */
    g_dv.est.thread = NULL;
    if (g_dv.root && job->seq == g_dv.est.seq && job->sync == g_dv.sync && !strcmp(job->uid, g_dv.uid))
      show_estimate(job);
    if (g_dv.est.again) estimate_start();
  } /* else cancelled: estimate_cancel() joined it already */
  estimate_job_free(job);
  return G_SOURCE_REMOVE;
}

static gpointer estimate_main(gpointer data) {
  estimate_job *job = data;
  job->status = mc_sync_estimate_profile(job->sync, job->uid, &job->profile, &job->est);
  g_idle_add(estimate_done, job);
  return NULL;
}

/* A job for p on the shown device; NULL if out of memory. */
static estimate_job *estimate_job_new(const mc_sync_profile *p) {
  estimate_job *job = g_new0(estimate_job, 1);
  job->sync = g_dv.sync;
  g_strlcpy(job->uid, g_dv.uid, sizeof job->uid);
  job->profile = *p;
  job->profile.name = NULL;
  job->profile.items = p->count > 0 ? g_memdup2(p->items, (gsize)p->count * sizeof *p->items) : NULL;
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
    clear_estimate();
    return;
  }
  estimate_job *job = estimate_job_new(p);
  job->seq = ++g_dv.est.seq;
  /* The idle that ends it can't run before this returns, so job->thread is set by then. */
  job->thread = g_dv.est.thread = g_thread_new("estimate", estimate_main, job);
}

static gboolean on_estimate_timeout(gpointer data) {
  (void)data;
  g_dv.estimate_source = 0;
  estimate_start();
  return G_SOURCE_REMOVE;
}

/* Works the estimate out again shortly, for the profile as it is then. */
static void estimate_request(void) {
  g_clear_handle_id(&g_dv.estimate_source, g_source_remove);
  g_dv.estimate_source = g_timeout_add(ESTIMATE_DELAY_MS, on_estimate_timeout, NULL);
}

/* Waits for the thread and drops its result: before sync goes. */
static void estimate_cancel(void) {
  g_clear_handle_id(&g_dv.estimate_source, g_source_remove);
  if (g_dv.est.thread) {
    g_thread_join(g_dv.est.thread); /* its idle frees the job, seeing it's no longer current */
    g_dv.est.thread = NULL;
  }
  if (g_dv.est.archive) {
    g_thread_join(g_dv.est.archive);
    g_dv.est.archive = NULL;
  }
  g_dv.est.again = FALSE;
}

/* --- Applying edits --- */

/* Applies the pending edits now: get, change, set. */
static void flush(void) {
  g_clear_handle_id(&g_dv.apply_source, g_source_remove);
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
  if (st == MC_ERR_LOCKED) toast(mc_str(MC_STR_PROFILE_LOCKED));
  else if (st != MC_OK) toast_status(MC_STR_PROFILE_SAVE_FAILED, st);
  else mc_sync_notify_changed(g_dv.sync);
  /* What's stored now, which may include others' edits made meanwhile. */
  load_profile();
  if (g_dv.root) {
    update_controls(FALSE);
    if (g_dv.profile && !g_dv.profile->sync_all) fill_list();
  }
}

static gboolean on_apply_timeout(gpointer data) {
  (void)data;
  g_dv.apply_source = 0;
  flush();
  return G_SOURCE_REMOVE;
}

static void schedule(void) {
  g_clear_handle_id(&g_dv.apply_source, g_source_remove);
  g_dv.apply_source = g_timeout_add(APPLY_DELAY_MS, on_apply_timeout, NULL);
  estimate_request(); /* for the edited profile, not waiting for it to be stored */
}

static gboolean on_tick(gpointer data) {
  (void)data;
  if (g_dv.root && g_dv.found) update_status();
  return G_SOURCE_CONTINUE;
}

static gboolean on_finished_timeout(gpointer data) {
  (void)data;
  g_dv.finished_source = 0;
  g_dv.finished = FALSE;
  update_status();
  return G_SOURCE_REMOVE;
}

static void on_name_apply(AdwEntryRow *row, gpointer data) {
  (void)data;
  if (!g_dv.sync || !g_dv.found || g_dv.removed) return;
  char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
  if (!*name || !strcmp(name, g_dv.name)) {
    set_entry_text(GTK_WIDGET(row), g_dv.name);
    g_free(name);
    return;
  }
  mc_status st;
  if (g_dv.is_self) {
    st = mc_sync_set_device_name(g_dv.sync, name);
  } else {
    mc_sync_profile *p = NULL;
    st = mc_sync_get_profile(g_dv.sync, g_dv.uid, &p);
    if (st == MC_OK) {
      free(p->name);
      p->name = malloc(strlen(name) + 1);
      if (p->name) strcpy(p->name, name);
      st = p->name ? mc_sync_set_profile(g_dv.sync, g_dv.uid, p) : MC_ERR_NOMEM;
    }
    mc_sync_profile_free(p);
  }
  if (st != MC_OK) {
    toast_status(MC_STR_DEVICE_RENAME_FAILED, st);
    set_entry_text(GTK_WIDGET(row), g_dv.name);
    g_free(name);
    return;
  }
  /* The core may have cut it. */
  char stored[MC_SYNC_DEVICE_NAME_MAX + 1];
  if (g_dv.is_self && mc_sync_get_device_name(g_dv.sync, stored, sizeof stored) == MC_OK && stored[0])
    g_strlcpy(g_dv.name, stored, sizeof g_dv.name);
  else
    g_strlcpy(g_dv.name, name, sizeof g_dv.name);
  g_free(name);
  if (!g_dv.is_self) mc_sync_notify_changed(g_dv.sync);
  gtk_label_set_text(GTK_LABEL(g_dv.title), g_dv.name);
  set_entry_text(GTK_WIDGET(row), g_dv.name);
  /* Either may come back here to refresh the page. */
  device_view_callbacks cb = g_dv.cb;
  char saved[sizeof g_dv.name];
  g_strlcpy(saved, g_dv.name, sizeof saved);
  if (g_dv.is_self && cb.rename_self) cb.rename_self(cb.user, saved);
  if (cb.devices_changed) cb.devices_changed(cb.user);
}

/* --- Leave group, Remove from group --- */

static void on_remove_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog, (void)data;
  if (strcmp(response, "remove") || !g_dv.sync || !g_dv.found) return;
  flush();
  mc_status st;
  if (g_dv.is_self) {
    st = mc_sync_leave_group(g_dv.sync); /* blocks for a few seconds at most */
  } else {
    st = mc_sync_remove_device(g_dv.sync, g_dv.uid);
  }
  if (st == MC_OK) {
    if (g_dv.cb.devices_changed) g_dv.cb.devices_changed(g_dv.cb.user);
    return;
  }
  char text[512];
  if (st == MC_ERR_LOCKED && g_dv.is_self)
    g_strlcpy(text, mc_str(MC_STR_LINUX_COMPUTER_LEAVE_ONLY_ARCHIVE), sizeof text);
  else if (st == MC_ERR_LOCKED)
    mc_format(text, sizeof text, mc_str(MC_STR_REMOVE_ONLY_ARCHIVE), MC_S(g_dv.name));
  else
    mc_format(text, sizeof text, mc_str(g_dv.is_self ? MC_STR_LEAVE_GROUP_FAILED : MC_STR_REMOVE_DEVICE_FAILED),
              MC_S(mc_status_str(st)));
  set_message(text);
}

static void on_remove_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  if (!g_dv.sync || !g_dv.found) return;
  AdwDialog *dialog;
  if (g_dv.is_self) {
    char *body = mc_format_alloc(mc_str(MC_STR_LINUX_COMPUTER_LEAVE_GROUP_CONFIRM),
                                 MC_S(g_dv.others && *g_dv.others ? g_dv.others : mc_str(MC_STR_OTHER_DEVICES)));
    dialog = adw_alert_dialog_new(mc_str(MC_STR_LEAVE_GROUP_TITLE), body);
    mc_free(body);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "remove",
                                   mc_str_mnemonic(MC_STR_LEAVE), NULL);
  } else {
    char *heading = mc_format_alloc(mc_str(MC_STR_REMOVE_DEVICE_TITLE), MC_S(g_dv.name));
    dialog = adw_alert_dialog_new(heading, mc_str(MC_STR_REMOVE_DEVICE_TEXT));
    mc_free(heading);
    adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "remove",
                                   mc_str_mnemonic(MC_STR_REMOVE), NULL);
  }
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_remove_response), NULL);
  adw_dialog_present(dialog, g_dv.root);
}

/* --- Sync now --- */

static void sync_now(void) {
  if (!g_dv.sync) return;
  flush();
  /* The user's say-so: confirms the profiles, so their files sync from now on. */
  mc_status st = mc_sync_confirm_now(g_dv.sync, g_dv.is_self ? NULL : g_dv.uid);
  char text[256] = "";
  if (st != MC_OK) mc_format(text, sizeof text, mc_str(MC_STR_SYNC_START_FAILED), MC_S(mc_status_str(st)));
  set_message(text);
  if (st == MC_OK) reload(TRUE); /* confirmed now: the banner goes */
}

/* Ends the sessions the page shows, which on_sync_done then reports as cancelled. */
static void cancel_sync(void) {
  if (g_dv.sync) mc_sync_cancel(g_dv.sync, g_dv.is_self ? NULL : g_dv.uid);
}

static void on_sync_now_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  if (syncing()) cancel_sync();
  else sync_now();
}

static void on_banner_button(AdwBanner *banner, gpointer data) {
  (void)banner, (void)data;
  sync_now();
}

/* --- Archive --- */

static void set_archive(gboolean archive) {
  if (!g_dv.sync || !g_dv.found) return;
  flush();
  mc_status st = mc_sync_set_archive(g_dv.sync, g_dv.uid, archive);
  if (st == MC_OK) {
    mc_sync_notify_changed(g_dv.sync);
  } else if (st == MC_ERR_LOCKED && !archive && g_dv.is_self) {
    toast(mc_str(MC_STR_LINUX_COMPUTER_ARCHIVE_LOCKED));
  } else {
    toast_status(MC_STR_PROFILE_ARCHIVE_FAILED, st);
  }
  reload(TRUE);
  if (g_dv.cb.devices_changed) g_dv.cb.devices_changed(g_dv.cb.user); /* the sidebar marks archives */
}

typedef struct archive_ask {
  char uid[MC_UID_STRLEN];
  mc_sync *sync;
} archive_ask;

static void on_archive_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  archive_ask *ask = g_object_get_data(G_OBJECT(dialog), "archive-ask");
  /* Still the device it was asked about. */
  if (!strcmp(response, "archive") && ask && ask->sync == g_dv.sync && !strcmp(ask->uid, g_dv.uid)) set_archive(TRUE);
}

/* The archive confirmation, with the whole library's size once it's known. */
static void confirm_archive_estimated(estimate_job *job) {
  if (job->status != MC_OK) return; /* the text without a size stays */
  char size[32];
  char *body = mc_format_alloc(mc_str(MC_STR_MAKE_ARCHIVE_TEXT), MC_S(fmt_size(job->est.total_bytes, size, sizeof size)));
  adw_alert_dialog_set_body(job->confirm, body);
  mc_free(body);
}

static void ask_make_archive(void) {
  if (!g_dv.profile) return;
  char *heading = mc_format_alloc(mc_str(MC_STR_MAKE_ARCHIVE_TITLE), MC_S(g_dv.name));
  AdwDialog *dialog = adw_alert_dialog_new(heading, mc_str(MC_STR_MAKE_ARCHIVE_TEXT_UNKNOWN));
  mc_free(heading);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", mc_str_mnemonic(MC_STR_CANCEL), "archive",
                                 mc_str_mnemonic(MC_STR_MAKE_ARCHIVE), NULL);
  adw_alert_dialog_set_response_appearance(alert, "archive", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(alert, "archive");
  adw_alert_dialog_set_close_response(alert, "cancel");
  archive_ask *ask = g_new0(archive_ask, 1);
  g_strlcpy(ask->uid, g_dv.uid, sizeof ask->uid);
  ask->sync = g_dv.sync;
  g_object_set_data_full(G_OBJECT(dialog), "archive-ask", ask, g_free);
  g_signal_connect(dialog, "response", G_CALLBACK(on_archive_response), NULL);

  /* How much the whole library comes to as originals: the profile as an archive. It is
   * joined in its own idle, or by estimate_cancel() before sync stops. */
  estimate_job *job = estimate_job_new(g_dv.profile);
  job->profile.archive = 1;
  job->confirm = g_object_ref(alert);
  if (g_dv.est.archive) { /* one asked a moment ago: let it finish first */
    g_thread_join(g_dv.est.archive);
    g_dv.est.archive = NULL;
  }
  job->thread = g_dv.est.archive = g_thread_new("estimate-archive", estimate_main, job);
  adw_dialog_present(dialog, g_dv.root);
}

static void on_archive_active(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  if (g_dv.filling || !g_dv.profile) return;
  gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  if (on == (g_dv.profile->archive != 0)) return;
  /* The switch shows what is, until it's confirmed and stored. */
  g_dv.filling = TRUE;
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), g_dv.profile->archive);
  g_dv.filling = FALSE;
  if (on) ask_make_archive();
  else set_archive(FALSE);
}

/* --- Edits from the controls --- */

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

static int64_t budget_from_spin(void) {
  int gb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_dv.budget_spin));
  return gb > 0 ? (int64_t)gb * GIB : 0;
}

/* Auto turns the budget on; any other format turns it off. */
static void on_format_selected(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  if (g_dv.filling || !g_dv.profile) return;
  guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  if (i == GTK_INVALID_LIST_POSITION) return;
  mc_sync_profile *p = g_dv.profile;
  int kbps = i >= FORMAT_FIRST_OPUS ? mc_opus_bitrates[i - FORMAT_FIRST_OPUS] : 0;
  p->format.format = i == FORMAT_AUTO ? MC_FORMAT_AUTO : kbps ? MC_FORMAT_OPUS : MC_FORMAT_ORIGINAL;
  p->format.bitrate_kbps = kbps;
  p->budget_enabled = i == FORMAT_AUTO;
  if (p->budget_enabled && p->budget_bytes <= 0) {
    int64_t bytes = budget_from_spin();
    p->budget_bytes = bytes ? bytes : DEFAULT_BUDGET_GB * GIB;
  }
  pend_format_budget();
}

/* A budget sets the format to Auto; none leaves it at originals if it was Auto. */
static void on_budget_changed(void) {
  if (g_dv.filling || !g_dv.profile) return;
  int64_t bytes = budget_from_spin();
  if (!bytes) bytes = DEFAULT_BUDGET_GB * GIB;
  mc_sync_profile *p = g_dv.profile;
  p->budget_enabled = gtk_switch_get_active(GTK_SWITCH(g_dv.budget_switch));
  p->budget_bytes = bytes;
  if (p->budget_enabled) p->format.format = MC_FORMAT_AUTO;
  else if (p->format.format == MC_FORMAT_AUTO) p->format.format = MC_FORMAT_ORIGINAL;
  if (p->format.format != MC_FORMAT_OPUS) p->format.bitrate_kbps = 0;
  pend_format_budget();
}

static void on_budget_active(GObject *sw, GParamSpec *pspec, gpointer data) {
  (void)sw, (void)pspec, (void)data;
  on_budget_changed();
}

static void on_budget_value(GtkSpinButton *spin, gpointer data) {
  (void)spin, (void)data;
  on_budget_changed();
}

static void on_sync_all_active(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  if (g_dv.filling || !g_dv.profile) return;
  g_dv.profile->sync_all = g_dv.pending.sync_all = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  g_dv.pending.mask |= PEND_SYNC_ALL;
  gtk_widget_set_visible(g_dv.picker, !g_dv.profile->sync_all);
  if (!g_dv.profile->sync_all && g_dv.rows_tab != g_dv.tab) fill_list();
  schedule();
}

static void on_tab_toggled(GtkToggleButton *button, gpointer data) {
  int tab = GPOINTER_TO_INT(data);
  if (!gtk_toggle_button_get_active(button) || tab == g_dv.tab) return;
  g_dv.tab = tab;
  fill_list();
}

/* --- Building --- */

static void on_destroy(GtkWidget *widget, gpointer data) {
  (void)widget, (void)data;
  /* Sync has been let go of by now (device_view_open(NULL) before it stops). */
  estimate_cancel();
  g_clear_handle_id(&g_dv.apply_source, g_source_remove);
  g_clear_handle_id(&g_dv.tick_source, g_source_remove);
  g_clear_handle_id(&g_dv.finished_source, g_source_remove);
  pending_clear();
  free_profile();
  g_clear_object(&g_dv.store);
  g_dv.root = NULL;
}

/* Tests the connections again: with every device online, on this computer's own page. The
 * row updates when the answers come in. */
static void on_reach_check_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  if (!g_dv.sync) return;
  mc_status st = mc_sync_check_reach(g_dv.sync, g_dv.is_self ? NULL : g_dv.uid);
  if (st != MC_OK) toast(mc_status_str(st));
}

static GtkWidget *info_row(const char *title) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  gtk_widget_add_css_class(row, "property");
  return row;
}

/* The icon beside the name and the buttons, with the status line under both; while syncing the
 * progress takes the status line's place, so the page doesn't move. */
static GtkWidget *build_header(void) {
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
  g_dv.icon = gtk_image_new_from_icon_name("computer-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(g_dv.icon), 48);
  gtk_widget_set_valign(g_dv.icon, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(g_dv.icon, "dim-label");
  gtk_grid_attach(GTK_GRID(grid), g_dv.icon, 0, 0, 1, 2);

  g_dv.title = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_dv.title), 0);
  gtk_label_set_ellipsize(GTK_LABEL(g_dv.title), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(g_dv.title, TRUE);
  gtk_widget_add_css_class(g_dv.title, "title-2");
  gtk_grid_attach(GTK_GRID(grid), g_dv.title, 1, 0, 1, 1);

  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_valign(buttons, GTK_ALIGN_CENTER);
  g_dv.sync_now = gtk_button_new_with_label(mc_str(MC_STR_SYNC_NOW));
  gtk_widget_add_css_class(g_dv.sync_now, "suggested-action");
  g_signal_connect(g_dv.sync_now, "clicked", G_CALLBACK(on_sync_now_clicked), NULL);
  g_dv.remove = gtk_button_new_with_label(mc_str(MC_STR_REMOVE_FROM_GROUP_BUTTON));
  g_signal_connect(g_dv.remove, "clicked", G_CALLBACK(on_remove_clicked), NULL);
  gtk_box_append(GTK_BOX(buttons), g_dv.sync_now);
  gtk_box_append(GTK_BOX(buttons), g_dv.remove);
  gtk_grid_attach(GTK_GRID(grid), buttons, 2, 0, 1, 1);

  g_dv.status = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_dv.status), 0);
  gtk_label_set_ellipsize(GTK_LABEL(g_dv.status), PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(g_dv.status, "dim-label");
  g_dv.progress_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  g_dv.progress = gtk_progress_bar_new();
  gtk_widget_set_size_request(g_dv.progress, 160, -1);
  gtk_widget_set_valign(g_dv.progress, GTK_ALIGN_CENTER);
  g_dv.progress_text = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_dv.progress_text), 0);
  gtk_label_set_ellipsize(GTK_LABEL(g_dv.progress_text), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(g_dv.progress_text, TRUE);
  gtk_widget_add_css_class(g_dv.progress_text, "dim-label");
  gtk_widget_add_css_class(g_dv.progress_text, "numeric");
  gtk_box_append(GTK_BOX(g_dv.progress_box), g_dv.progress);
  gtk_box_append(GTK_BOX(g_dv.progress_box), g_dv.progress_text);
  g_dv.status_stack = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(g_dv.status_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
  gtk_stack_add_named(GTK_STACK(g_dv.status_stack), g_dv.status, "status");
  gtk_stack_add_named(GTK_STACK(g_dv.status_stack), g_dv.progress_box, "progress");
  gtk_grid_attach(GTK_GRID(grid), g_dv.status_stack, 1, 1, 2, 1);

  GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_box_append(GTK_BOX(outer), grid);
  g_dv.message = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(g_dv.message), TRUE);
  gtk_label_set_xalign(GTK_LABEL(g_dv.message), 0);
  gtk_widget_add_css_class(g_dv.message, "warning");
  gtk_widget_set_visible(g_dv.message, FALSE);
  gtk_box_append(GTK_BOX(outer), g_dv.message);
  return outer;
}

static GtkWidget *build_profile_group(void) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_DEVICE_PROFILE));

  g_dv.name_row = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_dv.name_row), mc_str(MC_STR_NAME));
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(g_dv.name_row), TRUE);
  gtk_editable_set_max_width_chars(GTK_EDITABLE(g_dv.name_row), MC_SYNC_DEVICE_NAME_MAX);
  g_signal_connect(g_dv.name_row, "apply", G_CALLBACK(on_name_apply), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_dv.name_row);

  g_dv.archive_row = adw_switch_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(g_dv.archive_row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_dv.archive_row), mc_str(MC_STR_DEVICE_ARCHIVE_TITLE));
  g_signal_connect(g_dv.archive_row, "notify::active", G_CALLBACK(on_archive_active), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_dv.archive_row);

  GtkStringList *formats = gtk_string_list_new(NULL);
  gtk_string_list_append(formats, mc_str(MC_STR_FORMAT_AUTO));
  gtk_string_list_append(formats, mc_str(MC_STR_FORMAT_ORIGINAL));
  for (int b = 0; b < MC_OPUS_BITRATE_COUNT; b++) {
    char text[32];
    mc_format(text, sizeof text, mc_str(MC_STR_FORMAT_OPUS), MC_D(mc_opus_bitrates[b]));
    gtk_string_list_append(formats, text);
  }
  g_dv.format_row = adw_combo_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(g_dv.format_row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_dv.format_row), mc_str(MC_STR_SYNC_FORMAT));
  adw_combo_row_set_model(ADW_COMBO_ROW(g_dv.format_row), G_LIST_MODEL(formats));
  g_object_unref(formats);
  g_signal_connect(g_dv.format_row, "notify::selected", G_CALLBACK(on_format_selected), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_dv.format_row);

  /* Limit storage to [32] GB (o): the switch turns the budget on, which makes the format Auto. */
  g_dv.budget_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_dv.budget_row), mc_str(MC_STR_BUDGET_LIMIT));
  g_dv.budget_spin = gtk_spin_button_new_with_range(1, 99999, 1);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(g_dv.budget_spin), TRUE);
  gtk_widget_set_valign(g_dv.budget_spin, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(g_dv.budget_spin), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_BUDGET_LIMIT), -1);
  g_signal_connect(g_dv.budget_spin, "value-changed", G_CALLBACK(on_budget_value), NULL);
  GtkWidget *unit = gtk_label_new(mc_str(MC_STR_BUDGET_UNIT));
  g_dv.budget_switch = gtk_switch_new();
  gtk_widget_set_valign(g_dv.budget_switch, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(g_dv.budget_switch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_BUDGET_LIMIT), -1);
  g_signal_connect(g_dv.budget_switch, "notify::active", G_CALLBACK(on_budget_active), NULL);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_dv.budget_row), g_dv.budget_spin);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_dv.budget_row), unit);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_dv.budget_row), g_dv.budget_switch);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(g_dv.budget_row), g_dv.budget_switch);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_dv.budget_row);
  return group;
}

static GtkWidget *build_what_group(void) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_WHAT_TO_SYNC));
  g_dv.sync_all_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_dv.sync_all_row), mc_str(MC_STR_SYNC_EVERYTHING));
  g_signal_connect(g_dv.sync_all_row, "notify::active", G_CALLBACK(on_sync_all_active), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_dv.sync_all_row);

  /* The picker, shown while not syncing everything: a segmented choice of kind, the items
   * with checkboxes, and how many of each are picked. */
  g_dv.picker = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(g_dv.picker, 12);
  GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(tabs, "linked");
  gtk_widget_set_halign(tabs, GTK_ALIGN_START);
  for (int t = 0; t < NUM_TABS; t++) {
    g_dv.tabs[t] = gtk_toggle_button_new_with_label(mc_str(k_tab_name[t]));
    if (t) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(g_dv.tabs[t]), GTK_TOGGLE_BUTTON(g_dv.tabs[0]));
    g_signal_connect(g_dv.tabs[t], "toggled", G_CALLBACK(on_tab_toggled), GINT_TO_POINTER(t));
    gtk_box_append(GTK_BOX(tabs), g_dv.tabs[t]);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_dv.tabs[0]), TRUE);
  gtk_box_append(GTK_BOX(g_dv.picker), tabs);

  g_dv.store = g_list_store_new(DV_TYPE_ITEM);
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_item_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_item_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_item_unbind), NULL);
  GtkNoSelection *model = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(g_dv.store)));
  GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(model), factory);
  gtk_widget_add_css_class(list, "sync-picker");
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 320);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
  g_dv.empty = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(g_dv.empty), TRUE);
  gtk_widget_add_css_class(g_dv.empty, "dim-label");
  gtk_widget_set_margin_top(g_dv.empty, 48);
  gtk_widget_set_margin_bottom(g_dv.empty, 48);
  g_dv.list_stack = gtk_stack_new();
  gtk_widget_add_css_class(g_dv.list_stack, "card");
  gtk_widget_set_overflow(g_dv.list_stack, GTK_OVERFLOW_HIDDEN);
  gtk_stack_add_named(GTK_STACK(g_dv.list_stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(g_dv.list_stack), g_dv.empty, "empty");
  gtk_box_append(GTK_BOX(g_dv.picker), g_dv.list_stack);

  g_dv.count = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_dv.count), 0);
  gtk_label_set_wrap(GTK_LABEL(g_dv.count), TRUE);
  gtk_widget_add_css_class(g_dv.count, "dim-label");
  gtk_box_append(GTK_BOX(g_dv.picker), g_dv.count);
  gtk_widget_set_visible(g_dv.picker, FALSE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_dv.picker);
  return group;
}

GtkWidget *device_view_new(const device_view_callbacks *callbacks) {
  if (callbacks) g_dv.cb = *callbacks;
  for (int i = 0; i < NUM_TABS; i++)
    if (!g_dv.sel[i]) g_dv.sel[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  if (!g_dv.pending.toggles) g_dv.pending.toggles = g_array_new(FALSE, FALSE, sizeof(toggle));
  g_dv.rows_tab = -1;

  GtkWidget *header_group = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(header_group), build_header());

  g_dv.info_group = adw_preferences_group_new();
  g_dv.files_row = info_row(mc_str(MC_STR_DEVICE_FILES));
  g_dv.address_row = info_row(mc_str(MC_STR_DEVICE_ADDRESS));
  g_dv.reach_row = info_row(mc_str(MC_STR_LINUX_REACH));
  g_dv.reach_check = gtk_button_new_with_label(mc_str(MC_STR_LINUX_REACH_CHECK));
  gtk_widget_set_valign(g_dv.reach_check, GTK_ALIGN_CENTER);
  g_signal_connect(g_dv.reach_check, "clicked", G_CALLBACK(on_reach_check_clicked), NULL);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_dv.reach_row), g_dv.reach_check);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_dv.info_group), g_dv.files_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_dv.info_group), g_dv.address_row);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_dv.info_group), g_dv.reach_row);

  g_dv.profile_group = build_profile_group();
  g_dv.what_group = build_what_group();

  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(header_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_dv.info_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_dv.profile_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_dv.what_group));
  gtk_widget_set_vexpand(page, TRUE);

  GtkWidget *banner = adw_banner_new("");
  g_dv.banner = ADW_BANNER(banner);
#if ADW_CHECK_VERSION(1, 4, 0)
  adw_banner_set_use_markup(g_dv.banner, FALSE);
#endif
  g_signal_connect(banner, "button-clicked", G_CALLBACK(on_banner_button), NULL);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), banner);
  gtk_box_append(GTK_BOX(box), page);
  GtkWidget *overlay = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(overlay), box);
  g_dv.toasts = ADW_TOAST_OVERLAY(overlay);
  g_dv.root = overlay;
  g_signal_connect(overlay, "destroy", G_CALLBACK(on_destroy), NULL);
  g_dv.tick_source = g_timeout_add_seconds(TICK_S, on_tick, NULL);
  update_controls(TRUE);
  return overlay;
}

/* --- API --- */

void device_view_open(mc_sync *sync, mc_library *lib, const char *uid) {
  /* Edits still waiting go to the device they were made for, while its sync is still
   * there: opening with NULL comes before sync stops. */
  if (g_dv.sync && (sync == g_dv.sync || !sync)) flush();
  g_clear_handle_id(&g_dv.apply_source, g_source_remove);
  pending_clear();
  gboolean same = sync && sync == g_dv.sync && uid && !strcmp(uid, g_dv.uid);
  if (sync != g_dv.sync) estimate_cancel(); /* it uses the old one */
  if (!same) clear_estimate();
  g_dv.sync = sync;
  g_dv.lib = lib;
  if (!same) {
    g_strlcpy(g_dv.uid, uid && sync ? uid : "", sizeof g_dv.uid);
    g_dv.nbusy = 0;
    set_finished(FALSE);
    if (g_dv.root) set_message(NULL);
  }
  g_dv.rows_tab = -1; /* the library may have changed: reload */
  if (!g_dv.root) return;
  if (!same) {
    g_dv.filling = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_dv.tabs[TAB_ALBUMS]), TRUE);
    g_dv.filling = FALSE;
    g_dv.tab = TAB_ALBUMS;
  }
  reload(FALSE);
}

void device_view_refresh(void) {
  if (!g_dv.sync || !g_dv.root) return;
  reload(TRUE);
}

static int busy_index(const char *uid) {
  for (int i = 0; i < g_dv.nbusy; i++)
    if (!strcmp(g_dv.busy[i], uid)) return i;
  return -1;
}

static void busy_remove(int i) {
  if (i < 0) return;
  memmove(g_dv.busy + i, g_dv.busy + i + 1, (size_t)(g_dv.nbusy - i - 1) * sizeof g_dv.busy[0]);
  g_dv.nbusy--;
}

void device_view_on_sync_event(const sync_event *ev) {
  if (!g_dv.root || !g_dv.sync || !g_dv.found) return;
  if (ev->kind == SYNC_EV_REACH) { /* cheap enough to redo for any device */
    update_status();
    return;
  }
  if (!ev->uid[0]) return;
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
        busy_remove(i);
        /* One that got past connecting is reported by on_sync_done, which says how it went;
         * until then the bar shows full. */
        if (!syncing() && g_dv.phase != MC_SYNC_PHASE_CONNECTING) {
          g_strlcpy(g_dv.finished_text, mc_str(MC_STR_PHASE_DONE), sizeof g_dv.finished_text);
          set_finished(TRUE);
        }
        update_status();
        break;
      }
      if (i < 0 && g_dv.nbusy < MAX_BUSY) g_strlcpy(g_dv.busy[g_dv.nbusy++], ev->uid, MC_UID_STRLEN);
      set_finished(FALSE);
      g_dv.phase = ev->phase;
      g_dv.sent = ev->rows_sent;
      g_dv.received = ev->rows_received;
      g_dv.files_done = ev->files_done;
      g_dv.files_total = ev->files_total;
      g_dv.bytes_received = ev->bytes_received;
      g_dv.bytes_sent = ev->bytes_sent;
      if (i < 0) set_message(NULL);
      update_status();
      break;
    case SYNC_EV_DONE: {
      busy_remove(i);
      char text[256] = "";
      if (ev->cancelled) {
        g_strlcpy(text, mc_str(MC_STR_SYNC_CANCELLED), sizeof text);
      } else if (ev->status != MC_OK) {
        if (g_dv.is_self && ev->name[0])
          mc_format(text, sizeof text, mc_str(MC_STR_SYNC_FAILED_WITH), MC_S(ev->name), MC_S(mc_status_str(ev->status)));
        else
          mc_format(text, sizeof text, mc_str(MC_STR_SYNC_FAILED), MC_S(mc_status_str(ev->status)));
      } else if (!ev->library_synced) {
        g_strlcpy(text, mc_str(MC_STR_SYNC_DEVICE_LIST_ONLY), sizeof text);
      }
      set_message(text);
      /* The finished bar says what came in; a failure says so in the message instead. */
      if (ev->status != MC_OK) {
        set_finished(FALSE);
      } else if (g_dv.finished) {
        char n[32], size[32];
        if (ev->files_received > 0)
          mc_format(g_dv.finished_text, sizeof g_dv.finished_text,
                    mc_str_plural(MC_STR_SYNC_FINISHED_FILES, ev->files_received),
                    MC_S(fmt_count(ev->files_received, n, sizeof n)), MC_S(fmt_size(ev->bytes_received, size, sizeof size)));
        else
          g_strlcpy(g_dv.finished_text, mc_str(MC_STR_SYNC_FINISHED), sizeof g_dv.finished_text);
      }
      /* The last sync time, its files, maybe its profile, have changed. */
      reload(TRUE);
      return;
    }
    default:
      return;
  }
}

/* A string of mc_format_alloc()'s, as a g_free()able one. */
static char *take_formatted(char *text) {
  char *out = g_strdup(text);
  mc_free(text);
  return out;
}

char *device_view_reach_text(mc_sync *sync, const char *uid, const char *name, gboolean warning_only) {
  if (!sync || !uid) return NULL;
  const char *shown = name && *name ? name : mc_str(MC_STR_DEVICE_UNNAMED);
  if (!strcmp(uid, mc_sync_device_uid(sync))) {
    mc_sync_self_reach r;
    if (mc_sync_reach_self(sync, &r) != MC_OK) return NULL;
    if (r.state == MC_REACH_BLOCKED) {
      char *text = take_formatted(mc_format_alloc(mc_str_plural(MC_STR_LINUX_REACH_SELF_BLOCKED, r.blocked),
                                                  MC_D(r.blocked), MC_D(r.port)));
      if (r.default_port) return text;
      char *taken = take_formatted(mc_format_alloc(mc_str(MC_STR_LINUX_REACH_PORT_TAKEN), MC_D(MC_SYNC_DEFAULT_PORT)));
      char *both = g_strdup_printf("%s %s", text, taken);
      g_free(text);
      g_free(taken);
      return both;
    }
    if (warning_only) return NULL;
    return g_strdup(mc_str(r.state == MC_REACH_OK ? MC_STR_LINUX_REACH_SELF_OK : MC_STR_LINUX_REACH_UNKNOWN));
  }
  mc_sync_reach to_them, to_us;
  if (mc_sync_reach_peer(sync, uid, &to_them, &to_us) != MC_OK) to_them = to_us = MC_REACH_UNKNOWN;
  if (to_them == MC_REACH_BLOCKED)
    return take_formatted(mc_format_alloc(
        mc_str(to_us == MC_REACH_BLOCKED ? MC_STR_LINUX_REACH_PEER_BLOCKED_BOTH : MC_STR_LINUX_REACH_PEER_BLOCKED),
        MC_S(shown)));
  /* Its not reaching this computer shows on this computer's row instead. */
  if (warning_only) return NULL;
  if (to_us == MC_REACH_BLOCKED)
    return take_formatted(mc_format_alloc(mc_str(MC_STR_LINUX_REACH_PEER_US_BLOCKED), MC_S(shown)));
  if (to_them == MC_REACH_OK)
    return g_strdup(mc_str(to_us == MC_REACH_OK ? MC_STR_LINUX_REACH_PEER_OK : MC_STR_LINUX_REACH_PEER_OUT_OK));
  return g_strdup(mc_str(MC_STR_LINUX_REACH_UNKNOWN));
}
