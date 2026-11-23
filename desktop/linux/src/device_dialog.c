#include "device_dialog.h"

#include <string.h>

#include "app.h"
#include "issue_dialog.h"

static struct {
  AdwDialog *dialog; /* NULL while closed */
  mc_sync *sync;
  char uid[MC_UID_STRLEN];
  char name[MC_SYNC_DEVICE_NAME_MAX + 1];
  void (*removed)(void *user);
  void *user;
  GtkWidget *info_group, *warning_row, *issues_row, *files_row, *address_row, *reach_row, *reach_check;
  GtkWidget *profile_group, *archive_row, *format_row, *budget_row, *remove_group;
} g_dd;

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

static void set_group_description(GtkWidget *group, const char *text) {
  char *escaped = text && *text ? g_markup_escape_text(text, -1) : NULL;
  adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), escaped);
  g_free(escaped);
}

/* A row's subtitle, the row showing only with one. */
static void set_row(GtkWidget *row, const char *subtitle) {
  adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle ? subtitle : "");
  gtk_widget_set_visible(row, subtitle && *subtitle);
}

static void alert(const char *heading, const char *body) {
  AdwDialog *dialog = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(dialog, GTK_WIDGET(g_dd.dialog));
}

/* --- Filling --- */

/* "Online, synced 3 minutes ago", and its next automatic sync or a hold on it. */
static void status_text(const mc_sync_device *dev, char *text, size_t size) {
  char ago[64], note[256];
  if (dev->last_sync_ms > 0) fmt_ago(dev->last_sync_ms, ago, sizeof ago);
  if (dev->online && dev->last_sync_ms > 0) mc_format(text, size, mc_str(MC_STR_DEVICE_ONLINE_SYNCED), MC_S(ago));
  else if (dev->online) g_strlcpy(text, mc_str(MC_STR_DEVICE_ONLINE), size);
  else if (dev->last_sync_ms > 0) mc_format(text, size, mc_str(MC_STR_DEVICE_OFFLINE_SYNCED), MC_S(ago));
  else g_strlcpy(text, mc_str(MC_STR_DEVICE_NOT_SYNCED), size);
  mc_sync_device_note(dev, 0, 1, note, sizeof note);
  size_t used = strlen(text);
  if (note[0] && used < size) g_snprintf(text + used, size - used, "%s%s", mc_str(MC_STR_SEPARATOR_DOT), note);
}

/* Its own report: "1,204 of 1,210 files OK, 6 missing, 12.3 GB"; "" if it hasn't sent one. */
static void files_text(const mc_sync_device *dev, char *text, size_t size) {
  text[0] = '\0';
  if (!dev->has_status) return;
  char ok[32], expected[32], n[32], bytes[32];
  const char *sep = mc_str(MC_STR_IMPORT_LIST_SEPARATOR);
  size_t used = mc_format(text, size, mc_str_plural(MC_STR_FILES_OK, (long)dev->expected),
                          MC_S(fmt_count(dev->ok, ok, sizeof ok)), MC_S(fmt_count(dev->expected, expected, sizeof expected)));
  if (dev->missing > 0 && used < size) {
    used += (size_t)g_snprintf(text + used, size - used, "%s", sep);
    if (used < size)
      used += mc_format(text + used, size - used, mc_str(MC_STR_FILES_MISSING), MC_S(fmt_count(dev->missing, n, sizeof n)));
  }
  if (dev->corrupt > 0 && used < size) {
    used += (size_t)g_snprintf(text + used, size - used, "%s", sep);
    if (used < size)
      used += mc_format(text + used, size - used, mc_str(MC_STR_FILES_DAMAGED), MC_S(fmt_count(dev->corrupt, n, sizeof n)));
  }
  if (used < size) g_snprintf(text + used, size - used, "%s%s", sep, fmt_size(dev->bytes, bytes, sizeof bytes));
}

/* Its profile: what it syncs in a sentence, and its format and budget, or that it's an
 * archive. */
static void fill_profile(gboolean archive) {
  mc_sync_profile *p = NULL;
  mc_status st = mc_sync_get_profile(g_dd.sync, g_dd.uid, &p);
  char text[512];
  if (st != MC_OK) {
    mc_format(text, sizeof text, mc_str(MC_STR_PROFILE_READ_FAILED), MC_S(mc_status_str(st)));
    set_group_description(g_dd.profile_group, text);
    set_row(g_dd.archive_row, NULL);
    set_row(g_dd.format_row, NULL);
    set_row(g_dd.budget_row, NULL);
    return;
  }
  char summary[256];
  mc_sync_profile_summary(p, summary, sizeof summary);
  size_t used = g_strlcpy(text, summary, sizeof text);
  if (used < sizeof text) used += (size_t)g_snprintf(text + used, sizeof text - used, "\n");
  if (used < sizeof text) mc_format(text + used, sizeof text - used, mc_str(MC_STR_SYNC_CHANGE_ON_DEVICE), MC_S(g_dd.name));
  set_group_description(g_dd.profile_group, text);

  archive = archive || p->archive;
  char *only = mc_format_alloc(mc_str(MC_STR_PROFILE_ARCHIVE_OTHER), MC_S(g_dd.name));
  set_row(g_dd.archive_row, archive ? only : NULL);
  mc_free(only);
  char format[64] = "", budget[32] = "";
  if (p->format.format == MC_FORMAT_AUTO) g_strlcpy(format, mc_str(MC_STR_FORMAT_AUTO), sizeof format);
  else if (p->format.format == MC_FORMAT_OPUS) mc_format(format, sizeof format, mc_str(MC_STR_FORMAT_OPUS), MC_D(p->format.bitrate_kbps));
  else g_strlcpy(format, mc_str(MC_STR_FORMAT_ORIGINAL), sizeof format);
  if (p->budget_enabled && p->budget_bytes > 0) fmt_size(p->budget_bytes, budget, sizeof budget);
  set_row(g_dd.format_row, archive ? NULL : format);
  set_row(g_dd.budget_row, archive ? NULL : budget);
  mc_sync_profile_free(p);
}

