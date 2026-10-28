#include "devices.h"

#include <string.h>

#include "add_device.h"
#include "device_view.h"
#include "pair_dialog.h"
#include "sync_bridge.h"

enum {
  REFRESH_DEVICES_MS = 200, /* after sync events, once for a burst */
  REFRESH_LIBRARY_MS = 500, /* after library rows or files arrive, once for a burst */
  POLL_DEVICES_S = 10,      /* renames elsewhere fire no event */
  MAX_SYNCING = 16,         /* devices marked as syncing at once */
};

static struct {
  app *a;
  GtkWidget *paired, *nearby, *nearby_heading, *none;
  char selected[MC_UID_STRLEN]; /* the device whose page is picked; "" if none */
  char syncing[MAX_SYNCING][MC_UID_STRLEN]; /* devices with a session in progress */
  guint refresh_source, library_source, poll_source;
} d;

/* --- Helpers --- */

static GtkWidget *window(void) {
  return GTK_WIDGET(d.a->win);
}

static void alert(const char *heading, const char *body) {
  AdwDialog *dialog = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(dialog, window());
}

/* "address:port", with an IPv6 address in brackets; g_free it. */
static char *format_address(const char *host, unsigned port) {
  return g_strdup_printf(strchr(host, ':') ? "[%s]:%u" : "%s:%u", host, port);
}

static int syncing_slot(const char *uid) {
  for (int i = 0; i < MAX_SYNCING; i++)
    if (d.syncing[i][0] && !strcmp(d.syncing[i], uid)) return i;
  return -1;
}

/* Marks uid as syncing or not; TRUE if that changed. */
static gboolean set_syncing(const char *uid, gboolean syncing) {
  int i = syncing_slot(uid);
  if (!syncing) {
    if (i >= 0) d.syncing[i][0] = '\0';
    return i >= 0;
  }
  if (i >= 0 || !uid[0]) return FALSE;
  for (i = 0; i < MAX_SYNCING; i++)
    if (!d.syncing[i][0]) {
      g_strlcpy(d.syncing[i], uid, MC_UID_STRLEN);
      return TRUE;
    }
  return FALSE; /* too many at once: just not marked */
}

/* --- Settings --- */

/* The name saved in settings.json, or NULL; g_free it. */
static char *saved_name(void) {
  mc_settings *settings = NULL;
  if (mc_settings_load(d.a->settings_dir, &settings) != MC_OK) return NULL;
  char *name = g_strdup(mc_settings_device_name(settings));
  mc_settings_free(settings);
  return name;
}

static gboolean save_name(const char *name) {
  mc_settings *settings = NULL;
  if (mc_settings_load(d.a->settings_dir, &settings) != MC_OK) return FALSE;
  gboolean ok = mc_settings_set_device_name(settings, name) == MC_OK && mc_settings_save(settings) == MC_OK;
  mc_settings_free(settings);
  return ok;
}

/* A group may rename this computer from another device, and this computer takes the name on;
 * settings.json keeps it for next time. A computer with no saved name goes by its host name,
 * which isn't saved. */
static void save_own_name(mc_sync *sync) {
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  if (mc_sync_get_device_name(sync, name, sizeof name) != MC_OK || !name[0]) return;
  char *saved = saved_name();
  gboolean same = saved ? !strcmp(saved, name) : !strcmp(g_get_host_name(), name);
  g_free(saved);
  if (!same) save_name(name); /* failing, it's saved on the next refresh */
}

/* --- The section --- */

/* The kind's icon; not every icon theme has a watch, so phones stand in. */
static GtkWidget *kind_icon(int kind) {
  const char *names[] = {kind == MC_DEVICE_WATCH   ? "smartwatch-symbolic"
                         : kind == MC_DEVICE_PHONE ? "phone-symbolic"
                                                   : "computer-symbolic",
                         "phone-symbolic", NULL};
  GIcon *gicon = g_themed_icon_new_from_names((char **)names, -1);
  GtkWidget *icon = gtk_image_new_from_gicon(gicon);
  g_object_unref(gicon);
  return icon;
}

