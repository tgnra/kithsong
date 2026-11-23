/* Playback in the Win32 frontend: the core's player, and keeping everything that shows it in
 * step: the now-playing strip, the track list's playing row, the window title, the taskbar
 * thumbnail buttons (taskbar.c) and Windows' media flyout (media_controls.c). Media keys arrive
 * through WM_APPCOMMAND while the window has the focus, otherwise through the media controls.
 *
 * Plays are counted in the library and scrobbled when the player says they count, and each
 * one, counted or not, goes in the library's play log when it ends. The queue is the core's up
 * next (queue_popup.c shows it) and goes with the library, as its tracks do.
 *
 * The player is created on first play. Everything here runs on the UI thread.
 */
#ifndef KITHSONG_PLAYBACK_H
#define KITHSONG_PLAYBACK_H

#include <windows.h>

#include "kithcore/kithcore.h"

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


/* Call first in the main window's procedure. Returns TRUE, with *result set, if msg was
 * playback's: media keys, the taskbar's buttons. */
BOOL playback_window_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

/* Windows is shutting down or logging off, and may end the process without another message:
 * logs the play under way now. */
void playback_end_session(void);
/* Stops playing and empties the queue, letting go of the file that was playing; for before
 * the library's files move. */
void playback_clear(void);
/* Replaces the list being played and plays tracks[start], or with start -1 the first track
 * (shuffled, any track), and empties the queue, as this is someone playing something new. context
 * and context_uid (or NULL) say what's played, for the play log. */
void playback_play(const playback_track *tracks, int count, int start, mc_play_context context,
                   const char *context_uid);
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


/* Adds it to the end of the queue, the core's up next (Queue; an album while an album plays in
 * order goes after that album, see mc_player_up_next_add()), and the now-playing strip says so, or
 * that it has nothing with a file on this device. Nothing starts playing. Anything but a track goes
 * in as one block, named name, unless playback_set_queue_tracks() says otherwise. */
void playback_add(mc_up_next_kind what, const char *uid, const wchar_t *name);
/* Whether things go in the queue as a track each (mc_settings_queue_tracks()). Off to begin
 * with; settings_view.c keeps it. */
void playback_set_queue_tracks(BOOL on);
/* changed is called when the queue changes, whatever changed it, or what's playing does. */
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
/* What's playing or paused, for the top of the queue: its title, artist (may be NULL) and length
 * (0 if unknown). FALSE when stopped or with nothing. Valid until the queue changes. */
BOOL playback_now_playing(const wchar_t **title, const wchar_t **artist, int *duration_ms);
/* The queue's row for the rest of the album (e->rest): its title, "Rest of" the album (free() it),
 * and how long those tracks are, into duration_ms. */
wchar_t *playback_rest_title(const mc_up_next_entry *e, int64_t *duration_ms);
BOOL playback_has_next(void);

/* --demo-playing (main.c), for screenshots: shows track uid of the library in the
 * now-playing strip, paused at position_ms, and marks it in the track list, as if it had been
 * played and paused there. The player isn't touched: nothing plays, and Play starts afresh. */
void playback_demo_show(const char *uid, int position_ms);

#endif
