#include "playlist_dialogs.h"

#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "playlist_dialogs.c needs libadwaita 1.5 or later (AdwAlertDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct ask {
  mc_library *lib;
  char uid[MC_UID_STRLEN];
  char *name;
  GtkWidget *parent, *entry; /* entry: Rename's and Create's */
  const char *ok;            /* the response the entry enables: "rename" or "create" */
  void (*changed)(void *user);
  void (*created)(void *user, const char *uid);
  void *user;
} ask;

static void ask_free(gpointer data) {
  ask *a = data;
  g_free(a->name);
  g_object_unref(a->parent);
  g_free(a);
}

static ask *ask_new(AdwDialog *dialog, GtkWidget *parent, mc_library *lib, const char *uid,
                    const char *name, void (*changed)(void *user), void *user) {
  ask *a = g_new0(ask, 1);
  a->lib = lib;
  g_strlcpy(a->uid, uid, sizeof a->uid);
  a->name = g_strdup(name ? name : "");
  a->parent = g_object_ref(parent);
  a->changed = changed;
  a->user = user;
  g_object_set_data_full(G_OBJECT(dialog), "playlist-dialog", a, ask_free);
  return a;
}

static void fail(GtkWidget *parent, const char *heading) {
  AdwDialog *alert = adw_alert_dialog_new(heading, NULL);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, parent);
}

/* --- Rename --- */

static char *entered_name(ask *a) {
  return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(a->entry))));
}

static void on_name_changed(GtkEditable *entry, gpointer data) {
  (void)entry;
  AdwAlertDialog *dialog = data;
  ask *a = g_object_get_data(G_OBJECT(dialog), "playlist-dialog");
  char *name = entered_name(a);
  adw_alert_dialog_set_response_enabled(dialog, a->ok, *name && strcmp(name, a->name));
  g_free(name);
}

/* An alert asking for a name, with Cancel and ok (enabled once there's a name, and for
 * Rename a new one). */
static AdwDialog *name_dialog(const char *heading, GtkWidget *parent, mc_library *lib, const char *uid,
                              const char *name, const char *ok, enum mc_str_id ok_label,
                              void (*changed)(void *user), void *user) {
  AdwDialog *dialog = adw_alert_dialog_new(heading, NULL);
  ask *a = ask_new(dialog, parent, lib, uid, name, changed, user);
  a->ok = ok;
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), ok,
                                 mc_str_mnemonic(ok_label), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), ok, ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), ok);
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_response_enabled(ADW_ALERT_DIALOG(dialog), ok, FALSE);

  a->entry = gtk_entry_new();
  gtk_editable_set_text(GTK_EDITABLE(a->entry), a->name);
  gtk_entry_set_activates_default(GTK_ENTRY(a->entry), TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(a->entry), GTK_ACCESSIBLE_PROPERTY_LABEL, mc_str(MC_STR_NAME), -1);
  g_signal_connect(a->entry, "changed", G_CALLBACK(on_name_changed), dialog);
  adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), a->entry);
  adw_dialog_set_focus(dialog, a->entry);
  return dialog;
}

static void on_rename_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = g_object_get_data(G_OBJECT(dialog), "playlist-dialog");
  if (strcmp(response, "rename")) return;
  char *name = entered_name(a);
  if (!*name || !strcmp(name, a->name)) {
    g_free(name);
    return;
  }
  if (mc_playlist_rename(a->lib, a->uid, name) == MC_OK) {
    if (a->changed) a->changed(a->user);
  } else {
    fail(a->parent, mc_str(MC_STR_PLAYLIST_RENAME_FAILED));
  }
  g_free(name);
}

void playlist_rename_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                             void (*changed)(void *user), void *user) {
  if (!lib || !uid || strlen(uid) >= MC_UID_STRLEN) return;
  AdwDialog *dialog = name_dialog(mc_str(MC_STR_PLAYLIST_RENAME_TITLE), parent, lib, uid, name, "rename",
                                  MC_STR_RENAME, changed, user);
  g_signal_connect(dialog, "response", G_CALLBACK(on_rename_response), NULL);
  adw_dialog_present(dialog, parent);
}

/* --- New --- */

static void on_create_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = g_object_get_data(G_OBJECT(dialog), "playlist-dialog");
  if (strcmp(response, "create")) return;
  char *name = entered_name(a);
  char uid[MC_UID_STRLEN];
  if (*name) {
    if (mc_playlist_create(a->lib, name, uid) != MC_OK) fail(a->parent, mc_str(MC_STR_PLAYLIST_CREATE_FAILED));
    else if (a->created) a->created(a->user, uid);
  }
  g_free(name);
}

void playlist_create_present(GtkWidget *parent, mc_library *lib, void (*created)(void *user, const char *uid),
                             void *user) {
  if (!lib) return;
  AdwDialog *dialog = name_dialog(mc_str(MC_STR_NEW_PLAYLIST), parent, lib, "", NULL, "create", MC_STR_CREATE,
                                  NULL, user);
  ask *a = g_object_get_data(G_OBJECT(dialog), "playlist-dialog");
  a->created = created;
  g_signal_connect(dialog, "response", G_CALLBACK(on_create_response), NULL);
  adw_dialog_present(dialog, parent);
}

/* --- Delete --- */

static void on_delete_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = g_object_get_data(G_OBJECT(dialog), "playlist-dialog");
  if (strcmp(response, "delete")) return;
  if (mc_playlist_delete(a->lib, a->uid) == MC_OK) {
    if (a->changed) a->changed(a->user);
  } else {
    fail(a->parent, mc_str(MC_STR_PLAYLIST_DELETE_FAILED));
  }
}

void playlist_delete_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                             void (*changed)(void *user), void *user) {
  if (!lib || !uid || strlen(uid) >= MC_UID_STRLEN) return;
  char *heading = mc_format_alloc(mc_str(MC_STR_PLAYLIST_DELETE_TITLE), MC_S(name));
  AdwDialog *dialog = adw_alert_dialog_new(heading, mc_str(MC_STR_PLAYLIST_DELETE_TEXT));
  mc_free(heading);
  ask_new(dialog, parent, lib, uid, name, changed, user);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "delete",
                                 mc_str_mnemonic(MC_STR_DELETE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_delete_response), NULL);
  adw_dialog_present(dialog, parent);
}