/* A row: the kind's icon, the name, a dim line under it (NULL for none), and a spinner while
 * it syncs. */
static GtkWidget *device_row(const char *uid, int kind, const char *name, const char *sub, gboolean busy,
                             gboolean dim) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  GtkWidget *icon = kind_icon(kind);
  gtk_box_append(GTK_BOX(box), icon);
  GtkWidget *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
  GtkWidget *label = gtk_label_new(name && *name ? name : mc_str(MC_STR_DEVICE_UNNAMED));
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_box_append(GTK_BOX(text), label);
  if (sub) {
    GtkWidget *line = gtk_label_new(sub);
    gtk_label_set_xalign(GTK_LABEL(line), 0);
    gtk_label_set_ellipsize(GTK_LABEL(line), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(line, "caption");
    gtk_widget_add_css_class(line, "dim-label");
    gtk_box_append(GTK_BOX(text), line);
  }
  gtk_box_append(GTK_BOX(box), text);
  if (dim) {
    gtk_widget_add_css_class(icon, "dim-label");
    gtk_widget_add_css_class(label, "dim-label");
  }
  if (busy) {
    GtkWidget *spinner = gtk_spinner_new();
    gtk_spinner_start(GTK_SPINNER(spinner));
    gtk_widget_set_tooltip_text(spinner, mc_str(MC_STR_SYNCING_PROGRESS));
    gtk_box_append(GTK_BOX(box), spinner);
  }
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  g_object_set_data_full(G_OBJECT(row), "uid", g_strdup(uid), g_free);
  return row;
}

static void clear_list(GtkWidget *list) {
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(list)) && GTK_IS_LIST_BOX_ROW(child))
    gtk_list_box_remove(GTK_LIST_BOX(list), child);
}

static gboolean device_page_shown(void) {
  return !g_strcmp0(gtk_stack_get_visible_child_name(d.a->pages), "device");
}

/* Back to Albums, as when the shown device leaves the group. */
static void show_albums(void) {
  d.selected[0] = '\0';
  gtk_list_box_unselect_all(GTK_LIST_BOX(d.paired));
  GtkListBoxRow *albums = gtk_list_box_get_row_at_index(d.a->nav, 0);
  gtk_list_box_select_row(d.a->nav, albums);
  g_signal_emit_by_name(d.a->nav, "row-activated", albums);
}

/* Rebuilds the section from sync, and everything that shows the devices. */
static void refresh_devices(void) {
  g_clear_handle_id(&d.refresh_source, g_source_remove);
  mc_sync *sync = sync_bridge_get();
  mc_sync_device_list *paired = NULL;
  mc_sync_nearby_list *nearby = NULL;
  if (sync && mc_sync_list_devices(sync, &paired) != MC_OK) paired = NULL;
  if (sync && mc_sync_list_nearby(sync, &nearby) != MC_OK) nearby = NULL;

  clear_list(d.paired);
  clear_list(d.nearby);
  GtkWidget *selected = NULL;
  /* This computer first, then the online devices, then the offline ones, each in the order
   * listed (by name); removed ones left out. */
  for (int pass = 0; paired && pass < 3; pass++)
    for (int i = 0; i < paired->count; i++) {
      const mc_sync_device *dev = &paired->items[i];
      int in_pass = dev->is_self ? 0 : dev->online ? 1 : 2;
      if (dev->removed || in_pass != pass) continue;
      const char *sub = dev->is_self ? mc_str(MC_STR_LINUX_THIS_COMPUTER) : !dev->online ? mc_str(MC_STR_DEVICE_OFFLINE) : NULL;
      GtkWidget *row = device_row(dev->uid, dev->kind, dev->name, sub, syncing_slot(dev->uid) >= 0,
                                  !dev->is_self && !dev->online);
      gtk_list_box_append(GTK_LIST_BOX(d.paired), row);
      if (!strcmp(dev->uid, d.selected)) selected = row;
    }
  int nn = nearby ? nearby->count : 0;
  for (int i = 0; i < nn; i++) {
    const mc_sync_nearby *dev = &nearby->items[i];
    const char *sub = dev->in_other_group ? mc_str(MC_STR_DEVICE_OTHER_GROUP_TIP) : NULL;
    gtk_list_box_append(GTK_LIST_BOX(d.nearby), device_row(dev->uid, dev->kind, dev->name, sub, FALSE, FALSE));
  }
  char heading[128];
  mc_format(heading, sizeof heading, mc_str(MC_STR_DEVICES_UNPAIRED), MC_D(nn));
  gtk_label_set_text(GTK_LABEL(d.nearby_heading), heading);
  gtk_widget_set_visible(d.nearby_heading, nn > 0);
  gtk_widget_set_visible(d.nearby, nn > 0);
  /* The paired list's placeholder shows when there's nothing at all, or says sync is off. */
  char *off = devices_sync_off_text();
  gtk_label_set_text(GTK_LABEL(d.none), off ? off : mc_str(MC_STR_DEVICES_NONE));
  g_free(off);
  gtk_widget_set_visible(d.paired, (paired && paired->count > 0) || nn == 0);
  mc_sync_device_list_free(paired);
  mc_sync_nearby_list_free(nearby);

  if (selected) {
    gtk_list_box_select_row(GTK_LIST_BOX(d.paired), GTK_LIST_BOX_ROW(selected));
    if (device_page_shown()) device_view_refresh();
  } else if (d.selected[0]) {
    /* The shown device left the group. */
    gboolean shown = device_page_shown();
    d.selected[0] = '\0';
    if (shown) show_albums();
  }
  if (sync) save_own_name(sync);
}

