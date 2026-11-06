#include "item_menu.h"

#include <stdio.h>
#include <stdlib.h>

#include "add_to_playlist.h"
#include "delete_dialog.h"
#include "edit_dialog.h"
#include "playback.h"
#include "playlist_dialogs.h"
#include "prompt_dialog.h"
#include "radio_menu.h"
#include "remove_here.h"
#include "smart_editor.h"
#include "wstr.h"

enum {
  CMD_NEW_GROUP = 1,
  CMD_REMOVE_FROM_GROUP,
  CMD_MOVE_UP, /* within a group */
  CMD_MOVE_DOWN,
  CMD_RENAME,
  CMD_DELETE,
  CMD_EDIT,
  CMD_EDIT_RULES,
  CMD_REMOVE_HERE, /* Remove from this PC... */
  CMD_SYNC_HERE,   /* Sync to this PC */
  CMD_START_RADIO,
  CMD_PLAY,
  CMD_PLAY_NEXT,
  CMD_PLAY_LAST,
  CMD_REMOVE_RECENT,
  CMD_CLEAR_RECENTS,
  CMD_GROUP_FIRST = 100, /* Add to Album Group's groups, in order */
};

/* What the menu's for, copied: its message loop can free what's shown. */
typedef struct held {
  char uid[MC_UID_STRLEN];
  wchar_t *name;
} held;

static held hold(const item_menu_options *o) {
  held h;
  snprintf(h.uid, sizeof h.uid, "%s", o->uid);
  h.name = _wcsdup(o->name ? o->name : L"");
  return h;
}

/* Finishes the menu, shows it and destroys it (and any submenu). The command picked, or 0. */
static int show(HMENU menu, const item_menu_options *o) {
  if (o->recent) {
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, CMD_REMOVE_RECENT, mc_wstr_mnemonic(MC_STR_RECENT_REMOVE_MENU));
    AppendMenuW(menu, MF_STRING, CMD_CLEAR_RECENTS, mc_wstr_mnemonic(MC_STR_RECENTS_CLEAR_MENU));
  }
  UINT flags = TPM_RETURNCMD | TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : 0);
  int cmd = TrackPopupMenu(menu, flags, o->pt.x, o->pt.y, 0, o->wnd, NULL);
  DestroyMenu(menu);
  return cmd;
}

static item_menu_result result(int cmd, BOOL changed) {
  return cmd == CMD_REMOVE_RECENT   ? ITEM_MENU_REMOVE_RECENT
         : cmd == CMD_CLEAR_RECENTS ? ITEM_MENU_CLEAR_RECENTS
         : changed                  ? ITEM_MENU_CHANGED
                                    : ITEM_MENU_NOTHING;
}

/* Play Next and Play Last, first on everything but a station. */
static void append_queue(HMENU menu) {
  AppendMenuW(menu, MF_STRING, CMD_PLAY_NEXT, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_NEXT));
  AppendMenuW(menu, MF_STRING, CMD_PLAY_LAST, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_LAST));
}

/* If that's what was picked, adds it to the queue. */
static void queue_pick(int cmd, playback_what what, const held *h) {
  if (cmd == CMD_PLAY_NEXT || cmd == CMD_PLAY_LAST) playback_add(what, h->uid, h->name, cmd == CMD_PLAY_NEXT);
}

