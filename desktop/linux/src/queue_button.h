/* The play queue's button in the now-playing strip, and its popover listing what the player has
 * up next (playback.h): a row per entry, a track or a block (an album or the like) with what's
 * left of it. Rows drag into another order as whole blocks. Dragging is a gesture within the
 * popover rather than GTK's drag and drop, which would take the pointer from it and close it.
 * Only one exists at a time.
 */
#ifndef KITHSONG_QUEUE_BUTTON_H
#define KITHSONG_QUEUE_BUTTON_H

#include <gtk/gtk.h>

GtkWidget *queue_button_new(void);
/* The queue changed: shows it, if the popover is open. playback.c calls it. */
void queue_button_update(void);

#endif
