#include "add_to_playlist.h"

#include <stdlib.h>
#include <wchar.h>

#include "playlist_dialogs.h"
#include "wstr.h"

/* A menu item's text for name: & doubled, so it isn't taken for an accelerator. */
static wchar_t *menu_text(const char *name) {
  if (!name || !*name) name = mc_str(MC_STR_UNTITLED);
  int n = MultiByteToWideChar(CP_UTF8, 0, name, -1, NULL, 0);
  wchar_t *w = n > 0 ? malloc((size_t)n * sizeof *w) : NULL;
  wchar_t *out = w ? malloc((size_t)n * 2 * sizeof *out) : NULL;
  if (out) {
    MultiByteToWideChar(CP_UTF8, 0, name, -1, w, n);
    wchar_t *o = out;
    for (const wchar_t *p = w; *p; p++) {
      if (*p == L'&') *o++ = L'&';
      *o++ = *p;
    }
    *o = L'\0';
  }
  free(w);
  return out;
}

HMENU add_to_playlist_menu(mc_library *lib, UINT first_id, UINT new_id, mc_playlist_list **lists) {
  *lists = NULL;
  HMENU menu = CreatePopupMenu();
  if (!menu) return NULL;
  if (lib && mc_library_list_playlists(lib, lists) == MC_OK && (*lists)->count) {
    for (int i = 0; i < (*lists)->count; i++) {
      wchar_t *text = menu_text((*lists)->items[i].name);
      if (text) AppendMenuW(menu, MF_STRING, first_id + (UINT)i, text);
      free(text);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  }
  AppendMenuW(menu, MF_STRING | (lib ? 0 : MF_GRAYED), new_id, mc_wstr_mnemonic(MC_STR_WINDOWS_NEW_PLAYLIST_MENU));
  return menu;
}

BOOL add_to_playlist(HWND owner, mc_library *lib, const char *track_uid, const char *playlist_uid) {
  if (!lib || !track_uid || !*track_uid) return FALSE;
  char uid[MC_UID_STRLEN];
  BOOL created = FALSE;
  if (!playlist_uid) {
    if (!playlist_create_dialog(owner, lib, uid)) return FALSE;
    playlist_uid = uid;
    created = TRUE;
  }
  if (mc_playlist_append(lib, playlist_uid, track_uid, NULL) != MC_OK) {
    MessageBoxW(owner, mc_wstr(MC_STR_PLAYLIST_ADD_FAILED), mc_wstr(MC_STR_ADD_TO_PLAYLIST), MB_OK | MB_ICONERROR);
    return created;
  }
  return TRUE;
}
