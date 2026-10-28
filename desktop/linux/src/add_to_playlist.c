#include "add_to_playlist.h"

#include <string.h>

#if !ADW_CHECK_VERSION(1, 5, 0)
#error "add_to_playlist.c needs libadwaita 1.5 or later (AdwDialog)"
#endif

/* Owned by the dialog: freed with it. */
typedef struct picker {
  mc_library *lib;
  char track_uid[MC_UID_STRLEN];
  void (*changed)(void *user);
  void *user;
  GtkWidget *dialog, *toasts, *name, *list;
} picker;

static void toast(picker *p, const char *text) {
  adw_toast_overlay_add_toast(ADW_TOAST_OVERLAY(p->toasts), adw_toast_new(text));
}

/* Lists the playlists, focusing the one with uid focus (or none for NULL). */
static void fill(picker *p, const char *focus) {
  gtk_list_box_remove_all(GTK_LIST_BOX(p->list));
  mc_playlist_list *lists = NULL;
  if (mc_library_list_playlists(p->lib, 0, &lists) != MC_OK) {
    toast(p, mc_str(MC_STR_LINUX_PLAYLISTS_LIST_FAILED));
    return;
  }
  for (int i = 0; i < lists->count; i++) {
    const mc_playlist_info *info = &lists->items[i];
    GtkWidget *row = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), info->name);
    char count[64];
    mc_format(count, sizeof count, mc_str_plural(MC_STR_TRACKS, info->track_count), MC_D(info->track_count));
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), count);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), TRUE);
    g_object_set_data_full(G_OBJECT(row), "uid", g_strdup(info->uid), g_free);
    gtk_list_box_append(GTK_LIST_BOX(p->list), row);
    if (focus && !strcmp(info->uid, focus)) adw_dialog_set_focus(ADW_DIALOG(p->dialog), row);
  }
  mc_playlist_list_free(lists);
}

static void on_create(AdwEntryRow *entry, gpointer data) {
  picker *p = data;
  char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
  char uid[MC_UID_STRLEN];
  if (*name && mc_playlist_create(p->lib, name, uid) == MC_OK) {
    gtk_editable_set_text(GTK_EDITABLE(entry), "");
    fill(p, uid);
    if (p->changed) p->changed(p->user);
  } else if (*name) {
    toast(p, mc_str(MC_STR_PLAYLIST_CREATE_FAILED));
  }
  g_free(name);
}

static void on_playlist_activated(GtkListBox *list, GtkListBoxRow *row, gpointer data) {
  (void)list;
  picker *p = data;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  if (!uid || mc_playlist_append(p->lib, uid, p->track_uid, NULL) != MC_OK) {
    toast(p, mc_str(MC_STR_PLAYLIST_ADD_FAILED));
    return;
  }
  /* Close first: changed reloads the window behind. */
  g_object_ref(p->dialog);
  adw_dialog_close(ADW_DIALOG(p->dialog));
  if (p->changed) p->changed(p->user);
  g_object_unref(p->dialog);
}

void add_to_playlist_present(GtkWidget *parent, mc_library *lib, const char *track_uid,
                             void (*changed)(void *user), void *user) {
  if (!lib || !track_uid || strlen(track_uid) >= MC_UID_STRLEN) return;
  picker *p = g_new0(picker, 1);
  p->lib = lib;
  strcpy(p->track_uid, track_uid);
  p->changed = changed;
  p->user = user;

  p->dialog = GTK_WIDGET(adw_dialog_new());
  g_object_set_data_full(G_OBJECT(p->dialog), "add-to-playlist", p, g_free);
  adw_dialog_set_title(ADW_DIALOG(p->dialog), mc_str(MC_STR_ADD_TO_PLAYLIST));
  adw_dialog_set_content_width(ADW_DIALOG(p->dialog), 360);
  adw_dialog_set_content_height(ADW_DIALOG(p->dialog), 460);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_widget_set_margin_start(box, 12);
  gtk_widget_set_margin_end(box, 12);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 12);

  GtkWidget *create = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(create), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(create, "boxed-list");
  p->name = adw_entry_row_new();
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(p->name), mc_str(MC_STR_NEW_PLAYLIST));
  adw_entry_row_set_show_apply_button(ADW_ENTRY_ROW(p->name), TRUE);
  g_signal_connect(p->name, "apply", G_CALLBACK(on_create), p);
  gtk_list_box_append(GTK_LIST_BOX(create), p->name);
  gtk_box_append(GTK_BOX(box), create);

  p->list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(p->list, "boxed-list");
  gtk_widget_set_valign(p->list, GTK_ALIGN_START);
  GtkWidget *none = gtk_label_new(mc_str(MC_STR_NO_PLAYLISTS_TITLE));
  gtk_widget_add_css_class(none, "dim-label");
  gtk_widget_set_margin_top(none, 12);
  gtk_widget_set_margin_bottom(none, 12);
  gtk_list_box_set_placeholder(GTK_LIST_BOX(p->list), none);
  g_signal_connect(p->list, "row-activated", G_CALLBACK(on_playlist_activated), p);
  gtk_box_append(GTK_BOX(box), p->list);

  GtkWidget *scroller = gtk_scrolled_window_new();
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box);
  p->toasts = adw_toast_overlay_new();
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(p->toasts), scroller);

  GtkWidget *toolbar = adw_toolbar_view_new();
  adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
  adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), p->toasts);
  adw_dialog_set_child(ADW_DIALOG(p->dialog), toolbar);

  fill(p, NULL);
  adw_dialog_present(ADW_DIALOG(p->dialog), parent);
}