static gboolean on_refresh_timeout(gpointer data) {
  (void)data;
  d.refresh_source = 0;
  refresh_devices();
  return G_SOURCE_REMOVE;
}

/* Refreshes the devices shortly, once for a burst of sync events. */
static void schedule_refresh(void) {
  if (!d.refresh_source) d.refresh_source = g_timeout_add(REFRESH_DEVICES_MS, on_refresh_timeout, NULL);
}

static gboolean on_poll(gpointer data) {
  (void)data;
  refresh_devices();
  return G_SOURCE_CONTINUE;
}

static gboolean on_library_timeout(gpointer data) {
  (void)data;
  d.library_source = 0;
  app_reload_library(d.a);
  return G_SOURCE_REMOVE;
}

/* Reloads the library views shortly, once for a burst of rows and files from sync, as an
 * import does after each album. */
static void schedule_library_refresh(void) {
  if (!d.library_source) d.library_source = g_timeout_add(REFRESH_LIBRARY_MS, on_library_timeout, NULL);
}

static void open_device(const char *uid) {
  g_strlcpy(d.selected, uid, sizeof d.selected);
  gtk_list_box_unselect_all(d.a->nav);
  device_view_open(sync_bridge_get(), d.a->lib, uid);
  gtk_stack_set_visible_child_name(d.a->pages, "device");
}

static void on_paired_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list, (void)data;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  if (uid) open_device(uid);
}

/* --- Pairing --- */

