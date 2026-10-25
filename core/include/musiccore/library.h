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

/* A library is one folder the user chooses (e.g. ~/Music) holding everything: the music,
 * organised as Artist/Album (Year)/..., a cover.jpg per album folder, and the database
 * itself as library.db at the top. Imports copy files in. */
#define MC_LIBRARY_DB_NAME "library.db"

/* Opens the library in folder dir, creating the folder and database if needed, and migrates
 * it to the current schema. */
mc_status mc_library_open(const char *dir, mc_library **out);
void mc_library_close(mc_library *lib);

/* Schema version the database is at (PRAGMA user_version). */
int mc_library_schema_version(const mc_library *lib);

/* The library folder, as given to mc_library_open() without a trailing separator. */
const char *mc_library_dir(const mc_library *lib);

/* This device's stable node id, generated the first time the library is created. */
const char *mc_library_node_uid(const mc_library *lib);

/* Most recent SQLite error message for this handle. Valid until the next call. */
const char *mc_library_errmsg(const mc_library *lib);

mc_status mc_library_count(mc_library *lib, mc_entity entity, int64_t *out);

typedef struct mc_album_info {
  char uid[MC_UID_STRLEN];
  char *title;
  char *artist;
  int year;
  int is_singles; /* an artist's "<artist> Singles" album */
  int has_art;    /* folder holds cover.jpg or cover.png */
  char *folder;   /* absolute path of the album's folder; NULL if it has no files here */
} mc_album_info;

typedef struct mc_album_list {
  mc_album_info *items;
  int count;
} mc_album_list;

/* Every album, in display order: by artist (sort name), the artist's albums before their
 * singles, then by year and title. One query; cheap enough to call again after an import.
 * Free with mc_album_list_free(). */
mc_status mc_library_list_albums(mc_library *lib, mc_album_list **out);
void mc_album_list_free(mc_album_list *list);

#ifdef __cplusplus
}
#endif

#endif
