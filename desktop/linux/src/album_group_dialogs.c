#include "album_group_dialogs.h"

#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "album_group_dialogs.c needs libadwaita 1.5 or later (AdwAlertDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct ask {
  mc_library *lib;
  char uid[MC_UID_STRLEN];       /* the group; for Create, the album to add, or "" */
  char *name;                    /* the group's name as it was; "" for Create */
  GtkWidget *parent, *entry;     /* entry: Create's and Rename's */
  const char *ok;                /* the response the entry enables */
  void (*changed)(void *user);
  void (*created)(void *user, const char *uid); /* Create's, called after changed */
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
  g_strlcpy(a->uid, uid ? uid : "", sizeof a->uid);
  a->name = g_strdup(name ? name : "");
  a->parent = g_object_ref(parent);
  a->changed = changed;
  a->user = user;
  g_object_set_data_full(G_OBJECT(dialog), "album-group-dialog", a, ask_free);
  return a;
}

static ask *ask_of(AdwAlertDialog *dialog) {
  return g_object_get_data(G_OBJECT(dialog), "album-group-dialog");
}

static void fail(GtkWidget *parent, const char *heading) {
  AdwDialog *alert = adw_alert_dialog_new(heading, NULL);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, parent);
}

static void changed(ask *a) {
  if (a->changed) a->changed(a->user);
}

/* --- Create and Rename: a name --- */

static char *entered_name(ask *a) {
  return g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(a->entry))));
}

/* A name that isn't blank, and for Rename not the one it has. */
static gboolean name_ok(ask *a, const char *name) {
  return *name && strcmp(name, a->name);
}

static void on_name_changed(GtkEditable *entry, gpointer data) {
  (void)entry;
  AdwAlertDialog *dialog = data;
  ask *a = ask_of(dialog);
  char *name = entered_name(a);
  adw_alert_dialog_set_response_enabled(dialog, a->ok, name_ok(a, name));
  g_free(name);
}

/* An alert asking for a name, with Cancel and ok (enabled once there's a name). */
static AdwDialog *name_dialog(const char *heading, GtkWidget *parent, mc_library *lib, const char *uid,
                              const char *name, const char *ok, enum mc_str_id ok_label,
                              void (*changed_cb)(void *user), void *user) {
  AdwDialog *dialog = adw_alert_dialog_new(heading, NULL);
  ask *a = ask_new(dialog, parent, lib, uid, name, changed_cb, user);
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

static void on_create_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = ask_of(dialog);
  if (strcmp(response, "create")) return;
  char *name = entered_name(a);
  char group[MC_UID_STRLEN];
  if (!name_ok(a, name)) {
    g_free(name);
    return;
  }
  if (mc_album_group_create(a->lib, name, group) != MC_OK) {
    fail(a->parent, mc_str(MC_STR_ALBUM_GROUP_CREATE_FAILED));
  } else {
    if (a->uid[0] && mc_album_group_add(a->lib, group, a->uid) != MC_OK)
      fail(a->parent, mc_str(MC_STR_ALBUM_GROUP_ADD_FAILED));
    changed(a);
    if (a->created) a->created(a->user, group);
  }
  g_free(name);
}

void album_group_create_present(GtkWidget *parent, mc_library *lib, const char *album_uid,
                                void (*changed_cb)(void *user), void *user) {
  if (!lib || (album_uid && strlen(album_uid) >= MC_UID_STRLEN)) return;
  AdwDialog *dialog = name_dialog(mc_str(MC_STR_NEW_ALBUM_GROUP), parent, lib, album_uid, NULL, "create",
                                  MC_STR_CREATE, changed_cb, user);
  g_signal_connect(dialog, "response", G_CALLBACK(on_create_response), NULL);
  adw_dialog_present(dialog, parent);
}

void album_group_new_present(GtkWidget *parent, mc_library *lib, void (*created)(void *user, const char *uid),
                             void *user) {
  if (!lib) return;
  AdwDialog *dialog = name_dialog(mc_str(MC_STR_NEW_ALBUM_GROUP), parent, lib, NULL, NULL, "create", MC_STR_CREATE,
                                  NULL, user);
  ask_of(ADW_ALERT_DIALOG(dialog))->created = created;
  g_signal_connect(dialog, "response", G_CALLBACK(on_create_response), NULL);
  adw_dialog_present(dialog, parent);
}

static void on_rename_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = ask_of(dialog);
  if (strcmp(response, "rename")) return;
  char *name = entered_name(a);
  if (name_ok(a, name)) {
    if (mc_album_group_rename(a->lib, a->uid, name) == MC_OK) changed(a);
    else fail(a->parent, mc_str(MC_STR_ALBUM_GROUP_RENAME_FAILED));
  }
  g_free(name);
}

void album_group_rename_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                                void (*changed_cb)(void *user), void *user) {
  if (!lib || !uid || strlen(uid) >= MC_UID_STRLEN) return;
  AdwDialog *dialog = name_dialog(mc_str(MC_STR_ALBUM_GROUP_RENAME_TITLE), parent, lib, uid, name, "rename",
                                  MC_STR_RENAME, changed_cb, user);
  g_signal_connect(dialog, "response", G_CALLBACK(on_rename_response), NULL);
  adw_dialog_present(dialog, parent);
}

/* --- Delete --- */

static void on_delete_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = ask_of(dialog);
  if (strcmp(response, "delete")) return;
  if (mc_album_group_delete(a->lib, a->uid) == MC_OK) changed(a);
  else fail(a->parent, mc_str(MC_STR_ALBUM_GROUP_DELETE_FAILED));
}

void album_group_delete_present(GtkWidget *parent, mc_library *lib, const char *uid, const char *name,
                                void (*changed_cb)(void *user), void *user) {
  if (!lib || !uid || strlen(uid) >= MC_UID_STRLEN) return;
  char *heading = mc_format_alloc(mc_str(MC_STR_PLAYLIST_DELETE_TITLE), MC_S(name ? name : ""));
  AdwDialog *dialog = adw_alert_dialog_new(heading, mc_str(MC_STR_ALBUM_GROUP_DELETE_TEXT));
  mc_free(heading);
  ask_new(dialog, parent, lib, uid, name, changed_cb, user);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "delete",
                                 mc_str_mnemonic(MC_STR_DELETE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_delete_response), NULL);
  adw_dialog_present(dialog, parent);
}
