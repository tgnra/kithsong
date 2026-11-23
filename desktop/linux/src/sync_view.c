#include "sync_view.h"

#include <string.h>

#include "app.h"
#include "device_dialog.h"
#include "issue_dialog.h"

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "sync_view.c needs libadwaita 1.5 or later"
#endif

#define APPLY_DELAY_MS 400
#define ESTIMATE_DELAY_MS 250 /* a read of every track: once the edits pause */
#define DEFAULT_BUDGET_GB 32
#define GIB ((int64_t)1024 * 1024 * 1024)
#define MAX_BUSY 16

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

#define SV_TYPE_ITEM (sv_item_get_type())
G_DECLARE_FINAL_TYPE(SvItem, sv_item, SV, ITEM, GObject)

struct _SvItem {
  GObject parent;
  char uid[MC_UID_STRLEN];
  char *text, *detail;
};

G_DEFINE_FINAL_TYPE(SvItem, sv_item, G_TYPE_OBJECT)

static void sv_item_finalize(GObject *obj) {
  SvItem *self = SV_ITEM(obj);
  g_free(self->text);
  g_free(self->detail);
  G_OBJECT_CLASS(sv_item_parent_class)->finalize(obj);
}

static void sv_item_class_init(SvItemClass *klass) {
  G_OBJECT_CLASS(klass)->finalize = sv_item_finalize;
}

static void sv_item_init(SvItem *self) {
  (void)self;
}

static SvItem *sv_item_new(const char *uid, const char *text, const char *detail) {
  SvItem *item = g_object_new(SV_TYPE_ITEM, NULL);
  g_strlcpy(item->uid, uid, sizeof item->uid);
  item->text = g_strdup(text);
  item->detail = g_strdup(detail);
  return item;
}

static gboolean sv_item_same(const SvItem *a, const SvItem *b) {
  return !strcmp(a->uid, b->uid) && !g_strcmp0(a->text, b->text) && !g_strcmp0(a->detail, b->detail);
}

/* --- State --- */

static struct {
  sync_view_callbacks cb;
  GtkWidget *root; /* NULL once destroyed */
  AdwToastOverlay *toasts;
  AdwBanner *banner;
  GtkWidget *strip_text, *sync_now;
  GtkWidget *self_name, *self_state, *archive_row, *quality_group, *format_row, *budget_row, *budget_switch, *budget_spin;
  GtkWidget *what_group, *sync_all_row, *picker, *tabs[NUM_TABS], *select_box, *select_all, *select_none, *list_stack,
    *empty, *count;
  GtkWidget *members, *members_none, *nearby_group, *nearby, *leave_group;
  GListStore *store;
  GPtrArray *checks; /* every row's check button, bound or not */
  gboolean filling; /* ignore the controls' signals while we set them */

  mc_sync *sync;
  mc_library *lib;
  char uid[MC_UID_STRLEN]; /* this computer's */

  /* This computer, from mc_sync_list_devices(). */
  gboolean grouped; /* it's a member of a group */
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  char *others; /* the other members' names, for Leave group */
  mc_sync_lib_state lib_state;
  int chosen; /* its user has chosen what it syncs */
  char warning[768]; /* mc_sync_device_warning(), or outside a group whether others can reach it */
  char activity[160]; /* mc_sync_activity_text(): the last sync that changed anything */
  char message[256];  /* how the last session went, when that's worth saying; "" if not */

  /* Its profile, with the pending edits applied; NULL if it couldn't be read. */
  mc_sync_profile *profile;
  mc_status profile_status;
  GHashTable *sel[NUM_TABS]; /* the selected uids of each kind */
  GHashTable *removed_items[NUM_TABS]; /* removed from the device: Select all leaves them out */
  GThread *select_thread; /* Select all or Select none, storing the rows */

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
  guint apply_source, estimate_source;

  /* Sessions in progress, with any member. */
  char busy[MAX_BUSY][MC_UID_STRLEN];
  int nbusy;
  mc_sync_phase phase;
  int64_t sent, received;
  int files_done, files_total; /* the files this computer is fetching */
  int sends_done, sends_total; /* the files the other device is fetching from it */
  int64_t bytes_received, bytes_sent;
  char pace[96]; /* speed and time left, UTF-8: mc_sync_pace_text() */
  enum { BANNER_SYNC, BANNER_OFFER } banner_action; /* what the banner's button does */

  /* The profile's size estimate, worked out on a thread of its own: one at a time. */
  struct {
    GThread *thread; /* the one running, or NULL */
    unsigned seq;    /* of the last one started */
    gboolean again;  /* the profile changed while it ran */
    GThread *archive; /* the archive confirmation's, or NULL */
  } est;
} g_sv = {.rows_tab = -1};

static void estimate_request(void);
static void schedule(void);

/* Sync runs. */
static gboolean active(void) {
  return g_sv.sync != NULL;
}

/* --- Text helpers --- */

/* n with the locale's digit grouping: 1,204. */
static const char *fmt_count(int64_t n, char *out, size_t cap) {
  app_group_digits(n, out, cap);
  return out;
}

static const char *fmt_size(int64_t bytes, char *out, size_t cap) {
  mc_format_size(out, cap, bytes);
  return out;
}

static void toast(const char *text) {
  if (!g_sv.toasts || !text || !*text) return;
  char *escaped = g_markup_escape_text(text, -1);
  AdwToast *t = adw_toast_new(escaped);
  adw_toast_set_timeout(t, 6);
  adw_toast_overlay_add_toast(g_sv.toasts, t);
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
  for (int i = 0; i < NUM_TABS; i++) {
    g_hash_table_remove_all(g_sv.sel[i]);
    g_hash_table_remove_all(g_sv.removed_items[i]);
  }
  if (!g_sv.profile) return;
  for (int i = 0; i < g_sv.profile->count; i++) {
    int t = tab_of_kind(g_sv.profile->items[i].kind);
    if (t >= 0) g_hash_table_add(g_sv.sel[t], g_strdup(g_sv.profile->items[i].ref_uid));
  }
  for (int i = 0; i < g_sv.profile->removed_count; i++) {
    int t = tab_of_kind(g_sv.profile->removed[i].kind);
    if (t >= 0) g_hash_table_add(g_sv.removed_items[t], g_strdup(g_sv.profile->removed[i].ref_uid));
  }
}

static gboolean sel_has(int tab, const char *uid) {
  return tab >= 0 && g_hash_table_contains(g_sv.sel[tab], uid);
}

/* Whether the list's row for uid is ticked: Sync everything has all but what was removed, a
 * selection what it has. */
static gboolean row_ticked(int tab, const char *uid) {
  if (tab >= 0 && g_sv.profile && g_sv.profile->sync_all) return !g_hash_table_contains(g_sv.removed_items[tab], uid);
  return sel_has(tab, uid);
}

/* --- Pending edits --- */

static gboolean pending_any(void) {
  return g_sv.pending.mask || g_sv.pending.toggles->len;
}

static void pending_clear(void) {
  g_sv.pending.mask = 0;
  g_array_set_size(g_sv.pending.toggles, 0);
}

static void pending_toggle(mc_profile_item_kind kind, const char *uid, gboolean on) {
  for (guint i = 0; i < g_sv.pending.toggles->len; i++) {
    toggle *t = &g_array_index(g_sv.pending.toggles, toggle, i);
    if (t->kind == kind && !strcmp(t->uid, uid)) {
      t->on = on;
      return;
    }
  }
  toggle t = {.kind = kind, .on = on};
  g_strlcpy(t.uid, uid, sizeof t.uid);
  g_array_append_val(g_sv.pending.toggles, t);
}

/* Applies the pending edits to p, read just before. */
static mc_status apply_pending(mc_sync_profile *p) {
  if (g_sv.pending.mask & PEND_FORMAT) p->format = g_sv.pending.format;
  if (g_sv.pending.mask & PEND_BUDGET) {
    p->budget_enabled = g_sv.pending.budget_enabled;
    p->budget_bytes = g_sv.pending.budget_bytes;
  }
  if (g_sv.pending.mask & PEND_SYNC_ALL) p->sync_all = g_sv.pending.sync_all;
  guint nt = g_sv.pending.toggles->len;
  if (!nt) return MC_OK;

  /* The items, less every toggled one, then the toggled-on ones. p's items were malloc'd by
   * the core and are freed with the profile, so the new array is malloc'd too. */
  mc_profile_item *items = malloc(((size_t)p->count + nt) * sizeof *items);
  if (!items) return MC_ERR_NOMEM;
  int n = 0;
  for (int i = 0; i < p->count; i++) {
    gboolean toggled = FALSE;
    for (guint j = 0; j < nt && !toggled; j++) {
      const toggle *t = &g_array_index(g_sv.pending.toggles, toggle, j);
      toggled = t->kind == p->items[i].kind && !strcmp(t->uid, p->items[i].ref_uid);
    }
    if (!toggled) items[n++] = p->items[i];
  }
  for (guint j = 0; j < nt; j++) {
    const toggle *t = &g_array_index(g_sv.pending.toggles, toggle, j);
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
  mc_sync_profile_free(g_sv.profile);
  g_sv.profile = NULL;
}

static int busy_index(const char *uid) {
  for (int i = 0; i < g_sv.nbusy; i++)
    if (!strcmp(g_sv.busy[i], uid)) return i;
  return -1;
}

static void busy_remove(int i) {
  if (i < 0) return;
  memmove(g_sv.busy + i, g_sv.busy + i + 1, (size_t)(g_sv.nbusy - i - 1) * sizeof g_sv.busy[0]);
  g_sv.nbusy--;
}

static gboolean syncing(void) {
  return g_sv.nbusy > 0;
}

static void clear_list(GtkWidget *list) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(list)) && GTK_IS_LIST_BOX_ROW(child))
    gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

