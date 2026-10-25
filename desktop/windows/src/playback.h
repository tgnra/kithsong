/* Playback in the Win32 frontend: the core's player, and keeping everything that shows it
 * in step. That's the now-playing strip, the track list's playing row, the window title
 * (for the taskbar and Alt+Tab), the buttons under the taskbar thumbnail (taskbar.c) and
 * Windows' media flyout (media_controls.c).
 *
 * Media keys work while the window has the focus through WM_APPCOMMAND, and otherwise
 * through the media controls, which Windows sends them to.
 *
 * The player is created on first play. Everything here runs on the UI thread.
 */
#ifndef MUSICAPP_PLAYBACK_H
#define MUSICAPP_PLAYBACK_H

#include <windows.h>

/* A queue item. Strings are copied. */
typedef struct playback_track {
  const char *uid;
  const char *path; /* UTF-8 */
  const wchar_t *title, *artist, *album; /* artist and album may be NULL */
  const wchar_t *cover_dir;              /* the album folder, or NULL if it has no cover */
  int duration_ms;                       /* from the library; 0 if unknown */
} playback_track;

/* wnd is the main window. changed is called whenever playing starts or stops, so it can
 * redraw the Play/Pause button. */
void playback_init(HWND wnd, void (*changed)(void));
void playback_free(void);

/* Call first in the main window's procedure. Returns TRUE, with *result set, if msg was
 * playback's: media keys, the taskbar's buttons. */
BOOL playback_window_message(UINT msg, WPARAM wp, LPARAM lp, LRESULT *result);

/* Replaces the queue and plays tracks[start]. */
void playback_play(const playback_track *tracks, int count, int start);
/* Pauses or resumes. Returns FALSE if nothing has been queued. */
BOOL playback_toggle(void);
void playback_next(void);
void playback_previous(void);
void playback_seek(int ms);

BOOL playback_is_playing(void);

#endif
