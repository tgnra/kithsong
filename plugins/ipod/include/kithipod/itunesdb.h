/* The iTunesDB (iPod_Control/iTunes/iTunesDB): reading, editing and writing the pre-hash
 * database of click-wheel iPods.
 *
 * Round trip: whatever this library doesn't understand is kept. Each track and playlist
 * read from a file remembers its original bytes; header fields the library doesn't model,
 * mhods of unknown types and datasets other than tracks and playlists (album lists, smart
 * playlist data) are written back as they were. A database that is read and written without
 * changes comes out byte for byte the same.
 *
 * Track order matters: Play Counts and the On-The-Go files index tracks by their position
 * in the track list (ipod_db.tracks), so read those before editing (see playcounts.h). */
#ifndef KITHIPOD_ITUNESDB_H
#define KITHIPOD_ITUNESDB_H

#include "kithipod/common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* mhit filetype markers: four characters as a little-endian u32 ('MP3 ' -> 0x4D503320). */
#define IPOD_FILETYPE_MP3 0x4D503320u
#define IPOD_FILETYPE_M4A 0x4D344120u
#define IPOD_FILETYPE_M4P 0x4D345020u
#define IPOD_FILETYPE_WAV 0x57415620u

typedef struct ipod_track_priv ipod_track_priv;
typedef struct ipod_playlist_priv ipod_playlist_priv;
typedef struct ipod_db_priv ipod_db_priv;

/* One mhit. Strings are UTF-8 owned by the track (set them with ipod_set_str); NULL or ""
 * means absent. Times are Mac timestamps (see common.h). */
typedef struct ipod_track {
  uint32_t id;    /* unique within the database; playlists refer to it */
  uint64_t dbid;  /* 64-bit persistent id; the ArtworkDB refers to it */

  char *title, *artist, *album, *album_artist, *genre, *composer, *comment, *grouping;
  char *sort_title, *sort_artist, *sort_album, *sort_album_artist, *sort_composer;
  char *filetype_desc; /* mhod 6, e.g. "MPEG audio file" */
  char *location;      /* mhod 2, e.g. ":iPod_Control:Music:F07:KQ3Z.mp3" */

  uint32_t filetype;   /* IPOD_FILETYPE_* */
  uint8_t type1, type2; /* MP3 CBR 0/1, MP3 VBR 1/1, AAC and ALAC 0/0 */
  uint8_t compilation;
  uint8_t rating;      /* stars * 20 */
  uint32_t time_modified, time_added, last_played, time_released, last_skipped;
  uint32_t size;       /* file size in bytes */
  uint32_t length_ms;
  uint32_t track_number, track_total, disc_number, disc_total, year;
  uint32_t bitrate;    /* kbps */
  uint32_t sample_rate; /* Hz (stored as 16.16 fixed point) */
  int32_t volume;
  uint32_t start_ms, stop_ms, soundcheck;
  uint32_t play_count, play_count2;
  uint32_t skip_count;
  uint32_t bookmark_ms;
  uint8_t checked;     /* 0 = checked (sic) */
  uint16_t bpm;
  uint16_t artwork_count;
  uint32_t artwork_size; /* bytes of the source picture */
  uint8_t has_artwork;   /* 1 = has artwork, 2 = none */
  uint8_t mark_unplayed;
  uint16_t unk126;       /* 0xffff for MP3/AAC/ALAC */
  uint16_t unk144;       /* 0x000c MP3, 0x0033 AAC/ALAC */
  uint32_t media_type;   /* 1 = audio; 0 on old iPods */
  uint64_t sample_count;

  ipod_track_priv *priv; /* original bytes; NULL for tracks made here */
} ipod_track;

typedef struct ipod_playlist {
  char *name;
  int is_master;   /* the hidden library playlist, named after the iPod */
  int is_podcast;  /* the Podcasts playlist */
  int is_smart;    /* has smart-playlist rules (kept as they were) */
  uint64_t id;
  uint32_t timestamp;
  uint32_t *items; /* track ids, in order */
  size_t n_items, cap_items;
  ipod_playlist_priv *priv;
} ipod_playlist;

