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

/* This device's stable node id, generated the first time the library is created. It
 * stamps every change made here (the changelog's origin). */
const char *mc_library_node_uid(const mc_library *lib);

/* Gives the library a new node id, for when sync finds another device already using this
 * one (e.g. the library folder was copied). Changes made under the old id stay valid: to
 * peers they're just another origin. Other open connections pick it up on their next write. */
mc_status mc_library_rotate_node_uid(mc_library *lib);

/* Which library this is, generated when it's created and the same on every device that
 * syncs it. A sync group syncs exactly one library and never touches another. */
const char *mc_library_uid(const mc_library *lib);

/* Makes this library take on another's uid, so a device can join a group that syncs it.
 * Only allowed while the library has no tracks (MC_ERR_LIBRARY_MISMATCH otherwise), so two
 * different libraries are never merged; adopting the uid it already has always succeeds.
 * library_uid must be a lowercase textual UUID (MC_ERR_INVALID_ARG otherwise). */
mc_status mc_library_adopt_uid(mc_library *lib, const char *library_uid);

/* Most recent SQLite error message for this handle. Valid until the next call. */
const char *mc_library_errmsg(const mc_library *lib);

mc_status mc_library_count(mc_library *lib, mc_entity entity, int64_t *out);

/* Counts a play of track_uid on this device and makes now its last play. What counts as a
 * play (say, half the track) is the caller's call. MC_ERR_NOT_FOUND if there's no such track. */
mc_status mc_library_record_play(mc_library *lib, const char *track_uid);

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

typedef struct mc_track_info {
  char uid[MC_UID_STRLEN];
  char *title;
  int track_no; /* 0 if unknown */
  int disc_no;  /* 0 if unknown */
  int duration_ms;
  char *path; /* absolute path of the track's file on this device (the original, if there are
               * transcodes too); NULL if there's none here */
  char *artist;    /* the track's artist, else its album's; NULL if unknown */
  char *album;     /* NULL if it has none */
  char *cover_dir; /* the folder holding its album's cover, or NULL if there's none here */
  char entry_uid[MC_UID_STRLEN]; /* its playlist entry, for mc_playlist_move() and
                                  * mc_playlist_remove(); "" outside playlists */
} mc_track_info;

typedef struct mc_track_list {
  mc_track_info *items;
  int count;
} mc_track_list;

/* An album's tracks in order: by disc, then track number (unnumbered tracks last), then
 * title. Free with mc_track_list_free(). */
mc_status mc_library_list_album_tracks(mc_library *lib, const char *album_uid, mc_track_list **out);
/* A playlist's tracks in playlist order. A track that's in it twice is listed twice. */
mc_status mc_library_list_playlist_tracks(mc_library *lib, const char *playlist_uid, mc_track_list **out);
void mc_track_list_free(mc_track_list *list);

typedef struct mc_playlist_info {
  char uid[MC_UID_STRLEN];
  char *name;
  int track_count;
} mc_playlist_info;

typedef struct mc_playlist_list {
  mc_playlist_info *items;
  int count;
} mc_playlist_list;

/* Every playlist, by name. Free with mc_playlist_list_free(). */
mc_status mc_library_list_playlists(mc_library *lib, mc_playlist_list **out);
void mc_playlist_list_free(mc_playlist_list *list);

#ifdef __cplusplus
}
#endif

#endif
