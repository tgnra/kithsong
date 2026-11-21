/* Settings' Move Library: picks and checks a folder, then moves the library there
 * (mc_library_move(), on a worker thread) while a progress window stands in for the main
 * window. That window can't be closed (no close button; Alt+F4 and signing out are refused)
 * until the move has finished or been undone; Cancel works while it copies. The host then
 * reopens the library where it now is.
 *
 * Debug builds only: KITHSONG_TEST_MOVE_TO=<folder> replaces the picker (once: a folder that
 * won't do isn't asked for again), and KITHSONG_TEST_MOVE_DELAY_MS=<ms> slows each file so the
 * progress can be seen. The core's KITHSONG_TEST_MOVE_COPY and KITHSONG_TEST_MOVE_FREE_SPACE
 * (core/src/library/move_internal.h) make it copy, or run short of room, on one disk.
 */
#ifndef KITHSONG_MOVE_LIBRARY_H
#define KITHSONG_MOVE_LIBRARY_H

#include <windows.h>

#include "kithcore/kithcore.h"

typedef struct move_library_host {
  /* Stops everything that has the library open (playback, sync, imports, the views) and
   * closes it. */
  void (*close_library)(void);
  /* Opens the library in dir (UTF-8), where it is after the move, and shows the main
   * window again. */
  void (*reopen_library)(const char *dir);
} move_library_host;

/* Asks for a folder and moves the library from settings' library folder to it, saving the
 * new folder in settings. main is the main window; host must outlive the move. Returns at
 * once if the user cancels the picker; otherwise the move goes on after it returns. */
void move_library_start(HWND main, mc_settings *settings, const move_library_host *host);

BOOL move_library_running(void);

#endif
