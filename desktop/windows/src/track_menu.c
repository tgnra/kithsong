#include "track_menu.h"

#include <stdio.h>

#include "add_to_playlist.h"
#include "playback.h"
#include "radio_menu.h"
#include "wstr.h"

enum {
  CMD_NEW_PLAYLIST = 1,
  CMD_PLAY_NEXT,
  CMD_PLAY_LAST,
  CMD_START_RADIO,
  CMD_MOVE_UP,
  CMD_MOVE_DOWN,
  CMD_REMOVE,
  CMD_GO_TO_ALBUM,
  CMD_GO_TO_ARTIST,
  CMD_EDIT,
  CMD_DELETE,
  CMD_PLAYLIST_FIRST = 100, /* Add to Playlist's playlists */
};

track_menu_pick track_menu_show(HWND owner, POINT pt, const track_menu_options *o, char playlist_uid[MC_UID_STRLEN]) {
  HMENU menu = CreatePopupMenu();
  mc_playlist_list *lists = NULL;
  HMENU add = add_to_playlist_menu(o->lib, CMD_PLAYLIST_FIRST, CMD_NEW_PLAYLIST, &lists);
  if (add) AppendMenuW(menu, MF_POPUP, (UINT_PTR)add, mc_wstr_mnemonic(MC_STR_ADD_TO_PLAYLIST));
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  /* A track with no file on this device can't be played from here. */
  mc_track_list *track = NULL;
  UINT queue = o->lib && mc_library_get_track(o->lib, o->track_uid, &track) == MC_OK && track->items[0].path ? 0 : MF_GRAYED;
  mc_track_list_free(track);
  AppendMenuW(menu, MF_STRING | queue, CMD_PLAY_NEXT, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_NEXT));
  AppendMenuW(menu, MF_STRING | queue, CMD_PLAY_LAST, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_LAST));
  char seed[MC_UID_STRLEN];
  radio_menu_append_track(menu, CMD_START_RADIO, o->lib, o->track_uid, seed);
  if (o->go_to_album || o->go_to_artist) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  if (o->go_to_album) AppendMenuW(menu, MF_STRING, CMD_GO_TO_ALBUM, mc_wstr_mnemonic(MC_STR_GO_TO_ALBUM));
  if (o->go_to_artist) AppendMenuW(menu, MF_STRING, CMD_GO_TO_ARTIST, mc_wstr_mnemonic(MC_STR_GO_TO_ARTIST));
  if (o->in_playlist) {
    wchar_t item[128];
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (o->can_move_up ? 0 : MF_GRAYED), CMD_MOVE_UP,
                wstr_menu_item(item, 128, MC_STR_MOVE_UP, MC_STR_WINDOWS_SHORTCUT_MOVE_UP));
    AppendMenuW(menu, MF_STRING | (o->can_move_down ? 0 : MF_GRAYED), CMD_MOVE_DOWN,
                wstr_menu_item(item, 128, MC_STR_MOVE_DOWN, MC_STR_WINDOWS_SHORTCUT_MOVE_DOWN));
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_REMOVE,
                wstr_menu_item(item, 128, MC_STR_REMOVE_FROM_PLAYLIST, MC_STR_WINDOWS_SHORTCUT_REMOVE));
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_EDIT, mc_wstr_mnemonic(MC_STR_EDIT_MENU));
  AppendMenuW(menu, MF_STRING, CMD_DELETE, mc_wstr_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU));
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
  int cmd = TrackPopupMenu(menu, flags, pt.x, pt.y, 0, owner, NULL);
  DestroyMenu(menu); /* and the submenu */

  track_menu_pick pick = TRACK_MENU_NOTHING;
  playlist_uid[0] = '\0';
  if (cmd >= CMD_PLAYLIST_FIRST && lists && cmd - CMD_PLAYLIST_FIRST < lists->count) {
    snprintf(playlist_uid, MC_UID_STRLEN, "%s", lists->items[cmd - CMD_PLAYLIST_FIRST].uid);
    pick = TRACK_MENU_ADD_TO_PLAYLIST;
  }
  mc_playlist_list_free(lists);
  switch (cmd) {
    case CMD_PLAY_NEXT:
    case CMD_PLAY_LAST:
      playback_add(PLAYBACK_TRACK, o->track_uid, NULL, cmd == CMD_PLAY_NEXT);
      return TRACK_MENU_NOTHING;
    case CMD_NEW_PLAYLIST: return TRACK_MENU_NEW_PLAYLIST;
    case CMD_START_RADIO: return TRACK_MENU_START_RADIO;
    case CMD_MOVE_UP: return TRACK_MENU_MOVE_UP;
    case CMD_MOVE_DOWN: return TRACK_MENU_MOVE_DOWN;
    case CMD_REMOVE: return TRACK_MENU_REMOVE;
    case CMD_GO_TO_ALBUM: return TRACK_MENU_GO_TO_ALBUM;
    case CMD_GO_TO_ARTIST: return TRACK_MENU_GO_TO_ARTIST;
    case CMD_EDIT: return TRACK_MENU_EDIT;
    case CMD_DELETE: return TRACK_MENU_DELETE;
  }
  return pick;
}

BOOL track_menu_album_has_page(mc_library *lib, const char *album_uid) {
  mc_track_list *list = NULL;
  if (!lib || !album_uid[0] || mc_library_list_album_tracks(lib, album_uid, &list) != MC_OK) return FALSE;
  BOOL more = list->count > 1;
  mc_track_list_free(list);
  return more;
}
