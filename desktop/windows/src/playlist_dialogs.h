/* Creating, renaming and deleting a playlist, album group or smart list: modal dialogs in the
 * style of the import's review dialog (themed standard controls, dark title bar in dark mode).
 * A new smart list comes from the smart list editor (smart_editor.h) instead.
 *
 * New made for a track with no file on this PC says the playlist won't show on the Playlists
 * page until one of its tracks is here, as it'd be hidden at once. Create and Rename stay
 * disabled while the name is blank or, renaming, unchanged; Delete's default is Cancel. A
 * failure is reported in a message box of its own.
 */
#ifndef KITHSONG_PLAYLIST_DIALOGS_H
#define KITHSONG_PLAYLIST_DIALOGS_H

#include <windows.h>

#include "kithcore/kithcore.h"

/* Each returns TRUE if the library changed. owner is the main window. playable is FALSE
 * when the new playlist is for a track with no file here, which the dialog then says. */
BOOL playlist_create_dialog(HWND owner, mc_library *lib, BOOL playable, char out_uid[MC_UID_STRLEN]);
BOOL playlist_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);
BOOL playlist_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);

BOOL album_group_create_dialog(HWND owner, mc_library *lib, char out_uid[MC_UID_STRLEN]);
BOOL album_group_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);
BOOL album_group_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name);

/* subject says whether uid is a smart playlist or a smart album group. */
BOOL smart_rename_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name, mc_smart_subject subject);
BOOL smart_delete_dialog(HWND owner, mc_library *lib, const char *uid, const wchar_t *name, mc_smart_subject subject);

/* The Delete dialog for anything else (delete_dialog.c): "Delete "name"?" with note, which
 * may run to three lines, beneath, titled caption. TRUE if the user chose Delete. */
BOOL confirm_delete_dialog(HWND owner, const wchar_t *caption, const wchar_t *name, const wchar_t *note);
/* The same with room for a list in note, and heading and button as they are. Cancel is the
 * default. */
BOOL confirm_lost_dialog(HWND owner, const wchar_t *caption, const wchar_t *heading, const wchar_t *note,
                         const wchar_t *button);
/* The same, taller, for Remove from this PC (remove_here.c): the heading is title_id (an
 * mc_str_id formatting name), and the confirming button says button. */
BOOL confirm_remove_dialog(HWND owner, const wchar_t *caption, int title_id, const wchar_t *name,
                           const wchar_t *note, const wchar_t *button);

#endif
