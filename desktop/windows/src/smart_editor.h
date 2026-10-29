/* The smart playlist and smart album group editor (smartlist.h), a resizable modal dialog:
 *
 *   +- Edit smart playlist -------------------------------------------------------+
 *   | Name [ Jazz to catch up on        ]                                         |
 *   | Tracks matching [all v] of these:              | Preview                    |
 *   | | [Genre v]       [contains v]          [jazz]  → 412  x | Title    Artist  |
 *   | | Matching [any v] of these:                    → 40   x | So What  Miles.. |
 *   | | ┃ [Plays v]     [is less than v]      [3]            x | ...              |
 *   | | ┃ [Last played v] [isn't in the last v] [180] [days v] x |                 |
 *   | | ┃ + Condition                                          |                  |
 *   | + Condition  + Group                                     |                  |
 *   | Sort by [least played first v] x  then [...] x  + Sort   |                  |
 *   | (o) Sorted  ( ) Shuffled                                 |                  |
 *   | [x] Only the first [10] tracks                           |                  |
 *   | Tracks where genre contains “jazz” and (play count is ...) · 10 tracks      |
 *   | 40 tracks match · 10 in the list                          [Save] [Cancel]  |
 *   +-----------------------------------------------------------------------------+
 *
 * The conditions are standard controls in a scrolling panel; groups nest one level, each
 * level with a coloured rail down its left. A condition's value fits its field's type: text,
 * a number, a length (m:ss), a number of days, weeks or months or a date, Yes or No, or a
 * playlist, album group or file format from a list. The preview (beside the conditions when
 * the dialog is wide enough, else below them) and the counts follow the rule as it's edited,
 * a quarter of a second after the last change, worked out on a thread of their own with its
 * own library connection. For a rule matching all of its top-level conditions, each of those
 * shows how many match it and every one above it ("→ 40"). The sentence is
 * mc_smart_describe()'s.
 *
 * Save (Create, for a new list) is the default button and is greyed out while the name is
 * blank or a value is missing (the condition says so beneath it); Escape cancels. A rule this
 * editor can't show (nested deeper than one group, a "not" with no matching negative
 * operator, or one a newer version made) opens read-only, as its sentence (or its JSON), and
 * is never written back. */
#ifndef KITHSONG_SMART_EDITOR_H
#define KITHSONG_SMART_EDITOR_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* New Smart Playlist or New Smart Album Group: first a choice of ready-made rules (or a
 * blank one), then the editor with it filled in. TRUE, with the new list's uid in out_uid,
 * if one was created. */
BOOL smart_new_dialog(HWND owner, mc_library *lib, mc_smart_subject subject, char out_uid[MC_UID_STRLEN]);

/* Edit Rules for smart list uid, which may also rename it. TRUE if the library changed. */
BOOL smart_edit_dialog(HWND owner, mc_library *lib, const char *uid);

/* Smart list uid's rule as a sentence (mc_smart_describe()), malloc'd; NULL if it's gone or
 * its rule can't be read. Free with free(). */
wchar_t *smart_describe(mc_library *lib, const char *uid);

#endif
