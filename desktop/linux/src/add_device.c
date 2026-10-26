#include "add_device.h"

#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "add_device.c needs libadwaita 1.5 or later (AdwAlertDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct add_device {
  add_device_connect_fn connect;
  void *user;
  GtkWidget *dialog, *address, *port;
} add_device;

/* The trimmed address; g_free it. NULL if there's none, or it has spaces inside. */
static char *address_text(add_device *d) {
  char *host = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(d->address))));
  if (!*host || strpbrk(host, " \t")) {
    g_free(host);
    return NULL;
  }
  return host;
}

static void on_address_changed(GtkEditable *editable, gpointer data) {
  (void)editable;
  add_device *d = data;
  char *host = address_text(d);
  adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(d->dialog), "connect", host != NULL);
  g_free(host);
}

static void connect_and_close(add_device *d) {
  char *host = address_text(d);
  if (!host) return;
  guint16 port = (guint16)adw_spin_row_get_value(ADW_SPIN_ROW(d->port));
  /* Close first: connect may present the next dialog (the code entry) over the window. */
  g_object_ref(d->dialog);
  adw_dialog_force_close(ADW_DIALOG(d->dialog));
  if (d->connect) d->connect(d->user, host, port);
  g_object_unref(d->dialog);
  g_free(host);
}

static void on_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog;
  if (!g_strcmp0(response, "connect")) connect_and_close(data);
}

static void on_address_activated(AdwEntryRow *row, gpointer data) {
  (void)row;
  connect_and_close(data);
}

void add_device_present(GtkWidget *parent, add_device_connect_fn connect, void *user) {
  add_device *d = g_new0(add_device, 1);
  d->connect = connect;
  d->user = user;

  d->dialog = GTK_WIDGET(adw_alert_dialog_new(
    "Add Device", "Enter the address of a device on your network. It will show a code to enter next."));
  g_object_set_data_full(G_OBJECT(d->dialog), "add-device", d, g_free);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(d->dialog);
  adw_alert_dialog_add_responses(alert, "cancel", "Cancel", "connect", "Connect", NULL);
  adw_alert_dialog_set_response_appearance(alert, "connect", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_response_enabled(alert, "connect", FALSE);
  adw_alert_dialog_set_default_response(alert, "connect");
  adw_alert_dialog_set_close_response(alert, "cancel");
  g_signal_connect(alert, "response", G_CALLBACK(on_response), d);

  GtkWidget *list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(list, "boxed-list");

  d->address = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->address), "IP Address or Hostname");
  gtk_editable_set_enable_undo(GTK_EDITABLE(d->address), TRUE);
  g_signal_connect(d->address, "changed", G_CALLBACK(on_address_changed), d);
  g_signal_connect(d->address, "entry-activated", G_CALLBACK(on_address_activated), d);
  gtk_list_box_append(GTK_LIST_BOX(list), d->address);

  d->port = adw_spin_row_new_with_range(1, 65535, 1);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(d->port), "Port");
  adw_spin_row_set_value(ADW_SPIN_ROW(d->port), ADD_DEVICE_DEFAULT_PORT);
  gtk_list_box_append(GTK_LIST_BOX(list), d->port);

  adw_alert_dialog_set_extra_child(alert, list);
  adw_dialog_set_focus(ADW_DIALOG(d->dialog), d->address);
  adw_dialog_present(ADW_DIALOG(d->dialog), parent);
}
