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

/* Cover art. A playlist's cover is a 2x2 grid of four of its albums' covers, picked at random
 * but seeded by the playlist's uid: the same playlist always gets the same four (on every
 * device that has those covers), and adding an album only changes the picture if the new
 * cover ranks among the four. The order in the grid is the same random order, top left first.
 *
 * Distinct covers are counted by image (albums.art_hash), so two albums sharing a cover count
 * once. Only covers whose file is on this device count. With:
 *   - 4 or more: a JPEG of the grid, MC_PLAYLIST_ART_SIZE pixels square, each cover
 *     centre-cropped to a square. It's cached in cache_dir and made again only when the four
 *     covers change.
 *   - 1 to 3: the top-ranked one alone, as the album's own cover file (JPEG or PNG). Nothing
 *     is cached.
 *   - none: *out_path is NULL and the result MC_OK; show a placeholder.
 * A cover that can't be decoded is passed over for the next one in line.
 *
 * cache_dir is a per-device folder the caller owns, say <config>/MC_PLAYLIST_ART_CACHE_DIR;
 * it's created if missing. Generated files are named <playlist uid>-<key>.jpg, so the path
 * changes whenever the picture does and can key a frontend's own thumbnail cache; older
 * files for the playlist are deleted when a new one is made.
 *
 * playlist_uid must be a textual UUID (MC_ERR_INVALID_ARG otherwise); MC_ERR_NOT_FOUND if
 * there's no such playlist. *out_path is malloc'd (free with mc_free). A cache hit only reads the database and checks
 * the file exists; a miss decodes four covers, which can take a fraction of a second, so
 * frontends may want to call it from a worker with its own library connection. Calls for
 * the same playlist may overlap. */
#define MC_PLAYLIST_ART_SIZE 600
#define MC_PLAYLIST_ART_CACHE_DIR "playlist-art"
mc_status mc_playlist_art(mc_library *lib, const char *playlist_uid, const char *cache_dir,
                          char **out_path);

/* Deletes cached grids in cache_dir for playlists that no longer exist, and temp files
 * from writes abandoned over an hour ago. Only files named as mc_playlist_art() names them
 * are touched. A missing cache_dir is empty. */
mc_status mc_playlist_art_prune(mc_library *lib, const char *cache_dir);

#ifdef __cplusplus
}
#endif

#endif
