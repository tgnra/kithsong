#include "devices.h"

#include <string.h>

#include "add_device.h"
#include "issue_dialog.h"
#include "pair_dialog.h"
#include "playback.h"
#include "settings_view.h"
#include "sync_bridge.h"
#include "sync_view.h"

enum {
  REFRESH_DEVICES_MS = 200, /* after sync events, once for a burst */
  NETWORK_CHANGED_MS = 1000, /* after the network changes, once for a burst */
  REFRESH_LIBRARY_MS = 500, /* after library rows or files arrive, once for a burst */
  POLL_DEVICES_S = 10,      /* renames elsewhere fire no event */
};

static struct {
  app *a;
  guint refresh_source, library_source, poll_source, network_source;
  gulong network_handler; /* on g_network_monitor_get_default(), while sync runs */
  devices_state state; /* as of the last refresh */
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

/* --- Refreshing --- */

/* Shows the Sync page, picked in the sidebar. */
static void show_sync_page(void) {
  gtk_stack_set_visible_child_name(d.a->pages, "sync");
}

/* What's wrong with this computer, on the Sync row: in a group, mc_sync_device_warning();
 * outside one, whether other devices can reach it to pair. */
static void set_sync_warning(const char *text) {
  gtk_widget_set_visible(d.a->sync_warning, text[0] != '\0');
  gtk_widget_set_tooltip_text(d.a->sync_warning, text[0] ? text : NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(d.a->sync_warning), GTK_ACCESSIBLE_PROPERTY_LABEL, text, -1);
}

/* Reads this computer's place in sync, and refreshes everything that shows the devices. */
static void refresh_devices(void) {
  g_clear_handle_id(&d.refresh_source, g_source_remove);
  mc_sync *sync = sync_bridge_get();
  mc_sync_device_list *list = NULL;
  if (sync && mc_sync_list_devices(sync, &list) != MC_OK) list = NULL;
  gboolean grouped = FALSE;
  char warning[768] = "";
  for (int i = 0; list && i < list->count && !grouped; i++)
    if (list->items[i].is_self && !list->items[i].removed) {
      grouped = TRUE;
      mc_sync_device_warning(sync, &list->items[i], 0, warning, sizeof warning);
    }
  mc_sync_device_list_free(list);
  if (sync && !grouped) {
    char name[MC_SYNC_DEVICE_NAME_MAX + 1] = "";
    mc_sync_get_device_name(sync, name, sizeof name);
    mc_sync_reach_text(sync, mc_sync_device_uid(sync), name, 1, warning, sizeof warning);
  }
  set_sync_warning(warning);
  sync_view_refresh();
  if (sync) save_own_name(sync);
  devices_state state = !sync ? DEVICES_SYNC_OFF : grouped ? DEVICES_GROUPED : DEVICES_ALONE;
  if (state != d.state) {
    d.state = state;
    app_update_empty_pages(d.a); /* pairing, or the sync profile */
  }
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

gboolean devices_open(const char *uid) {
  show_sync_page();
  return !uid || sync_view_open_device(uid);
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

/* A nearby device was picked on the Sync page. Even one in another group: pairing then says
 * why it can't. */
static void pair_nearby(void *user, const char *picked) {
  (void)user;
  mc_sync *sync = sync_bridge_get();
  if (!picked) return;
  if (!sync) {
    sync_off_message();
    return;
  }
  char uid[MC_UID_STRLEN], *name = NULL;
  g_strlcpy(uid, picked, sizeof uid); /* the row goes when the page refreshes */
  mc_sync_nearby_list *nearby = NULL;
  if (mc_sync_list_nearby(sync, &nearby) == MC_OK) {
    for (int i = 0; i < nearby->count && !name; i++)
      if (!strcmp(nearby->items[i].uid, uid)) name = g_strdup(nearby->items[i].name ? nearby->items[i].name : "");
    mc_sync_nearby_list_free(nearby);
  }
  mc_status st = mc_sync_pair_request(sync, uid);
  pair_started(st, name, NULL);
  if (st == MC_OK) pair_dialog_set_target(uid, NULL, 0);
  g_free(name);
}

static void on_add_device_connect(void *user, const char *host, guint16 port) {
  (void)user;
  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    sync_off_message();
    return;
  }
  char *address = format_address(host, port ? port : MC_SYNC_DEFAULT_PORT);
  mc_status st = mc_sync_pair_request_address(sync, host, port);
  pair_started(st, NULL, address);
  if (st == MC_OK) pair_dialog_set_target(NULL, host, port);
  g_free(address);
}

static void add_device(void *user, GtkWidget *parent) {
  add_device_present(parent, on_add_device_connect, user);
}

devices_state devices_sync_state(void) {
  return d.state;
}

void devices_open_self(void) {
  mc_sync *sync = sync_bridge_get();
  if (!sync || d.state != DEVICES_GROUPED) return;
  show_sync_page();
  sync_view_focus_what_to_sync();
}

/* --- The pairing dialogs' side --- */

/* Choose What to Sync: the Sync page, at What to sync when this computer is the one that
 * joined. */
static void pair_show_device(const char *uid) {
  refresh_devices();
  show_sync_page();
  mc_sync *sync = sync_bridge_get();
  if (sync && !strcmp(uid, mc_sync_device_uid(sync))) sync_view_focus_what_to_sync();
}

static gboolean open_library_for_pairing(void *user, const char *dir) {
  return app_open_library(user, dir);
}

/* Create a New Library's wait for the folder picker. */
typedef struct new_library_wait {
  void (*done)(gpointer data, gboolean switched);
  gpointer data;
} new_library_wait;

static void on_new_library_chosen(gpointer data, const char *dir, gboolean saved) {
  new_library_wait *w = data;
  if (dir && !saved) alert(mc_str(MC_STR_SETTINGS), mc_str(MC_STR_SETTINGS_SAVE_FAILED));
  w->done(w->data, dir != NULL);
}

/* Create a New Library, from a pairing refused because this computer has music of its own:
 * Settings' Open Another Library, taking only an empty folder. */
static void pair_new_library(GtkWidget *parent, void (*done)(gpointer data, gboolean switched), gpointer data) {
  new_library_wait *w = g_new0(new_library_wait, 1);
  w->done = done;
  w->data = data;
  settings_view_choose_library(parent, d.a->settings_dir, open_library_for_pairing, d.a, TRUE, on_new_library_chosen,
                               w, g_free);
}

/* Pair Again, once the new library is open: by uid if discovery has seen the device again,
 * else at the address it had. */
static void pair_again(const char *uid, const char *name, const char *host, guint16 port) {
  mc_sync *sync = sync_bridge_get();
  if (!sync) {
    sync_off_message();
    return;
  }
  mc_status st = uid && *uid ? mc_sync_pair_request(sync, uid) : MC_ERR_NOT_FOUND;
  if (st == MC_ERR_NOT_FOUND && host && *host) {
    char *address = format_address(host, port ? port : MC_SYNC_DEFAULT_PORT);
    st = mc_sync_pair_request_address(sync, host, port);
    pair_started(st, name, address);
    g_free(address);
  } else {
    pair_started(st, name, NULL);
  }
  if (st == MC_OK) pair_dialog_set_target(uid, host, port);
}

/* One-time explanations: TRUE the first time key is asked about, which settings.json then
 * records. Not saved, it shows again next run. */
static gboolean pair_first_time(const char *key) {
  mc_settings *settings = NULL;
  if (mc_settings_load(d.a->settings_dir, &settings) != MC_OK) return TRUE;
  gboolean first = !mc_settings_bool(settings, key, 0);
  if (first && mc_settings_set_bool(settings, key, 1) == MC_OK) mc_settings_save(settings);
  mc_settings_free(settings);
  return first;
}

static const pair_dialog_host k_pair_host = {pair_show_device, pair_new_library, pair_again, pair_first_time};

/* --- Sync events --- */

static void on_sync_event(void *user, const sync_event *ev) {
  (void)user;
  mc_sync *sync = sync_bridge_get();
  switch (ev->kind) {
    case SYNC_EV_PEER:
    case SYNC_EV_REACH:
    case SYNC_EV_VERSION:
      schedule_refresh();
      sync_view_on_sync_event(ev);
      break;
    case SYNC_EV_DONE:
      schedule_refresh();
      /* Reconcile may have removed files after the last progress. */
      if (ev->library_synced) schedule_library_refresh();
      sync_view_on_sync_event(ev);
      break;
    case SYNC_EV_PAIR_CODE:
      pair_dialog_show_code(window(), sync, ev);
      break;
    case SYNC_EV_PAIR_RESULT:
      /* Paired: the dialog's Choose What to Sync opens the Sync page. */
      if (pair_dialog_on_result(window(), ev)) refresh_devices();
      break;
    case SYNC_EV_PROGRESS:
      if (ev->library_changed) schedule_library_refresh();
      sync_view_on_sync_event(ev);
      break;
    case SYNC_EV_WAITING:
      app_update_waiting(d.a);
      sync_view_on_sync_event(ev);
      issue_dialog_refresh();
      break;
    case SYNC_EV_DISSOLVED:
      refresh_devices();
      if (d.a->toasts) {
        char text[512];
        mc_format(text, sizeof text, mc_str(MC_STR_SYNC_GROUP_DISSOLVED), MC_S(ev->name));
        AdwToast *toast = adw_toast_new("");
        adw_toast_set_use_markup(toast, FALSE);
        adw_toast_set_title(toast, text);
        adw_toast_overlay_add_toast(d.a->toasts, toast);
      }
      break;
  }
}

/* The Sync page left the group, or removed a member. */
static void on_group_changed(void *user) {
  (void)user;
  schedule_refresh();
}

/* --- API --- */

void devices_init(app *a) {
  d.a = a;
  pair_dialog_init(&k_pair_host);
  const sync_view_callbacks cb = {pair_nearby, add_device, on_group_changed, a};
  gtk_stack_add_named(a->pages, sync_view_new(&cb), "sync");
}

static gboolean on_network_timeout(gpointer data) {
  (void)data;
  d.network_source = 0;
  mc_sync *sync = sync_bridge_get();
  if (sync) mc_sync_network_changed(sync);
  return G_SOURCE_REMOVE;
}

/* An address came or went, or a network: sync looks again for the devices, once for a burst
 * (joining a network sends several). */
static void on_network_changed(GNetworkMonitor *monitor, gboolean available, gpointer data) {
  (void)monitor, (void)available, (void)data;
  g_clear_handle_id(&d.network_source, g_source_remove);
  d.network_source = g_timeout_add(NETWORK_CHANGED_MS, on_network_timeout, NULL);
}

void devices_start(app *a) {
  char *name = saved_name();
  sync_bridge_start(a->lib, a->settings_dir, name, on_sync_event, a);
  g_free(name);
  sync_view_open(sync_bridge_get(), a->lib);
  if (!d.poll_source) d.poll_source = g_timeout_add_seconds(POLL_DEVICES_S, on_poll, NULL);
  if (!d.network_handler)
    d.network_handler = g_signal_connect(g_network_monitor_get_default(), "network-changed", G_CALLBACK(on_network_changed), NULL);
  issue_dialog_start(a);
  refresh_devices();
  app_update_waiting(a); /* the old sync's banner goes; the new one's comes with its event */
}

void devices_stop(app *a) {
  (void)a;
  pair_dialog_close_all();
  sync_view_open(NULL, NULL);
  issue_dialog_stop();
  if (d.network_handler) g_signal_handler_disconnect(g_network_monitor_get_default(), d.network_handler);
  d.network_handler = 0;
  g_clear_handle_id(&d.network_source, g_source_remove);
  sync_bridge_stop();
  g_clear_handle_id(&d.refresh_source, g_source_remove);
  g_clear_handle_id(&d.library_source, g_source_remove);
  g_clear_handle_id(&d.poll_source, g_source_remove);
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
