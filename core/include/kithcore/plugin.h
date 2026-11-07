/* The plugin ABI: what a Kithsong plugin exports, and what core offers it. See
 * docs/plugins.md.
 *
 * A plugin is a shared library (.dll on Windows, .so on Linux) that exports one function:
 *
 *   KITH_PLUGIN_EXPORT const kith_plugin *kith_plugin_init(const kith_host *host);
 *
 * Core calls it once, after loading the library, with a host table of its own for that
 * plugin (keep the pointer: it stays valid until the plugin is unloaded). The plugin answers
 * with a kith_plugin that stays valid as long, or NULL if it can't run (say why through
 * host->log first).
 *
 * This header is all a plugin needs: it includes no other kithcore header and uses only C
 * types. Conventions:
 *  - Strings are UTF-8, file paths included, on Windows too. Paths use the platform's own
 *    separators.
 *  - Times are milliseconds since the Unix epoch (int64_t), whatever the device stores.
 *  - Callbacks return KITH_OK (0) or a negative kith_status, unless noted.
 *  - Memory crosses the boundary only one way at a time: what core returns is freed with
 *    host->free(), and what a plugin returns with its own kith_plugin.free, because the two
 *    may use different C runtimes. Most data is lent rather than returned: pointers in
 *    structs passed to a callback are valid only during that call, and lists are handed
 *    over item by item through visitor callbacks, so neither side allocates for the other.
 *  - Threads: core calls a device target's callbacks for one open target from one thread at
 *    a time, though not always the same thread. An encoder's encode() may run on several
 *    threads at once, on different files, so it must not keep state between calls in
 *    globals. The host functions may be called from any thread.
 *  - The ABI version must match exactly: core refuses a plugin whose kith_plugin.abi isn't
 *    KITH_PLUGIN_ABI, and a plugin should refuse a host whose kith_host.abi isn't. There are
 *    no compatibility shims before 1.0; any change to this file bumps KITH_PLUGIN_ABI.
 *  - Structs that core or a plugin fills in start with what's needed to check them: abi
 *    and size in the two top-level tables. Other structs are fixed by the ABI version. */
#ifndef KITHCORE_PLUGIN_H
#define KITHCORE_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KITH_PLUGIN_ABI 1

/* What kith_plugin_init() is declared with, so it's exported with C linkage. */
#ifdef __cplusplus
#define KITH_PLUGIN_EXTERN_C extern "C"
#else
#define KITH_PLUGIN_EXTERN_C
#endif
#if defined(_WIN32)
#define KITH_PLUGIN_EXPORT KITH_PLUGIN_EXTERN_C __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define KITH_PLUGIN_EXPORT KITH_PLUGIN_EXTERN_C __attribute__((visibility("default")))
#else
#define KITH_PLUGIN_EXPORT KITH_PLUGIN_EXTERN_C
#endif

/* The name core looks the entry point up by. */
#define KITH_PLUGIN_INIT_SYMBOL "kith_plugin_init"

/* Results. The values match kithcore's mc_status where they mean the same, so core passes
 * them on as they are. */
typedef enum kith_status {
  KITH_OK = 0,
  KITH_ERR_UNSUPPORTED = -1, /* not something this plugin or device does */
  KITH_ERR_INVALID_ARG = -2,
  KITH_ERR_NOMEM = -3,
  KITH_ERR_IO = -4,          /* reading or writing the device or a file failed */
  KITH_ERR_FORMAT = -7,      /* a file or database it can't make sense of */
  KITH_ERR_NOT_FOUND = -8,   /* no such key, file or setting */
  KITH_ERR_CANCELLED = -16,  /* host->cancelled() said to stop; nothing half-done is left */
  KITH_ERR_FULL = -100       /* the device has no room for it */
} kith_status;

/* Longest key a device target gives a track, NUL included. */
#define KITH_KEY_LEN 64
/* A textual UUID, NUL included: track and file uids. */
#define KITH_UID_LEN 37
/* A hex SHA-256, NUL included. */
#define KITH_SHA256_LEN 65

/* Opaque handles. kith_source and kith_job are core's; kith_target is the plugin's. */
typedef struct kith_source kith_source; /* decoded audio, from host->decode_open() */
typedef struct kith_job kith_job;       /* a sync or encode in progress, for host->cancelled() */
typedef struct kith_target kith_target; /* an open device, from kith_device_target.open() */

/* --- Audio --- */

/* How samples are laid out in what host->decode_read() fills: interleaved, frames ×
 * channels of one of these. */