static GtkWidget *kind_icon(int kind) {
  const char *names[] = {kind == MC_DEVICE_PHONE ? "phone-symbolic" : "computer-symbolic", NULL};
  GIcon *gicon = g_themed_icon_new_from_names((char **)names, -1);
  GtkWidget *icon = gtk_image_new_from_gicon(gicon);
  g_object_unref(gicon);
  return icon;
}

/* A device's row: the kind's icon, the name, a line under it (NULL for none), a warning
 * triangle with warning as its tooltip (NULL for none), a spinner while it syncs, and an arrow
 * if activating it opens something. */
static GtkWidget *device_row(const char *uid, int kind, const char *name, const char *sub, const char *warning,
                             gboolean busy, gboolean dim, gboolean opens) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), name && *name ? name : mc_str(MC_STR_DEVICE_UNNAMED));
  if (sub) adw_action_row_set_subtitle(ADW_ACTION_ROW(row), sub);
  adw_action_row_add_prefix(ADW_ACTION_ROW(row), kind_icon(kind));
  if (warning) {
    GtkWidget *triangle = gtk_image_new_from_icon_name("dialog-warning-symbolic");
    gtk_widget_add_css_class(triangle, "warning");
    gtk_widget_set_tooltip_text(triangle, warning);
    gtk_accessible_update_property(GTK_ACCESSIBLE(triangle), GTK_ACCESSIBLE_PROPERTY_LABEL, warning, -1);
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), triangle);
  }
  if (busy) {
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_tooltip_text(spinner, mc_str(MC_STR_SYNCING_PROGRESS));
    adw_action_row_add_suffix(ADW_ACTION_ROW(row), spinner);
  }
  if (opens) adw_action_row_add_suffix(ADW_ACTION_ROW(row), gtk_image_new_from_icon_name("go-next-symbolic"));
  if (dim) gtk_widget_add_css_class(row, "dim-label");
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
  g_object_set_data_full(G_OBJECT(row), "uid", g_strdup(uid), g_free);
  return row;
}

/* A member's row, under it what it syncs in a sentence ("Offline · 3 albums and 2
 * playlists"), and what's wrong, most pressing first (mc_sync_device_warning(): its version, a
 * hold on its side, reach, an archive away for long, too little space to transcode). */
static GtkWidget *member_row(const mc_sync_device *dev) {
  char summary[256] = "", sub[320], warning[768];
  mc_sync_profile *p = NULL;
  if (mc_sync_get_profile(g_sv.sync, dev->uid, &p) == MC_OK) mc_sync_profile_summary(p, summary, sizeof summary);
  mc_sync_profile_free(p);
  if (dev->online)
    g_strlcpy(sub, summary, sizeof sub);
  else
    g_snprintf(sub, sizeof sub, "%s%s%s", mc_str(MC_STR_DEVICE_OFFLINE), summary[0] ? mc_str(MC_STR_SEPARATOR_DOT) : "",
               summary);
  mc_sync_device_warning(g_sv.sync, dev, 0, warning, sizeof warning);
  return device_row(dev->uid, dev->kind, dev->name, sub[0] ? sub : NULL, warning[0] ? warning : NULL,
                    busy_index(dev->uid) >= 0, !dev->online, TRUE);
}

/* A nearby device's row: whether it's in another group, and why pairing with it can't work
 * (its version) or might not (reach). */
static GtkWidget *nearby_row(const mc_sync_nearby *dev) {
  char warning[512] = "";
  if (dev->version.outdated != MC_SYNC_VERSION_OK) mc_sync_version_text(dev->name, &dev->version, warning, sizeof warning);
  else mc_sync_reach_text(g_sv.sync, dev->uid, dev->name, 1, warning, sizeof warning);
  return device_row(dev->uid, dev->kind, dev->name, dev->in_other_group ? mc_str(MC_STR_DEVICE_OTHER_GROUP_TIP) : NULL,
                    warning[0] ? warning : NULL, FALSE, FALSE, FALSE);
}

/* Reads this computer's place in the group from sync, and lists the other members (online
 * first, then offline, each in the order listed, by name) and the devices nearby. */
static void load_devices(void) {
  g_sv.grouped = FALSE;
  g_clear_pointer(&g_sv.others, g_free);
  g_sv.lib_state = MC_SYNC_LIBRARY_GROUP;
  g_sv.name[0] = g_sv.warning[0] = '\0';
  g_sv.chosen = TRUE;
  if (!g_sv.root) return;
  clear_list(g_sv.members);
  clear_list(g_sv.nearby);
  mc_sync_device_list *list = NULL;
  mc_sync_nearby_list *nearby = NULL;
  if (g_sv.sync && mc_sync_list_devices(g_sv.sync, &list) != MC_OK) list = NULL;
  if (g_sv.sync && mc_sync_list_nearby(g_sv.sync, &nearby) != MC_OK) nearby = NULL;
  GString *others = g_string_new(NULL);
  for (int pass = 0; list && pass < 2; pass++)
    for (int i = 0; i < list->count; i++) {
      const mc_sync_device *dev = &list->items[i];
      if (dev->removed || (dev->online ? 0 : 1) != pass) continue;
      if (dev->is_self) {
        g_sv.grouped = TRUE;
        g_strlcpy(g_sv.name, dev->name ? dev->name : "", sizeof g_sv.name);
        mc_sync_device_warning(g_sv.sync, dev, 0, g_sv.warning, sizeof g_sv.warning);
        continue;
      }
      if (dev->name) {
        if (others->len) g_string_append(others, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
        g_string_append(others, dev->name);
      }
      gtk_list_box_append(GTK_LIST_BOX(g_sv.members), member_row(dev));
    }
  g_sv.others = g_string_free(others, FALSE);
  mc_sync_device_list_free(list);
  /* Outside a group, what matters is whether other devices can reach it to pair. */
  if (g_sv.sync && !g_sv.grouped) {
    if (mc_sync_get_device_name(g_sv.sync, g_sv.name, sizeof g_sv.name) != MC_OK) g_sv.name[0] = '\0';
    mc_sync_reach_text(g_sv.sync, g_sv.uid, g_sv.name, 1, g_sv.warning, sizeof g_sv.warning);
  }
  int nn = nearby ? nearby->count : 0;
  for (int i = 0; i < nn; i++) gtk_list_box_append(GTK_LIST_BOX(g_sv.nearby), nearby_row(&nearby->items[i]));
  mc_sync_nearby_list_free(nearby);
  char heading[128];
  mc_format(heading, sizeof heading, mc_str(MC_STR_DEVICES_UNPAIRED), MC_D(nn));
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g_sv.nearby_group), heading);
  gtk_widget_set_visible(g_sv.nearby_group, nn > 0);
  gtk_widget_set_visible(g_sv.leave_group, g_sv.grouped);
  if (g_sv.sync) {
    gtk_label_set_text(GTK_LABEL(g_sv.members_none), mc_str(MC_STR_DEVICES_NONE));
  } else {
    char *off = mc_format_alloc(mc_str(MC_STR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
    gtk_label_set_text(GTK_LABEL(g_sv.members_none), off);
    mc_free(off);
  }
  if (!g_sv.sync) return;
  mc_sync_outlook o;
  g_sv.chosen = !g_sv.grouped || mc_sync_get_outlook(g_sv.sync, &o) != MC_OK || o.chosen;
  mc_sync_lib_state state;
  if (mc_sync_library_state(g_sv.sync, &state) == MC_OK) g_sv.lib_state = state;
  mc_sync_activity act;
  g_sv.activity[0] = '\0';
  if (mc_sync_get_last_activity(g_sv.sync, &act) == MC_OK) mc_sync_activity_text(&act, g_sv.activity, sizeof g_sv.activity);
}

/* Reads the profile afresh and lays the pending edits over it. */
static void load_profile(void) {
  free_profile();
  g_sv.profile_status = MC_ERR_NOT_FOUND;
  if (g_sv.sync && g_sv.grouped) {
    g_sv.profile_status = mc_sync_get_profile(g_sv.sync, g_sv.uid, &g_sv.profile);
    if (g_sv.profile_status == MC_OK) g_sv.profile_status = apply_pending(g_sv.profile);
    if (g_sv.profile_status != MC_OK) free_profile();
  }
  build_sel();
  estimate_request();
}

/* An archive's format, budget and selection are fixed. */
static gboolean editable(void) {
  return g_sv.profile && !g_sv.profile->archive;
}

/* --- Controls from state --- */

/* A narrow strip across a page: a line of text, cut short if need be with the whole of it as
 * its tooltip, and a button at the end, which calls clicked. */
static GtkWidget *strip_new(GtkWidget **text, GtkWidget **button, const char *label, GCallback clicked) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  *text = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(*text), 0);
  gtk_label_set_ellipsize(GTK_LABEL(*text), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(*text, TRUE);
  gtk_widget_add_css_class(*text, "numeric");
  *button = gtk_button_new_with_label(label);
  gtk_widget_set_valign(*button, GTK_ALIGN_CENTER);
  g_signal_connect(*button, "clicked", clicked, NULL);
  gtk_box_append(GTK_BOX(box), *text);
  gtk_box_append(GTK_BOX(box), *button);
  return box;
}

