/* iPod_Control/kithsong.json: what Kithsong wrote to this iPod, so any computer running
 * Kithsong recognises the files and doesn't write them again.
 *
 *   {"version": 1,
 *    "tracks": {"<key>": {"track_uid": "...", "file_uid": "...", "format": "alac",
 *                         "sha256": "<hex>"}, ...}}
 *
 * The key is the plugin's own key for a track (see device.h: the mhit dbid in hex). */
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
  char *format;  /* "alac", "mp3", "copy" ... */
  char *sha256;  /* hex of the file as written */
} ipod_kith_entry;

typedef struct ipod_kith_map {
  ipod_kith_entry *entries;
  size_t n, cap;
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
int ipod_kith_remove(ipod_kith_map *map, const char *key);
void ipod_kith_free(ipod_kith_map *map);

#ifdef __cplusplus
}
#endif

#endif
