/* Library stats, a modal dialog opened from the sidebar, with the numbers Android's Library stats
 * page has (mc_library_read_stats()):
 *
 *   +- Library stats ------------------------------------+
 *   | On this PC               Whole library             |
 *   | +----------------------+ +----------------------+  |
 *   | | Tracks         4,120 | | Tracks         6,204 |  |
 *   | | Albums           311 | | Albums           480 |  |
 *   | | ...                  | | ...                  |  |
 *   | +----------------------+ +----------------------+  |
 *   | Lists                    Listening                 |
 *   | +----------------------+ +----------------------+  |
 *   | | Playlists          9 | | Plays         12,093 |  |
 *   | | Album groups       5 | | ...                  |  |
 *   | +----------------------+ +----------------------+  |
 *   |                                          [Close]   |
 *   +----------------------------------------------------+
 *
 * Whole library is left out while it would say the same as On this PC, Lists and Listening
 * then sharing the second column; a library with no music says so instead.
 *
 * The numbers are read on a thread with its own library connection, and again whenever the
 * library changes while it's open, as a sync can. The dialog opens once they're in, or after a
 * tenth of a second at most, with Counting… in their place, and takes their size when they come.
 *
 * Mica shows through the whole dialog, under cards drawn as Windows 11's are; all of it is drawn
 * with canvas.c, as GDI would lose the alpha. Without Mica (high contrast, Windows 10) it's the
 * frame colour, with solid cards. Screen readers see each card as a group of its numbers (uia.h). */
#ifndef KITHSONG_STATS_DIALOG_H
#define KITHSONG_STATS_DIALOG_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* Runs the dialog. lib is only read here, for its folder. */
void stats_dialog_show(HWND owner, mc_library *lib);
/* The library changed: reads the numbers again, if the dialog is open. */
void stats_dialog_refresh(void);

#endif
