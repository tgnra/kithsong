/* See issue_dialog.h. */
#include "issue_dialog.h"

#include <string.h>

#include "delete_dialog.h"
#include "device_view.h"
#include "devices.h"

enum {
  DIALOG_W = 520,
  DIALOG_MAX_H = 600,
  OFFER_WAIT_MS = 2000, /* after problems change, once for a burst, before reading the offer */
};

static struct {
  app *a;

  /* The problems dialog, while it's open. */
  AdwDialog *dialog;
  GtkWidget *list; /* the issues go in here */
  char filter[MC_UID_STRLEN]; /* only this device's; "" for all */

  /* The offer: read on a thread, one at a time, shortly after the problems change. */
  GThread *thread;
  mc_sync *thread_sync; /* the sync it reads, for telling a stale read */
  gboolean again;       /* the problems changed during the read */
  gboolean stopping;    /* closing the dialogs as sync stops: not the user's seeing them */
  guint wait_source;
  mc_sync_archive_offer *read; /* what the thread found, until on_read_done takes it */
  mc_sync_archive_offer *offer; /* the last one read; NULL if none */
  AdwDialog *offer_dialog;      /* while it's open */
} g;

static GtkWidget *parent_or_window(GtkWidget *parent) {
  return parent ? parent : g.a ? GTK_WIDGET(g.a->win) : NULL;
}

static GtkWidget *wrapped_label(const char *text, const char *css) {
  GtkWidget *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE); /* folder names and errors can be copied */
  if (css) gtk_widget_add_css_class(label, css);
  return label;
}

static void tell(GtkWidget *parent, const char *heading, const char *body) {
  AdwDialog *alert = adw_alert_dialog_new(heading, body);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(alert), "close", mc_str_mnemonic(MC_STR_CLOSE));
  adw_dialog_present(alert, parent_or_window(parent));
}

/* --- The offer --- */

static void on_offer_response(AdwAlertDialog *dialog, const char *response, gpointer user) {
  (void)user;
  const char *uid = g_object_get_data(G_OBJECT(dialog), "uid");
  mc_sync *sync = devices_sync();
  if (strcmp(response, "take") || !sync || !uid) return;
  mc_status st = mc_sync_take_over_archive(sync, uid);
  if (st != MC_OK) {
    tell(NULL, adw_alert_dialog_get_heading(dialog), mc_status_str(st));
    return;
  }
  /* Progress comes as an MC_ISSUE_TAKEOVER issue, with on_problems. */
  mc_sync_notify_changed(sync);
}

static void on_offer_destroy(GtkWidget *widget, gpointer user) {
  (void)widget, (void)user;
  g.offer_dialog = NULL;
}

/* The dialog for offer: its text, and a button only when this computer has room. */
static void show_offer(GtkWidget *parent, const mc_sync_archive_offer *offer) {
  if (g.offer_dialog || !parent_or_window(parent)) return;
  mc_sync_offer_text t;
  mc_sync_archive_offer_describe(offer, MC_DEVICE_DESKTOP, &t);
  if (!t.title[0]) return; /* nothing to say */
  AdwDialog *dialog = adw_alert_dialog_new(t.title, t.body[0] ? t.body : NULL);
  g_object_set_data_full(G_OBJECT(dialog), "uid", g_strdup(offer->device_uid), g_free);
  adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "later", mc_str(MC_STR_SYNC_OFFER_LATER));
  if (t.button[0]) {
    adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), "take", t.button);
    adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), "take", ADW_RESPONSE_SUGGESTED);
  }
  adw_alert_dialog_set_default_response(ADW_ALERT_DIALOG(dialog), "later");
  adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), "later");
  g_signal_connect(dialog, "response", G_CALLBACK(on_offer_response), NULL);
  g_signal_connect(dialog, "destroy", G_CALLBACK(on_offer_destroy), NULL);
  g.offer_dialog = dialog;
  adw_dialog_present(dialog, parent_or_window(parent));
}

static gboolean on_read_done(gpointer user);

static gpointer read_main(gpointer data) {
  mc_sync_archive_offer *offer = NULL;
  if (mc_sync_get_archive_offer(data, &offer) != MC_OK) offer = NULL;
  g.read = offer;
  g_idle_add(on_read_done, NULL);
  return NULL;
}

static void read_start(void) {
  mc_sync *sync = devices_sync();
  if (!sync) return;
  if (g.thread) {
    g.again = TRUE;
    return;
  }
  g.thread_sync = sync;
  g.thread = g_thread_new("archive-offer", read_main, sync);
}