static void sync_off_message(void) {
  char *text = mc_format_alloc(mc_str(MC_STR_LINUX_COMPUTER_PAIR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
  alert(mc_str(MC_STR_PAIR_DEVICE), text);
  mc_free(text);
}

/* After mc_sync_pair_request() or _address(): opens the enter-code dialog, or says why
 * pairing couldn't start. name names the other device, or is NULL when it's known only by
 * its address. */
static void pair_started(mc_status st, const char *name, const char *address) {
  char *text = NULL;
  switch (st) {
    case MC_OK:
      pair_dialog_begin_enter(window(), sync_bridge_get(), name, address);
      return;
    case MC_ERR_LOCKED:
      text = g_strdup(mc_str(MC_STR_PAIR_ALREADY_PAIRING));
      break;
    case MC_ERR_NOT_FOUND:
      if (name && *name) address = name;
      if (address) {
        char *t = mc_format_alloc(mc_str(MC_STR_PAIR_GONE), MC_S(address));
        text = g_strdup(t);
        mc_free(t);
      } else {
        text = g_strdup(mc_str(MC_STR_PAIR_GONE_UNNAMED));
      }
      break;
    default: {
      char *t = mc_format_alloc(mc_str(MC_STR_PAIR_START_FAILED), MC_S(mc_status_str(st)));
      text = g_strdup(t);
      mc_free(t);
      break;
    }
  }
  alert(mc_str(MC_STR_PAIR_DEVICE), text);
  g_free(text);
}

/* A nearby device was picked. Even one in another group: pairing then says why it can't. */
static void on_nearby_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list, (void)data;
  mc_sync *sync = sync_bridge_get();
  const char *picked = g_object_get_data(G_OBJECT(row), "uid");
  if (!picked) return;
  if (!sync) {
    sync_off_message();
    return;
  }
  char uid[MC_UID_STRLEN], *name = NULL;
  g_strlcpy(uid, picked, sizeof uid); /* the row goes when the section refreshes */
  mc_sync_nearby_list *nearby = NULL;
  if (mc_sync_list_nearby(sync, &nearby) == MC_OK) {
    for (int i = 0; i < nearby->count && !name; i++)
      if (!strcmp(nearby->items[i].uid, uid)) name = g_strdup(nearby->items[i].name ? nearby->items[i].name : "");
    mc_sync_nearby_list_free(nearby);
  }
  pair_started(mc_sync_pair_request(sync, uid), name, NULL);
  g_free(name);
}

/* The Add Device dialog closed with Connect. */
static void on_add_device_connect(void *user, const char *host, guint16 port) {
  (void)user;
  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    sync_off_message();
    return;
  }
  char *address = format_address(host, port ? port : MC_SYNC_DEFAULT_PORT);
  pair_started(mc_sync_pair_request_address(sync, host, port), NULL, address);
  g_free(address);
}

static void on_add_device(GtkButton *button, gpointer user) {
  add_device_present(GTK_WIDGET(button), on_add_device_connect, user);
}

/* --- Sync events --- */

static void on_sync_event(void *user, const sync_event *ev) {
  (void)user;
  mc_sync *sync = sync_bridge_get();
  switch (ev->kind) {
    case SYNC_EV_PEER:
      schedule_refresh();
      device_view_on_sync_event(ev);
      break;
    case SYNC_EV_DONE:
      set_syncing(ev->uid, FALSE);
      schedule_refresh();
      /* Reconcile may have removed files after the last progress. */
      if (ev->library_synced) schedule_library_refresh();
      device_view_on_sync_event(ev);
      break;
    case SYNC_EV_PAIR_CODE:
      pair_dialog_show_code(window(), sync, ev);
      break;
    case SYNC_EV_PAIR_RESULT:
      if (pair_dialog_on_result(window(), ev)) { /* paired: show the new device's page */
        refresh_devices();
        if (ev->uid[0]) {
          open_device(ev->uid);
          refresh_devices(); /* selects its row */
        }
      }
      break;
    case SYNC_EV_PROGRESS:
      if (set_syncing(ev->uid, ev->phase != MC_SYNC_PHASE_DONE)) schedule_refresh();
      if (ev->library_changed) schedule_library_refresh();
      device_view_on_sync_event(ev);
      break;
  }
}

/* --- Device page callbacks --- */

static void on_rename_self(void *user, const char *name) {
  devices_renamed(user, name);
}

static void on_devices_changed(void *user) {
  (void)user;
  schedule_refresh();
}

/* --- API --- */

