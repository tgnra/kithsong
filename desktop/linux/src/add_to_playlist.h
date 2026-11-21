/* The Add to Playlist dialog, from a track's right-click menu. For a track with no file here, a
 * note says a playlist of it won't show on the Playlists page until one of its tracks is here.
 *
 * Needs libadwaita 1.5 (AdwDialog).
 */
#ifndef KITHSONG_ADD_TO_PLAYLIST_H
#define KITHSONG_ADD_TO_PLAYLIST_H

#include <adwaita.h>

#include "kithcore/kithcore.h"

/* Shows the dialog over parent (any widget in the main window) for track_uid, which has a
 * file here if here. changed is called whenever the library's playlists change: one is
 * created, or the track is added. */
void add_to_playlist_present(GtkWidget *parent, mc_library *lib, const char *track_uid, gboolean here,
                             void (*changed)(void *user), void *user);

#endif
