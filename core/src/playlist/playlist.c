/* Playlists, ordered by fractional index keys (see schema_v1.sql, playlist_entries). */
#include "musiccore/playlist.h"

mc_status mc_playlist_create(mc_library *lib, const char *name, char out_uid[MC_UID_STRLEN]) {
  (void)lib;
  (void)name;
  (void)out_uid;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_playlist_rename(mc_library *lib, const char *playlist_uid, const char *name) {
  (void)lib;
  (void)playlist_uid;
  (void)name;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_playlist_delete(mc_library *lib, const char *playlist_uid) {
  (void)lib;
  (void)playlist_uid;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_playlist_insert(mc_library *lib, const char *playlist_uid, const char *track_uid,
                             const char *after_entry_uid, char out_entry_uid[MC_UID_STRLEN]) {
  (void)lib;
  (void)playlist_uid;
  (void)track_uid;
  (void)after_entry_uid;
  (void)out_entry_uid;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_playlist_move(mc_library *lib, const char *entry_uid, const char *after_entry_uid) {
  (void)lib;
  (void)entry_uid;
  (void)after_entry_uid;
  return MC_ERR_UNIMPLEMENTED;
}

mc_status mc_playlist_remove(mc_library *lib, const char *entry_uid) {
  (void)lib;
  (void)entry_uid;
  return MC_ERR_UNIMPLEMENTED;
}
