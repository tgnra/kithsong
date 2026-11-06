/* Library stats, a dialog opened from the sidebar, with the numbers Windows' dialog and Android's
 * page have (mc_library_read_stats()), in cards under their headings:
 *
 *   +---------------- Library stats ---------------- x -+
 *   | On this computer          Whole library           |
 *   | +---------------------+   +---------------------+ |
 *   | | Tracks        4,120 |   | Tracks        6,204 | |
 *   | | ...                 |   | ...                 | |
 *   | +---------------------+   +---------------------+ |
 *   | Lists                     Listening               |
 *   | +---------------------+   +---------------------+ |
 *   | | Playlists         9 |   | Plays        12,093 | |
 *   | +---------------------+   +---------------------+ |
 *   +---------------------------------------------------+
 *
 * Whole library is left out while it would say the same as On this computer, Lists and Listening
 * then sharing the second column. Too narrow for two columns, they go one under another. A library
 * with no music says so instead.
 *
 * The numbers are read on a thread with its own library connection, and again whenever the library
 * changes while it's open, as a sync can. The dialog opens once they're in, or after a tenth of a
 * second at most, with Counting… in their place. Closing it doesn't wait for a read: what it finds
 * is dropped. */
#ifndef KITHSONG_STATS_DIALOG_H
#define KITHSONG_STATS_DIALOG_H

#include "app.h"

/* Opens the dialog over a's window, unless it's open already or there's no library. */
void stats_dialog_present(app *a);
/* The library changed: reads the numbers again, if the dialog is open. */
void stats_dialog_refresh(void);
/* Before the window or the library's files go: waits for a read still going, and an opening that
 * hasn't happened yet doesn't. */
void stats_dialog_stop(void);

#endif
