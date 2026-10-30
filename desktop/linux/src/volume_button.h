/* The volume control, at the right of the header bar: a speaker button whose icon shows the
 * level (muted, low, medium or high), and drops down a popover with the speaker again, which
 * mutes and unmutes, beside a slider:
 *
 *   [spk]
 *   +------------------------------+
 *   | [spk]  =========O----------  |
 *   +------------------------------+
 *
 * The slider has the focus when it opens, so the arrow keys move it; a click elsewhere or
 * Escape closes it. It shows playback.c's volume and muting whichever way they change (the
 * keyboard, MPRIS), as playback.c's setters call volume_button_update(). Only one exists at a
 * time.
 */
#ifndef KITHSONG_VOLUME_BUTTON_H
#define KITHSONG_VOLUME_BUTTON_H

#include <gtk/gtk.h>

/* set_volume is called with the volume the user picked, 0 to 100; set_muted when the user
 * mutes or unmutes. Neither changes playback itself, so main.c can keep the volume in
 * settings.json as it does the keyboard's. */
GtkWidget *volume_button_new(void (*set_volume)(void *user, int volume), void (*set_muted)(void *user, gboolean muted),
                             void *user);

/* Shows playback_volume() and playback_muted(), without calling back. */
void volume_button_update(void);

#endif
