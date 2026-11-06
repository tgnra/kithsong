/* Playback in the GTK frontend: the core's player, and keeping everything that shows it in
 * step: the now-playing strip, the track list's playing row, the window title and MPRIS
 * (mpris.c), which is also how media keys and the desktop's media controls reach us.
 *
 * The player's callbacks come on its own thread and are passed to the UI thread with
 * g_idle_add(); while playing, a timer moves the seekbar on. The player is created on first
 * play. Plays are counted in the library and scrobbled when the player says they count. A
 * station's queue grows as it plays, and what's added is looked up in the library as it comes.
 * Call everything here on the UI thread.
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
/* changed is called, with playback_init()'s user, when a play just logged has changed the
 * recents: a station goes in them once its first track ends (mc_play_event_starts_station()). */
void playback_on_recents_changed(void (*changed)(void *user));
/* failed is called, with playback_init()'s user, when playback_play_radio() was asked for a
 * station that can't play, to tell the user so. */
void playback_on_radio_failed(void (*failed)(void *user));


/* Stops playing and empties the queue, letting go of the file that was playing; for before
 * the library's files move. */
void playback_clear(void);
/* album_uid's cover was changed: what's queued from it takes the cover from lib, and what's
 * showing the playing track shows it. */
void playback_art_changed(mc_library *lib, const char *album_uid);
/* The library changed, perhaps by sync: a cover that has arrived for the playing track shows. */
void playback_cover_check(mc_library *lib);
/* An edit may have changed what's queued: its titles and artists are read again from lib, what's
 * showing the playing track shows them, and the player gets any file's new place. Nothing stops. */
void playback_tracks_changed(mc_library *lib);
/* Replaces the queue and plays tracks[start], or with start -1 the first track (shuffled,
 * any track). context and context_uid (or NULL) say what's played, for the play log. */
void playback_play(const playback_track *tracks, int count, int start, mc_play_context context,
                   const char *context_uid);
/* Replaces the queue with a station from a track (see mc_player_play_radio()), in the library
 * set with playback_set_library(). A seed that can't be used leaves what's playing as it is, and
 * says so (playback_on_radio_failed()). */
void playback_play_radio(const char *seed_track_uid);
/* As playback_play_radio(), for a station that starts from start_uid, or if that can't be played
 * or is NULL or "", from its seed (mc_player_play_station()). */
void playback_play_station(const char *seed_track_uid, const char *start_uid);
/* Whether a track can start a station, for a menu to say so. It reads that one track. */
gboolean playback_can_radio(const char *seed_track_uid);
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
 * created, and the header bar's volume control (volume_button.c) and MPRIS show them. */
void playback_set_volume(int volume);
int playback_volume(void);
void playback_set_muted(gboolean muted);
gboolean playback_muted(void);

gboolean playback_is_playing(void);
mc_player_state playback_state(void);
/* Whether there's a current track, and one after it (always, shuffled or from a station that's
 * still adding). */
gboolean playback_has_current(void);
gboolean playback_has_next(void);
/* The current track's uid, or "" with none or for one without. Valid until the queue changes. */
const char *playback_current_uid(void);
/* What's queued so far: how many tracks, and track i's uid ("" past the end). */
int playback_queue_count(void);
const char *playback_queue_uid(int i);
int64_t playback_position_ms(void);
int64_t playback_duration_ms(void);

#endif