/* Sets a strip's text, dim, or as a warning. */
static void strip_set_text(GtkWidget *text, const char *s, gboolean warning) {
  gtk_label_set_text(GTK_LABEL(text), s);
  gtk_widget_set_tooltip_text(text, *s ? s : NULL);
  gtk_widget_set_css_classes(text, (const char *[]){"numeric", warning ? "warning" : "dim-label", NULL});
}

/* How the sessions in progress are going: the phase, and what's come and gone. */
static void progress_text(char *text, size_t size) {
  static const enum mc_str_id phases[] = {
    [MC_SYNC_PHASE_CONNECTING] = MC_STR_PHASE_CONNECTING,
    [MC_SYNC_PHASE_GROUP] = MC_STR_PHASE_GROUP,
    [MC_SYNC_PHASE_LIBRARY] = MC_STR_PHASE_LIBRARY,
    [MC_SYNC_PHASE_FILES] = MC_STR_PHASE_FILES,
    [MC_SYNC_PHASE_DONE] = MC_STR_PHASE_DONE,
  };
  /* The other device's turn: it's fetching from this computer, or it's all there is. */
  gboolean sending = g_sv.sends_total > 0 && (g_sv.sends_done < g_sv.sends_total || g_sv.files_total <= 0);
  gboolean known = (unsigned)g_sv.phase <= (unsigned)MC_SYNC_PHASE_DONE;
  const char *phase = mc_str(known ? phases[g_sv.phase] : MC_STR_SYNCING_PROGRESS);
  if (g_sv.phase == MC_SYNC_PHASE_CONNECTING) {
    g_strlcpy(text, phase, size);
  } else if (g_sv.phase == MC_SYNC_PHASE_FILES && sending) {
    char done[32], total[32], bytes[32];
    mc_format(text, size, mc_str_plural(MC_STR_PROGRESS_FILES_SENT, (long)g_sv.sends_total), MC_S(phase),
              MC_S(fmt_count(g_sv.sends_done, done, sizeof done)), MC_S(fmt_count(g_sv.sends_total, total, sizeof total)),
              MC_S(fmt_size(g_sv.bytes_sent, bytes, sizeof bytes)));
  } else if (g_sv.phase == MC_SYNC_PHASE_FILES && g_sv.files_total > 0) {
    char done[32], total[32], bytes[32];
    mc_format(text, size, mc_str_plural(MC_STR_PROGRESS_FILES, (long)g_sv.files_total), MC_S(phase),
              MC_S(fmt_count(g_sv.files_done, done, sizeof done)), MC_S(fmt_count(g_sv.files_total, total, sizeof total)),
              MC_S(fmt_size(g_sv.bytes_received, bytes, sizeof bytes)));
  } else if (g_sv.phase == MC_SYNC_PHASE_FILES) { /* fetching nothing; maybe serving */
    char received[32], sent[32];
    mc_format(text, size, mc_str(MC_STR_PROGRESS_SIZES), MC_S(phase),
              MC_S(fmt_size(g_sv.bytes_received, received, sizeof received)),
              MC_S(fmt_size(g_sv.bytes_sent, sent, sizeof sent)));
  } else {
    char sent[32], received[32];
    mc_format(text, size, mc_str(MC_STR_PROGRESS_ROWS), MC_S(phase), MC_S(fmt_count(g_sv.sent, sent, sizeof sent)),
              MC_S(fmt_count(g_sv.received, received, sizeof received)));
  }
  /* Of several sessions, whichever spoke last would have its speed shown: none is better. */
  if (g_sv.phase == MC_SYNC_PHASE_FILES && g_sv.nbusy == 1 && g_sv.pace[0]) {
    size_t used = strlen(text);
    if (used < size) g_snprintf(text + used, size - used, "%s%s", mc_str(MC_STR_SEPARATOR_DOT), g_sv.pace);
  }
}

/* The strip: how the sessions in progress are going; else how the last one went, when that's
 * worth saying; else what the last sync that changed anything did; else what the next would
 * copy. Its Sync now reads Cancel sync while sessions run. */
static void update_strip(void) {
  char text[256] = "", outlook[128] = "";
  mc_sync_outlook o;
  if (g_sv.grouped && !syncing() && mc_sync_get_outlook(g_sv.sync, &o) == MC_OK && o.chosen)
    mc_sync_outlook_text(&o, outlook, sizeof outlook);
  if (syncing()) progress_text(text, sizeof text);
  else if (g_sv.message[0]) g_strlcpy(text, g_sv.message, sizeof text);
  else g_strlcpy(text, g_sv.activity[0] ? g_sv.activity : outlook, sizeof text);
  strip_set_text(g_sv.strip_text, text, !syncing() && g_sv.message[0]);

  char *tip = g_strjoin("\n", mc_str(MC_STR_LINUX_SYNC_EVERYONE_TOOLTIP), outlook[0] ? outlook : NULL, NULL);
  gtk_widget_set_tooltip_text(g_sv.sync_now, tip);
  g_free(tip);
  gtk_widget_set_visible(g_sv.sync_now, g_sv.grouped);
  /* Set only on a change, not every progress. */
  const char *want = mc_str(syncing() ? MC_STR_CANCEL_SYNC : MC_STR_SYNC_NOW);
  if (!g_strcmp0(gtk_button_get_label(GTK_BUTTON(g_sv.sync_now)), want)) return;
  gtk_button_set_label(GTK_BUTTON(g_sv.sync_now), want);
  if (syncing()) gtk_widget_remove_css_class(g_sv.sync_now, "suggested-action");
  else gtk_widget_add_css_class(g_sv.sync_now, "suggested-action");
}

/* The banner: why sync is off, or files aren't syncing, with Sync now where pressing it helps;
 * the offer to take a lost archive's place; or what's wrong with this computer. */
