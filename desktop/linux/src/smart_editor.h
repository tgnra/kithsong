/* The smart playlist and smart album group editor (smartlist.h): a dialog that shows a rule
 * one group at a time, each group a page of its own, with a path at the top back to the list.
 * The list's page has its name, how its conditions combine, its order and limit and a preview;
 * a group's page has the group's name and a preview of what it alone matches. Groups nest
 * without limit; the core leaves an empty group out of the rule.
 *
 * Every row shows how many tracks or albums it matches on its own, and the bar at the bottom
 * how many the whole rule does. They are recounted 250 ms after the last change on a thread
 * with its own library connection. Save is insensitive while any value is missing or wrong
 * (listed in a card at the bottom) or the name is blank.
 *
 * A "not" around a condition shows as its negative operator, or if it has none as a group of
 * that one condition matching none of it. A rule the editor can't show (from a newer version)
 * opens read-only and is never written back. Group names are in docs/schema.md (Smart lists). */
#ifndef KITHSONG_SMART_EDITOR_H
#define KITHSONG_SMART_EDITOR_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* New Smart Playlist… or New Smart Album Group…, over parent (any widget in the main window).
 * Calls done with the new list's uid once it's made. */
void smart_editor_new(GtkWidget *parent, mc_library *lib, mc_smart_subject subject,
                      void (*done)(void *user, const char *uid), void *user);

/* Edit Rules… for the smart list uid, which can rename it too. Calls done with uid if the
 * library changed. */
void smart_editor_edit(GtkWidget *parent, mc_library *lib, const char *uid,
                       void (*done)(void *user, const char *uid), void *user);

#endif
