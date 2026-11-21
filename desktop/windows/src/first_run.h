/* The first run: before there's a library, a window over the main one asks where it should
 * live. It shows the wordmark, what the library folder is for, a card for the suggested
 * folder (Kithsong Library in the user's Music folder), a card for a folder the user chose
 * once there is one, Choose a Folder..., and Start, which stays greyed out until a card is
 * selected. The main window can't be used meanwhile.
 *
 * The window has Mica and no title bar; its close button is drawn here, and closing it quits.
 * Everything is drawn into one canvas, with the cards and buttons described to UI Automation
 * (uia.h). */
#ifndef KITHSONG_FIRST_RUN_H
#define KITHSONG_FIRST_RUN_H

#include <windows.h>

/* Runs the first run over owner (which is disabled meanwhile) and returns the library folder
 * chosen as a malloc'd UTF-8 path, or NULL if the user closed it. The folder is empty,
 * doesn't exist yet, or already holds a library. */
char *first_run_choose(HWND owner);

/* Whether KITHSONG_TEST_MUSIC_DIR is set in a debug build: a test of the first run, which
 * should come up even from a checkout, where it otherwise doesn't. */
BOOL first_run_test_music(void);

#endif