static void update_banner(void) {
  const char *notice = NULL;
  char off[256];
  gboolean button = FALSE, offered = FALSE;
  const mc_sync_archive_offer *offer = g_sv.sync ? issue_dialog_offer() : NULL;
  mc_sync_offer_text offer_text;
  if (!g_sv.sync) {
    mc_format(off, sizeof off, mc_str(MC_STR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
    notice = off;
  } else if (g_sv.grouped && g_sv.lib_state == MC_SYNC_LIBRARY_OTHER) {
    notice = mc_str(MC_STR_LINUX_COMPUTER_SYNC_OTHER_LIBRARY);
  } else if (g_sv.grouped && g_sv.lib_state == MC_SYNC_LIBRARY_UNKNOWN) {
    notice = mc_str(MC_STR_SYNC_UNKNOWN_LIBRARY);
  } else if (!g_sv.chosen) {
    notice = mc_str(MC_STR_SYNC_CHOOSE_FIRST);
    button = TRUE;
  } else if (offer) {
    mc_sync_archive_offer_describe(offer, MC_DEVICE_DESKTOP, &offer_text);
    offered = offer_text.title[0] != '\0';
    if (offered) notice = offer_text.title;
  }
  if (!notice && g_sv.warning[0]) notice = g_sv.warning;
  g_sv.banner_action = offered ? BANNER_OFFER : BANNER_SYNC;
  if (notice) adw_banner_set_title(g_sv.banner, notice);
  adw_banner_set_button_label(g_sv.banner, offered  ? mc_str(MC_STR_LINUX_SYNC_OFFER_BANNER_BUTTON)
                                           : button ? mc_str(MC_STR_SYNC_NOW)
                                                    : NULL);
  adw_banner_set_revealed(g_sv.banner, notice != NULL);
}

static void update_status(void) {
  if (!g_sv.root) return;
  update_strip();
  update_banner();
}

/* The list shows but for an archive. Under Sync everything it has albums and artists, ticked
 * but what was removed: playlists and groups would select nothing more. */
static void update_picker(void) {
  const mc_sync_profile *p = g_sv.profile;
  gboolean choosing = p && !p->sync_all;
  gtk_widget_set_visible(g_sv.picker, p && !p->archive);
  for (int t = TAB_PLAYLISTS; t < NUM_TABS; t++) gtk_widget_set_visible(g_sv.tabs[t], choosing);
  gtk_widget_set_visible(g_sv.select_box, choosing);
  gtk_widget_set_visible(g_sv.count, choosing);
}

static void update_count(void) {
  GString *counts = g_string_new(NULL);
  for (int i = 0; i < NUM_TABS; i++) {
    int n = (int)g_hash_table_size(g_sv.sel[i]);
    if (!n) continue;
    char one[64];
    if (counts->len) g_string_append(counts, mc_str(MC_STR_IMPORT_LIST_SEPARATOR));
    mc_format(one, sizeof one, mc_str_plural(k_tab_count[i], n), MC_D(n));
    g_string_append(counts, one);
  }
  if (counts->len) {
    char *text = mc_format_alloc(mc_str(MC_STR_COUNT_SELECTED), MC_S(counts->str));
    gtk_label_set_text(GTK_LABEL(g_sv.count), text);
    mc_free(text);
  } else {
    gtk_label_set_text(GTK_LABEL(g_sv.count), mc_str(MC_STR_NOTHING_SELECTED));
  }
  g_string_free(counts, TRUE);
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

/* Every control's text, value and sensitivity from the loaded state and profile. The budget is
 * left alone while it has the focus, unless force. */
static void update_controls(gboolean force) {
  if (!g_sv.root) return;
  g_sv.filling = TRUE;
  gtk_label_set_text(GTK_LABEL(g_sv.self_name), !active()      ? mc_str(MC_STR_SYNC_IS_OFF)
                                                : g_sv.name[0] ? g_sv.name
                                                               : mc_str(MC_STR_DEVICE_UNNAMED));
  gtk_label_set_text(GTK_LABEL(g_sv.self_state),
                     mc_str(g_sv.grouped ? MC_STR_LINUX_THIS_COMPUTER : MC_STR_LINUX_THIS_COMPUTER_NO_GROUP));
  gtk_widget_set_visible(g_sv.self_state, active());
  update_status();

  /* Outside a group there's no profile. */
  gtk_widget_set_visible(g_sv.what_group, g_sv.grouped);
  gtk_widget_set_visible(g_sv.quality_group, g_sv.grouped);

  const mc_sync_profile *p = g_sv.profile;
  gboolean edit = editable();
  adw_switch_row_set_active(ADW_SWITCH_ROW(g_sv.archive_row), p && p->archive);
  gtk_widget_set_sensitive(g_sv.archive_row, p != NULL);
  adw_combo_row_set_selected(ADW_COMBO_ROW(g_sv.format_row), format_index(p));
  gtk_widget_set_sensitive(g_sv.format_row, edit);
  gtk_switch_set_active(GTK_SWITCH(g_sv.budget_switch), p && p->budget_enabled);
  gtk_widget_set_sensitive(g_sv.budget_row, edit);
  if (force || !has_focus_within(g_sv.budget_spin)) {
    int64_t gb = p && p->budget_bytes > 0 ? (p->budget_bytes + GIB / 2) / GIB : DEFAULT_BUDGET_GB;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sv.budget_spin), (double)(gb > 0 ? gb : 1));
  }
  gtk_widget_set_sensitive(g_sv.budget_spin, edit && p->budget_enabled);

  adw_switch_row_set_active(ADW_SWITCH_ROW(g_sv.sync_all_row), !p || p->sync_all);
  gtk_widget_set_sensitive(g_sv.sync_all_row, edit);
  update_picker();
  gtk_widget_set_sensitive(g_sv.picker, edit);

  char note[256] = "";
  if (p && p->archive) g_strlcpy(note, mc_str(MC_STR_DEVICE_ARCHIVE_NOTE), sizeof note);
  else if (!p && g_sv.grouped)
    mc_format(note, sizeof note, mc_str(MC_STR_PROFILE_READ_FAILED), MC_S(mc_status_str(g_sv.profile_status)));
  set_group_description(g_sv.what_group, note);

  update_count();
  g_sv.filling = FALSE;
}

/* --- The selection list --- */

/* Select all and Select none, each greyed out while pressing it would change nothing. */
static void update_select_buttons(void) {
  gboolean can_all = FALSE, can_none = FALSE;
  gboolean choosing = g_sv.profile && !g_sv.profile->sync_all;
  guint n = g_sv.rows_tab >= 0 && editable() && choosing ? g_list_model_get_n_items(G_LIST_MODEL(g_sv.store)) : 0;
  for (guint i = 0; i < n && !(can_all && can_none); i++) {
    SvItem *item = g_list_model_get_item(G_LIST_MODEL(g_sv.store), i);
    if (sel_has(g_sv.rows_tab, item->uid)) can_none = TRUE;
    else if (!g_hash_table_contains(g_sv.removed_items[g_sv.rows_tab], item->uid)) can_all = TRUE;
    g_object_unref(item);
  }
  gtk_widget_set_sensitive(g_sv.select_all, can_all);
  gtk_widget_set_sensitive(g_sv.select_none, can_none);
}

/* A row's detail: that uid was removed from the device, or what's given. */
static const char *row_detail(const char *uid, const char *detail) {
  return g_hash_table_contains(g_sv.removed_items[g_sv.tab], uid) ? mc_str(MC_STR_PROFILE_ITEM_REMOVED) : detail;
}

/* Ticks the bound rows again, from what's stored and pending now. */
static void retick_rows(void) {
  if (!g_sv.checks) return;
  gboolean was = g_sv.filling;
  g_sv.filling = TRUE;
  for (guint i = 0; i < g_sv.checks->len; i++) {
    GtkCheckButton *check = g_sv.checks->pdata[i];
    SvItem *item = g_object_get_data(G_OBJECT(check), "item");
    if (item) gtk_check_button_set_active(check, row_ticked(g_sv.rows_tab, item->uid));
  }
  g_sv.filling = was;
}

/* The list's rows: the current tab's items, "Artist – Title (year)" or a name, and a count.
 * A list view keeps its scroll position by the item at the top, and goes back to the top when
 * that item leaves the store, even for an equal one; so the rows that haven't changed stay,
 * and only the run between the unchanged start and end is replaced. */
static void fill_list(void) {
  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
  mc_status st = MC_ERR_INVALID_ARG;
  char text[512], detail[64];
  if (g_sv.lib) {
    switch (g_sv.tab) {
      case TAB_ALBUMS: {
        mc_album_list *list = NULL;
        st = mc_library_list_albums(g_sv.lib, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++) {
          const mc_album_info *al = &list->items[i];
          size_t len = al->artist ? mc_format(text, sizeof text, mc_str(MC_STR_TRACK_BYLINE), MC_S(al->artist), MC_S(al->title))
                                  : g_strlcpy(text, al->title ? al->title : "", sizeof text);
          if (al->year > 0 && len < sizeof text)
            mc_format(text + len, sizeof text - len, mc_str(MC_STR_ALBUM_YEAR_SUFFIX), MC_D(al->year));
          g_ptr_array_add(items, sv_item_new(al->uid, text, row_detail(al->uid, NULL)));
        }
        mc_album_list_free(list);
        break;
      }
      case TAB_ARTISTS: {
        mc_artist_list *list = NULL;
        st = mc_library_list_artists(g_sv.lib, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++) {
          int n = list->items[i].album_count;
          if (n > 0) mc_format(detail, sizeof detail, mc_str_plural(MC_STR_COUNT_ALBUMS, n), MC_D(n));
          g_ptr_array_add(items, sv_item_new(list->items[i].uid, list->items[i].name,
                                             row_detail(list->items[i].uid, n > 0 ? detail : NULL)));
        }
        mc_artist_list_free(list);
        break;
      }
      case TAB_PLAYLISTS: {
        mc_playlist_list *list = NULL;
        st = mc_library_list_playlists(g_sv.lib, 0, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++)
          g_ptr_array_add(items, sv_item_new(list->items[i].uid, list->items[i].name, NULL));
        mc_playlist_list_free(list);
        break;
      }
      default: {
        mc_album_group_list *list = NULL;
        st = mc_library_list_album_groups(g_sv.lib, 0, &list);
        for (int i = 0; st == MC_OK && list && i < list->count; i++) {
          int n = list->items[i].album_count;
          if (n > 0) mc_format(detail, sizeof detail, mc_str_plural(MC_STR_COUNT_ALBUMS, n), MC_D(n));
          g_ptr_array_add(items, sv_item_new(list->items[i].uid, list->items[i].name, n > 0 ? detail : NULL));
        }
        mc_album_group_list_free(list);
        break;
      }
    }
  }
  g_sv.rows_status = st;
  g_sv.rows_tab = g_sv.tab; /* before the splice, which binds the rows */
  GListModel *old = G_LIST_MODEL(g_sv.store);
  guint n_old = g_list_model_get_n_items(old), n_new = items->len, head = 0, tail = 0;
  while (head < n_old && head < n_new) {
    SvItem *item = g_list_model_get_item(old, head);
    gboolean same = sv_item_same(item, items->pdata[head]);
    g_object_unref(item);
    if (!same) break;
    head++;
  }
  while (tail < n_old - head && tail < n_new - head) {
    SvItem *item = g_list_model_get_item(old, n_old - 1 - tail);
    gboolean same = sv_item_same(item, items->pdata[n_new - 1 - tail]);
    g_object_unref(item);
    if (!same) break;
    tail++;
  }
  if (head + tail < n_old || head + tail < n_new)
    g_list_store_splice(g_sv.store, head, n_old - head - tail, items->pdata + head, n_new - head - tail);
  retick_rows(); /* the rows kept: the profile, or the tab, may have changed */

  if (st == MC_OK) {
    gtk_label_set_text(GTK_LABEL(g_sv.empty), mc_str(k_tab_none[g_sv.tab]));
  } else {
    char msg[256];
    mc_format(msg, sizeof msg, mc_str(MC_STR_LIST_FAILED), MC_S(mc_status_str(st)));
    gtk_label_set_text(GTK_LABEL(g_sv.empty), msg);
  }
  gtk_stack_set_visible_child_name(GTK_STACK(g_sv.list_stack), items->len ? "list" : "empty");
  g_ptr_array_unref(items);
  update_select_buttons();
}

static gboolean exclude_tick(gpointer data);

/* A tick under Sync everything, stored once the check button's signal is over: the rows it
 * sits in are made again. */
typedef struct tick {
  char device[MC_UID_STRLEN], uid[MC_UID_STRLEN];
  mc_profile_item_kind kind;
  gboolean on;
} tick;

static void on_item_toggled(GtkCheckButton *check, gpointer data) {
  (void)data;
  if (g_sv.filling || g_sv.rows_tab < 0) return;
  SvItem *item = g_object_get_data(G_OBJECT(check), "item");
  if (!item) return;
  gboolean on = gtk_check_button_get_active(check);
  if (g_sv.profile && g_sv.profile->sync_all) {
    tick *t = g_new0(tick, 1);
    g_strlcpy(t->device, g_sv.uid, sizeof t->device);
    g_strlcpy(t->uid, item->uid, sizeof t->uid);
    t->kind = k_tab_kind[g_sv.rows_tab];
    t->on = on;
    g_idle_add(exclude_tick, t);
    return;
  }
  if (on) g_hash_table_add(g_sv.sel[g_sv.rows_tab], g_strdup(item->uid));
  else g_hash_table_remove(g_sv.sel[g_sv.rows_tab], item->uid);
  pending_toggle(k_tab_kind[g_sv.rows_tab], item->uid, on);
  update_count();
  update_select_buttons();
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
  if (g_sv.checks) g_ptr_array_add(g_sv.checks, check);
  gtk_list_item_set_child(li, check);
  gtk_list_item_set_activatable(li, FALSE);
}

static void on_item_bind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data) {
  (void)factory, (void)data;
  SvItem *item = gtk_list_item_get_item(li);
  GtkWidget *check = gtk_list_item_get_child(li);
  GtkWidget *box = gtk_check_button_get_child(GTK_CHECK_BUTTON(check));
  GtkWidget *text = gtk_widget_get_first_child(box), *detail = gtk_widget_get_next_sibling(text);
  gtk_label_set_text(GTK_LABEL(text), item->text ? item->text : "");
  gtk_label_set_text(GTK_LABEL(detail), item->detail ? item->detail : "");
  gtk_widget_set_visible(detail, item->detail != NULL);
  g_object_set_data(G_OBJECT(check), "item", item);
  gboolean was = g_sv.filling;
  g_sv.filling = TRUE;
  gtk_check_button_set_active(GTK_CHECK_BUTTON(check), row_ticked(g_sv.rows_tab, item->uid));
  g_sv.filling = was;
}

