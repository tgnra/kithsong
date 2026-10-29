#include "delete_dialog.h"

#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "delete_dialog.c needs libadwaita 1.5 or later (AdwAlertDialog)"
#endif

static void (*before_delete)(const mc_delete_plan *plan);

void delete_dialog_init(void (*before)(const mc_delete_plan *plan)) {
  before_delete = before;
}

/* Owned by the dialog: freed with it. */
typedef struct ask {
  mc_library *lib;
  char *config_dir;
  mc_delete_plan *plan;
  GtkWidget *parent;
  void (*changed)(void *user);
  void *user;
} ask;

static void ask_free(gpointer data) {
  ask *a = data;
  mc_delete_plan_free(a->plan);
  g_free(a->config_dir);
  g_object_unref(a->parent);
  g_free(a);
}

static void tell(GtkWidget *parent, const char *heading, const char *body) {
  AdwDialog *alert = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, parent);
}

static void on_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = g_object_get_data(G_OBJECT(dialog), "delete-dialog");
  if (strcmp(response, "delete")) return;
  if (before_delete) before_delete(a->plan);
  mc_delete_result res;
  mc_status st = mc_delete_run(a->lib, a->config_dir, a->plan, &res);
  if ((res.tracks_deleted || res.files_trashed) && a->changed) a->changed(a->user);
  if (st != MC_OK) {
    tell(a->parent, mc_str(MC_STR_DELETE_FAILED), NULL);
  } else if (res.tracks_kept) {
    char *body = mc_format_alloc(mc_str_plural(MC_STR_LINUX_DELETE_KEPT_TRASH, res.tracks_kept), MC_D(res.tracks_kept));
    tell(a->parent, mc_str(MC_STR_DELETE_FAILED), body);
    mc_free(body);
  }
}

void delete_dialog_present(GtkWidget *parent, mc_library *lib, const char *config_dir, mc_delete_kind kind,
                           const char *uid, const char *name, void (*changed)(void *user), void *user) {
  if (!lib || !config_dir || !uid) return;
  const char *uids[] = {uid};
  mc_delete_plan *plan = NULL;
  if (mc_delete_plan_new(lib, kind, uids, 1, &plan) != MC_OK) {
    tell(parent, mc_str(MC_STR_DELETE_FAILED), NULL);
    return;
  }
  if (!mc_delete_plan_tracks(plan)) { /* gone already */
    mc_delete_plan_free(plan);
    return;
  }
  int files = mc_delete_plan_files(plan);
  char *heading = mc_format_alloc(mc_str(MC_STR_PLAYLIST_DELETE_TITLE), MC_S(name && *name ? name : mc_str(MC_STR_UNKNOWN_VALUE)));
  char *body = files ? mc_format_alloc(mc_str_plural(MC_STR_LINUX_DELETE_TO_TRASH_TEXT, files), MC_D(files)) : NULL;
  AdwDialog *dialog = adw_alert_dialog_new(heading, body ? body : mc_str(MC_STR_DELETE_NO_FILES_TEXT));
  mc_free(heading);
  mc_free(body);

  ask *a = g_new0(ask, 1);
  a->lib = lib;
  a->config_dir = g_strdup(config_dir);
  a->plan = plan;
  a->parent = g_object_ref(parent);
  a->changed = changed;
  a->user = user;
  g_object_set_data_full(G_OBJECT(dialog), "delete-dialog", a, ask_free);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "delete",
                                 mc_str_mnemonic(MC_STR_DELETE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "delete", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_response), NULL);
  adw_dialog_present(dialog, parent);
}