typedef struct ipod_db {
  ipod_track **tracks;        /* in database order */
  size_t n_tracks, cap_tracks;
  ipod_playlist **playlists;  /* [0] is the master playlist */
  size_t n_playlists, cap_playlists;
  uint32_t version;           /* mhbd version */
  uint64_t id;                /* mhbd database id */
  ipod_db_priv *priv;
} ipod_db;

/* An empty database whose master playlist is called name. */
ipod_db *ipod_db_new(const char *name);
ipod_db *ipod_db_parse(const uint8_t *data, size_t len, ipod_error *err);
ipod_db *ipod_db_read(const char *path, ipod_error *err);
void ipod_db_free(ipod_db *db);

/* Serializes to a new buffer (free() it). */
int ipod_db_serialize(ipod_db *db, uint8_t **out, size_t *out_len, ipod_error *err);
/* Writes path atomically (path.tmp, flushed, renamed over path). If backup_path is not
 * NULL and path exists, path is first copied there. */
int ipod_db_write(ipod_db *db, const char *path, const char *backup_path, ipod_error *err);

/* The iPod's name: the master playlist's. */
const char *ipod_db_name(const ipod_db *db);
int ipod_db_set_name(ipod_db *db, const char *name);
ipod_playlist *ipod_db_master(ipod_db *db);
/* Whether the database read had a hash (mhbd hashing scheme set): never write those. */
int ipod_db_is_hashed(const ipod_db *db);

/* A new track with iTunes' defaults (visible, has_artwork = 2, checked). */
ipod_track *ipod_track_new(void);
void ipod_track_free(ipod_track *t);
/* Fills filetype, type1/2, unk126/144 and filetype_desc for an encoded file:
 * codec "mp3" (vbr says which) or "alac" or "aac". */
void ipod_track_set_kind(ipod_track *t, const char *codec, int vbr);

/* Appends a track; gives it an id and dbid if they're 0. The database owns it after. The
 * master playlist always lists every track (it's rebuilt on write). */
int ipod_db_add_track(ipod_db *db, ipod_track *t);
/* Removes and frees the track with this id, and drops it from every playlist. */
int ipod_db_remove_track(ipod_db *db, uint32_t id);
ipod_track *ipod_db_find_track(const ipod_db *db, uint32_t id);
ipod_track *ipod_db_find_dbid(const ipod_db *db, uint64_t dbid);
/* Index of the track in database order, or -1. */
long ipod_db_track_index(const ipod_db *db, uint32_t id);

ipod_playlist *ipod_playlist_new(const char *name);
void ipod_playlist_free(ipod_playlist *pl);
int ipod_playlist_add(ipod_playlist *pl, uint32_t track_id);
void ipod_playlist_clear(ipod_playlist *pl);
/* Appends a playlist (owned by the database after); gives it an id if 0. */
int ipod_db_add_playlist(ipod_db *db, ipod_playlist *pl);
/* Removes and frees a playlist (never the master). */
int ipod_db_remove_playlist(ipod_db *db, ipod_playlist *pl);
ipod_playlist *ipod_db_find_playlist(const ipod_db *db, const char *name);

/* Replaces *field with a copy of v (NULL clears it). */
int ipod_set_str(char **field, const char *v);

/* The path of a track's file relative to the mount, '/'-separated
 * (":iPod_Control:Music:F07:KQ3Z.mp3" -> "iPod_Control/Music/F07/KQ3Z.mp3"). */
char *ipod_location_to_path(const char *location);
/* The reverse: "iPod_Control/Music/F07/KQ3Z.mp3" -> ":iPod_Control:Music:F07:KQ3Z.mp3". */
char *ipod_path_to_location(const char *relpath);

#ifdef __cplusplus
}
#endif

#endif
