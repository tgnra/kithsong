/* The smart playlist and smart album group editor (smartlist.h): a resizable modal dialog that
 * shows a rule one group at a time, each group a page of its own, with a path at the top back
 * to the list. The list's page has its name, how its conditions combine, and its order and
 * limit; a group's page has the group's name instead of the order. Beside the conditions (or
 * below them in a narrow window) is a preview of what the list, or the group alone, matches.
 * The core leaves an empty group out of the rule.
 *
 * Every row shows how many tracks or albums it matches on its own, and the bottom line how
 * many the whole rule does. They are recounted 250 ms after the last change on a thread with
 * its own library connection. Save is greyed out while any value is missing or wrong (listed
 * at the bottom) or the name is blank.
 *
 * A rule the editor can't show (from a newer version, or a "not" with no negative operator to
 * show it as) opens read-only and is never written back. Group names are in docs/schema.md
 * (Smart lists).
 *
 * As in the main window, Mica shows on the frame (title bar, path, margins and the bottom line)
 * and drawing there goes through canvas.c; the page, preview and problems are a solid card, so
 * their standard controls need no alpha. Without Mica the frame is the frame colour. */
#ifndef KITHSONG_SMART_EDITOR_H
#define KITHSONG_SMART_EDITOR_H

#include <windows.h>

#include "kithcore/kithcore.h"

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