typedef enum kith_sample_type {
  KITH_SAMPLE_DEFAULT = 0, /* in a request: keep what it is now */
  KITH_SAMPLE_S16,         /* int16_t */
  KITH_SAMPLE_S32,         /* int32_t, the sample in the top `bits` bits (so a 24-bit sample
                            * is shifted left by 8), the rest zero */
  KITH_SAMPLE_F32          /* float, -1.0 to 1.0 */
} kith_sample_type;

/* A PCM format. In a request, 0 (or KITH_SAMPLE_DEFAULT) in a field keeps that field as it
 * is; a max_ field caps without raising. */
typedef struct kith_pcm_format {
  int sample_rate;           /* Hz: resampled to exactly this */
  int channels;              /* exactly this: more are downmixed, mono is copied to both */
  int bits;                  /* significant bits per sample, 16 or 24 (or 32 for F32);
                              * fewer than the source's are dithered (TPDF) */
  kith_sample_type sample;
  /* Limits, applied after the exact fields above: a source over them is brought down to
   * them (a 96 kHz source with max_sample_rate 48000 comes out at 48 kHz; 44.1 kHz stays as
   * it is). 0 for none. */
  int max_sample_rate;
  int max_channels;
  int max_bits;
} kith_pcm_format;

/* What a source delivers, and what it was decoded from. */
typedef struct kith_source_info {
  kith_pcm_format format;   /* what decode_read() fills now (limits all 0) */
  int source_rate;          /* the file's own */
  int source_channels;
  int source_bits;          /* 0 for a lossy file */
  int lossless;             /* the file is FLAC, WAV, AIFF or ALAC */
  int64_t frames;           /* at format.sample_rate; 0 if unknown */
  int duration_ms;          /* 0 if unknown */
} kith_source_info;

/* A track's tags, lent by core (valid during the call, or until decode_close() for
 * decode_tags()). NULL or 0 for unknown. */
typedef struct kith_tags {
  const char *title;
  const char *artist;       /* the track's artist credit, as shown ("A feat. B") */
  const char *album;
  const char *album_artist;
  const char *genre;
  const char *composer;     /* NULL: Kithsong doesn't keep composers yet */
  const char *mb_trackid;   /* MusicBrainz recording */
  const char *mb_albumid;   /* MusicBrainz release */
  int year;
  int track_no, track_total;
  int disc_no, disc_total;
  int compilation;          /* the album is by various artists */
} kith_tags;

/* An image, lent by core. data NULL if there's none. */
typedef struct kith_cover {
  const unsigned char *data;
  size_t size;
  const char *mime;         /* "image/jpeg" or "image/png" */
} kith_cover;

/* --- Settings ---
 * A plugin's settings are declared, not drawn: Settings › Plugins shows each option under
 * the plugin, with a control for its kind, and stores its value under the plugin's id in
 * settings.json, where host->settings_get() reads it as text. */

typedef enum kith_option_kind {
  KITH_OPTION_SWITCH, /* "1" or "0" */
  KITH_OPTION_CHOICE, /* one of choices[].id */
  KITH_OPTION_NUMBER  /* a whole number from min to max, in decimal */
} kith_option_kind;

typedef struct kith_option_choice {
  const char *id;    /* what's stored */
  const char *label; /* what's shown, in the UI language */
} kith_option_choice;

typedef struct kith_option {
  const char *key;          /* unique within the plugin: [a-z0-9_]+ */
  kith_option_kind kind;
  const char *label;        /* in the UI language (see host->string()) */
  const char *description;  /* a line under it, or NULL */
  const char *def;          /* the value when none is stored */
  const kith_option_choice *choices; /* KITH_OPTION_CHOICE */
  int choice_count;
  int64_t min, max;         /* KITH_OPTION_NUMBER */
} kith_option;

/* --- Logging --- */

typedef enum kith_log_level {
  KITH_LOG_DEBUG,
  KITH_LOG_INFO,
  KITH_LOG_WARNING,
  KITH_LOG_ERROR
} kith_log_level;

/* --- The host table ---
 * What a plugin may call. Each plugin gets its own copy, and the functions that need to
 * know which plugin is calling take that copy as `self`. */
typedef struct kith_host kith_host;
struct kith_host {
  int abi;     /* KITH_PLUGIN_ABI */
  int size;    /* sizeof(kith_host) */

