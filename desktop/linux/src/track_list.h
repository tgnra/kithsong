/* The track list: the selected album's or playlist's tracks, in the pane beside the grid.
 *
 *   +------+  OK Computer                  [x]
 *   | art  |  Radiohead
 *   +------+  1997 · 12 tracks · 53 min
 *   ----------------------------------------
 *    1   Airbag                        4:44
 *   >)   Paranoid Android              6:23   <- playing, in the accent colour
 *
 * Track numbers and durations are dimmed; on albums with more than one disc the numbers
 * read "2-5"; in a playlist they count its places. Double-click or Enter plays a track;
 * right-click opens its menu (Add to Playlist; in a playlist also Move Up, Move Down and
 * Remove from Playlist). A playlist's tracks can be dragged into another order too, and its
 * header has a menu (the "..." beside close) to rename or delete it. Escape or the close
 * button closes the pane. A GtkListView, so long albums stay cheap. Only one list exists at
 * a time.
 */
#ifndef MUSICAPP_TRACK_LIST_H
#define MUSICAPP_TRACK_LIST_H

#include <gtk/gtk.h>

#include "album_grid.h"
#include "musiccore/musiccore.h"

/* What the user asks of the list. The list changes nothing itself: the callee does, then
 * reloads it with track_list_show(). Any may be NULL. */
typedef struct track_list_callbacks {
  void (*play)(void *user, int index);
  void (*close)(void *user);
  void (*add_to_playlist)(void *user, const char *track_uid);
  /* In the shown playlist: take an entry out, or move it to after after_entry_uid (NULL =
   * to the start). */
  void (*remove)(void *user, const char *entry_uid);
  void (*move)(void *user, const char *entry_uid, const char *after_entry_uid);
  /* The header menu's Rename... and Delete Playlist..., for the shown playlist. */
  void (*rename_playlist)(void *user);
  void (*delete_playlist)(void *user);
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

/* Marks the playing track, by uid, or none for NULL. It stays marked while other albums are
 * shown, and when its album is shown again. */
void track_list_set_playing(const char *uid);

#endif
