/* The smart playlist and smart album group editor (smartlist.h), a dialog over the main window
 * that shows a rule one group at a time, each group a page of its own:
 *
 *   +- [Cancel]          Edit smart playlist           [Save] -+
 *   | Jazz to catch up on  ›  Fresh stuff  ›  Old favourites    |  <- the path, once a group is open
 *   |-----------------------------------------------------------|
 *   | Name                                                      |
 *   | [ Jazz to catch up on                                  ]  |
 *   | [All of the following v]                                  |
 *   | +-------------------------------------------------------+ |
 *   | | [Genre v] [contains v]       [jazz          ]  412  x | |
 *   | | Fresh stuff                                  40  x  > | |  <- a group: opens its page
 *   | |   Tracks where date added is in the last 30 days or.. | |
 *   | +-------------------------------------------------------+ |
 *   | [+ Add condition]  [+ Add group]                          |
 *   | Order                                                     |
 *   | | Sort by           [least played first v]           x  | |
 *   | [+ Add sort order]                                        |
 *   | | Shuffled                                       [   ]  | |
 *   | | [x] Only the first                         [10] tracks| |
 *   | Preview                                                   |
 *   | | So What                                               | |
 *   | |   Miles Davis                                         | |
 *   |-----------------------------------------------------------|
 *   | +- Fix this to save ------------------------------------+ |
 *   | | Fresh stuff › Artist: needs a value           [Show]  | |
 *   | +-------------------------------------------------------+ |
 *   | 40 tracks match · 10 in the list                          |
 *   +-----------------------------------------------------------+
 *
 * The first page is the list's own: its name, then how its conditions combine (all, any or
 * none of them), one row each, then its order and limit and a preview of what it holds. A
 * group's row shows its name, with what it matches as a sentence (mc_smart_describe()) beneath;
 * activating it slides in the group's page, the same but for the group's name (an entry whose
 * placeholder is that sentence) and, in place of the order, a preview of what the group alone
 * matches. Groups nest without limit. An empty group is left out of the rule by the core, and
 * its row and page say so.
 *
 * A condition's value fits its field's type: text, a whole number, a length (m:ss), a date or a
 * number of days, weeks or months back, Yes or No, or a playlist, album group or format from a
 * list. Every row shows how many tracks or albums it matches on its own, and the bar at the
 * bottom how many the whole rule does; they follow the rule as it's edited, a quarter of a
 * second after the last change, worked out on a thread with its own library connection.
 *
 * The path at the top names the list and the groups opened, a click on one going back to it;
 * Escape, Alt+Left or the mouse's back button go back one group, and on the list's page Escape
 * cancels. A value missing or wrong anywhere is listed in a card at the bottom, with Show to open
 * its group and focus it; its groups' rows and the path mark it too. Save is insensitive while
 * there's any, or while the name is blank.
 *
 * A new list starts with a choice of ready-made rules, or a blank one. A rule the editor can't
 * show (from a newer version, or a "not" with no negative operator to show it as) opens
 * read-only, as its sentence, and is never written back. Group names are in
 * docs/schema.md (Smart lists). */
#ifndef KITHSONG_SMART_EDITOR_H
#define KITHSONG_SMART_EDITOR_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* New Smart Playlist… or New Smart Album Group…, over parent (any widget in the main window).
 * Calls done with the new list's uid once it's made. */
void smart_editor_new(GtkWidget *parent, mc_library *lib, mc_smart_subject subject,
                      void (*done)(void *user, const char *uid), void *user);

/* Edit Rules… for the smart list uid, which can rename it too. Calls done with uid if the
 * library changed. */
void smart_editor_edit(GtkWidget *parent, mc_library *lib, const char *uid,
                       void (*done)(void *user, const char *uid), void *user);

#endif
