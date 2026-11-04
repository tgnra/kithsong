/* The shell's folder and file pickers. Need COM initialised on the calling thread. */
#ifndef KITHSONG_PICK_FOLDER_H
#define KITHSONG_PICK_FOLDER_H

#include <windows.h>

/* Returns the chosen folder as a malloc'd UTF-8 path, or NULL if cancelled. initial (UTF-8,
 * may be NULL) is the folder the picker opens in. */
char *pick_folder(HWND owner, const wchar_t *title, const char *initial);

/* Like pick_folder, but several folders may be picked. Returns a malloc'd array of *count
 * malloc'd UTF-8 paths (free each, then the array), or NULL if cancelled. */
char **pick_folders(HWND owner, const wchar_t *title, int *count);

/* The picker for one file, showing those matching pattern ("*.jpg;*.png"), which types names.
 * Returns the file's path, malloc'd, or NULL if cancelled. */
wchar_t *pick_file(HWND owner, const wchar_t *title, const wchar_t *types, const wchar_t *pattern);

/* For a library folder picked: if dir (UTF-8) is inside a OneDrive folder, warns that sync
 * can damage the database and asks whether to use it anyway. Returns whether to go ahead. */
BOOL confirm_onedrive(HWND owner, const char *dir);

#endif