/* Everything from the device list: FALSE if the device is no longer a member. */
static gboolean fill(void) {
  mc_sync_device_list *list = NULL;
  const mc_sync_device *dev = NULL;
  if (mc_sync_list_devices(g_dd.sync, &list) == MC_OK && list)
    for (int i = 0; i < list->count && !dev; i++)
      if (!strcmp(list->items[i].uid, g_dd.uid) && !list->items[i].removed && !list->items[i].is_self)
        dev = &list->items[i];
  if (!dev) {
    mc_sync_device_list_free(list);
    return FALSE;
  }
  g_strlcpy(g_dd.name, dev->name && *dev->name ? dev->name : mc_str(MC_STR_DEVICE_UNNAMED), sizeof g_dd.name);
  adw_dialog_set_title(g_dd.dialog, g_dd.name);

  char text[1024];
  status_text(dev, text, sizeof text);
  set_group_description(g_dd.info_group, text);
  /* What's wrong, most pressing first: its version, a hold on its side, reach, an archive away
   * for long, too little space to transcode. */
  mc_sync_device_warning(g_dd.sync, dev, 0, text, sizeof text);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(g_dd.warning_row), text);
  gtk_widget_set_visible(g_dd.warning_row, text[0] != '\0');

  /* Sync problems about it, with Details… opening them. */
  char first[256];
  int issues = issue_dialog_count(g_dd.uid, first, sizeof first);
  if (issues > 1)
    mc_format(text, sizeof text, mc_str_plural(MC_STR_SYNC_ISSUES_MORE, issues - 1), MC_S(first), MC_D(issues - 1));
  else
    g_strlcpy(text, issues > 0 ? first : "", sizeof text);
  set_row(g_dd.issues_row, text);

  files_text(dev, text, sizeof text);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dd.files_row), text[0] ? text : mc_str(MC_STR_DEVICE_FILES_NONE));
  if (dev->address) {
    char *address = mc_format_alloc(mc_str(MC_STR_DEVICE_ADDRESS_PORT), MC_S(dev->address), MC_D(dev->port));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dd.address_row), address);
    mc_free(address);
  } else {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dd.address_row), mc_str(MC_STR_DEVICE_ADDRESS_UNKNOWN));
  }
  mc_sync_reach_text(g_dd.sync, g_dd.uid, g_dd.name, 0, text, sizeof text);
  adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dd.reach_row), text);
  gtk_widget_set_sensitive(g_dd.reach_check, dev->online);

  fill_profile(dev->archive);
  gtk_widget_set_visible(g_dd.remove_group, !dev->archive);
  mc_sync_device_list_free(list);
  return TRUE;
}

/* --- Actions --- */

static void on_details_clicked(GtkButton *button, gpointer data) {
  (void)data;
  issue_dialog_present(GTK_WIDGET(button), g_dd.uid);
}

/* Tests the connections again; the row updates when the answers come in. */
static void on_reach_check_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  mc_status st = mc_sync_check_reach(g_dd.sync, g_dd.uid);
  if (st != MC_OK) adw_action_row_set_subtitle(ADW_ACTION_ROW(g_dd.reach_row), mc_status_str(st));
}

static void on_remove_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)dialog, (void)data;
  if (strcmp(response, "remove") || !g_dd.dialog) return;
  mc_status st = mc_sync_remove_device(g_dd.sync, g_dd.uid);
  if (st == MC_OK) {
    void (*removed)(void *) = g_dd.removed;
    void *user = g_dd.user;
    adw_dialog_force_close(g_dd.dialog);
    if (removed) removed(user);
    return;
  }
  char text[512];
  if (st == MC_ERR_LOCKED) mc_format(text, sizeof text, mc_str(MC_STR_REMOVE_ONLY_ARCHIVE), MC_S(g_dd.name));
  else mc_format(text, sizeof text, mc_str(MC_STR_REMOVE_DEVICE_FAILED), MC_S(mc_status_str(st)));
  alert(text, NULL);
}

