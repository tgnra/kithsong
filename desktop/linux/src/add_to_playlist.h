/* The Add to Playlist dialog, from a track's right-click menu in the track list:
 *
 *   +------------------------------------------+
 *   |             Add to Playlist              |
 *   |  | New Playlist                     [v] | |
 *   |                                          |
 *   |  | Driving                              | |
 *   |  | 12 tracks                            | |
 *   |  | Mix                                  | |
 *   |  | 3 tracks                             | |
 *   +------------------------------------------+
 *
 * Clicking a playlist adds the track to its end and closes the dialog. Typing a name and
 * pressing Enter (or the tick) creates a playlist, which then appears in the list.
 *
 * Needs libadwaita 1.5 (AdwDialog).
 */
#ifndef MUSICAPP_ADD_TO_PLAYLIST_H
#define MUSICAPP_ADD_TO_PLAYLIST_H

#include <adwaita.h>

#include "musiccore/musiccore.h"

/* Shows the dialog over parent (any widget in the main window) for track_uid. changed is
 * called whenever the library's playlists change: one is created, or the track is added. */
void add_to_playlist_present(GtkWidget *parent, mc_library *lib, const char *track_uid,
                             void (*changed)(void *user), void *user);

#endif