GtkWidget *devices_new(app *a) {
  d.a = a;
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

  /* The Devices heading, with + at the end to add a device by address. */
  GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_start(header, 18);
  gtk_widget_set_margin_end(header, 12);
  gtk_widget_set_margin_top(header, 12);
  GtkWidget *heading = gtk_label_new(mc_str(MC_STR_DEVICES));
  gtk_widget_add_css_class(heading, "heading");
  gtk_widget_add_css_class(heading, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_set_hexpand(heading, TRUE);
  gtk_box_append(GTK_BOX(header), heading);
  GtkWidget *add = gtk_button_new_from_icon_name("list-add-symbolic");
  gtk_widget_add_css_class(add, "flat");
  gtk_widget_set_tooltip_text(add, mc_str(MC_STR_ADD_DEVICE_BY_ADDRESS));
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_device), a);
  gtk_box_append(GTK_BOX(header), add);
  gtk_box_append(GTK_BOX(box), header);

  /* The group's devices. */
  d.paired = gtk_list_box_new();
  gtk_widget_add_css_class(d.paired, "navigation-sidebar");
  d.none = gtk_label_new(mc_str(MC_STR_DEVICES_NONE));
  gtk_label_set_wrap(GTK_LABEL(d.none), TRUE);
  gtk_widget_set_margin_start(d.none, 12);
  gtk_widget_set_margin_end(d.none, 12);
  gtk_widget_add_css_class(d.none, "dim-label");
  gtk_list_box_set_placeholder(GTK_LIST_BOX(d.paired), d.none);
  g_signal_connect(d.paired, "row-activated", G_CALLBACK(on_paired_activated), a);
  gtk_box_append(GTK_BOX(box), d.paired);

  /* Devices nearby, outside the group: picking one asks it to pair. */
  d.nearby_heading = gtk_label_new(NULL);
  gtk_widget_add_css_class(d.nearby_heading, "heading");
  gtk_widget_add_css_class(d.nearby_heading, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(d.nearby_heading), 0);
  gtk_widget_set_margin_start(d.nearby_heading, 18);
  gtk_widget_set_margin_top(d.nearby_heading, 12);
  gtk_widget_set_visible(d.nearby_heading, FALSE);
  gtk_box_append(GTK_BOX(box), d.nearby_heading);
  d.nearby = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(d.nearby), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(d.nearby, "navigation-sidebar");
  gtk_widget_set_visible(d.nearby, FALSE);
  g_signal_connect(d.nearby, "row-activated", G_CALLBACK(on_nearby_activated), a);
  gtk_box_append(GTK_BOX(box), d.nearby);

  const device_view_callbacks cb = {on_rename_self, on_devices_changed, a};
  gtk_stack_add_named(a->pages, device_view_new(&cb), "device");
  return box;
}

void devices_start(app *a) {
  memset(d.syncing, 0, sizeof d.syncing);
  char *name = saved_name();
  sync_bridge_start(a->lib, a->settings_dir, name, on_sync_event, a);
  g_free(name);
  /* After a library switch on a device's page, it shows again on the new sync (or Albums, if
   * the refresh finds the device gone). */
  if (d.selected[0]) device_view_open(sync_bridge_get(), a->lib, d.selected);
  if (!d.poll_source) d.poll_source = g_timeout_add_seconds(POLL_DEVICES_S, on_poll, NULL);
  refresh_devices();
}

void devices_stop(app *a) {
  (void)a;
  pair_dialog_close_all();
  device_view_open(NULL, NULL, NULL);
  sync_bridge_stop();
  g_clear_handle_id(&d.refresh_source, g_source_remove);
  g_clear_handle_id(&d.library_source, g_source_remove);
  g_clear_handle_id(&d.poll_source, g_source_remove);
}

void devices_unselect(void) {
  d.selected[0] = '\0';
  if (d.paired) gtk_list_box_unselect_all(GTK_LIST_BOX(d.paired));
}

mc_sync *devices_sync(void) {
  return sync_bridge_get();
}

char *devices_sync_off_text(void) {
  if (sync_bridge_get()) return NULL;
  char *t = mc_format_alloc(mc_str(MC_STR_SYNC_OFF), MC_S(mc_status_str(sync_bridge_error())));
  char *out = g_strdup(t);
  mc_free(t);
  return out;
}

void devices_renamed(app *a, const char *name) {
  if (!save_name(name)) {
    AdwDialog *dialog = adw_alert_dialog_new(mc_str(MC_STR_NAME_SAVE_FAILED), NULL);
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", mc_str_mnemonic(MC_STR_CLOSE));
    adw_dialog_present(dialog, GTK_WIDGET(a->win));
  }
  schedule_refresh();
}

void devices_refresh(app *a) {
  (void)a;
  refresh_devices();
}