static void on_item_unbind(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data) {
  (void)factory, (void)data;
  GtkWidget *check = gtk_list_item_get_child(li);
  if (check) g_object_set_data(G_OBJECT(check), "item", NULL);
}

static void on_item_teardown(GtkSignalListItemFactory *factory, GtkListItem *li, gpointer data) {
  (void)factory, (void)data;
  GtkWidget *check = gtk_list_item_get_child(li);
  if (check && g_sv.checks) g_ptr_array_remove_fast(g_sv.checks, check);
}

/* Reloads everything, keeping the pending edits. keep: it's the same sync, so a focused budget
 * keeps its text. */
static void reload(gboolean keep) {
  load_devices();
  load_profile();
  update_controls(!keep);
  if (g_sv.profile && !g_sv.profile->archive) fill_list();
  else if (!keep) g_sv.rows_tab = -1; /* filled when the picker shows */
}

/* --- Size estimate ---
 * mc_sync_estimate_profile() reads every track (about half a second for 50,000), so it runs
 * on a thread, for the profile as shown (pending edits included), once the edits pause. Like
 * sync's own callbacks, the thread only calls in while the mc_sync exists: it is joined
 * before the page lets go of it (sync_view_open() with another, or NULL before sync
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
  char text[512] = "", a[32], b[32];
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
  if (job->status == MC_OK && e->imports_dropped > 0) {
    size_t len = g_strlcat(text, "\n", sizeof text);
    if (len < sizeof text)
      mc_format(text + len, sizeof text - len, mc_str_plural(MC_STR_LINUX_IMPORTS_DROPPED_COMPUTER, e->imports_dropped),
                MC_D(e->imports_dropped));
  }
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_sv.format_row), text);
  if (over) gtk_widget_add_css_class(g_sv.format_row, "warning");
  else gtk_widget_remove_css_class(g_sv.format_row, "warning");
}

static void clear_estimate(void) {
  if (!g_sv.root) return;
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_sv.format_row), "");
  gtk_widget_remove_css_class(g_sv.format_row, "warning");
}

static void estimate_start(void);
static void confirm_archive_estimated(estimate_job *job);

static gboolean estimate_done(gpointer data) {
  estimate_job *job = data;
  if (job->confirm) { /* the archive confirmation's */
    if (g_sv.est.archive == job->thread) {
      g_thread_join(job->thread);
      g_sv.est.archive = NULL;
      confirm_archive_estimated(job);
    } /* else cancelled: estimate_cancel() joined it already */
    estimate_job_free(job);
    return G_SOURCE_REMOVE;
  }
  if (g_sv.est.thread == job->thread) {
    g_thread_join(job->thread); /* it has posted: about to return */
    g_sv.est.thread = NULL;
    if (g_sv.root && job->seq == g_sv.est.seq && job->sync == g_sv.sync && !strcmp(job->uid, g_sv.uid))
      show_estimate(job);
    if (g_sv.est.again) estimate_start();
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

/* A job for p, this computer's profile. */
static estimate_job *estimate_job_new(const mc_sync_profile *p) {
  estimate_job *job = g_new0(estimate_job, 1);
  job->sync = g_sv.sync;
  g_strlcpy(job->uid, g_sv.uid, sizeof job->uid);
  job->profile = *p;
  job->profile.name = NULL;
  job->profile.removed = NULL;
  job->profile.removed_count = 0;
  job->profile.items = p->count > 0 ? g_memdup2(p->items, (gsize)p->count * sizeof *p->items) : NULL;
  return job;
}

static void estimate_start(void) {
  if (g_sv.est.thread) {
    g_sv.est.again = TRUE;
    return;
  }
  g_sv.est.again = FALSE;
  const mc_sync_profile *p = g_sv.profile;
  if (!g_sv.sync || !p || !g_sv.grouped) {
    clear_estimate();
    return;
  }
  estimate_job *job = estimate_job_new(p);
  job->seq = ++g_sv.est.seq;
  /* The idle that ends it can't run before this returns, so job->thread is set by then. */
  job->thread = g_sv.est.thread = g_thread_new("estimate", estimate_main, job);
}

static gboolean on_estimate_timeout(gpointer data) {
  (void)data;
  g_sv.estimate_source = 0;
  estimate_start();
  return G_SOURCE_REMOVE;
}

/* Works the estimate out again shortly, for the profile as it is then. */
static void estimate_request(void) {
  g_clear_handle_id(&g_sv.estimate_source, g_source_remove);
  g_sv.estimate_source = g_timeout_add(ESTIMATE_DELAY_MS, on_estimate_timeout, NULL);
}

/* Waits for the thread and drops its result: before sync goes. */
/* Lets a press of Select all or Select none being stored finish: what's edited next is read
 * after it. */
static void select_wait(void) {
  if (!g_sv.select_thread) return;
  g_thread_join(g_sv.select_thread);
  g_sv.select_thread = NULL;
}

static void estimate_cancel(void) {
  g_clear_handle_id(&g_sv.estimate_source, g_source_remove);
  select_wait(); /* it uses sync too; its idle frees the job, seeing it's no longer current */
  if (g_sv.est.thread) {
    g_thread_join(g_sv.est.thread); /* its idle frees the job, seeing it's no longer current */
    g_sv.est.thread = NULL;
  }
  if (g_sv.est.archive) {
    g_thread_join(g_sv.est.archive);
    g_sv.est.archive = NULL;
  }
  g_sv.est.again = FALSE;
}

/* --- Applying edits --- */

/* Says how storing edits went, and shows what's stored now, which may include others' edits
 * made meanwhile. */
static void show_stored(mc_status st) {
  if (st == MC_ERR_LOCKED) toast(mc_str(MC_STR_PROFILE_LOCKED));
  else if (st != MC_OK) toast_status(MC_STR_PROFILE_SAVE_FAILED, st);
  else if (g_sv.sync) mc_sync_notify_changed(g_sv.sync);
  load_profile();
  if (g_sv.root) {
    update_controls(FALSE);
    if (g_sv.profile && !g_sv.profile->archive) fill_list();
  }
}

/* Applies the pending edits now: get, change, set. */
static void flush(void) {
  g_clear_handle_id(&g_sv.apply_source, g_source_remove);
  select_wait();
  if (!pending_any() || !active()) {
    pending_clear();
    return;
  }
  mc_sync_profile *p = NULL;
  mc_status st;
  st = mc_sync_get_profile(g_sv.sync, g_sv.uid, &p);
  if (st == MC_OK) st = apply_pending(p);
  if (st == MC_OK) st = mc_sync_set_profile(g_sv.sync, g_sv.uid, p);
  mc_sync_profile_free(p);
  pending_clear();
  show_stored(st);
}

static gboolean on_apply_timeout(gpointer data) {
  (void)data;
  g_sv.apply_source = 0;
  flush();
  return G_SOURCE_REMOVE;
}

static void schedule(void) {
  g_clear_handle_id(&g_sv.apply_source, g_source_remove);
  g_sv.apply_source = g_timeout_add(APPLY_DELAY_MS, on_apply_timeout, NULL);
  estimate_request(); /* for the edited profile, not waiting for it to be stored */
}

/* --- Leave group --- */

static void on_leave_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog, (void)data;
  if (strcmp(response, "leave") || !g_sv.sync || !g_sv.grouped) return;
  flush();
  mc_status st = mc_sync_leave_group(g_sv.sync); /* blocks for a few seconds at most */
  if (st == MC_OK) {
    if (g_sv.cb.changed) g_sv.cb.changed(g_sv.cb.user);
  } else if (st == MC_ERR_LOCKED) {
    toast(mc_str(MC_STR_LINUX_COMPUTER_LEAVE_ONLY_ARCHIVE));
  } else {
    toast_status(MC_STR_LEAVE_GROUP_FAILED, st);
  }
}

