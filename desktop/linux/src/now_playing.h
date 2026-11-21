/* The now-playing strip, in the middle of the header bar: cover, track, seekbar, shuffle and
 * the queue's button (queue_button.h). The bar and the shuffle toggle take the playing cover's
 * accent (art_accent.h). A drag seeks when it's let go. Its track menu has no Move Up, Move
 * Down or Remove from Playlist, which are for a place in a list, and what's picked applies to
 * the track it was opened on. A left click is left to the header bar, so the window can still
 * be dragged by the strip. Only one strip exists at a time.
 */
#ifndef KITHSONG_NOW_PLAYING_H
#define KITHSONG_NOW_PLAYING_H

#include <gtk/gtk.h>

#include "kithcore/kithcore.h"
#include "track_list.h"

/* seek is called with the position the user picked, in ms; shuffle when the user turns
 * shuffle on or off. */
GtkWidget *now_playing_new(void (*seek)(void *user, int ms), void (*shuffle)(void *user, gboolean on), void *user);

/* Sets up the track's menu: cb's add_to_playlist, delete_track, edit_track, go_to_album and
 * go_to_artist do what it asks. library says where the track is, or NULL for no menu (nothing
 * open, or an import session has the library). */
void now_playing_set_track_menu(const track_list_callbacks *cb, mc_library *(*library)(void *user));

/* Shows shuffle on or off, without calling shuffle. */
void now_playing_set_shuffle(gboolean on);

/* Shows a track: its title, artist (may be NULL), length (0 if unknown) and the folder
 * holding its album's cover (NULL if there's none). A NULL title shows nothing playing.
 * The position goes back to 0. */
void now_playing_show(const char *title, const char *artist, int duration_ms, const char *cover_dir);

/* Shows the track's cover again, now in cover_dir, leaving the rest as it is: it was changed. */
void now_playing_art_changed(const char *cover_dir);
/* The same if what's in cover_dir isn't what's showing: a cover that has come since. Otherwise
 * nothing, so a library changing during a track leaves it be. Whether it was shown. */
gboolean now_playing_cover_check(const char *cover_dir);

/* Where playback is, in ms. Ignored while the user drags the bar. */
void now_playing_set_position(int ms);

/* Shows text in the track's place for a moment, e.g. "Volume 40%" after a shortcut. */
void now_playing_flash(const char *text);
/* The same for a sentence to read, which stays longer. */
void now_playing_notice(const char *text);

#endif