  /* To the app's log, prefixed with the plugin's id. */
  void (*log)(const kith_host *self, kith_log_level level, const char *message);
  /* Frees what a host function returned. NULL is a no-op. */
  void (*free)(void *ptr);

  /* Decoding. Opens path (any format core reads: FLAC, MP3, WAV, AIFF, Opus) for reading as
   * PCM in the format want asks for (NULL: as the file is, as F32). The source also has the
   * track's tags and cover: the library's, when core opened it for an encode; the file's
   * own, when a plugin opened it. */
  int (*decode_open)(const char *path, const kith_pcm_format *want, kith_source **out);
  /* What the source delivers and came from. */
  int (*decode_info)(kith_source *src, kith_source_info *out);
  /* Changes what the source delivers, before the first decode_read() only
   * (KITH_ERR_INVALID_ARG after). Fields left 0 keep the current value; limits already
   * applied stay applied, so an encoder asking for S16 from a source core opened capped at
   * 16 bits and 48 kHz still gets at most 48 kHz. */
  int (*decode_set_format)(kith_source *src, const kith_pcm_format *want);
  /* Reads up to frames frames into buf (frames × channels samples of the source's sample
   * type). Returns the frames read, fewer only at the end and 0 there, or a negative
   * kith_status. */
  int64_t (*decode_read)(kith_source *src, void *buf, int64_t frames);
  /* Closes a source the plugin opened. Never close one core passed to encode(). */
  void (*decode_close)(kith_source *src);
  /* The track's tags, valid until the source is closed. */
  int (*decode_tags)(kith_source *src, kith_tags *out);
  /* The album's cover, valid until the source is closed. KITH_ERR_NOT_FOUND if none. */
  int (*decode_cover)(kith_source *src, kith_cover *out);

  /* The plugin's own settings, stored under its id in settings.json. settings_get returns
   * the stored text (free with host->free) or NULL if there's none (use the option's def).
   * settings_set stores value (NULL removes it); it reaches settings.json the next time
   * the frontend saves its settings. Keys need not be declared options: a plugin may keep
   * small state of its own here. */
  char *(*settings_get)(const kith_host *self, const char *key);
  int (*settings_set)(const kith_host *self, const char *key, const char *value);

  /* Core's strings, by key. Only "language" for now: the UI language as a BCP 47 tag
   * ("en", "de", "pt-BR"), so a plugin can pick its own translations. Lent for as long as
   * the plugin is loaded; NULL for a key core doesn't know. */
  const char *(*string)(const char *key);

  /* Whether the user stopped job: long callbacks (put(), encode()) check now and then, and
   * return KITH_ERR_CANCELLED, leaving nothing half-written. A NULL job is never cancelled. */
  int (*cancelled)(const kith_job *job);
};

/* --- Encoders ---
 * An encoder turns a decoded source into a file of one format. Core caches what it makes
 * (<config>/transcode-cache/<file_uid>/<format>-<setting>.<ext>) and decides when to call
 * it; the encoder only encodes. */

typedef struct kith_encoder_setting {
  const char *id;    /* stored and in file names: [a-z0-9]+, such as "v0" or "320" */
  const char *label; /* in the UI language: "V0 (about 245 kbps)" */
  int kbps;          /* what it averages, for size estimates; 0 for lossless (core
                      * estimates those from the source) */
} kith_encoder_setting;

typedef struct kith_encoder_format {
  const char *id;        /* "alac", "mp3": [a-z0-9]+, unique across plugins */
  const char *name;      /* "ALAC", "MP3": shown as is */
  const char *extension; /* without the dot: "m4a", "mp3" */
  int lossless;
  /* Its quality settings, best first. A format with none (lossless ones, usually) has
   * setting_count 0 and is asked for with setting "". */
  const kith_encoder_setting *settings;
  int setting_count;
  int default_setting;   /* index into settings */
  /* What it takes: decode_set_format() fields it needs (0 for any). Core opens the source
   * with these already applied, on top of the device's limits. */
  kith_pcm_format input;
} kith_encoder_format;

typedef struct kith_encoder {
  const kith_encoder_format *formats;
  int format_count;
  /* Reads src to its end and writes out_path in format (one of formats[].id) at setting
   * (one of its settings' ids, or ""). The source has been opened in the format's input
   * format and the device's limits; decode_info() says what it is. Writes the tags and
   * cover from decode_tags() and decode_cover(). out_path is a part file of core's: write it
   * whole, close it, and return KITH_OK; on failure core deletes it. Called on several
   * threads at once for different files. job may be NULL. */
  int (*encode)(kith_source *src, const char *format, const char *setting, const char *out_path,
                const kith_job *job);
} kith_encoder;