static void on_leave_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  if (!g_sv.sync || !g_sv.grouped) return;
  char *body = mc_format_alloc(mc_str(MC_STR_LINUX_COMPUTER_LEAVE_GROUP_CONFIRM),
                               MC_S(g_sv.others && *g_sv.others ? g_sv.others : mc_str(MC_STR_OTHER_DEVICES)));
  AdwDialog *dialog = adw_alert_dialog_new(mc_str(MC_STR_LEAVE_GROUP_TITLE), body);
  mc_free(body);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "leave",
                                 mc_str_mnemonic(MC_STR_LEAVE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "leave", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_leave_response), NULL);
  adw_dialog_present(dialog, g_sv.root);
}

/* --- Sync now --- */

/* Syncs with every member online. */
static void sync_now(void) {
  if (!g_sv.sync) return;
  flush();
  /* Also chooses this computer's profile as it stands, if it wasn't. */
  mc_status st = mc_sync_now(g_sv.sync, NULL);
  g_sv.message[0] = '\0';
  if (st != MC_OK) mc_format(g_sv.message, sizeof g_sv.message, mc_str(MC_STR_SYNC_START_FAILED), MC_S(mc_status_str(st)));
  if (st == MC_OK) reload(TRUE); /* chosen now: the banner goes */
  else update_status();
}

static void on_sync_now_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  /* Ends every session, which on_sync_done then reports as cancelled. */
  if (syncing()) mc_sync_cancel(g_sv.sync, NULL);
  else sync_now();
}

static void on_banner_button(AdwBanner *banner, gpointer data) {
  (void)data;
  if (g_sv.banner_action == BANNER_OFFER) issue_dialog_present_offer(GTK_WIDGET(banner));
  else sync_now();
}

/* --- Archive --- */

static void set_archive(gboolean archive) {
  if (!g_sv.sync || !g_sv.grouped) return;
  flush();
  mc_status st = mc_sync_set_archive(g_sv.sync, g_sv.uid, archive);
  if (st == MC_OK) mc_sync_notify_changed(g_sv.sync);
  else if (st == MC_ERR_LOCKED && !archive) toast(mc_str(MC_STR_LINUX_COMPUTER_ARCHIVE_LOCKED));
  else toast_status(MC_STR_PROFILE_ARCHIVE_FAILED, st);
  reload(TRUE);
}

typedef struct archive_ask {
  char uid[MC_UID_STRLEN];
  mc_sync *sync;
} archive_ask;

static void on_archive_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  archive_ask *ask = g_object_get_data(G_OBJECT(dialog), "archive-ask");
  /* Still the sync it was asked about. */
  if (!strcmp(response, "archive") && ask && ask->sync == g_sv.sync && !strcmp(ask->uid, g_sv.uid)) set_archive(TRUE);
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
  if (!g_sv.profile) return;
  char *heading = mc_format_alloc(mc_str(MC_STR_MAKE_ARCHIVE_TITLE), MC_S(g_sv.name));
  AdwDialog *dialog = adw_alert_dialog_new(heading, mc_str(MC_STR_MAKE_ARCHIVE_TEXT_UNKNOWN));
  mc_free(heading);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", mc_str_mnemonic(MC_STR_CANCEL), "archive",
                                 mc_str_mnemonic(MC_STR_MAKE_ARCHIVE), NULL);
  adw_alert_dialog_set_response_appearance(alert, "archive", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(alert, "archive");
  adw_alert_dialog_set_close_response(alert, "cancel");
  archive_ask *ask = g_new0(archive_ask, 1);
  g_strlcpy(ask->uid, g_sv.uid, sizeof ask->uid);
  ask->sync = g_sv.sync;
  g_object_set_data_full(G_OBJECT(dialog), "archive-ask", ask, g_free);
  g_signal_connect(dialog, "response", G_CALLBACK(on_archive_response), NULL);

  /* How much the whole library comes to as originals: the profile as an archive. It is
   * joined in its own idle, or by estimate_cancel() before sync stops. */
  estimate_job *job = estimate_job_new(g_sv.profile);
  job->profile.archive = 1;
  job->confirm = g_object_ref(alert);
  if (g_sv.est.archive) { /* one asked a moment ago: let it finish first */
    g_thread_join(g_sv.est.archive);
    g_sv.est.archive = NULL;
  }
  job->thread = g_sv.est.archive = g_thread_new("estimate-archive", estimate_main, job);
  adw_dialog_present(dialog, g_sv.root);
}

static void on_archive_active(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  if (g_sv.filling || !g_sv.profile) return;
  gboolean on = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  if (on == (g_sv.profile->archive != 0)) return;
  /* The switch shows what is, until it's confirmed and stored. */
  g_sv.filling = TRUE;
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), g_sv.profile->archive);
  g_sv.filling = FALSE;
  if (on) ask_make_archive();
  else set_archive(FALSE);
}

/* --- Edits from the controls --- */

/* The format and the budget exclude each other (the budget picks the format, as Auto), so
 * an edit of either is pending for both. */
static void pend_format_budget(void) {
  g_sv.pending.format = g_sv.profile->format;
  g_sv.pending.budget_enabled = g_sv.profile->budget_enabled;
  g_sv.pending.budget_bytes = g_sv.profile->budget_bytes;
  g_sv.pending.mask |= PEND_FORMAT | PEND_BUDGET;
  update_controls(FALSE);
  schedule();
}

static int64_t budget_from_spin(void) {
  int gb = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sv.budget_spin));
  return gb > 0 ? (int64_t)gb * GIB : 0;
}

/* Auto turns the budget on; any other format turns it off. */
static void on_format_selected(GObject *row, GParamSpec *pspec, gpointer data) {
  (void)pspec, (void)data;
  if (g_sv.filling || !g_sv.profile) return;
  guint i = adw_combo_row_get_selected(ADW_COMBO_ROW(row));
  if (i == GTK_INVALID_LIST_POSITION) return;
  mc_sync_profile *p = g_sv.profile;
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
  if (g_sv.filling || !g_sv.profile) return;
  int64_t bytes = budget_from_spin();
  if (!bytes) bytes = DEFAULT_BUDGET_GB * GIB;
  mc_sync_profile *p = g_sv.profile;
  p->budget_enabled = gtk_switch_get_active(GTK_SWITCH(g_sv.budget_switch));
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
  if (g_sv.filling || !g_sv.profile) return;
  g_sv.profile->sync_all = g_sv.pending.sync_all = adw_switch_row_get_active(ADW_SWITCH_ROW(row));
  g_sv.pending.mask |= PEND_SYNC_ALL;
  update_picker();
  if (g_sv.profile->sync_all && g_sv.tab >= TAB_PLAYLISTS)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_sv.tabs[TAB_ALBUMS]), TRUE); /* fills the list */
  else fill_list(); /* the ticks mean something else now */
  schedule();
}

static gboolean exclude_tick(gpointer data) {
  tick *t = data;
  if (g_sv.sync && !strcmp(t->device, g_sv.uid)) {
    flush();
    char uid[1][MC_UID_STRLEN];
    memcpy(uid[0], t->uid, MC_UID_STRLEN);
    show_stored(mc_sync_exclude_items(g_sv.sync, g_sv.uid, t->kind, (const char(*)[MC_UID_STRLEN])uid, 1, !t->on));
  }
  g_free(t);
  return G_SOURCE_REMOVE;
}

/* One press: a long list takes seconds to store, and the window has to stay alive for it. */
typedef struct select_job {
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  mc_profile_item_kind kind;
  char (*rows)[MC_UID_STRLEN];
  int count, on;
  mc_status status;
} select_job;

static gboolean select_done(gpointer data) {
  select_job *job = data;
  select_wait();
  if (job->sync == g_sv.sync && !strcmp(job->uid, g_sv.uid)) show_stored(job->status);
  g_free(job->rows);
  g_free(job);
  return G_SOURCE_REMOVE;
}

static gpointer select_main(gpointer data) {
  select_job *job = data;
  job->status = mc_sync_select_items(job->sync, job->uid, job->kind, (const char(*)[MC_UID_STRLEN])job->rows,
                                     job->count, job->on);
  g_idle_add(select_done, job);
  return NULL;
}

/* Select all and Select none: the rows shown, stored at once as one write rather than waiting
 * as a toggle each, which a long list couldn't afford, and on a thread of its own. Edits made
 * before them go first. A press while one is being stored does nothing. */
static void on_select_clicked(GtkButton *button, gpointer data) {
  (void)button;
  if (g_sv.rows_tab < 0 || !g_sv.sync || !editable() || g_sv.select_thread) return;
  flush(); /* may fill the list again */
  guint n = g_list_model_get_n_items(G_LIST_MODEL(g_sv.store));
  if (!n) return;
  char (*uids)[MC_UID_STRLEN] = g_malloc_n(n, MC_UID_STRLEN);
  for (guint i = 0; i < n; i++) {
    SvItem *item = g_list_model_get_item(G_LIST_MODEL(g_sv.store), i);
    memcpy(uids[i], item->uid, MC_UID_STRLEN);
    g_object_unref(item);
  }
  select_job *job = g_new0(select_job, 1);
  job->sync = g_sv.sync;
  memcpy(job->uid, g_sv.uid, MC_UID_STRLEN);
  job->kind = k_tab_kind[g_sv.rows_tab];
  job->rows = uids;
  job->count = (int)n;
  job->on = GPOINTER_TO_INT(data);
  g_sv.select_thread = g_thread_new("select", select_main, job);
}

