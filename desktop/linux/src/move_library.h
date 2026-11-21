/* Settings' Move library (the Linux counterpart of desktop/windows/src/move_library.c): picks
 * and checks a folder, then moves the library there (mc_library_move(), on a worker thread)
 * while a progress window stands in for the main window. That window refuses close-request
 * (Alt+F4) and logging out until the move has finished or been undone; Cancel works while it
 * copies. The host then reopens the library where it now is.
 *
 * Debug builds only: KITHSONG_TEST_MOVE_TO=<folder> replaces the picker (once: a folder that
 * won't do isn't asked for again), and KITHSONG_TEST_MOVE_DELAY_MS=<ms> slows each file so the
 * progress can be seen. The core's KITHSONG_TEST_MOVE_COPY and KITHSONG_TEST_MOVE_FREE_SPACE
 * (core/src/library/move_internal.h) make it copy, or run short of room, on one disk.
 */
#ifndef KITHSONG_MOVE_LIBRARY_H
#define KITHSONG_MOVE_LIBRARY_H

#include <adwaita.h>

typedef struct move_library_host {
  /* Stops everything that has the library open (playback, imports, the views) and closes
   * it. */
  void (*close_library)(void *user);
  /* Opens the library in dir, where it is after the move, and presents the main window. */
  void (*reopen_library)(void *user, const char *dir);
  void *user;
} move_library_host;

/* Asks for a folder over win (the main window), unless to names one, and moves the library from
 * the folder settings.json in settings_dir names to it, saving the new folder there. host must
 * outlive the move. Returns at once; the rest happens on the main loop. */
void move_library_start(GtkWindow *win, const char *settings_dir, const move_library_host *host, const char *to);

#endif
