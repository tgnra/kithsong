/* Settings' Move Library: picks and checks a folder, then moves the library there
 * (mc_library_move(), on a worker thread) while a progress window stands in for the main
 * window:
 *
 *   +--------------------------------------------------+
 *   | Moving the library                               |
 *   +--------------------------------------------------+
 *   |  Moving the library to D:\Music                  |
 *   |  [=================                   ]           |
 *   |  120 of 2,400 files, 1.2 GB of 24 GB             |
 *   |  Radiohead\OK Computer (1997)\02 Paranoid...     |
 *   |  Please wait. The library opens again ...        |
 *   +--------------------------------------------------+
 *
 * A folder that won't do (not empty, inside the library, too little room...) is explained
 * and the picker opens again. Once the move starts the main window is hidden and the
 * progress window can't be closed: it has no close button, and Alt+F4 and signing out are
 * refused, until the move has finished or been undone. Then it goes, and the host reopens
 * the library where it now is and shows the main window, followed by a message if the move
 * failed or left files behind.
 *
 * Debug builds only: KITHSONG_TEST_MOVE_TO=<folder> in the environment takes the place of
 * the picker (once: a folder that won't do isn't asked for again), and
 * KITHSONG_TEST_MOVE_DELAY_MS=<ms> slows each file down, so the progress can be seen. The
 * core's KITHSONG_TEST_MOVE_COPY and KITHSONG_TEST_MOVE_FREE_SPACE (see
 * core/src/library/move_internal.h) make it copy, or run short of room, on one disk.
 */
#ifndef KITHSONG_MOVE_LIBRARY_H
#define KITHSONG_MOVE_LIBRARY_H

#include <windows.h>

#include "musiccore/musiccore.h"

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

/* Whether a move is under way. */
BOOL move_library_running(void);

#endif
