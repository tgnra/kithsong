/* Dragging tracks onto playlists and albums onto album groups, within the window.
 *
 * A track list's row drags its track, and an album grid's tile its album (drag_source_new()).
 * What takes them: a manual playlist's or group's recent in the sidebar and its tile on the
 * Playlists or Album groups page, which add it (GDK_ACTION_COPY), and a playlist's own
 * tracks or a manual group's own albums, which move it there (GDK_ACTION_MOVE). Anything
 * else refuses it, so the pointer shows that it can't be dropped there.
 *
 * Targets ask drag_current() what's being dragged while it's over them, rather than reading
 * the dragged text, which only arrives on drop; drag_from_us() tells our drags from text
 * dragged in from other programs. */
#ifndef KITHSONG_DRAG_H
#define KITHSONG_DRAG_H

#include <gtk/gtk.h>

#include "musiccore/musiccore.h"

typedef enum drag_kind {
  DRAG_TRACK, /* onto a manual playlist */
  DRAG_ALBUM, /* onto a manual album group */
} drag_kind;

/* What a drag carries: a track (with its playlist entry, if it's dragged from a playlist,
 * else "") or an album. */
typedef struct drag_item {
  drag_kind kind;
  char uid[MC_UID_STRLEN];
  char entry[MC_UID_STRLEN];
} drag_item;

/* A drag source for widget. prepare says what a drag starting at x, y carries, filling
 * *item, or FALSE for no drag; the source then offers it to copy (onto a playlist or group)
 * or move (within one), with the widget's look as the drag icon. */
GtkDragSource *drag_source_new(GtkWidget *widget, gboolean (*prepare)(gpointer user, drag_item *item), gpointer user);

/* The item being dragged now, or NULL when there's no drag of ours. */
const drag_item *drag_current(void);
/* Whether target's drop is one of ours (drag_current() then says what it is). */
gboolean drag_from_us(GtkDropTarget *target);

/* A drop target for strings, taking actions; with preload off, as drag_current() is read. */
GtkDropTarget *drag_target_new(GdkDragAction actions);

#endif