/* --- Device targets ---
 * A device that can't run Kithsong: core works out what it should hold, and the plugin
 * writes it. Every callback but probe() takes the kith_target that open() made. */

/* A format the device plays, with its limits (0 for none). */
typedef struct kith_device_format {
  const char *format;   /* an encoder format id, or a file format core reads: "alac",
                         * "mp3", "aac" */
  int max_sample_rate;  /* Hz */
  int max_bits;
  int max_channels;
  int max_kbps;         /* lossy files over this are re-encoded; 0 for any */
} kith_device_format;

/* What open() reports, lent by the plugin until close(). */
typedef struct kith_device_info {
  /* Stable across plugs and computers: for the iPod, its FireWire GUID, else its serial
   * number. [A-Za-z0-9._-], at most 63 characters. */
  const char *id;
  const char *name;       /* the device's own name, as its user gave it */
  const char *model;      /* shown under the name: "iPod nano (2nd generation)"; may be NULL */
  int64_t capacity_bytes;
  int64_t free_bytes;     /* now, before anything is written */
  const kith_device_format *formats;
  int format_count;
  /* The format policy's defaults (see "Format policy" in docs/plugins.md): what lossless
   * sources are written as, and what lossy ones in a format the device doesn't play are
   * re-encoded to. Each an encoder format id and one of its setting ids (or ""). */
  const char *lossless_format, *lossless_setting;
  const char *lossy_format, *lossy_setting;
  int has_playlists;      /* playlists_write() does something */
} kith_device_info;

/* A track on the device, lent by the plugin during list()'s visitor call. */
typedef struct kith_device_track {
  const char *key;        /* the plugin's own, < KITH_KEY_LEN */
  /* What Kithsong wrote it as (from the device's own record, such as kithsong.json), or
   * NULL for a track Kithsong didn't write: core never changes or removes those. */
  const char *track_uid;
  const char *file_uid;   /* the original it was made from; NULL as track_uid */
  const char *setting;    /* the encoder setting it was written at, "" for a copy as is */
  const char *sha256;     /* of the file as written; NULL if not recorded */
  const char *format;     /* "alac", "mp3", "aac" */
  int bitrate_kbps;       /* 0 if unknown */
  int64_t size_bytes;
  /* For matching tracks Kithsong didn't write to the library (plays and playlists). */
  const char *title, *artist, *album;
  int duration_ms;
  /* What the device shows now. */
  int play_count;
  int64_t last_played_ms; /* 0 if never */
} kith_device_track;

/* A track for put() or update(), lent by core during the call. */
typedef struct kith_track {
  char track_uid[KITH_UID_LEN];
  char file_uid[KITH_UID_LEN];  /* the original the file was made from */
  const char *format;           /* the file's: an encoder format id, or "mp3" for a copy */
  const char *setting;          /* the encoder setting, "" for a copy as is */
  char sha256[KITH_SHA256_LEN]; /* of the file put() is given; "" in update() */
  int64_t size_bytes;           /* of that file; 0 in update() */
  int bitrate_kbps;             /* nominal; 0 if unknown */
  int sample_rate;
  int duration_ms;
  kith_tags tags;
  kith_cover cover;             /* the album's; data NULL if none */
  int play_count;               /* the library's total, the device's own plays included */
  int64_t last_played_ms;       /* 0 if never played */
} kith_track;

/* A playlist core wants on the device (playlists_write()), lent during the call. */
typedef struct kith_playlist {
  const char *uid;              /* the library's */
  const char *name;
  const char *const *keys;      /* device track keys, in order */
  int count;
} kith_playlist;

/* Plays made on the device since they were last cleared (plays_read()). */
typedef struct kith_device_play {
  const char *key;
  int count;                    /* plays since the last clear, at least 1 */
  int64_t last_played_ms;       /* the latest one's time */
} kith_device_play;

/* A playlist made on the device (playlists_read()). */
typedef struct kith_device_playlist {
  const char *name;             /* the device's own, if it names them; may be NULL */
  const char *const *keys;
  int count;
} kith_device_playlist;

/* What device_done() clears. */
#define KITH_DONE_PLAYS 1u
#define KITH_DONE_PLAYLISTS 2u

/* Visitors: core's, called by the plugin once per item. Return 0 to go on, non-zero to
 * stop (the callback then returns KITH_OK). */