static void on_tab_toggled(GtkToggleButton *button, gpointer data) {
  int tab = GPOINTER_TO_INT(data);
  if (!gtk_toggle_button_get_active(button) || tab == g_sv.tab) return;
  g_sv.tab = tab;
  fill_list();
}

/* --- Building --- */

static void on_destroy(GtkWidget *widget, gpointer data) {
  (void)widget, (void)data;
  /* Sync has been let go of by now (sync_view_open(NULL) before it stops). */
  estimate_cancel();
  g_clear_handle_id(&g_sv.apply_source, g_source_remove);
  pending_clear();
  free_profile();
  g_clear_object(&g_sv.store);
  g_clear_pointer(&g_sv.checks, g_ptr_array_unref);
  g_sv.root = NULL;
}

/* This computer's name, with whether it's in a group under it. */
static GtkWidget *build_self_header(void) {
  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
  GtkWidget *icon = gtk_image_new_from_icon_name("computer-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
  gtk_widget_add_css_class(icon, "dim-label");
  gtk_grid_attach(GTK_GRID(grid), icon, 0, 0, 1, 2);
  g_sv.self_name = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_sv.self_name), 0);
  gtk_label_set_ellipsize(GTK_LABEL(g_sv.self_name), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(g_sv.self_name, TRUE);
  gtk_widget_add_css_class(g_sv.self_name, "title-3");
  gtk_grid_attach(GTK_GRID(grid), g_sv.self_name, 1, 0, 1, 1);
  g_sv.self_state = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_sv.self_state), 0);
  gtk_label_set_wrap(GTK_LABEL(g_sv.self_state), TRUE);
  gtk_widget_add_css_class(g_sv.self_state, "dim-label");
  gtk_grid_attach(GTK_GRID(grid), g_sv.self_state, 1, 1, 1, 1);
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), grid);
  return group;
}

/* Quality: whether it's an archive, its format and its budget. */
static GtkWidget *build_quality_group(void) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_LINUX_SYNC_QUALITY));

  g_sv.archive_row = adw_switch_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(g_sv.archive_row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_sv.archive_row), mc_str(MC_STR_DEVICE_ARCHIVE_TITLE));
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_sv.archive_row), mc_str(MC_STR_DEVICE_ARCHIVE_TEXT));
  g_signal_connect(g_sv.archive_row, "notify::active", G_CALLBACK(on_archive_active), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_sv.archive_row);

  GtkStringList *formats = gtk_string_list_new(NULL);
  gtk_string_list_append(formats, mc_str(MC_STR_FORMAT_AUTO));
  gtk_string_list_append(formats, mc_str(MC_STR_FORMAT_ORIGINAL));
  for (int b = 0; b < MC_OPUS_BITRATE_COUNT; b++) {
    char text[32];
    mc_format(text, sizeof text, mc_str(MC_STR_FORMAT_OPUS), MC_D(mc_opus_bitrates[b]));
    gtk_string_list_append(formats, text);
  }
  g_sv.format_row = adw_combo_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(g_sv.format_row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_sv.format_row), mc_str(MC_STR_SYNC_FORMAT));
  adw_combo_row_set_model(ADW_COMBO_ROW(g_sv.format_row), G_LIST_MODEL(formats));
  g_object_unref(formats);
  g_signal_connect(g_sv.format_row, "notify::selected", G_CALLBACK(on_format_selected), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_sv.format_row);

  /* Limit storage to [32] GB (o): the switch turns the budget on, which makes the format Auto. */
  g_sv.budget_row = adw_action_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_sv.budget_row), mc_str(MC_STR_BUDGET_LIMIT));
  g_sv.budget_spin = gtk_spin_button_new_with_range(1, 99999, 1);
  gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(g_sv.budget_spin), TRUE);
  gtk_widget_set_valign(g_sv.budget_spin, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(g_sv.budget_spin), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_BUDGET_LIMIT), -1);
  g_signal_connect(g_sv.budget_spin, "value-changed", G_CALLBACK(on_budget_value), NULL);
  GtkWidget *unit = gtk_label_new(mc_str(MC_STR_BUDGET_UNIT));
  g_sv.budget_switch = gtk_switch_new();
  gtk_widget_set_valign(g_sv.budget_switch, GTK_ALIGN_CENTER);
  gtk_accessible_update_property(GTK_ACCESSIBLE(g_sv.budget_switch), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 mc_str(MC_STR_BUDGET_LIMIT), -1);
  g_signal_connect(g_sv.budget_switch, "notify::active", G_CALLBACK(on_budget_active), NULL);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_sv.budget_row), g_sv.budget_spin);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_sv.budget_row), unit);
  adw_action_row_add_suffix(ADW_ACTION_ROW(g_sv.budget_row), g_sv.budget_switch);
  adw_action_row_set_activatable_widget(ADW_ACTION_ROW(g_sv.budget_row), g_sv.budget_switch);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_sv.budget_row);
  return group;
}

static GtkWidget *build_what_group(void) {
  GtkWidget *group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), mc_str(MC_STR_WHAT_TO_SYNC));
  g_sv.sync_all_row = adw_switch_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_sv.sync_all_row), mc_str(MC_STR_SYNC_EVERYTHING));
  g_signal_connect(g_sv.sync_all_row, "notify::active", G_CALLBACK(on_sync_all_active), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_sv.sync_all_row);

  /* The picker, shown while not syncing everything: a segmented choice of kind, the items
   * with checkboxes, and how many of each are picked. */
  g_sv.picker = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(g_sv.picker, 12);
  GtkWidget *tabs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_add_css_class(tabs, "linked");
  gtk_widget_set_halign(tabs, GTK_ALIGN_START);
  for (int t = 0; t < NUM_TABS; t++) {
    g_sv.tabs[t] = gtk_toggle_button_new_with_label(mc_str(k_tab_name[t]));
    if (t) gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(g_sv.tabs[t]), GTK_TOGGLE_BUTTON(g_sv.tabs[0]));
    g_signal_connect(g_sv.tabs[t], "toggled", G_CALLBACK(on_tab_toggled), GINT_TO_POINTER(t));
    gtk_box_append(GTK_BOX(tabs), g_sv.tabs[t]);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_sv.tabs[0]), TRUE);
  gtk_box_append(GTK_BOX(g_sv.picker), tabs);

  /* Select all and Select none, for the tab open. */
  GtkWidget *select = g_sv.select_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  g_sv.select_all = gtk_button_new_with_label(mc_str(MC_STR_SELECT_ALL));
  g_signal_connect(g_sv.select_all, "clicked", G_CALLBACK(on_select_clicked), GINT_TO_POINTER(TRUE));
  g_sv.select_none = gtk_button_new_with_label(mc_str(MC_STR_SELECT_NONE));
  g_signal_connect(g_sv.select_none, "clicked", G_CALLBACK(on_select_clicked), GINT_TO_POINTER(FALSE));
  gtk_box_append(GTK_BOX(select), g_sv.select_all);
  gtk_box_append(GTK_BOX(select), g_sv.select_none);
  gtk_box_append(GTK_BOX(g_sv.picker), select);

  g_sv.store = g_list_store_new(SV_TYPE_ITEM);
  g_sv.checks = g_ptr_array_new();
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_item_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(on_item_bind), NULL);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_item_unbind), NULL);
  g_signal_connect(factory, "teardown", G_CALLBACK(on_item_teardown), NULL);
  GtkNoSelection *model = gtk_no_selection_new(G_LIST_MODEL(g_object_ref(g_sv.store)));
  GtkWidget *list = gtk_list_view_new(GTK_SELECTION_MODEL(model), factory);
  gtk_widget_add_css_class(list, "sync-picker");
  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scroller), 320);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
  g_sv.empty = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(g_sv.empty), TRUE);
  gtk_widget_add_css_class(g_sv.empty, "dim-label");
  gtk_widget_set_margin_top(g_sv.empty, 48);
  gtk_widget_set_margin_bottom(g_sv.empty, 48);
  g_sv.list_stack = gtk_stack_new();
  gtk_widget_add_css_class(g_sv.list_stack, "card");
  gtk_widget_set_overflow(g_sv.list_stack, GTK_OVERFLOW_HIDDEN);
  gtk_stack_add_named(GTK_STACK(g_sv.list_stack), scroller, "list");
  gtk_stack_add_named(GTK_STACK(g_sv.list_stack), g_sv.empty, "empty");
  gtk_box_append(GTK_BOX(g_sv.picker), g_sv.list_stack);

  g_sv.count = gtk_label_new(NULL);
  gtk_label_set_xalign(GTK_LABEL(g_sv.count), 0);
  gtk_label_set_wrap(GTK_LABEL(g_sv.count), TRUE);
  gtk_widget_add_css_class(g_sv.count, "dim-label");
  gtk_box_append(GTK_BOX(g_sv.picker), g_sv.count);
  gtk_widget_set_visible(g_sv.picker, FALSE);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), g_sv.picker);
  return group;
}

static void on_member_removed(void *user) {
  (void)user;
  if (g_sv.cb.changed) g_sv.cb.changed(g_sv.cb.user);
}

static void on_member_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)data;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  if (uid && g_sv.sync) device_dialog_present(GTK_WIDGET(list), g_sv.sync, uid, on_member_removed, NULL);
}

static void on_nearby_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list, (void)data;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  if (uid && g_sv.cb.pair) g_sv.cb.pair(g_sv.cb.user, uid);
}

static void on_add_device_clicked(GtkButton *button, gpointer data) {
  (void)data;
  if (g_sv.cb.add_device) g_sv.cb.add_device(g_sv.cb.user, GTK_WIDGET(button));
}

