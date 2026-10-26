/* Playlists. Entries refer to track uids and are ordered by fractional position keys so
 * concurrent edits on different devices merge (see docs/sync-protocol.md). A track can be
 * in a playlist more than once. Listing them is in library.h (mc_library_list_playlists(),
 * mc_library_list_playlist_tracks()).
 *
 * MC_ERR_NOT_FOUND means the playlist, track or entry named doesn't exist. */
#ifndef MUSICCORE_PLAYLIST_H
#define MUSICCORE_PLAYLIST_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;

/* name must not be empty; names needn't be unique. */
mc_status mc_playlist_create(mc_library *lib, const char *name, char out_uid[MC_UID_STRLEN]);
mc_status mc_playlist_rename(mc_library *lib, const char *playlist_uid, const char *name);
/* Deletes the playlist and its entries; the tracks are untouched. */
mc_status mc_playlist_delete(mc_library *lib, const char *playlist_uid);

/* Inserts track_uid after the entry after_entry_uid (NULL = at the start). out_entry_uid
 * may be NULL. */
mc_status mc_playlist_insert(mc_library *lib, const char *playlist_uid, const char *track_uid,
                             const char *after_entry_uid, char out_entry_uid[MC_UID_STRLEN]);
/* Adds track_uid at the end. out_entry_uid may be NULL. */
mc_status mc_playlist_append(mc_library *lib, const char *playlist_uid, const char *track_uid,
                             char out_entry_uid[MC_UID_STRLEN]);
/* Moves an entry to after after_entry_uid (NULL = to the start), within its playlist. */
mc_status mc_playlist_move(mc_library *lib, const char *entry_uid, const char *after_entry_uid);
mc_status mc_playlist_remove(mc_library *lib, const char *entry_uid);

#ifdef __cplusplus
}
#endif

#endif
