/* Library stats, a modal dialog opened from the sidebar, with the numbers Android's Library
 * stats page has (mc_library_read_stats()). Whole library is left out while it would say the
 * same as On this PC.
 *
 * The numbers are read on a thread with its own library connection, and again whenever the
 * library changes while it's open, as a sync can. The dialog opens once they're in, or after
 * 100 ms at most showing Counting…, and resizes to fit when they come.
 *
 * Mica shows through the whole dialog under cards drawn as Windows 11's are, so all of it is
 * drawn with canvas.c (GDI would lose the alpha). Without Mica (high contrast, Windows 10) it's
 * the frame colour with solid cards. Screen readers see each card as a group (uia.h). */
#ifndef KITHSONG_STATS_DIALOG_H
#define KITHSONG_STATS_DIALOG_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* Runs the dialog. lib is only read here, for its folder. */
void stats_dialog_show(HWND owner, mc_library *lib);
/* The library changed: reads the numbers again, if the dialog is open. */
void stats_dialog_refresh(void);

#endif
