/* The track list: the selected album's or playlist's tracks, in the pane beside the grid.
 *
 *   +------+  OK Computer                  [x]
 *   | art  |  Radiohead
 *   +------+  1997 · 12 tracks · 53 min
 *   [     |> Play     ] [    >< Shuffle    ]
 *   ----------------------------------------
 *    1   Airbag                        4:44
 *   >)   Paranoid Android              6:23   <- playing, in the accent colour
 *
 * Play and Shuffle play the whole list, in order or shuffled; they're insensitive when
 * nothing in it can be played here. Track numbers and durations are dimmed; on albums with
 * more than one disc the numbers read "2-5"; in a playlist they count its places.
 * Double-click or Enter plays a track; right-click, the Menu key or Shift+F10 opens its menu
 * (Add to Playlist, Delete from Library; in a playlist also Move Up, Move Down and Remove from
 * Playlist). A playlist's tracks can be dragged into another order too, or moved with Alt+Up
 * and Alt+Down; Delete removes one. Its header has a menu (the "..." beside close, or F2, or
 * the Menu key with no track selected) to rename or delete it. A smart playlist's tracks
 * can't be moved or removed, and it has no menu; in random order its header has a Reshuffle
 * button instead. Escape or the close button closes the pane. A GtkListView,
 * so long albums stay cheap. Only one list exists at a time.
 */
#ifndef KITHSONG_TRACK_LIST_H
#define KITHSONG_TRACK_LIST_H

#include <gtk/gtk.h>

#include "album_grid.h"
#include "musiccore/musiccore.h"

/* What the user asks of the list. The list changes nothing itself: the callee does, then
 * reloads it with track_list_show(). Any may be NULL. */
typedef struct track_list_callbacks {
  void (*play)(void *user, int index);
  /* The header's Play and Shuffle: every track, from the first in order (shuffle off), or
   * shuffled (shuffle on). */
  void (*play_all)(void *user, gboolean shuffled);
  void (*close)(void *user);
  void (*add_to_playlist)(void *user, const char *track_uid, gboolean here); /* here: it has a file here */
  /* In the shown playlist: take an entry out, or move it to after after_entry_uid (NULL =
   * to the start). */
  void (*remove)(void *user, const char *entry_uid);
  void (*move)(void *user, const char *entry_uid, const char *after_entry_uid);
  /* The header menu's Rename... and Delete Playlist..., for the shown playlist. */
  void (*rename_playlist)(void *user);
  void (*delete_playlist)(void *user);
  /* The track menu's Delete from Library... (see delete_dialog.h). */
  void (*delete_track)(void *user, const char *track_uid, const char *title);
  /* The Reshuffle button, for the shown smart playlist. */
  void (*reshuffle)(void *user);
  void *user;
} track_list_callbacks;

GtkWidget *track_list_new(const track_list_callbacks *cb);

/* Shows an album's or playlist's tracks, or nothing for NULL. Showing the same one again
 * reloads it (after an import, say), keeping the selected track. */
void track_list_show(mc_library *lib, AlbumItem *album);

/* The album or playlist shown (borrowed), or NULL. */
AlbumItem *track_list_album(void);

/* The shown tracks, in order, for the play queue. Valid until the list next changes. */
typedef struct track_list_item {
  const char *uid;
  const char *title;
  int duration_ms;  /* 0 if unknown */
  const char *path; /* NULL if the track has no file on this device */
  const char *artist, *album; /* NULL if unknown */
  const char *cover_dir;      /* the album folder, or NULL if it has no cover */
} track_list_item;
int track_list_count(void);
track_list_item track_list_item_at(int i);

/* Selects the shown track with uid, focused and scrolled into view, if it's there. */
void track_list_select(const char *uid);

/* Marks the playing track, by uid, or none for NULL. It stays marked while other albums are
 * shown, and when its album is shown again. */
void track_list_set_playing(const char *uid);

#endif
