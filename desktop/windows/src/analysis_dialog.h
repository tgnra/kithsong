/* The dialog for a large batch of audio analysis: what it's doing, a progress bar, and a choice
 * of Background (carry on paced, with a row in the sidebar) or Cancel. Modeless, and it doesn't
 * take the focus when it opens: it comes up by itself, while someone may be typing.
 *
 *   +--------------------------------------------+
 *   | Analysing your music                    [x]|
 *   | Kithsong is listening to your music, to... |
 *   | [##########---------------------]          |
 *   | 120 of 796 tracks                          |
 *   |                     [Background] [Cancel]  |
 *   +--------------------------------------------+
 *
 * Closing it or pressing Esc is Background: the work isn't lost, and Cancel is there to stop it. */
#ifndef KITHSONG_ANALYSIS_DIALOG_H
#define KITHSONG_ANALYSIS_DIALOG_H

#include <windows.h>

typedef struct analysis_dialog_host {
  void (*background)(void);
  void (*cancel)(void);
} analysis_dialog_host;

/* Opens it, if it isn't open. */
void analysis_dialog_show(HWND owner, const analysis_dialog_host *host);
BOOL analysis_dialog_is_open(void);
void analysis_dialog_update(int done, int total);
void analysis_dialog_close(void);
/* For the message loop, so Tab and Esc work: TRUE if it took msg. */
BOOL analysis_dialog_translate(MSG *msg);

#endif
