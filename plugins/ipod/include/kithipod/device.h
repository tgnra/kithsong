/* An iPod mounted as a volume: everything the plugin's device target needs, without the
 * plugin ABI. One ipod_device is used from one thread at a time.
 *
 * Track keys: a track's key is its mhit dbid as 16 lower-case hex digits. It survives
 * rewrites of the database (track ids don't: iTunes renumbers them), and it's what
 * kithsong.json and the ArtworkDB use too.
 *
 * Lifecycle, as the plugin calls it:
 *   ipod_probe()          cheap check, no writes
 *   ipod_open()           reads SysInfo, the iTunesDB, kithsong.json, the ArtworkDB, and the
 *                         Play Counts and On-The-Go files (mapped to keys right away, while
 *                         they still match the database's track order)
 *   ipod_device_plays() / ipod_device_otg_playlists()
 *   ipod_device_done()    deletes Play Counts and/or the OTG files, once they're taken in
 *   ipod_device_put() / _update() / _remove()
 *   ipod_device_commit()  writes ArtworkDB, iTunesDB, kithsong.json (repeatable)
 *   ipod_device_close()   doesn't commit
 *
 * Plays are never lost. Play Counts and the OTG files are deleted by ipod_device_done() and
 * by nothing else. Until then (nobody took them in: an older caller, or one that failed
 * before it got to them) they stay on the iPod as they are, and stay true for the new
 * database: its track order keeps every track they index where it was. Tracks are only
 * ever added at the end or changed in place, and ipod_device_remove() of a track is put off
 * (the track and its file stay, still listed in kithsong.json, so the next sync removes it
 * again) while either file is waiting. */
#ifndef KITHIPOD_DEVICE_H
#define KITHIPOD_DEVICE_H

#include "kithipod/common.h"
#include "kithipod/itunesdb.h"
#include "kithipod/kithjson.h"
#include "kithipod/sysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IPOD_KEY_LEN 17 /* 16 hex digits + NUL */

typedef struct ipod_device ipod_device;

/* Whether mount is an iPod this library can write: iPod_Control/iTunes/iTunesDB exists,
 * SysInfo names a supported model, and the database isn't hashed. Returns an ipod_support
 * (>= 0), or IPOD_ERR_NOT_FOUND when it isn't an iPod at all. si may be NULL. */
int ipod_probe(const char *mount, ipod_sysinfo *si);

enum {
  IPOD_OPEN_ALLOW_UNKNOWN = 1, /* open a model missing from the table (no artwork) */
  IPOD_OPEN_CREATE = 2         /* make iPod_Control and an empty iTunesDB if they're missing */
};

ipod_device *ipod_open(const char *mount, unsigned flags, ipod_error *err);
void ipod_device_close(ipod_device *dev);

typedef struct ipod_device_info {
  char id[64];      /* stable: "guid:<FirewireGuid>", "serial:<serial>", or "db:<mhbd id>" */
  const char *name; /* the iPod's name (master playlist); valid until the next call */
  ipod_sysinfo sysinfo;
  const ipod_model *model; /* NULL when unknown */
  ipod_generation generation;
  uint64_t capacity, free_bytes;
  int artwork; /* whether covers are written (ArtworkDB) */
  /* Format limits for the encoders: 16-bit, 44.1 or 48 kHz, at most two channels. */
  uint32_t max_sample_rate;
  uint16_t max_bits, max_channels;
} ipod_device_info;

/* Fills info, reading free space afresh. */
int ipod_device_info_get(ipod_device *dev, ipod_device_info *info, ipod_error *err);
const char *ipod_device_mount(const ipod_device *dev);
ipod_db *ipod_device_db(ipod_device *dev);
const ipod_kith_map *ipod_device_kith(const ipod_device *dev);

void ipod_track_key(const ipod_track *t, char key[IPOD_KEY_LEN]);
ipod_track *ipod_device_find(ipod_device *dev, const char *key);
/* The kithsong.json entry of a track, or NULL when Kithsong didn't write it. */
const ipod_kith_entry *ipod_device_kith_entry(const ipod_device *dev, const char *key);
/* The track's file on the volume (free() it), or NULL. */
char *ipod_device_track_path(ipod_device *dev, const char *key);

typedef struct ipod_put_info {
  const ipod_tags *tags;
  const char *codec;        /* "alac", "mp3" or "aac" */
  int vbr;                  /* for "mp3" */
  uint32_t length_ms;
  uint32_t bitrate_kbps;    /* average */
  uint32_t sample_rate;
  uint64_t sample_count;
  uint32_t play_count;
  int64_t last_played;      /* Unix time, 0 = never */
  int64_t added;            /* Unix time, 0 = now */
  int rating;               /* 0-100 (stars * 20) */
  /* For kithsong.json; track_uid NULL means not recorded there. */
  const char *track_uid, *file_uid, *format, *sha256;
  const char *setting;      /* the encoder setting, for kithsong.json; NULL for none */
  /* Replace this track (same key, playlists and play history) instead of adding one. */
  const char *replace_key;
} ipod_put_info;