static void on_remove_clicked(GtkButton *button, gpointer data) {
  (void)button, (void)data;
  char *heading = mc_format_alloc(mc_str(MC_STR_REMOVE_DEVICE_TITLE), MC_S(g_dd.name));
  AdwDialog *dialog = adw_alert_dialog_new(heading, mc_str(MC_STR_REMOVE_DEVICE_TEXT));
  mc_free(heading);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "remove",
                                 mc_str_mnemonic(MC_STR_REMOVE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_remove_response), NULL);
  adw_dialog_present(dialog, GTK_WIDGET(g_dd.dialog));
}

static void on_closed(AdwDialog *dialog, gpointer data) {
  (void)dialog, (void)data;
  g_dd.dialog = NULL;
}

/* --- Building --- */

static GtkWidget *info_row(GtkWidget *group, const char *title) {
  GtkWidget *row = adw_action_row_new();
  adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
  gtk_widget_add_css_class(row, "property");
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
  return row;
}

static GtkWidget *row_button(GtkWidget *row, GtkWidget *button, GCallback clicked) {
  gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
  g_signal_connect(button, "clicked", clicked, NULL);
  adw_action_row_add_suffix(ADW_ACTION_ROW(row), button);
  return button;
}

static GtkWidget *warning_icon(void) {
  GtkWidget *triangle = gtk_image_new_from_icon_name("dialog-warning-symbolic");
  gtk_widget_add_css_class(triangle, "warning");
  return triangle;
}

static AdwDialog *build(void) {
  g_dd.info_group = adw_preferences_group_new();
  g_dd.warning_row = info_row(g_dd.info_group, "");
  gtk_widget_remove_css_class(g_dd.warning_row, "property");
  adw_action_row_set_title_lines(ADW_ACTION_ROW(g_dd.warning_row), 0);
  adw_action_row_add_prefix(ADW_ACTION_ROW(g_dd.warning_row), warning_icon());
  g_dd.issues_row = info_row(g_dd.info_group, mc_str(MC_STR_SYNC_ISSUES_TITLE));
  adw_action_row_add_prefix(ADW_ACTION_ROW(g_dd.issues_row), warning_icon());
  row_button(g_dd.issues_row, gtk_button_new_with_mnemonic(mc_str(MC_STR_SYNC_ISSUES_DETAILS)), G_CALLBACK(on_details_clicked));
  g_dd.files_row = info_row(g_dd.info_group, mc_str(MC_STR_DEVICE_FILES));
  g_dd.address_row = info_row(g_dd.info_group, mc_str(MC_STR_DEVICE_ADDRESS));
  g_dd.reach_row = info_row(g_dd.info_group, mc_str(MC_STR_LINUX_REACH));
  g_dd.reach_check = row_button(g_dd.reach_row, gtk_button_new_with_label(mc_str(MC_STR_LINUX_REACH_CHECK)),
                                G_CALLBACK(on_reach_check_clicked));

  g_dd.profile_group = adw_preferences_group_new();
  adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(g_dd.profile_group), mc_str(MC_STR_DEVICE_PROFILE));
  g_dd.archive_row = info_row(g_dd.profile_group, mc_str(MC_STR_DEVICE_ARCHIVE_TITLE));
  g_dd.format_row = info_row(g_dd.profile_group, mc_str(MC_STR_SYNC_FORMAT));
  g_dd.budget_row = info_row(g_dd.profile_group, mc_str(MC_STR_BUDGET_LIMIT));

  g_dd.remove_group = adw_preferences_group_new();
  GtkWidget *remove = gtk_button_new_with_label(mc_str(MC_STR_REMOVE_FROM_GROUP_BUTTON));
  gtk_widget_add_css_class(remove, "destructive-action");
  gtk_widget_set_halign(remove, GTK_ALIGN_START);
  g_signal_connect(remove, "clicked", G_CALLBACK(on_remove_clicked), NULL);
  adw_preferences_group_add(ADW_PREFERENCES_GROUP(g_dd.remove_group), remove);

  GtkWidget *page = adw_preferences_page_new();
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_dd.info_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_dd.profile_group));
  adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(g_dd.remove_group));
  GtkWidget *view = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(view), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(view), page);
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_content_width(dialog, 520);
  adw_dialog_set_content_height(dialog, 600);
  adw_dialog_set_child(dialog, view);
  g_signal_connect(dialog, "closed", G_CALLBACK(on_closed), NULL);
  return dialog;
}

/* --- API --- */

gboolean device_dialog_present(GtkWidget *parent, mc_sync *sync, const char *uid, void (*removed)(void *user),
                               void *user) {
  device_dialog_close();
  g_dd.sync = sync;
  g_strlcpy(g_dd.uid, uid, sizeof g_dd.uid);
  g_dd.removed = removed;
  g_dd.user = user;
  g_dd.dialog = build();
  if (!fill()) {
    g_object_unref(g_object_ref_sink(g_dd.dialog));
    g_dd.dialog = NULL;
    return FALSE;
  }
  adw_dialog_present(g_dd.dialog, parent);
  return TRUE;
}

void device_dialog_refresh(void) {
  if (g_dd.dialog && !fill()) adw_dialog_force_close(g_dd.dialog);
}

void device_dialog_close(void) {
  if (g_dd.dialog) adw_dialog_force_close(g_dd.dialog);
}
