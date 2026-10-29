/* Playback in the GTK frontend: the core's player, and keeping everything that shows it in
 * step: the now-playing strip, the track list's playing row, the window title and MPRIS
 * (mpris.c), which is also how media keys and the desktop's media controls reach us.
 *
 * The player's callbacks come on its own thread and are passed to the UI thread with
 * g_idle_add(); while playing, a timer moves the seekbar on. The player is created on first
 * play. Plays are counted in the library and scrobbled when the player says they count. Call
 * everything here on the UI thread.
 */
#ifndef KITHSONG_PLAYBACK_H
#define KITHSONG_PLAYBACK_H

#include <gtk/gtk.h>

#include "musiccore/musiccore.h"

/* A queue item. Strings are copied. */
typedef struct playback_track {
  const char *uid;
  const char *path;
  const char *title, *artist, *album; /* artist and album may be NULL */
  const char *cover_dir;              /* the album folder, or NULL if it has no cover */
  int duration_ms;                    /* from the library; 0 if unknown */
} playback_track;

/* win's title follows the playing track. changed is called whenever what the playback
 * buttons should show may have changed: playing or not, and what's queued. */
void playback_init(GtkWindow *win, void (*changed)(void *user), void *user);
void playback_free(void);

/* Where plays are counted: lib (used on the UI thread only), and a scrobbler for it with
 * the sync.db in config_dir. NULL, before the library closes. Plays already counted go to
 * the library that was set before. */
void playback_set_library(mc_library *lib, const char *config_dir);
/* The scrobbler set up for the library, or NULL, for the Settings page to sign in with. It's
 * freed when the library changes; closing (if set) is called just before, so a sign-in in
 * progress with it can stop first. */
mc_scrobbler *playback_scrobbler(void);
void playback_on_scrobbler_closing(void (*closing)(void));
/* Whether this device scrobbles (mc_settings_scrobble(); on to begin with). */
void playback_set_scrobble(gboolean on);


/* Stops playing and empties the queue, letting go of the file that was playing; for before
 * the library's files move. */
void playback_clear(void);
/* Replaces the queue and plays tracks[start], or with start -1 the first track (shuffled,
 * any track). */
void playback_play(const playback_track *tracks, int count, int start);
/* Shuffle on or off (see mc_player_set_shuffle()): it applies to what's queued, and to
 * whatever is queued next. Off to begin with; main.c keeps it in the settings. */
void playback_set_shuffle(gboolean on);
gboolean playback_shuffle(void);
/* Pauses or resumes. Returns FALSE if nothing has been queued. */
gboolean playback_toggle(void);
void playback_resume(void);
void playback_pause(void);
void playback_stop(void);
void playback_next(void);
void playback_previous(void);
void playback_seek(int64_t ms);
/* Moves the position by delta_ms, within the current track. */
void playback_seek_by(int64_t delta_ms);

/* The volume, 0 to 100, and muted (see mc_player_set_volume()). 100 and not muted to begin
 * with; main.c keeps the volume in settings.json. They carry over to the player when it's
 * created. */
void playback_set_volume(int volume);
int playback_volume(void);
void playback_set_muted(gboolean muted);
gboolean playback_muted(void);

gboolean playback_is_playing(void);
mc_player_state playback_state(void);
/* Whether there's a current track, and one after it (always, shuffled). */
gboolean playback_has_current(void);
gboolean playback_has_next(void);
/* What's queued: how many tracks, and track i's uid ("" past the end). */
int playback_queue_count(void);
const char *playback_queue_uid(int i);
int64_t playback_position_ms(void);
int64_t playback_duration_ms(void);

#endif
