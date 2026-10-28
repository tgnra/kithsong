/* The now-playing strip, in the middle of the header bar:
 *
 *   +----+         Paranoid Android — Radiohead
 *   |art |  1:02  ======O---------------------------  -5:21  [><]
 *   +----+
 *
 * The cover sits in a rounded square, the track above the seekbar, with the time so far on
 * its left and the time left on its right, then the shuffle toggle (in the accent colour
 * while on). Dragging the bar (or the arrow keys once it has focus) moves the position; a
 * drag seeks when it's let go. With nothing playing the bar is greyed out. Only one strip
 * exists at a time.
 */
#ifndef KITHSONG_NOW_PLAYING_H
#define KITHSONG_NOW_PLAYING_H

#include <gtk/gtk.h>

/* seek is called with the position the user picked, in ms; shuffle when the user turns
 * shuffle on or off. */
GtkWidget *now_playing_new(void (*seek)(void *user, int ms), void (*shuffle)(void *user, gboolean on), void *user);

/* Shows shuffle on or off, without calling shuffle. */
void now_playing_set_shuffle(gboolean on);

/* Shows a track: its title, artist (may be NULL), length (0 if unknown) and the folder
 * holding its album's cover (NULL if there's none). A NULL title shows nothing playing.
 * The position goes back to 0. */
void now_playing_show(const char *title, const char *artist, int duration_ms, const char *cover_dir);

/* Where playback is, in ms. Ignored while the user drags the bar. */
void now_playing_set_position(int ms);

/* Shows text in the track's place for a moment, e.g. "Volume 40%" after a shortcut. */
void now_playing_flash(const char *text);

#endif