/* Same offer, as far as the page and the row show it. */
static gboolean same_offer(const mc_sync_archive_offer *a, const mc_sync_archive_offer *b) {
  if (!a || !b) return a == b;
  return !strcmp(a->device_uid, b->device_uid) && a->can_take == b->can_take && a->nobody_can == b->nobody_can &&
         a->reinstall == b->reinstall;
}

/* The thread has ended, or issue_dialog_stop() saw to it already. */
static gboolean on_read_done(gpointer user) {
  (void)user;
  if (!g.thread) return G_SOURCE_REMOVE;
  g_thread_join(g.thread);
  g.thread = NULL;
  mc_sync_archive_offer *offer = g.read;
  g.read = NULL;
  if (g.thread_sync != devices_sync()) { /* sync restarted meanwhile */
    mc_sync_archive_offer_free(offer);
    read_start();
    return G_SOURCE_REMOVE;
  }
  gboolean changed = !same_offer(offer, g.offer);
  mc_sync_archive_offer_free(g.offer);
  g.offer = offer;
  /* Once by itself; then from the device's page and row. */
  if (offer && !offer->asked && !g.offer_dialog && g.a && g.a->win) {
    show_offer(NULL, offer);
    mc_sync *sync = devices_sync();
    if (sync && g.offer_dialog) mc_sync_archive_offer_seen(sync, offer->device_uid);
    offer->asked = 1;
  }
  if (changed) device_view_update();
  if (g.again) {
    g.again = FALSE;
    read_start();
  }
  return G_SOURCE_REMOVE;
}

static gboolean on_wait(gpointer user) {
  (void)user;
  g.wait_source = 0;
  read_start();
  return G_SOURCE_REMOVE;
}

const mc_sync_archive_offer *issue_dialog_offer(void) {
  return g.offer;
}

void issue_dialog_present_offer(GtkWidget *parent) {
  if (g.offer) show_offer(parent, g.offer);
}

/* --- The problems dialog --- */

static gboolean wanted(const mc_sync_issue *issue, const char *device_uid) {
  return !device_uid || !device_uid[0] || !strcmp(issue->device_uid, device_uid);
}

int issue_dialog_count(const char *device_uid, char *title, size_t size) {
  if (title && size) title[0] = '\0';
  mc_sync *sync = devices_sync();
  mc_sync_problems *p = NULL;
  if (!sync || (g.a && mc_library_is_stress(g.a->lib)) || mc_sync_get_problems(sync, &p) != MC_OK) return 0;
  int n = 0;
  for (int i = 0; i < p->issue_count; i++) {
    if (!wanted(&p->issues[i], device_uid)) continue;
    if (!n++ && title && size) {
      mc_sync_issue_text t;
      mc_sync_issue_describe(&p->issues[i], 0, &t);
      g_strlcpy(title, t.title, size);
    }
  }
  mc_sync_problems_free(p);
  return n;
}

static void on_lost_removed(void *user) {
  app_reload_library(user);
}

static void on_remove_lost(GtkButton *button, gpointer user) {
  (void)button, (void)user;
  if (!g.a) return;
  delete_dialog_lost(g.dialog ? GTK_WIDGET(g.dialog) : GTK_WIDGET(g.a->win), g.a->lib, on_lost_removed, g.a);
}

static void on_offer_clicked(GtkButton *button, gpointer user) {
  (void)user;
  issue_dialog_present_offer(GTK_WIDGET(button));
}

static GtkWidget *issue_box(const mc_sync_issue *issue) {
  mc_sync_issue_text t;
  mc_sync_issue_describe(issue, 0, &t);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_add_css_class(box, "card");
  GtkWidget *inner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  gtk_widget_set_margin_start(inner, 12);
  gtk_widget_set_margin_end(inner, 12);
  gtk_widget_set_margin_top(inner, 12);
  gtk_widget_set_margin_bottom(inner, 12);
  gtk_box_append(GTK_BOX(box), inner);
  gtk_box_append(GTK_BOX(inner), wrapped_label(t.title, "heading"));
  if (t.body[0]) gtk_box_append(GTK_BOX(inner), wrapped_label(t.body, NULL));
  if (t.fix[0]) {
    char fix[1024];
    mc_format(fix, sizeof fix, mc_str(MC_STR_SYNC_ISSUE_FIX), MC_S(t.fix));
    gtk_box_append(GTK_BOX(inner), wrapped_label(fix, "dim-label"));
  }
  GtkWidget *button = NULL;
  if (issue->kind == MC_ISSUE_LOST) {
    button = gtk_button_new_with_label(mc_str(MC_STR_SYNC_LOST_REMOVE));
    g_signal_connect(button, "clicked", G_CALLBACK(on_remove_lost), NULL);
  } else if ((issue->kind == MC_ISSUE_ARCHIVE_LOST || issue->kind == MC_ISSUE_ARCHIVE_REINSTALL) && g.offer) {
    button = gtk_button_new_with_label(mc_str(MC_STR_LINUX_SYNC_OFFER_BANNER_BUTTON));
    g_signal_connect(button, "clicked", G_CALLBACK(on_offer_clicked), NULL);
  }
  if (button) {
    gtk_widget_set_halign(button, GTK_ALIGN_START);
    gtk_widget_set_margin_top(button, 6);
    gtk_box_append(GTK_BOX(inner), button);
  }
  /* Read out as one: the title, then the rest. */
  char *name = g_strjoin(". ", t.title, t.body[0] ? t.body : NULL, NULL);
  gtk_accessible_update_property(GTK_ACCESSIBLE(box), GTK_ACCESSIBLE_PROPERTY_LABEL, name, -1);
  g_free(name);
  return box;
}