item_menu_result item_menu_album(const item_menu_options *o) {
  held h = hold(o);
  mc_library *lib = o->lib;
  HWND owner = GetAncestor(o->wnd, GA_ROOT);
  char group[MC_UID_STRLEN], up_after[MC_UID_STRLEN], down_after[MC_UID_STRLEN];
  snprintf(group, sizeof group, "%s", o->group ? o->group : "");
  snprintf(up_after, sizeof up_after, "%s", o->up_after ? o->up_after : "");
  snprintf(down_after, sizeof down_after, "%s", o->down_after ? o->down_after : "");

  HMENU menu = CreatePopupMenu();
  char seed[MC_UID_STRLEN];
  append_queue(menu);
  radio_menu_append_album(menu, CMD_START_RADIO, lib, h.uid, seed);
  mc_album_group_list *groups = NULL;
  HMENU add = add_to_album_group_menu(lib, CMD_GROUP_FIRST, CMD_NEW_GROUP, &groups);
  if (add) AppendMenuW(menu, MF_POPUP, (UINT_PTR)add, mc_wstr_mnemonic(MC_STR_ADD_TO_ALBUM_GROUP));
  if (group[0]) {
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING | (o->first ? MF_GRAYED : 0), CMD_MOVE_UP, mc_wstr_mnemonic(MC_STR_MOVE_UP));
    AppendMenuW(menu, MF_STRING | (o->last ? MF_GRAYED : 0), CMD_MOVE_DOWN, mc_wstr_mnemonic(MC_STR_MOVE_DOWN));
    AppendMenuW(menu, MF_STRING, CMD_REMOVE_FROM_GROUP, mc_wstr_mnemonic(MC_STR_REMOVE_FROM_ALBUM_GROUP));
  }
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_EDIT, mc_wstr_mnemonic(MC_STR_EDIT_MENU));
  remove_here_append_menu(menu, MC_PROFILE_ALBUM, h.uid, CMD_REMOVE_HERE, CMD_SYNC_HERE);
  AppendMenuW(menu, MF_STRING, CMD_DELETE, mc_wstr_mnemonic(MC_STR_DELETE_FROM_LIBRARY_MENU));
  int cmd = show(menu, o);

  BOOL changed = FALSE;
  if (cmd == CMD_START_RADIO) playback_play_radio(seed);
  queue_pick(cmd, PLAYBACK_ALBUM, &h);
  if (cmd == CMD_EDIT) changed = edit_dialog_album(owner, lib, h.uid);
  if (cmd == CMD_DELETE) changed = delete_dialog(owner, lib, MC_DELETE_ALBUMS, h.uid, h.name);
  if (cmd == CMD_REMOVE_HERE || cmd == CMD_SYNC_HERE)
    changed = remove_here_command(owner, (UINT)cmd, CMD_REMOVE_HERE, CMD_SYNC_HERE, MC_PROFILE_ALBUM, h.uid, h.name);
  if (cmd >= CMD_GROUP_FIRST && groups && cmd - CMD_GROUP_FIRST < groups->count)
    changed = add_to_album_group(owner, lib, h.uid, groups->items[cmd - CMD_GROUP_FIRST].uid);
  mc_album_group_list_free(groups);
  if (cmd == CMD_NEW_GROUP) changed = add_to_album_group(owner, lib, h.uid, NULL);
  if (cmd == CMD_REMOVE_FROM_GROUP) {
    changed = lib && mc_album_group_remove(lib, group, h.uid) == MC_OK;
    if (!changed)
      message_dialog(owner, mc_wstr(MC_STR_ALBUM_GROUP), mc_wstr(MC_STR_ALBUM_GROUP_REMOVE_FAILED), MB_ICONERROR);
  }
  if (cmd == CMD_MOVE_UP || cmd == CMD_MOVE_DOWN) {
    const char *after = cmd == CMD_MOVE_UP ? up_after : down_after;
    changed = lib && mc_album_group_move(lib, group, h.uid, after[0] ? after : NULL) == MC_OK;
    if (!changed)
      message_dialog(owner, mc_wstr(MC_STR_ALBUM_GROUP), mc_wstr(MC_STR_ALBUM_GROUP_MOVE_FAILED), MB_ICONERROR);
  }
  free(h.name);
  return result(cmd, changed);
}

item_menu_result item_menu_artist(const item_menu_options *o) {
  held h = hold(o);
  mc_library *lib = o->lib;
  HWND owner = GetAncestor(o->wnd, GA_ROOT);

  HMENU menu = CreatePopupMenu();
  char seed[MC_UID_STRLEN];
  append_queue(menu);
  radio_menu_append_artist(menu, CMD_START_RADIO, lib, h.uid, seed);
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_EDIT, mc_wstr_mnemonic(MC_STR_EDIT_MENU));
  remove_here_append_menu(menu, MC_PROFILE_ARTIST, h.uid, CMD_REMOVE_HERE, CMD_SYNC_HERE);
  int cmd = show(menu, o);

  BOOL changed = FALSE;
  if (cmd == CMD_START_RADIO) playback_play_radio(seed);
  queue_pick(cmd, PLAYBACK_ARTIST, &h);
  if (cmd == CMD_EDIT) changed = edit_dialog_artist(owner, lib, h.uid);
  if (cmd == CMD_REMOVE_HERE || cmd == CMD_SYNC_HERE)
    changed = remove_here_command(owner, (UINT)cmd, CMD_REMOVE_HERE, CMD_SYNC_HERE, MC_PROFILE_ARTIST, h.uid, h.name);
  free(h.name);
  return result(cmd, changed);
}

