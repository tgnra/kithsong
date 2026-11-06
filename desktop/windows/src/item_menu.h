/* The menus of what the library holds, the same wherever it shows: the album grid's tiles, the
 * artist page, the Home page's tiles and search results.
 *
 *   an album    Play Next, Play Last, Start radio, Add to Album Group >, inside a manual group Move
 *               Up, Move Down and Remove from Album Group, then Edit..., Remove from This PC... or
 *               Sync to This PC where they apply, and Delete from Library...
 *   an artist   Play Next, Play Last, Start radio, Edit..., Remove from This PC... or Sync to This PC
 *               where they apply.
 *               An artist can't be deleted from the library, only their albums and tracks
 *   a playlist  Play Next, Play Last, Rename... and Delete Playlist...
 *   a group     Play Next, Play Last, Rename... and Delete Album Group...
 *   smart ones  Edit Rules... after those, and their own Delete...
 *   a station   Play, greyed out if it can't, as Start radio is. Nothing of radio can be queued
 *
 * On a recent, any of them goes on with Remove from Recents and Clear Recents.
 *
 * Each shows its menu and carries out what's picked, dialogs and all, reporting failures itself,
 * except for the two that are the recents': those are the caller's, as main.c has them. The
 * menu's and the dialogs' message loops run other messages, so what's shown may be gone by the
 * time one returns. Everything of options is copied first, so it can point into what's shown. */
#ifndef KITHSONG_ITEM_MENU_H
#define KITHSONG_ITEM_MENU_H

#include <windows.h>

#include "musiccore/musiccore.h"

typedef enum item_menu_result {
  ITEM_MENU_NOTHING,
  ITEM_MENU_CHANGED, /* the library changed, so the views should reload */
  ITEM_MENU_REMOVE_RECENT,
  ITEM_MENU_CLEAR_RECENTS,
} item_menu_result;

typedef struct item_menu_options {
  HWND wnd;            /* owns the menu; the dialogs go over its top-level window */
  mc_library *lib;     /* may be NULL */
  const char *uid;     /* the item, or for a station its seed track */
  const wchar_t *name; /* what it's called, for the dialogs; may be NULL */
  POINT pt;            /* where the menu opens, in screen coordinates */
  BOOL recent;         /* it's a recent: add Remove from Recents and Clear Recents */
  BOOL rename_key;     /* Rename... shows its shortcut, where the page has one */

  /* An album inside the manual group group, or NULL. up_after and down_after are the albums
   * that Move Up and Move Down put it after, "" for first. */
  const char *group;
  const char *up_after, *down_after;
  BOOL first, last; /* greys out Move Up or Move Down */
} item_menu_options;

item_menu_result item_menu_album(const item_menu_options *o);
item_menu_result item_menu_artist(const item_menu_options *o);
item_menu_result item_menu_playlist(const item_menu_options *o, BOOL smart);
item_menu_result item_menu_group(const item_menu_options *o, BOOL smart);
item_menu_result item_menu_station(const item_menu_options *o);

/* A playlist's items, for a menu with commands of its own: the track list's [...] button. */
void item_menu_append_playlist(HMENU menu, BOOL smart, BOOL rename_key, UINT play_next, UINT play_last, UINT edit_rules,
                               UINT rename, UINT del);

#endif