typedef int (*kith_track_fn)(void *ctx, const kith_device_track *track);
typedef int (*kith_play_fn)(void *ctx, const kith_device_play *play);
typedef int (*kith_playlist_fn)(void *ctx, const kith_device_playlist *playlist);

typedef struct kith_device_target {
  const char *id;    /* unique within the plugin: "ipod" */
  const char *name;  /* the kind of device, shown in the device list: "iPod" */

  /* Whether the volume mounted at mount_path is one of this target's devices: 1 or 0. Cheap
   * (a few small reads), never writes. */
  int (*probe)(const char *mount_path);
  /* Opens it, filling *info (lent until close()). */
  int (*open)(const char *mount_path, kith_target **out, kith_device_info *info);
  /* Calls each for every track on the device, Kithsong's or not. */
  int (*list)(kith_target *t, kith_track_fn each, void *ctx);
  /* Adds a track from the encoded file at file_path (core's: copy it, don't move it), with
   * track's tags, cover, play count and last played time. key is in and out: "" to add a
   * new track, or the key of one to replace; on KITH_OK it holds the key the track is
   * under. Records track_uid, file_uid, setting and sha256 on the device, for list().
   * KITH_ERR_FULL if it doesn't fit. */
  int (*put)(kith_target *t, const kith_track *track, const char *file_path, char key[KITH_KEY_LEN],
             const kith_job *job);
  /* Rewrites key's tags, play count and last played time from track, without copying the
   * file again (track's sha256 and size_bytes are empty). */
  int (*update)(kith_target *t, const char *key, const kith_track *track);
  /* Removes key's track and its file. */
  int (*remove)(kith_target *t, const char *key);
  /* The path of key's file on the volume, for core to hash and compare with what it put()
   * (free with kith_plugin.free). NULL if there's no telling: the check is then skipped. */
  char *(*file_path)(kith_target *t, const char *key);

  /* Optional (NULL if the device has none). Plays made on the device since they were last
   * cleared: calls each for every track played. token (in KITH_SHA256_LEN bytes) is filled
   * with what identifies this set of plays, such as the hash of the iPod's Play Counts
   * file, or "" if there are none; with each NULL only token is filled. Core takes a set in
   * once: one whose token it has seen is cleared without being read. */
  int (*plays_read)(kith_target *t, char token[KITH_SHA256_LEN], kith_play_fn each, void *ctx);
  /* Optional. Playlists made on the device, such as the iPod's On-The-Go playlists, with a
   * token as for plays_read(). */
  int (*playlists_read)(kith_target *t, char token[KITH_SHA256_LEN], kith_playlist_fn each,
                        void *ctx);
  /* Optional (needs has_playlists). Makes the device's Kithsong playlists these, replacing
   * every one an earlier call made; playlists made on the device or by other software are
   * left alone. Before commit(). */
  int (*playlists_write)(kith_target *t, const kith_playlist *lists, int count);
  /* Clears what core has taken in (KITH_DONE_PLAYS, KITH_DONE_PLAYLISTS), once it is safely
   * in the library. It may wait for the next commit(). NULL if the device has neither. */
  int (*device_done)(kith_target *t, unsigned what);
  /* Writes the device's database. Core calls it every 50 tracks or 2 minutes during a
   * sync, and at the end, so unplugging loses little. */
  int (*commit)(kith_target *t);
  /* Closes it, without committing. */
  void (*close)(kith_target *t);
  /* Optional. Flushes and ejects, if the device needs more than the OS's own eject. Called
   * on a closed device's mount path. */
  int (*eject)(const char *mount_path);
} kith_device_target;

/* --- The plugin --- */

typedef struct kith_plugin {
  int abi;               /* KITH_PLUGIN_ABI */
  int size;              /* sizeof(kith_plugin) */
  const char *id;        /* reverse-DNS: "ng.kithso.ipod" */
  const char *name;      /* "iPod sync", in the UI language */
  const char *version;   /* "1.0.0" */
  /* Frees what this plugin returned (file_path()). */
  void (*free)(void *ptr);
  /* Optional. Called once before the library is unloaded, after every session using the
   * plugin has ended. */
  void (*shutdown)(void);

  const kith_option *options;
  int option_count;
  const kith_device_target *device_targets;
  int device_target_count;
  const kith_encoder *encoders;
  int encoder_count;
} kith_plugin;

typedef const kith_plugin *(*kith_plugin_init_fn)(const kith_host *host);

#ifdef __cplusplus
}
#endif

#endif