/* Fills the list from the problems as they are now. */
static void fill(void) {
  if (!g.list) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(g.list))) gtk_box_remove(GTK_BOX(g.list), child);
  mc_sync *sync = devices_sync();
  mc_sync_problems *p = NULL;
  int n = 0;
  if (sync && mc_sync_get_problems(sync, &p) == MC_OK) {
    for (int i = 0; i < p->issue_count; i++) {
      if (!wanted(&p->issues[i], g.filter)) continue;
      gtk_box_append(GTK_BOX(g.list), issue_box(&p->issues[i]));
      n++;
    }
  }
  mc_sync_problems_free(p);
  if (!n) {
    GtkWidget *none = wrapped_label(mc_str(MC_STR_SYNC_ISSUES_NONE), "dim-label");
    gtk_label_set_xalign(GTK_LABEL(none), 0.5f);
    gtk_label_set_justify(GTK_LABEL(none), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_top(none, 24);
    gtk_widget_set_margin_bottom(none, 24);
    gtk_box_append(GTK_BOX(g.list), none);
  }
}

static void on_closed(AdwDialog *dialog, gpointer user) {
  (void)dialog, (void)user;
  mc_sync *sync = devices_sync();
  if (sync && !g.stopping) mc_sync_dismiss_notices(sync); /* on_problems follows: the banner updates */
}

static void on_destroy(GtkWidget *widget, gpointer user) {
  (void)widget, (void)user;
  g.dialog = NULL;
  g.list = NULL;
}

void issue_dialog_present(GtkWidget *parent, const char *device_uid) {
  g_strlcpy(g.filter, device_uid ? device_uid : "", sizeof g.filter);
  if (g.dialog) { /* shows the device asked for */
    fill();
    adw_dialog_present(g.dialog, parent_or_window(parent));
    return;
  }
  if (!parent_or_window(parent)) return;
  g.list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(g.list, 12);
  gtk_widget_set_margin_end(g.list, 12);
  gtk_widget_set_margin_top(g.list, 6);
  gtk_widget_set_margin_bottom(g.list, 18);
  fill();
  GtkWidget *scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), TRUE);
  gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroll), DIALOG_MAX_H);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), g.list);
  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), scroll);
  g.dialog = adw_dialog_new();
  adw_dialog_set_title(g.dialog, mc_str(MC_STR_SYNC_ISSUES_TITLE));
  adw_dialog_set_content_width(g.dialog, DIALOG_W);
  adw_dialog_set_follows_content_size(g.dialog, FALSE);
  adw_dialog_set_child(g.dialog, toolbar);
  g_signal_connect(g.dialog, "closed", G_CALLBACK(on_closed), NULL);
  g_signal_connect(g.dialog, "destroy", G_CALLBACK(on_destroy), NULL);
  adw_dialog_present(g.dialog, parent_or_window(parent));
}

void issue_dialog_refresh(void) {
  fill();
  if (!g.wait_source) g.wait_source = g_timeout_add(OFFER_WAIT_MS, on_wait, NULL);
}

void issue_dialog_start(app *a) {
  g.a = a;
  issue_dialog_refresh(); /* the offer, if any, without waiting for a change */
}

void issue_dialog_stop(void) {
  g.stopping = TRUE;
  if (g.dialog) adw_dialog_force_close(g.dialog);
  if (g.offer_dialog) adw_dialog_force_close(g.offer_dialog);
  g.stopping = FALSE;
  g_clear_handle_id(&g.wait_source, g_source_remove);
  if (g.thread) {
    g_thread_join(g.thread); /* its idle finds g.thread NULL and does nothing */
    g.thread = NULL;
    mc_sync_archive_offer_free(g.read);
    g.read = NULL;
  }
  g.again = FALSE;
  gboolean had = g.offer != NULL;
  g_clear_pointer(&g.offer, mc_sync_archive_offer_free);
  if (had) device_view_update();
}
