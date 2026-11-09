/* The cast button, right after the shuffle toggle in the now-playing strip, and its popover,
 * for Google Cast (playback.h's playback_cast(); see docs/cast.md):
 *
 *                     [><] [cast] [=]
 *   +--------------------------------+          +--------------------------------+
 *   | Cast to                    (o) |          | Casting to Living Room TV      |
 *   |--------------------------------|          |--------------------------------|
 *   | [cast] Living Room TV          |          | [-) ==========O-------- [-)))  |
 *   |        Chromecast              |          |                                |
 *   | [cast] Kitchen speaker         |          | [ Stop casting               ] |
 *   +--------------------------------+          +--------------------------------+
 *
 * It shows only while there's a device to cast to, or a cast connecting or under way, and
 * while its popover is open. Its icon says which: the cast icon, the connected one in the
 * accent colour while casting, and that one pulsing while connecting; its tooltip and name for
 * accessibility say so in words ("Casting to Living Room TV").
 *
 * The popover lists the devices found, as they're found (a device's name, its model under it),
 * or while there are none says it's looking. Picking one connects to it, and the popover
 * stays to show that ("Connecting to Living Room TV…"). While connected it has the device's
 * volume and Stop casting instead; the slider has the focus when it opens, so the arrow keys
 * move it. Only one exists at a time.
 */
#ifndef KITHSONG_CAST_BUTTON_H
#define KITHSONG_CAST_BUTTON_H

#include <gtk/gtk.h>

GtkWidget *cast_button_new(void);
/* The cast changed: shows its state, the volume, and with devices, the list again (on
 * MC_CAST_EVENT_DEVICES). playback.c calls it. */
void cast_button_update(gboolean devices);

#endif
