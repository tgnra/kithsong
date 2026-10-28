/* Windows' media controls (SystemMediaTransportControls): the media flyout that comes up
 * with the volume keys and in Quick Settings, the lock screen, and Bluetooth headsets. It's
 * also how the keyboard's media keys reach the app while another window has the focus.
 *
 * Shows the playing track (title, artist, album, cover) and a timeline that can be seeked.
 * Stays hidden until something has played. Call everything on the UI thread.
 */
#ifndef KITHSONG_MEDIA_CONTROLS_H
#define KITHSONG_MEDIA_CONTROLS_H

#include <stdint.h>
#include <windows.h>

#include "musiccore/musiccore.h"

typedef enum media_button {
  MEDIA_PLAY,
  MEDIA_PAUSE,
  MEDIA_STOP,
  MEDIA_NEXT,
  MEDIA_PREVIOUS,
  MEDIA_SEEK, /* to the position in ms in lParam */
} media_button;

/* Attaches the controls to wnd's taskbar button. Presses arrive on another thread, so
 * they're posted to notify as msg, with the media_button in wParam. Returns FALSE where
 * Windows doesn't have them. */
BOOL media_controls_init(HWND wnd, HWND notify, UINT msg);
void media_controls_free(void);

/* A NULL title shows nothing playing. cover_dir is the album folder, or NULL. */
void media_controls_set_track(const wchar_t *title, const wchar_t *artist, const wchar_t *album,
                              const wchar_t *cover_dir);
void media_controls_set_state(mc_player_state state, BOOL has_previous, BOOL has_next);
/* Windows moves the position on by itself while playing, so call this on changes: a new
 * track, play or pause, a seek. */
void media_controls_set_timeline(int64_t position_ms, int64_t duration_ms);

#endif
