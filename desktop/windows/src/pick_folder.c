#define COBJMACROS
#include "pick_folder.h"

#include <shobjidl.h>
#include <stdlib.h>

/* The item's file system path, as a malloc'd UTF-8 string, or NULL. */
static char *item_path(IShellItem *item) {
  wchar_t *path = NULL;
  char *result = NULL;
  if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
    int n = WideCharToMultiByte(CP_UTF8, 0, path, -1, NULL, 0, NULL, NULL);
    if (n > 0 && (result = malloc((size_t)n))) WideCharToMultiByte(CP_UTF8, 0, path, -1, result, n, NULL, NULL);
    CoTaskMemFree(path);
  }
  return result;
}

char *pick_folder(HWND owner, const wchar_t *title, const char *initial) {
  IFileOpenDialog *dlg = NULL;
  char *result = NULL;
  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IFileOpenDialog, (void **)&dlg)))
    return NULL;
  DWORD opts = 0;
  IFileOpenDialog_GetOptions(dlg, &opts);
  IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
  IFileOpenDialog_SetTitle(dlg, title);

  wchar_t winitial[MAX_PATH * 2];
  if (initial && MultiByteToWideChar(CP_UTF8, 0, initial, -1, winitial, MAX_PATH * 2) > 0) {
    IShellItem *folder = NULL;
    if (SUCCEEDED(SHCreateItemFromParsingName(winitial, NULL, &IID_IShellItem, (void **)&folder))) {
      IFileOpenDialog_SetFolder(dlg, folder);
      IShellItem_Release(folder);
    }
  }

  if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner))) {
    IShellItem *item = NULL;
    if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
      result = item_path(item);
      IShellItem_Release(item);
    }
  }
  IFileOpenDialog_Release(dlg);
  return result;
}

char **pick_folders(HWND owner, const wchar_t *title, int *count) {
  IFileOpenDialog *dlg = NULL;
  char **result = NULL;
  *count = 0;
  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IFileOpenDialog, (void **)&dlg)))
    return NULL;
  DWORD opts = 0;
  IFileOpenDialog_GetOptions(dlg, &opts);
  IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_ALLOWMULTISELECT);
  IFileOpenDialog_SetTitle(dlg, title);

  IShellItemArray *items = NULL;
  DWORD n = 0;
  if (SUCCEEDED(IFileOpenDialog_Show(dlg, owner)) && SUCCEEDED(IFileOpenDialog_GetResults(dlg, &items)) &&
      SUCCEEDED(IShellItemArray_GetCount(items, &n)) && n > 0 && (result = calloc(n, sizeof *result))) {
    for (DWORD i = 0; i < n; i++) {
      IShellItem *item = NULL;
      if (FAILED(IShellItemArray_GetItemAt(items, i, &item))) continue;
      char *path = item_path(item);
      if (path) result[(*count)++] = path;
      IShellItem_Release(item);
    }
    if (!*count) {
      free(result);
      result = NULL;
    }
  }
  if (items) IShellItemArray_Release(items);
  IFileOpenDialog_Release(dlg);
  return result;
}
