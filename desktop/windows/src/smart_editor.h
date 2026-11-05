/* The smart playlist and smart album group editor (smartlist.h), a resizable modal dialog
 * that shows a rule one group at a time, each group a page of its own:
 *
 *   +- Edit smart playlist ------------------------------------------------------+
 *   | [<] Jazz to catch up on › Fresh stuff › Old favourites                     |  <- the path, once a group is open
 *   | Name: [ Jazz to catch up on          ]          | Preview                  |
 *   | [All of the following v]                        | Title       Artist       |
 *   | [Genre v] [contains v] [jazz        ]  412  x   | So What     Miles Davis  |
 *   | +---------------------------------------+       | ...                      |
 *   | | Fresh stuff                    40 ! > |  x    |                          |  <- a group: opens its page
 *   | | Tracks where date added is in the l.. |       |                          |
 *   | +---------------------------------------+       |                          |
 *   | [Add condition] [Add group]                     |                          |
 *   | Order                                           |                          |
 *   | Sort by [least played first v]  x               |                          |
 *   | [Add sort order]                                |                          |
 *   | [ ] Shuffled                                    |                          |
 *   | [x] Only the first [10] [tracks v]              |                          |
 *   | +- Fix this to save --------------------------------------------------+   |
 *   | | Fresh stuff › Artist: needs a value                         [Show]  |   |
 *   | +---------------------------------------------------------------------+   |
 *   | 40 tracks match · 10 in the list                        [Save] [Cancel]   |
 *   +----------------------------------------------------------------------------+
 *
 * The first page is the list's own: its name, then how its conditions combine (all, any or
 * none of them), one row each, then its order and limit. A group's row is a button showing
 * its name, with what it matches as a sentence (mc_smart_describe()) beneath; pressing it
 * opens the group's page, the same but for the group's name (a box whose cue is that
 * sentence) and without the order. Groups nest as deep as the rule format allows. An empty
 * group is left out of the rule by the core, and its row and page say so. Beside the
 * conditions (or below them, in a narrow window) is a preview of what the list holds, or on
 * a group's page what the group alone matches.
 *
 * A condition's value fits its field's type: text, a whole number, a length (m:ss), a date
 * or a number of days, weeks or months back, Yes or No, or a playlist, album group or format
 * from a list. Every row shows how many tracks or albums it matches on its own, and the line
 * at the bottom how many the whole rule does; they follow the rule as it's edited, a quarter
 * of a second after the last change, worked out on a thread with its own library connection.
 *
 * The path at the top names the list and the groups opened, a click on one going back to it;
 * Escape, Alt+Left, the Back button and the mouse's back button go back one group, and on
 * the list's page Escape cancels. A value missing or wrong anywhere is listed in a box at
 * the bottom, with Show to open its group and focus it; its groups' rows and the path mark
 * it too. Save is greyed out while there's any, or while the name is blank.
 *
 * A new list starts with a choice of ready-made rules, or a blank one. A rule the editor
 * can't show (from a newer version, or a "not" with no negative operator to show it as)
 * opens read-only, as its sentence, and is never written back. Group names are in
 * docs/schema.md (Smart lists).
 *
 * As in the main window, Mica shows on the frame (the title bar, the path, the margins, and
 * the line at the bottom with Save and Cancel) and the rest is a solid card: the page, the
 * preview and the problems, whose standard controls need no alpha. What's on the frame is
 * drawn here with canvas.c. Without Mica the frame is the frame colour. */
#ifndef KITHSONG_SMART_EDITOR_H
#define KITHSONG_SMART_EDITOR_H

#include <windows.h>

#include "musiccore/musiccore.h"

/* New Smart Playlist or New Smart Album Group: the ready-made rules (or a blank one), then
 * the editor with it filled in. TRUE, with the new list's uid in out_uid, if one was
 * created. */
BOOL smart_new_dialog(HWND owner, mc_library *lib, mc_smart_subject subject, char out_uid[MC_UID_STRLEN]);

/* Edit Rules for smart list uid, which may also rename it. TRUE if the library changed. */
BOOL smart_edit_dialog(HWND owner, mc_library *lib, const char *uid);

/* Smart list uid's rule as a sentence (mc_smart_describe()), malloc'd; NULL if it's gone or
 * its rule can't be read. Free with free(). */
wchar_t *smart_describe(mc_library *lib, const char *uid);

#endif
