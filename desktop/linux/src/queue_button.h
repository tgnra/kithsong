/* The play queue's button, after the shuffle toggle in the now-playing strip, and its popover:
 *
 *                                        [><] [=]
 *   +-----------------------------------------+
 *   | Queue                         [ Clear ] |
 *   |-----------------------------------------|
 *   | = Title                       4:12    x |
 *   |   Artist                                |
 *   | ...                                     |
 *   +-----------------------------------------+
 *
 * It shows what the player has up next (playback.h), a row for each entry: a track, or an album or
 * the like as a block with what's left of it, and keeps up as that changes. Rows drag into another
 * order, as whole blocks, and the x on a row takes it out; Clear empties the queue and leaves
 * playback alone.
 *
 * The keyboard does the same. The arrow keys, Page Up and Down, Home and End go through the rows, up
 * past the first to Clear, and Tab goes between the list and Clear; Delete takes out the row and
 * Alt+Up and Alt+Down move it. Escape stops a drag, or closes the popover. Dragging is a gesture
 * within the popover rather than GTK's drag and drop, which would take the pointer from it and close
 * it. Only one exists at a time.
 */
#ifndef KITHSONG_QUEUE_BUTTON_H
#define KITHSONG_QUEUE_BUTTON_H

#include <gtk/gtk.h>

GtkWidget *queue_button_new(void);
/* The queue changed: shows it, if the popover is open. playback.c calls it. */
void queue_button_update(void);

#endif