static GtkWidget *boxed_list(GCallback activated) {
  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");
  g_signal_connect(list, "row-activated", activated, NULL);
  return list;
}

/* The other members, the devices nearby that could pair, with + to add one by its address, and
 * Leave group. */
static GtkWidget *build_devices_page(void) {
  GtkWidget *members = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(members), mc_str(MC_STR_DEVICES));
  GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_set_tooltip_text(add, mc_str(MC_STR_ADD_DEVICE_BY_ADDRESS));
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_device_clicked), NULL);
  adw_preferences_group_set_header_suffix(ADW_PREFERENCES_GROUP(members), add);
  g_sv.members = boxed_list(G_CALLBACK(on_member_activated));
  g_sv.members_none = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(g_sv.members_none), TRUE);
  gtk_widget_set_margin_top(g_sv.members_none, 12);
  gtk_widget_set_margin_bottom(g_sv.members_none, 12);
  gtk_widget_set_margin_start(g_sv.members_none, 12);
  gtk_widget_set_margin_end(g_sv.members_none, 12);
  gtk_widget_add_css_class(g_sv.members_none, "dim-label");
  gtk_list_box_set_placeholder(GTK_LIST_BOX(g_sv.members), g_sv.members_none);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(members), g_sv.members);

  /* Picking one asks it to pair. */
  g_sv.nearby_group = adw_preferences_group_new();
  g_sv.nearby = boxed_list(G_CALLBACK(on_nearby_activated));
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_sv.nearby_group), g_sv.nearby);
  gtk_widget_set_visible(g_sv.nearby_group, FALSE);

  g_sv.leave_group = adw_preferences_group_new();
  GtkWidget *leave = gtk_button_new_with_label(mc_str(MC_STR_LEAVE_GROUP_BUTTON));
  gtk_widget_add_css_class(leave, "destructive-action");
  gtk_widget_set_halign(leave, GTK_ALIGN_START);
  g_signal_connect(leave, "clicked", G_CALLBACK(on_leave_clicked), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_sv.leave_group), leave);
  gtk_widget_set_visible(g_sv.leave_group, FALSE);

  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(members));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_sv.nearby_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_sv.leave_group));
  return page;
}

/* What's shown comes up to date as the page comes into view. */
static void on_map(GtkWidget *widget, gpointer data) {
  (void)widget, (void)data;
  reload(TRUE);
}

GtkWidget *sync_view_new(const sync_view_callbacks *callbacks) {
  if (callbacks) g_sv.cb = *callbacks;
  for (int i = 0; i < NUM_TABS; i++) {
    if (!g_sv.sel[i]) g_sv.sel[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    if (!g_sv.removed_items[i]) g_sv.removed_items[i] = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  }
  if (!g_sv.pending.toggles) g_sv.pending.toggles = g_array_new(FALSE, FALSE, sizeof(toggle));
  g_sv.rows_tab = -1;

  /* This computer: what it syncs above, in what quality below. */
  GtkWidget *self = adw_preferences_page_new();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), ADW_PREFERENCES_GROUP(build_self_header()));
  g_sv.what_group = build_what_group();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), ADW_PREFERENCES_GROUP(g_sv.what_group));
  g_sv.quality_group = build_quality_group();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(self), ADW_PREFERENCES_GROUP(g_sv.quality_group));

  GtkWidget *panels = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_box_set_homogeneous(GTK_BOX(panels), TRUE);
  gtk_widget_set_vexpand(panels, TRUE);
  gtk_box_append(GTK_BOX(panels), self);
  gtk_box_append(GTK_BOX(panels), build_devices_page());

  GtkWidget *banner = adw_banner_new("");
  g_sv.banner = ADW_BANNER(banner);
  adw_banner_set_use_markup(g_sv.banner, FALSE);
  g_signal_connect(banner, "button-clicked", G_CALLBACK(on_banner_button), NULL);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_box_append(GTK_BOX(box), banner);
  gtk_box_append(GTK_BOX(box), strip_new(&g_sv.strip_text, &g_sv.sync_now, mc_str(MC_STR_SYNC_NOW),
                                         G_CALLBACK(on_sync_now_clicked)));
  gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(box), panels);
  GtkWidget *overlay = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(overlay), box);
  g_sv.toasts = ADW_TOAST_OVERLAY(overlay);
  g_sv.root = overlay;
  g_signal_connect(overlay, "destroy", G_CALLBACK(on_destroy), NULL);
  g_signal_connect(overlay, "map", G_CALLBACK(on_map), NULL);
  update_controls(TRUE);
  return overlay;
}

/* --- API --- */

void sync_view_open(mc_sync *sync, mc_library *lib) {
  /* Edits still waiting go to the profile they were made for, while its sync is still there:
   * opening with NULL comes before sync stops. */
  if (g_sv.sync && (sync == g_sv.sync || !sync)) flush();
  g_clear_handle_id(&g_sv.apply_source, g_source_remove);
  pending_clear();
  gboolean same = sync && sync == g_sv.sync;
  if (!same) {
    estimate_cancel(); /* it uses the old one */
    device_dialog_close();
    clear_estimate();
    g_strlcpy(g_sv.uid, sync ? mc_sync_device_uid(sync) : "", sizeof g_sv.uid);
    g_sv.nbusy = 0;
    g_sv.message[0] = '\0';
  }
  g_sv.sync = sync;
  g_sv.lib = lib;
  g_sv.rows_tab = -1; /* the library may have changed: reload */
  if (!g_sv.root) return;
  if (!same) {
    g_sv.filling = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_sv.tabs[TAB_ALBUMS]), TRUE);
    g_sv.filling = FALSE;
    g_sv.tab = TAB_ALBUMS;
  }
  reload(FALSE);
}

void sync_view_refresh(void) {
  if (g_sv.root && gtk_widget_get_mapped(g_sv.root)) reload(TRUE);
  device_dialog_refresh();
}

gboolean sync_view_open_device(const char *uid) {
  return g_sv.root && g_sv.sync && device_dialog_present(g_sv.root, g_sv.sync, uid, on_member_removed, NULL);
}

static gboolean focus_what_to_sync(gpointer data) {
  (void)data;
  if (g_sv.sync_all_row) gtk_widget_grab_focus(g_sv.sync_all_row); /* and the page scrolls to it */
  return G_SOURCE_REMOVE;
}

void sync_view_focus_what_to_sync(void) {
  g_idle_add(focus_what_to_sync, NULL);
}

/* The rows' spinners came or went. */
static void refresh_rows(void) {
  if (gtk_widget_get_mapped(g_sv.root)) load_devices();
  update_status();
}

void sync_view_on_sync_event(const sync_event *ev) {
  if (!g_sv.root || !g_sv.sync) return;
  if (ev->kind == SYNC_EV_REACH || ev->kind == SYNC_EV_VERSION || ev->kind == SYNC_EV_WAITING) {
    update_status(); /* the outlook, the offer */
    device_dialog_refresh(); /* its connections, version, problems */
    return;
  }
  if (!ev->uid[0]) return;
  int i = busy_index(ev->uid);
  switch (ev->kind) {
    case SYNC_EV_PROGRESS:
      if (ev->phase == MC_SYNC_PHASE_DONE) { /* over; on_sync_done may follow, or not */
        if (i < 0) return;
        busy_remove(i);
        refresh_rows();
        break;
      }
      g_sv.phase = ev->phase;
      g_sv.sent = ev->rows_sent;
      g_sv.received = ev->rows_received;
      g_sv.files_done = ev->files_done;
      g_sv.files_total = ev->files_total;
      g_sv.sends_done = ev->sends_done;
      g_sv.sends_total = ev->sends_total;
      g_sv.bytes_received = ev->bytes_received;
      g_sv.bytes_sent = ev->bytes_sent;
      g_strlcpy(g_sv.pace, ev->pace, sizeof g_sv.pace);
      if (i < 0 && g_sv.nbusy < MAX_BUSY) {
        g_strlcpy(g_sv.busy[g_sv.nbusy++], ev->uid, MC_UID_STRLEN);
        g_sv.message[0] = '\0';
        refresh_rows();
      } else {
        update_status();
      }
      break;
    case SYNC_EV_DONE:
      busy_remove(i);
      g_sv.message[0] = '\0';
      if (ev->cancelled || ev->peer_cancelled) {
        g_strlcpy(g_sv.message, mc_str(MC_STR_SYNC_CANCELLED), sizeof g_sv.message);
      } else if (ev->status != MC_OK && ev->name[0]) {
        mc_format(g_sv.message, sizeof g_sv.message, mc_str(MC_STR_SYNC_FAILED_WITH), MC_S(ev->name),
                  MC_S(mc_status_str(ev->status)));
      } else if (ev->status != MC_OK) {
        mc_format(g_sv.message, sizeof g_sv.message, mc_str(MC_STR_SYNC_FAILED), MC_S(mc_status_str(ev->status)));
      } else if (!ev->library_synced) {
        g_strlcpy(g_sv.message, mc_str(MC_STR_SYNC_DEVICE_LIST_ONLY), sizeof g_sv.message);
      } else {
        g_strlcpy(g_sv.message, ev->finished, sizeof g_sv.message); /* tracks still missing, or "" */
      }
      /* The last activity, the rows, maybe the profile, have changed. */
      reload(TRUE);
      break;
    default:
      break;
  }
}

void sync_view_update(void) {
  if (g_sv.root && g_sv.sync) update_status();
}
