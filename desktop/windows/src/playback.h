/* Playback in the Win32 frontend: the core's player, and keeping everything that shows it
 * in step. That's the now-playing strip, the track list's playing row, the window title
 * (for the taskbar and Alt+Tab), the buttons under the taskbar thumbnail (taskbar.c) and
 * Windows' media flyout (media_controls.c).
 *
 * Media keys work while the window has the focus through WM_APPCOMMAND, and otherwise
 * through the media controls, which Windows sends them to.
 *
 * Plays are counted in the library and scrobbled when the player says they count.
 *
 * The player is created on first play. Everything here runs on the UI thread.
 */
#ifndef MUSICAPP_PLAYBACK_H
#define MUSICAPP_PLAYBACK_H

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


/* Call first in the main window's procedure. Returns TRUE, with *result set, if msg was
 * playback's: media keys, the taskbar's buttons. */
BOOL playback_window_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

/* Stops playing and empties the queue, letting go of the file that was playing; for before
 * the library's files move. */
void playback_clear(void);
/* Replaces the queue and plays tracks[start], or with start -1 the first track (shuffled,
 * any track). */
void playback_play(const playback_track *tracks, int count, int start);
/* Shuffle on or off (see mc_player_set_shuffle()): it applies to what's queued, and to
 * whatever is queued next. Off to begin with; main.c keeps it in the settings. */
void playback_set_shuffle(BOOL on);
BOOL playback_shuffle(void);
/* Pauses or resumes. Returns FALSE if nothing has been queued. */
BOOL playback_toggle(void);
void playback_next(void);
void playback_previous(void);
void playback_seek(int ms);

BOOL playback_is_playing(void);
/* Whether there's a current track, and one after it (always, shuffled). */
BOOL playback_has_current(void);
BOOL playback_has_next(void);

#endif
