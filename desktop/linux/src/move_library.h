/* Settings' Move library (the Linux counterpart of desktop/windows/src/move_library.c):
 * picks and checks a folder, then moves the library there (mc_library_move(), on a worker
 * thread) while a progress window stands in for the main window:
 *
 *   +--------------------------------------------------+
 *   |                Moving the library                |
 *   |  Moving the library to /media/music/Library      |
 *   |  [=================                   ]           |
 *   |  120 of 2,400 files, 1.2 GB of 24 GB             |
 *   |  Radiohead/OK Computer (1997)/02 Paranoid...     |
 *   |  Please wait. The library opens again ...        |
 *   +--------------------------------------------------+
 *
 * A folder that won't do (not empty, inside the library, too little room...) is explained
 * and the picker opens again. Once the move starts the main window is hidden, and the
 * progress window has no close button and refuses close-request (Alt+F4) and logging out
 * until the move has finished or been undone. Then it goes, and the host reopens the
 * library where it now is and shows the main window, followed by a message if the move
 * failed or left files behind.
 *
 * Debug builds only: MUSICAPP_TEST_MOVE_TO=<folder> in the environment takes the place of
 * the picker (once: a folder that won't do isn't asked for again), and
 * MUSICAPP_TEST_MOVE_DELAY_MS=<ms> slows each file down, so the progress can be seen. The
 * core's MUSICAPP_TEST_MOVE_COPY and MUSICAPP_TEST_MOVE_FREE_SPACE (see
 * core/src/library/move_internal.h) make it copy, or run short of room, on one disk.
 */
#ifndef MUSICAPP_MOVE_LIBRARY_H
#define MUSICAPP_MOVE_LIBRARY_H

#include <adwaita.h>

typedef struct move_library_host {
  /* Stops everything that has the library open (playback, imports, the views) and closes
   * it. */
  void (*close_library)(void *user);
  /* Opens the library in dir, where it is after the move, and presents the main window. */
  void (*reopen_library)(void *user, const char *dir);
  void *user;
} move_library_host;

/* Asks for a folder over win (the main window) and moves the library from the folder
 * settings.json in settings_dir names to it, saving the new folder there. host must outlive
 * the move. Returns at once; the rest happens on the main loop. */
void move_library_start(GtkWindow *win, const char *settings_dir, const move_library_host *host);

#endif
