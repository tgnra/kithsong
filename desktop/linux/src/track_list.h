/* The track list: the selected album's tracks, in the pane beside the album grid.
 *
 *   +------+  OK Computer                  [x]
 *   | art  |  Radiohead
 *   +------+  1997 · 12 tracks · 53 min
 *   ----------------------------------------
 *    1   Airbag                        4:44
 *   >)   Paranoid Android              6:23   <- playing, in the accent colour
 *
 * Track numbers and durations are dimmed; on albums with more than one disc the numbers
 * read "2-5". Double-click or Enter plays a track; Escape or the close button closes the
 * pane. A GtkListView, so long albums stay cheap. Only one list exists at a time.
 */
#ifndef MUSICAPP_TRACK_LIST_H
#define MUSICAPP_TRACK_LIST_H

#include <gtk/gtk.h>

#include "album_grid.h"
#include "musiccore/musiccore.h"

/* play is called when the user plays track index; close when they close the pane. */
GtkWidget *track_list_new(void (*play)(void *user, int index), void (*close)(void *user), void *user);

/* Shows an album's tracks, or nothing for NULL. Showing the same album again reloads it
 * (after an import, say), keeping the selected track. */
void track_list_show(mc_library *lib, AlbumItem *album);

/* The album shown (borrowed), or NULL. */
AlbumItem *track_list_album(void);

/* The shown album's tracks, in order, for the play queue. Valid until the list next changes. */
typedef struct track_list_item {
  const char *uid;
  const char *title;
  int duration_ms;  /* 0 if unknown */
  const char *path; /* NULL if the track has no file on this device */
} track_list_item;
int track_list_count(void);
track_list_item track_list_item_at(int i);

/* Marks the playing track, by uid, or none for NULL. It stays marked while other albums are
 * shown, and when its album is shown again. */
void track_list_set_playing(const char *uid);

#endif
