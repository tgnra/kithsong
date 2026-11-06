/* Playback in the Win32 frontend: the core's player, and keeping everything that shows it
 * in step. That's the now-playing strip, the track list's playing row, the window title
 * (for the taskbar and Alt+Tab), the buttons under the taskbar thumbnail (taskbar.c) and
 * Windows' media flyout (media_controls.c).
 *
 * Media keys work while the window has the focus through WM_APPCOMMAND, and otherwise
 * through the media controls, which Windows sends them to.
 *
 * Plays are counted in the library and scrobbled when the player says they count, and each
 * one, counted or not, goes in the library's play log when it ends.
 *
 * The queue the app shows, Play Next and Play Last, is the core's up next: its tracks are items
 * like the rest, and queue_popup.c shows the list. It goes with the library, as its tracks do.
 *
 * The player is created on first play. Everything here runs on the UI thread.
 */
#ifndef KITHSONG_PLAYBACK_H
#define KITHSONG_PLAYBACK_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* A queue item. Strings are copied. */
typedef struct playback_track {
  const char *uid;
  const char *path; /* UTF-8 */
  const wchar_t *title, *artist, *album; /* artist and album may be NULL */
  const wchar_t *cover_dir;              /* the album folder, or NULL if it has no cover */
  int duration_ms;                       /* from the library; 0 if unknown */
} playback_track;

/* wnd is the main window. changed is called whenever what the playback buttons should show
 * may have changed: playing or not, and what's queued. */
void playback_init(HWND wnd, void (*changed)(void));
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
void playback_set_scrobble(BOOL on);
/* changed is called when a play just logged has changed the recents: a station goes in them
 * once its first track ends (mc_play_event_starts_station()). */
void playback_on_recents_changed(void (*changed)(void));


/* Call first in the main window's procedure. Returns TRUE, with *result set, if msg was
 * playback's: media keys, the taskbar's buttons. */
BOOL playback_window_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

/* Windows is shutting down or logging off, and may end the process without another message:
 * logs the play under way now. */
void playback_end_session(void);
/* Stops playing and empties the queue, letting go of the file that was playing; for before
 * the library's files move. */
void playback_clear(void);
/* Replaces the queue and plays tracks[start], or with start -1 the first track (shuffled,
 * any track). context and context_uid (or NULL) say what's played, for the play log. */
void playback_play(const playback_track *tracks, int count, int start, mc_play_context context,
                   const char *context_uid);
/* Replaces the queue with a station from that track (mc_player_play_radio()), which plays at
 * once. If the track can't seed one, the now-playing strip says so, and what's playing plays on,
 * as it does if no library is open. */
void playback_play_radio(const char *seed_track_uid);
/* As playback_play_radio(), for a station that starts from start_uid, or if that can't be played
 * or is NULL or "", from its seed (mc_player_play_station()). */
void playback_play_station(const char *seed_track_uid, const char *start_uid);
/* Shuffle on or off (see mc_player_set_shuffle()): it applies to what's queued, and to
 * whatever is queued next. Off to begin with; main.c keeps it in the settings. */
void playback_set_shuffle(BOOL on);
BOOL playback_shuffle(void);
/* Called when the media flyout's shuffle button asks for shuffle on or off, so main.c can
 * keep it in the settings; with none set, it's just playback_set_shuffle(). */
void playback_on_shuffle_requested(void (*requested)(BOOL on));
/* Pauses or resumes. Returns FALSE if nothing has been queued. */
BOOL playback_toggle(void);
void playback_next(void);
void playback_previous(void);
void playback_seek(int ms);
/* Moves the position by delta_ms, within the current track. */
void playback_seek_by(int delta_ms);

/* The volume, 0 to 100, and muted (see mc_player_set_volume()). 100 and not muted to begin
 * with; main.c keeps the volume in the settings. They carry over to the player when it's
 * created. */
void playback_set_volume(int volume);
int playback_volume(void);
void playback_set_muted(BOOL muted);
BOOL playback_muted(void);

BOOL playback_is_playing(void);

/* What can be added to the queue, which is the core's up next (mc_player_play_next()). Not
 * stations: nothing of radio goes in it. */
typedef enum playback_what {
  PLAYBACK_TRACK,
  PLAYBACK_ALBUM,
  PLAYBACK_ARTIST,
  PLAYBACK_PLAYLIST,
  PLAYBACK_SMART_PLAYLIST,
  PLAYBACK_GROUP,
  PLAYBACK_SMART_GROUP,
} playback_what;

/* Adds it to the front of the queue (Play Next) or the end (Play Last), and the now-playing strip
 * says so, or that it has nothing with a file on this device. Nothing starts playing. Anything but
 * a track goes in as one block, named name, unless playback_set_queue_tracks() says otherwise. */
void playback_add(playback_what what, const char *uid, const wchar_t *name, BOOL next);
/* Whether things go in the queue as a track each (mc_settings_queue_tracks()). Off to begin
 * with; settings_view.c keeps it. */
void playback_set_queue_tracks(BOOL on);
/* changed is called when the queue changes, whatever changed it. */
void playback_on_up_next_changed(void (*changed)(void));
int playback_up_next_count(void);
/* The queue as it is, NULL if there's no player yet. Free with mc_up_next_list_free(). */
mc_up_next_list *playback_up_next(void);
void playback_up_next_move(uint32_t id, int to);
void playback_up_next_remove(uint32_t id);
/* Empties the queue; playback goes on. */
void playback_up_next_clear(void);

/* The tracks the player has, by uid ("" for one without), e.g. to stop before their files go. */
int playback_queue_count(void);
const char *playback_queue_uid(int i);
/* Whether there's a current track, and one after it (always, shuffled). */
BOOL playback_has_current(void);
/* An edit may have changed what's queued: its titles and artists are read again, what's showing
 * the playing track shows them, and the player gets any file's new place. Nothing stops. */
void playback_tracks_changed(void);

/* The current track's uid, or "" with none or for one without. Valid until the queue changes. */
const char *playback_current_uid(void);
BOOL playback_has_next(void);

#endif