/* Copies src_path into iPod_Control/Music/Fnn/ under a short random name and adds (or
 * replaces) its track. The extension comes from src_path. key_out may be NULL. */
int ipod_device_put(ipod_device *dev, const char *src_path, const ipod_put_info *info,
                    char key_out[IPOD_KEY_LEN], ipod_error *err);

typedef struct ipod_update_info {
  const ipod_tags *tags;    /* NULL: leave tags and cover alone */
  int keep_cover;           /* with tags: leave the cover as it is (tags' cover is ignored) */
  int set_plays;            /* replace play_count and last_played */
  uint32_t play_count;
  int64_t last_played;      /* Unix time */
  int set_rating;
  int rating;
} ipod_update_info;

int ipod_device_update(ipod_device *dev, const char *key, const ipod_update_info *info, ipod_error *err);
/* Deletes the file, the track, its artwork and its kithsong.json entry; or, while Play
 * Counts or OTG files wait to be taken in, marks it to go once they're done (see the top). */
int ipod_device_remove(ipod_device *dev, const char *key, ipod_error *err);
/* Whether key's removal is put off (it's gone as far as the caller is concerned). */
int ipod_device_removal_deferred(const ipod_device *dev, const char *key);

/* Replaces (or creates) the playlist called name with these keys, skipping unknown ones.
 * NULL keys with n = 0 deletes it. Never the master or podcast playlist. */
int ipod_device_set_playlist(ipod_device *dev, const char *name, const char *const *keys, size_t n,
                             ipod_error *err);

/* A playlist of Kithsong's for ipod_device_set_kith_playlists(). */
typedef struct ipod_kith_playlist_in {
  const char *uid;          /* the library's */
  const char *name;
  const char *const *keys;
  size_t n;
} ipod_kith_playlist_in;

/* Makes Kithsong's playlists on the iPod exactly these: earlier ones (recorded in
 * kithsong.json) not among them are deleted, renamed ones renamed, and the rest replaced.
 * Playlists Kithsong didn't make are never touched; a new one whose name one of those has
 * already is written as "<name> (Kithsong)". */
int ipod_device_set_kith_playlists(ipod_device *dev, const ipod_kith_playlist_in *lists, size_t n,
                                   ipod_error *err);

typedef struct ipod_play {
  char key[IPOD_KEY_LEN];
  uint32_t count;         /* plays since the last sync */
  int64_t last_played;    /* Unix time (the iPod's local time read with this computer's offset) */
  int rating;             /* -1 when the firmware doesn't record it */
} ipod_play;

/* Plays recorded since the last sync, read at open. The array stays owned by dev. */
const ipod_play *ipod_device_plays(const ipod_device *dev, size_t *n);
/* The Play Counts file's bytes as read at open (for an "already imported" hash), or NULL. */
const uint8_t *ipod_device_playcounts_bytes(const ipod_device *dev, size_t *len);

typedef struct ipod_otg_playlist {
  char name[32];  /* "On-The-Go 1", ... */
  char (*keys)[IPOD_KEY_LEN];
  size_t n;
} ipod_otg_playlist;

const ipod_otg_playlist *ipod_device_otg_playlists(const ipod_device *dev, size_t *n);
/* The OTG files' bytes as read at open, one after another (each after its file number as a
 * little-endian u32), for an "already imported" hash; NULL when there are none. */
const uint8_t *ipod_device_otg_bytes(const ipod_device *dev, size_t *len);

/* A random value that changes each time ipod_device_done() has cleared everything waiting,
 * kept on the iPod (iPod_Control/kithsong-records), "" before the first time. Hashed into
 * "already imported" tokens, so On-The-Go lists made again later with the same tracks (the
 * same bytes) aren't taken for ones already imported. It never changes while anything
 * still waits, so a token for that comes out the same after an unplug. */
const char *ipod_device_records_nonce(const ipod_device *dev);

enum { IPOD_DONE_PLAYS = 1, IPOD_DONE_PLAYLISTS = 2 };
/* What the caller has taken in, safely: deletes Play Counts (IPOD_DONE_PLAYS) and the OTG
 * files (IPOD_DONE_PLAYLISTS) now; once neither is waiting, renews the records nonce and
 * carries out the removals that were put off. Plays and playlists read at open are gone
 * afterwards. */
int ipod_device_done(ipod_device *dev, unsigned what, ipod_error *err);

/* Writes what changed: the ArtworkDB and .ithmb files, the iTunesDB (the first commit of a
 * session keeps a copy as iTunesDB.kithsong-backup) and kithsong.json. Never touches Play
 * Counts or the OTG files (see the top). */
int ipod_device_commit(ipod_device *dev, ipod_error *err);

#ifdef __cplusplus
}
#endif

#endif
