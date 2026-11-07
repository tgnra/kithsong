/* iPod_Control/kithsong.json: what Kithsong wrote to this iPod, so any computer running
 * Kithsong recognises the files and doesn't write them again.
 *
 *   {"version": 1,
 *    "tracks": {"<key>": {"track_uid": "...", "file_uid": "...", "format": "alac",
 *                         "setting": "", "sha256": "<hex>"}, ...},
 *    "playlists": {"<uid>": {"name": "Mix", "id": "<mhyp id, 16 hex digits>"}, ...}}
 *
 * The key is the plugin's own key for a track (see device.h: the mhit dbid in hex). format
 * and setting are the encoder format and setting it was written at ("mp3" and "" for an MP3
 * copied as it was). "playlists" are the iPod playlists Kithsong made, by the library's
 * playlist uid, so it replaces only its own. */
#ifndef KITHIPOD_KITHJSON_H
#define KITHIPOD_KITHJSON_H

#include "kithipod/common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipod_kith_entry {
  char *key;
  char *track_uid;
  char *file_uid;
  char *format;  /* "alac", "mp3" ... */
  char *setting; /* "v0", "320" ...; NULL or "" for none */
  char *sha256;  /* hex of the file as written */
} ipod_kith_entry;

typedef struct ipod_kith_playlist {
  char *uid;     /* the library's */
  char *name;    /* as written on the iPod */
  uint64_t id;   /* the iPod playlist's (mhyp) id */
} ipod_kith_playlist;

typedef struct ipod_kith_map {
  ipod_kith_entry *entries;
  size_t n, cap;
  ipod_kith_playlist *playlists;
  size_t n_playlists, cap_playlists;
} ipod_kith_map;

/* A missing file gives an empty map and 0. An unreadable one gives IPOD_ERR_FORMAT and an
 * empty map, so the caller can carry on (the tracks then count as not Kithsong's). */
int ipod_kith_read(const char *path, ipod_kith_map *out, ipod_error *err);
int ipod_kith_parse(const char *json, size_t len, ipod_kith_map *out, ipod_error *err);
/* Atomic: path.tmp, flushed, renamed over path. */
int ipod_kith_write(const char *path, const ipod_kith_map *map, ipod_error *err);
/* The JSON text (free() it). */
char *ipod_kith_serialize(const ipod_kith_map *map, size_t *out_len);

const ipod_kith_entry *ipod_kith_find(const ipod_kith_map *map, const char *key);
/* Adds or replaces the entry for key. NULL fields are stored as absent. */
int ipod_kith_set(ipod_kith_map *map, const char *key, const char *track_uid, const char *file_uid,
                  const char *format, const char *sha256);
/* The same, with the encoder setting. */
int ipod_kith_set2(ipod_kith_map *map, const char *key, const char *track_uid, const char *file_uid,
                   const char *format, const char *setting, const char *sha256);
int ipod_kith_remove(ipod_kith_map *map, const char *key);

/* Kithsong's playlists: adds or replaces uid's; removes it. */
int ipod_kith_playlist_set(ipod_kith_map *map, const char *uid, const char *name, uint64_t id);
int ipod_kith_playlist_remove(ipod_kith_map *map, const char *uid);
const ipod_kith_playlist *ipod_kith_playlist_find(const ipod_kith_map *map, const char *uid);

void ipod_kith_free(ipod_kith_map *map);

#ifdef __cplusplus
}
#endif

#endif
