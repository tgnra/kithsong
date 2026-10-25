/* Library database: artists, albums, tracks, files, playlists, and the change log
 * that sync is built on. Backed by one SQLite file in WAL mode. */
#ifndef MUSICCORE_LIBRARY_H
#define MUSICCORE_LIBRARY_H

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;

typedef enum mc_entity {
  MC_ENTITY_ARTISTS,
  MC_ENTITY_ALBUMS,
  MC_ENTITY_TRACKS,
  MC_ENTITY_FILES,
  MC_ENTITY_PLAYLISTS
} mc_entity;

/* Opens (creating if needed) the library at path and migrates it to the current schema. */
mc_status mc_library_open(const char *path, mc_library **out);
void mc_library_close(mc_library *lib);

/* Schema version the database is at (PRAGMA user_version). */
int mc_library_schema_version(const mc_library *lib);

/* This device's stable node id, generated the first time the library is created. */
const char *mc_library_node_uid(const mc_library *lib);

/* Most recent SQLite error message for this handle. Valid until the next call. */
const char *mc_library_errmsg(const mc_library *lib);

mc_status mc_library_count(mc_library *lib, mc_entity entity, int64_t *out);

#ifdef __cplusplus
}
#endif

#endif
