/* Previous, Play/Pause and Next under the window's taskbar thumbnail, as media players have.
 *
 * Windows only takes the buttons once the taskbar button exists, which it announces with a
 * registered message, so the window procedure passes messages through taskbar_message().
 * Clicks come back as WM_COMMAND with THBN_CLICKED and one of the TASKBAR_* ids. The icons
 * are Segoe Fluent Icons glyphs, drawn in the taskbar's colour (it has its own light or
 * dark setting, apart from apps').
 */
#ifndef MUSICAPP_TASKBAR_H
#define MUSICAPP_TASKBAR_H

#include <windows.h>

enum { TASKBAR_PREVIOUS = 0x7100, TASKBAR_PLAY, TASKBAR_NEXT };

void taskbar_init(HWND wnd);
void taskbar_free(void);

/* Call for every message to the window. Returns TRUE if it was the taskbar's. */
BOOL taskbar_message(UINT msg, WPARAM wp, LPARAM lp);

/* has_track enables Play; playing shows Pause instead. */
void taskbar_set_state(BOOL has_track, BOOL playing, BOOL has_previous, BOOL has_next);

#endif
