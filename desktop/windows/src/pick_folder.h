/* The shell's folder picker. Needs COM initialised on the calling thread. */
#ifndef MUSICAPP_PICK_FOLDER_H
#define MUSICAPP_PICK_FOLDER_H

#include <windows.h>

/* Returns the chosen folder as a malloc'd UTF-8 path, or NULL if cancelled. initial (UTF-8,
 * may be NULL) is the folder the picker opens in. */
char *pick_folder(HWND owner, const wchar_t *title, const char *initial);

#endif
