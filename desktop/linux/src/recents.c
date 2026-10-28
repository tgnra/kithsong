#include "recents.h"

#include <string.h>

enum { MAX_RECENTS = 10 };

static struct {
  GtkWidget *heading, *list;
} r;

/* The icon of the sidebar page it opens on. */
static const char *kind_icon(mc_recent_kind kind) {
  switch (kind) {
    case MC_RECENT_PLAYLIST: return "view-list-symbolic";
    case MC_RECENT_ARTIST: return "avatar-default-symbolic";
    case MC_RECENT_ALBUM_GROUP: return "folder-symbolic";
    default: return "media-optical-symbolic";
  }
}

/* A row like the pages above it: the icon and the name, cut off if it's long, with the
 * whole name and the album's artist (or what it is: "Playlist", "Artist", "Album group",
 * "Smart playlist", "Smart album group") in the tooltip. */
static GtkWidget *recent_row(const mc_recent_info *info) {
  const char *name = info->name && *info->name ? info->name : mc_str(MC_STR_UNTITLED);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_start(box, 6);
  gtk_widget_set_margin_end(box, 6);
  gtk_widget_set_margin_top(box, 6);
  gtk_widget_set_margin_bottom(box, 6);
  gtk_box_append(GTK_BOX(box), gtk_image_new_from_icon_name(kind_icon(info->kind)));
  GtkWidget *label = gtk_label_new(name);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_box_append(GTK_BOX(box), label);

  const char *detail = info->kind == MC_RECENT_PLAYLIST && info->is_smart      ? mc_str(MC_STR_SMART_PLAYLIST)
                       : info->kind == MC_RECENT_PLAYLIST                      ? mc_str(MC_STR_PLAYLIST)
                       : info->kind == MC_RECENT_ARTIST                        ? mc_str(MC_STR_ARTIST)
                       : info->kind == MC_RECENT_ALBUM_GROUP && info->is_smart ? mc_str(MC_STR_SMART_ALBUM_GROUP)
                       : info->kind == MC_RECENT_ALBUM_GROUP                   ? mc_str(MC_STR_ALBUM_GROUP)
                       : info->artist                        ? info->artist
                                                             : mc_str(MC_STR_UNKNOWN_ARTIST);
  char *tip = g_strdup_printf("%s\n%s", name, detail);
  GtkWidget *row = gtk_list_box_row_new();
  gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
  gtk_widget_set_tooltip_text(row, tip);
  g_free(tip);
  g_object_set_data_full(G_OBJECT(row), "uid", g_strdup(info->uid), g_free);
  g_object_set_data(G_OBJECT(row), "kind", GINT_TO_POINTER(info->kind));
  return row;
}

static void on_activated(GtkListBox *list, GtkListBoxRow *row, gpointer user) {
  (void)list;
  const char *uid = g_object_get_data(G_OBJECT(row), "uid");
  if (!uid) return;
  char copy[MC_UID_STRLEN];
  g_strlcpy(copy, uid, sizeof copy); /* the row goes if opening refreshes the section */
  switch ((mc_recent_kind)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "kind"))) {
    case MC_RECENT_ARTIST: app_open_artist(user, copy); break;
    case MC_RECENT_ALBUM_GROUP: app_open_album_group(user, copy); break;
    case MC_RECENT_PLAYLIST: app_open(user, TRUE, copy); break;
    default: app_open(user, FALSE, copy); break;
  }
}

GtkWidget *recents_new(app *a) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  r.heading = gtk_label_new(mc_str(MC_STR_SECTION_RECENTS));
  gtk_widget_add_css_class(r.heading, "heading");
  gtk_widget_add_css_class(r.heading, "dim-label");
  gtk_label_set_xalign(GTK_LABEL(r.heading), 0);
  gtk_widget_set_margin_start(r.heading, 18);
  gtk_widget_set_margin_top(r.heading, 12);
  gtk_box_append(GTK_BOX(box), r.heading);

  /* Buttons rather than pages: nothing stays selected. */
  r.list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(r.list), GTK_SELECTION_NONE);
  gtk_widget_add_css_class(r.list, "navigation-sidebar");
  g_signal_connect(r.list, "row-activated", G_CALLBACK(on_activated), a);
  gtk_box_append(GTK_BOX(box), r.list);
  gtk_widget_set_visible(box, FALSE); /* until there are some */
  return box;
}

void recents_refresh(app *a) {
  if (!r.list) return;
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(r.list))) gtk_list_box_remove(GTK_LIST_BOX(r.list), child);

  mc_recent_list *list = NULL;
  if (!a->lib || mc_library_list_recents(a->lib, 0, MC_LIST_PLAYABLE, &list) != MC_OK) list = NULL;
  int n = 0;
  for (int i = 0; list && i < list->count && n < MAX_RECENTS; i++) {
    const mc_recent_info *info = &list->items[i];
    gtk_list_box_append(GTK_LIST_BOX(r.list), recent_row(info));
    n++;
  }
  mc_recent_list_free(list);
  gtk_widget_set_visible(gtk_widget_get_parent(r.list), n > 0);
}