void item_menu_append_playlist(HMENU menu, BOOL smart, BOOL rename_key, UINT play_next, UINT play_last, UINT edit_rules,
                               UINT rename, UINT del) {
  wchar_t item[128];
  AppendMenuW(menu, MF_STRING, play_next, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_NEXT));
  AppendMenuW(menu, MF_STRING, play_last, mc_wstr_mnemonic(MC_STR_WINDOWS_PLAY_LAST));
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  if (smart) AppendMenuW(menu, MF_STRING, edit_rules, mc_wstr_mnemonic(MC_STR_SMART_EDIT_MENU));
  AppendMenuW(menu, MF_STRING, rename,
              rename_key ? wstr_menu_item(item, 128, MC_STR_PLAYLIST_RENAME_MENU, MC_STR_WINDOWS_SHORTCUT_RENAME)
                         : mc_wstr_mnemonic(MC_STR_PLAYLIST_RENAME_MENU));
  if (smart) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, del, mc_wstr_mnemonic(smart ? MC_STR_SMART_DELETE_PLAYLIST_MENU : MC_STR_PLAYLIST_DELETE_MENU));
}

item_menu_result item_menu_playlist(const item_menu_options *o, BOOL smart) {
  held h = hold(o);
  mc_library *lib = o->lib;
  HWND owner = GetAncestor(o->wnd, GA_ROOT);
  HMENU menu = CreatePopupMenu();
  item_menu_append_playlist(menu, smart, o->rename_key, CMD_PLAY_NEXT, CMD_PLAY_LAST, CMD_EDIT_RULES, CMD_RENAME, CMD_DELETE);
  int cmd = show(menu, o);

  BOOL changed = FALSE;
  queue_pick(cmd, smart ? PLAYBACK_SMART_PLAYLIST : PLAYBACK_PLAYLIST, &h);
  if (cmd == CMD_EDIT_RULES) changed = smart_edit_dialog(owner, lib, h.uid);
  if (cmd == CMD_RENAME)
    changed = smart ? smart_rename_dialog(owner, lib, h.uid, h.name, MC_SMART_TRACKS)
                    : playlist_rename_dialog(owner, lib, h.uid, h.name);
  if (cmd == CMD_DELETE)
    changed = smart ? smart_delete_dialog(owner, lib, h.uid, h.name, MC_SMART_TRACKS)
                    : playlist_delete_dialog(owner, lib, h.uid, h.name);
  free(h.name);
  return result(cmd, changed);
}

item_menu_result item_menu_group(const item_menu_options *o, BOOL smart) {
  held h = hold(o);
  mc_library *lib = o->lib;
  HWND owner = GetAncestor(o->wnd, GA_ROOT);
  HMENU menu = CreatePopupMenu();
  wchar_t item[128];
  append_queue(menu);
  AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  if (smart) AppendMenuW(menu, MF_STRING, CMD_EDIT_RULES, mc_wstr_mnemonic(MC_STR_SMART_EDIT_MENU));
  AppendMenuW(menu, MF_STRING, CMD_RENAME,
              smart && o->rename_key ? wstr_menu_item(item, 128, MC_STR_PLAYLIST_RENAME_MENU, MC_STR_WINDOWS_SHORTCUT_RENAME)
                                     : mc_wstr_mnemonic(MC_STR_PLAYLIST_RENAME_MENU));
  if (smart) AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
  AppendMenuW(menu, MF_STRING, CMD_DELETE,
              mc_wstr_mnemonic(smart ? MC_STR_SMART_DELETE_GROUP_MENU : MC_STR_ALBUM_GROUP_DELETE_MENU));
  int cmd = show(menu, o);

  BOOL changed = FALSE;
  queue_pick(cmd, smart ? PLAYBACK_SMART_GROUP : PLAYBACK_GROUP, &h);
  if (cmd == CMD_EDIT_RULES) changed = smart_edit_dialog(owner, lib, h.uid);
  if (cmd == CMD_RENAME)
    changed = smart ? smart_rename_dialog(owner, lib, h.uid, h.name, MC_SMART_ALBUMS)
                    : album_group_rename_dialog(owner, lib, h.uid, h.name);
  if (cmd == CMD_DELETE)
    changed = smart ? smart_delete_dialog(owner, lib, h.uid, h.name, MC_SMART_ALBUMS)
                    : album_group_delete_dialog(owner, lib, h.uid, h.name);
  free(h.name);
  return result(cmd, changed);
}

item_menu_result item_menu_station(const item_menu_options *o) {
  held h = hold(o);
  HMENU menu = CreatePopupMenu();
  char seed[MC_UID_STRLEN], start[MC_UID_STRLEN]; /* the tile can go while the menu is up */
  snprintf(start, sizeof start, "%s", o->start ? o->start : "");
  radio_menu_append_station(menu, CMD_PLAY, o->lib, h.uid, seed);
  int cmd = show(menu, o);
  if (cmd == CMD_PLAY) playback_play_station(seed, start);
  free(h.name);
  return result(cmd, FALSE);
}
