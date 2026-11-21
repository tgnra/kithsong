#include "drag.h"

#include <string.h>

static struct {
  gboolean active;
  drag_item item;
} d;

typedef struct source {
  gboolean (*prepare)(gpointer user, drag_item *item);
  gpointer user;
} source;

static GdkContentProvider *on_prepare(GtkDragSource *drag, double x, double y, gpointer data) {
  source *s = data;
  drag_item item = {0};
  if (!s->prepare(s->user, &item)) return NULL;
  d.item = item;
  d.active = TRUE;
  GtkWidget *widget = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(drag));
  GdkPaintable *icon = gtk_widget_paintable_new(widget);
  gtk_drag_source_set_icon(drag, icon, (int)x, (int)y);
  g_object_unref(icon);
  /* Text too, though only our own targets know what to do with it. */
  char *text = g_strdup_printf("kithsong:%s:%s", item.kind == DRAG_TRACK ? "track" : "album", item.uid);
  GdkContentProvider *content = gdk_content_provider_new_typed(G_TYPE_STRING, text);
  g_free(text);
  return content;
}

static void on_end(GtkDragSource *drag, GdkDrag *gdrag, gboolean delete_data, gpointer data) {
  (void)drag;
  (void)gdrag;
  (void)delete_data;
  (void)data;
  d.active = FALSE;
}

GtkDragSource *drag_source_new(GtkWidget *widget, gboolean (*prepare)(gpointer user, drag_item *item), gpointer user) {
  source *s = g_new0(source, 1);
  s->prepare = prepare;
  s->user = user;
  GtkDragSource *drag = gtk_drag_source_new();
  gtk_drag_source_set_actions(drag, GDK_ACTION_COPY | GDK_ACTION_MOVE);
  g_signal_connect_data(drag, "prepare", G_CALLBACK(on_prepare), s, (GClosureNotify)(void (*)(void))g_free, 0);
  g_signal_connect(drag, "drag-end", G_CALLBACK(on_end), NULL);
  gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(drag));
  return drag;
}

const drag_item *drag_current(void) {
  return d.active ? &d.item : NULL;
}

gboolean drag_from_us(GtkDropTarget *target) {
  GdkDrop *drop = gtk_drop_target_get_current_drop(target);
  return d.active && drop && gdk_drop_get_drag(drop); /* a drag from this process */
}

GtkDropTarget *drag_target_new(GdkDragAction actions) {
  return gtk_drop_target_new(G_TYPE_STRING, actions);
}
