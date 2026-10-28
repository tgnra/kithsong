#include "remove_here.h"

#include <string.h>

#include "sync_bridge.h"

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "remove_here.c needs libadwaita 1.5 or later (AdwAlertDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct ask {
  mc_here_plan *plan;
  int kept; /* the plan's originals that stay for now */
  GtkWidget *parent;
  void (*changed)(void *user);
  void *user;
} ask;

static void ask_free(gpointer data) {
  ask *a = data;
  mc_here_plan_free(a->plan);
  g_object_unref(a->parent);
  g_free(a);
}

static void tell(GtkWidget *parent, const char *heading, const char *body) {
  AdwDialog *alert = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, parent);
}

void remove_here_append(GMenu *menu, mc_profile_item_kind kind, const char *uid, const char *remove_action,
                        const char *sync_action) {
  mc_sync *sync = sync_bridge_get();
  mc_here_state state = MC_HERE_UNAVAILABLE;
  if (!sync || !uid || !*uid || mc_sync_here_state(sync, kind, uid, &state) != MC_OK) return;
  if (state == MC_HERE_HELD)
    g_menu_append(menu, mc_str_mnemonic(MC_STR_LINUX_REMOVE_FROM_THIS_COMPUTER), remove_action);
  else if (state == MC_HERE_REMOVED)
    g_menu_append(menu, mc_str_mnemonic(MC_STR_LINUX_SYNC_TO_THIS_COMPUTER), sync_action);
}

static void on_response(AdwAlertDialog *dialog, const char *response, gpointer data) {
  (void)data;
  ask *a = g_object_get_data(G_OBJECT(dialog), "remove-here");
  mc_sync *sync = sync_bridge_get();
  if (strcmp(response, "remove") || !sync) return;
  mc_here_result res;
  mc_status st = mc_sync_remove_here(sync, a->plan, &res);
  if (st == MC_OK && a->changed) a->changed(a->user);
  if (st != MC_OK) {
    tell(a->parent, mc_str(MC_STR_REMOVE_HERE_FAILED), NULL);
  } else if (res.files_kept > a->kept) {
    int n = res.files_kept - a->kept;
    char *body = mc_format_alloc(mc_str_plural(MC_STR_REMOVE_HERE_IN_USE, n), MC_D(n));
    tell(a->parent, mc_str(MC_STR_REMOVE_HERE_FAILED), body);
    mc_free(body);
  }
}

void remove_here_present(GtkWidget *parent, mc_profile_item_kind kind, const char *uid, const char *name,
                         void (*changed)(void *user), void *user) {
  mc_sync *sync = sync_bridge_get();
  if (!sync || !uid) return;
  mc_here_plan *plan = NULL;
  if (mc_sync_here_plan_new(sync, kind, uid, &plan) != MC_OK) {
    tell(parent, mc_str(MC_STR_REMOVE_HERE_FAILED), NULL);
    return;
  }
  int files = mc_here_plan_files(plan), kept = mc_here_plan_kept(plan);
  /* What goes (or that nothing's here), what stays for now, how to get it back. */
  GString *body = g_string_new(NULL);
  if (files) {
    char *s = mc_format_alloc(mc_str_plural(MC_STR_REMOVE_HERE_FILES, files), MC_D(files));
    g_string_append(body, s);
    mc_free(s);
  } else if (!kept) {
    g_string_append(body, mc_str(MC_STR_REMOVE_HERE_NO_FILES));
  }
  if (kept) {
    char *s = mc_format_alloc(mc_str_plural(MC_STR_REMOVE_HERE_KEPT, kept), MC_D(kept));
    if (body->len) g_string_append_c(body, ' ');
    g_string_append(body, s);
    mc_free(s);
  }
  if (body->len) g_string_append_c(body, ' ');
  g_string_append(body, mc_str(MC_STR_LINUX_REMOVE_FROM_THIS_COMPUTER_BACK));
  char *heading = mc_format_alloc(mc_str(MC_STR_LINUX_REMOVE_FROM_THIS_COMPUTER_TITLE),
                                  MC_S(name && *name ? name : mc_str(MC_STR_UNKNOWN_VALUE)));
  AdwDialog *dialog = adw_alert_dialog_new(heading, body->str);
  mc_free(heading);
  g_string_free(body, TRUE);

  ask *a = g_new0(ask, 1);
  a->plan = plan;
  a->kept = kept;
  a->parent = g_object_ref(parent);
  a->changed = changed;
  a->user = user;
  g_object_set_data_full(G_OBJECT(dialog), "remove-here", a, ask_free);
  adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), "cancel", mc_str_mnemonic(MC_STR_CANCEL), "remove",
                                 mc_str_mnemonic(MC_STR_REMOVE), NULL);
  adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "remove", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "cancel");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "cancel");
  g_signal_connect(dialog, "response", G_CALLBACK(on_response), NULL);
  adw_dialog_present(dialog, parent);
}

void remove_here_restore(GtkWidget *parent, mc_profile_item_kind kind, const char *uid,
                         void (*changed)(void *user), void *user) {
  mc_sync *sync = sync_bridge_get();
  if (!sync || !uid) return;
  if (mc_sync_restore_here(sync, kind, uid) != MC_OK) {
    tell(parent, mc_str(MC_STR_SYNC_HERE_FAILED), NULL);
    return;
  }
  mc_sync_now(sync, NULL); /* bring it back soon, rather than at the next scheduled sync */
  if (changed) changed(user);
}
