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

/* Opening a library last opened by a version before artist credits (schema v13) starts a
 * refresh in the background, once: credits such as "A feat. B" imported as one artist are
 * split into their artists, from MusicBrainz where the album has a release id and it can be
 * reached, else from the string, and the files' artist tags rewritten to match. It needs
 * mc_init(), starts a few seconds after the open, goes on from where it was if the library
 * was closed, and tries MusicBrainz again on a later open if it couldn't reach it. Nothing
 * waits for it. This says whether this handle's refresh is running (1) or not (0), and how
 * many of the albums and tracks with no album it found to do are done; done and total may
 * be NULL. A frontend may show it; it needn't. */
int mc_library_refresh_progress(mc_library *lib, int *done, int *total);

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

/* Numbers about some of the library's music. */
typedef struct mc_library_tally {
  int64_t tracks;
  int64_t albums;      /* as mc_library_list_albums() has them: singles left out, mixes in */
  int64_t singles;     /* albums of one track that aren't mixes, and tracks with no album */
  int64_t artists;     /* as mc_library_list_artists() and mc_library_list_artists_here() */
  int64_t genres;      /* the tracks' different genres, whatever their case */
  int64_t duration_ms; /* the tracks' lengths; one not known adds nothing */
} mc_library_tally;

typedef struct mc_library_stats {
  mc_library_tally all;  /* everything in the library, here or on other devices */
  /* What has a file on this device: the test that gives mc_track_info a path and mc_album_info
   * a folder. An album or a single stays what the whole library makes it, so an album with
   * one of its tracks here is an album. */
  mc_library_tally here;
  /* The files on this device, a track's transcodes as well as its original. size_bytes is what
   * the library recorded for them; library.db and the covers aren't in it. */
  int64_t files;
  int64_t size_bytes;
  /* Each as saved, here or not, so with the example smart lists until they're deleted, and
   * whether or not MC_LIST_PLAYABLE would list them. */
  int64_t playlists; /* by hand; smart ones are next */
  int64_t smart_playlists;
  int64_t album_groups;
  int64_t smart_album_groups;
  /* On every device, of the tracks still in the library. plays is what plays counted
   * (mc_library_record_play()), tracks_played how many tracks they were of, and listened_ms
   * the time heard, from the play log, which only goes back to schema v18. */
  int64_t plays;
  int64_t tracks_played;
  int64_t listened_ms;
} mc_library_stats;

/* Fills in out, all of it as of one moment. A few aggregate queries, cheap enough to read
 * whenever a page opens, though keep it off a thread that mustn't wait on the disk. */
mc_status mc_library_read_stats(mc_library *lib, mc_library_stats *out);

/* Counts a play of track_uid on this device and makes now its last play. What counts as a
 * play (say, half the track) is the caller's call. MC_ERR_NOT_FOUND if there's no such track. */
mc_status mc_library_record_play(mc_library *lib, const char *track_uid);

/* The play log: one row for each time a track was started, counted as a play or not, which
 * is the raw history a home screen or radio can read back. The player fills a row in (see
 * mc_player_take_ended()) and the frontend hands it to mc_library_log_play(). Rows are this
 * device's own, are never changed once written, and are kept for good; see docs/schema.md.
 * Its types are in musiccore/play_log.h, as the player has them too. */

/* Adds a row, as this device's. MC_ERR_NOT_FOUND if the track has gone meanwhile. */
mc_status mc_library_log_play(mc_library *lib, const mc_play_event *event);

typedef struct mc_play_log_item {
  char uid[MC_UID_STRLEN];
  char node_uid[MC_UID_STRLEN]; /* the device it was played on */
  mc_play_event event;
} mc_play_log_item;

typedef struct mc_play_log {
  mc_play_log_item *items;
  int count;
} mc_play_log;

/* The rows of every device with from_ms <= started_ms < to_ms, oldest first (ties by uid, so
 * every device reads the same order). Wall clocks differ between devices, so rows of two
 * devices may not interleave quite as they happened. Filtering, say of very short plays or
 * failed ones, is the reader's. */
mc_status mc_library_list_play_events(mc_library *lib, int64_t from_ms, int64_t to_ms, mc_play_log **out);
void mc_play_log_free(mc_play_log *log);

typedef struct mc_album_info {
  char uid[MC_UID_STRLEN];
  char *title;
  char *artist;                   /* its artist credit as shown ("A & B") */
  char artist_uid[MC_UID_STRLEN]; /* its first artist; "" if it has no artist */
  int year;
  /* The album has exactly one track in the library and isn't a continuous mix (edit.h): a
   * single (see mc_library_list_artist_singles()). Always 0 from mc_library_list_albums() and
   * mc_library_list_artist_albums(), which leave singles out. */
  int is_singles;
  int has_art;    /* folder holds a cover (see mc_coverart_find()) */
  char *folder;   /* absolute path of the album's folder; NULL if it has no files here */
  /* From mc_library_list_artist_albums() only (0 elsewhere): the album's tracks, and how
   * many of them are credited to the artist. */
  int track_count;
  int credited_count;
} mc_album_info;

typedef struct mc_album_list {
  mc_album_info *items;
  int count;
} mc_album_list;

/* Every album but the singles (albums with one track in the library, which the Artist view
 * shows instead: see mc_library_list_artist_singles(); a continuous mix of one track isn't
 * one, see edit.h), in display order: by artist (sort name), then by year and title. One query; cheap enough to call again after an import.
 * Free with mc_album_list_free(). */
mc_status mc_library_list_albums(mc_library *lib, mc_album_list **out);
void mc_album_list_free(mc_album_list *list);

/* The cover in an album's folder (mc_album_info.folder, say): the first of cover, folder
 * and front, each as .jpg, .jpeg or .png, in any case. The same file the importer writes and
 * sync sends, so frontends needn't know the names. A full path to free with mc_free(), or
 * NULL if there's none. Lists the folder, so keep it off a thread that mustn't wait on the
 * disk. */
char *mc_coverart_find(const char *dir);

typedef struct mc_track_info {
  char uid[MC_UID_STRLEN];
  char *title;
  int track_no; /* 0 if unknown */
  int disc_no;  /* 0 if unknown */
  int duration_ms;
  char *path; /* absolute path of the track's file on this device (the original, if there are
               * transcodes too); NULL if there's none here */
  char *artist;    /* the track's artist credit ("A feat. B"), else its album's; NULL if unknown */
  char artist_uid[MC_UID_STRLEN]; /* its first artist's uid; "" if unknown */
  char *album;     /* NULL if it has none */
  char *cover_dir; /* the folder holding its album's cover, or NULL if there's none here */
  char entry_uid[MC_UID_STRLEN]; /* its playlist entry, for mc_playlist_move() and
                                  * mc_playlist_remove(); "" outside playlists */
  int year;                      /* the track's own, else its album's; 0 if unknown */
  char album_uid[MC_UID_STRLEN]; /* "" if it has no album */
} mc_track_info;

typedef struct mc_track_list {
  mc_track_info *items;
  int count;
} mc_track_list;

/* The one track with track_uid, or an empty list if the library has no such track (it was
 * deleted, say, while it played on). Free with mc_track_list_free(). */
mc_status mc_library_list_track(mc_library *lib, const char *track_uid, mc_track_list **out);
/* An album's tracks in order: by disc, then track number (unnumbered tracks last), then
 * title. Free with mc_track_list_free(). */
mc_status mc_library_list_album_tracks(mc_library *lib, const char *album_uid, mc_track_list **out);
/* What playing several albums in turn queues, as an album group does: the tracks with a file
 * on this device of count albums, the albums in the order given and each one's tracks in its
 * own. An album with none here, or that's gone, adds nothing and the rest carry on. Each
 * track's album_uid says which album it's from. Free with mc_track_list_free(). */
mc_status mc_library_list_albums_tracks(mc_library *lib, const char *const *album_uids, int count,
                                        mc_track_list **out);
/* A playlist's tracks in playlist order. A track that's in it twice is listed twice. */
mc_status mc_library_list_playlist_tracks(mc_library *lib, const char *playlist_uid, mc_track_list **out);
void mc_track_list_free(mc_track_list *list);

/* Flags for listing playlists, album groups, smart lists and recents.
 *
 * MC_LIST_PLAYABLE leaves out what's empty only here, as selective sync leaves it: a playlist
 * with tracks none of which has a file here (an original, or a copy or transcode sync
 * brought); an album group with albums none of which has one; a smart playlist that picks
 * tracks, or a smart album group that picks albums, none of them with a file here. What's
 * empty everywhere is kept, as it's waiting for tracks rather than missing them: a playlist
 * or album group with nothing in it, and a smart list whose rule picks nothing in the whole
 * library. But not a smart list on a device with no files at all, whose empty pages say what
 * to do next (the example smart lists would fill them), nor one whose rule can't be evaluated
 * (MC_ERR_FORMAT, e.g. from a newer app). "Has a file here" is the
 * same test that gives a mc_track_info a path, which is what the track lists grey out or
 * hide by. Views that are about playing (the Playlists page, recents) pass it; pickers that
 * are about choosing (Add to playlist, a sync profile's playlists) don't, so a playlist that
 * has nothing here yet can still be added to or synced. A frontend lists again whenever files come or go (an import,
 * sync bringing or removing files, a delete), as it does for any library change. */
#define MC_LIST_PLAYABLE 1u

typedef struct mc_playlist_info {
  char uid[MC_UID_STRLEN];
  char *name;
  int track_count;    /* entries, a track that's in it twice counted twice */
  int playable_count; /* the entries whose track has a file on this device */
} mc_playlist_info;

typedef struct mc_playlist_list {
  mc_playlist_info *items;
  int count;
  int hidden; /* playlists MC_LIST_PLAYABLE left out, e.g. to say why the list is empty */
} mc_playlist_list;

/* Every playlist, by name; with MC_LIST_PLAYABLE in flags, only those with a track to play
 * here or no tracks at all. Free with mc_playlist_list_free(). */
mc_status mc_library_list_playlists(mc_library *lib, unsigned flags, mc_playlist_list **out);
void mc_playlist_list_free(mc_playlist_list *list);

typedef struct mc_artist_info {
  char uid[MC_UID_STRLEN];
  char *name;
  /* What artists are ordered by: its sort name ("Beatles, The"), or its name if it has none. */
  char *sort_name;
  int album_count; /* albums credited to it (as an album artist, featured or not), singles
                    * left out (as mc_library_list_albums() leaves them); 0 if it's only on
                    * tracks */
  /* From mc_library_list_artists_here() only (0 elsewhere): the tracks credited to it (see
   * mc_library_list_artist_albums()) that have a file on this device. */
  int track_count;
  /* The artist's picture: the folder holding the cover of their first album here that has
   * one (as mc_album_info.folder; see mc_coverart_find()), their own albums (artist_uid
   * theirs) by year then title, singles left out. NULL if none has a cover here, for which
   * frontends show a placeholder. */
  char *cover_dir;
} mc_artist_info;

typedef struct mc_artist_list {
  mc_artist_info *items;
  int count;
} mc_artist_list;

/* Every artist, album artists and those credited only on tracks alike, by sort name. Free
 * with mc_artist_list_free(). */
mc_status mc_library_list_artists(mc_library *lib, mc_artist_list **out);
/* The artists with something to play on this device, in the same order: album_count is
 * their albums with a file here, track_count their credited tracks with a file here, and
 * one of them is above 0. "A file here" is the test that gives mc_album_info a folder and
 * mc_track_info a path. Artists are told apart by uid, so two of the same name are listed
 * and counted apart. Free with mc_artist_list_free(). */
mc_status mc_library_list_artists_here(mc_library *lib, mc_artist_list **out);
void mc_artist_list_free(mc_artist_list *list);

/* A track is credited to its own artists, or to its album's when it has none. A credit can
 * name several ("A feat. B", "A & B"): the track or album is each one's, and lists on each
 * one's page. mc_track_info.artist and mc_album_info.artist are the credit as shown, and
 * artist_uid its first artist; mc_library_list_credit() has them all.
 *
 * The albums artist_uid appears on, as an album artist or credited on any track, with
 * track_count and credited_count filled in; singles are left out (see below). Their own
 * albums come first: those credited to them, then by the share of the tracks credited to
 * them, most first; then by year and title. Free with mc_album_list_free(). */
mc_status mc_library_list_artist_albums(mc_library *lib, const char *artist_uid, mc_album_list **out);
/* Every track credited to artist_uid, by title (then album and track number). Free with
 * mc_track_list_free(). */
mc_status mc_library_list_artist_tracks(mc_library *lib, const char *artist_uid, mc_track_list **out);
/* artist_uid's singles: the tracks credited to them, or on an album of theirs, that are the
 * only track of their album in the library, or have no album at all, but for a continuous mix
 * of one track, which is an album. Other albums of one track are left out of the album
 * listings, so this is where they show. By year (the track's, else its
 * album's), then title. album_uid and album are the album's ("" and NULL for a track with
 * none); cover_dir is set when that album has art and the file is here, the same test as
 * mc_album_info.has_art. Free with mc_track_list_free(). */
mc_status mc_library_list_artist_singles(mc_library *lib, const char *artist_uid, mc_track_list **out);

/* One artist of a credit: "A feat. B" is A with join " feat. ", then B with join "". */
typedef struct mc_credit_artist {
  char uid[MC_UID_STRLEN];
  char *name; /* the artist's own name, which the credit may spell otherwise */
  char *join; /* what follows it in the credit; "" after the last */
} mc_credit_artist;

typedef struct mc_credit_list {
  mc_credit_artist *items;
  int count;
} mc_credit_list;

/* The artists track or album uid is credited to, in order, say to link each to their page.
 * A track with no artist of its own has its album's. Empty if there are none;
 * MC_ERR_NOT_FOUND if there's no such track or album. Free with mc_credit_list_free(). */
mc_status mc_library_list_credit(mc_library *lib, const char *uid, mc_credit_list **out);
void mc_credit_list_free(mc_credit_list *list);

typedef struct mc_album_group_info {
  char uid[MC_UID_STRLEN];
  char *name;
  int album_count;    /* its albums, here or not */
  int playable_count; /* those with a file on this device */
} mc_album_group_info;

typedef struct mc_album_group_list {
  mc_album_group_info *items;
  int count;
  int hidden; /* groups MC_LIST_PLAYABLE left out, e.g. to say why the list is empty */
} mc_album_group_list;

/* Every manual album group (smart ones are left out), by name; with MC_LIST_PLAYABLE in
 * flags, only those with an album to play here or no albums at all. Free with
 * mc_album_group_list_free(). */
mc_status mc_library_list_album_groups(mc_library *lib, unsigned flags, mc_album_group_list **out);
void mc_album_group_list_free(mc_album_group_list *list);
/* A manual album group's albums in the group's order, each once. MC_ERR_NOT_FOUND if
 * there's no such group. Free with mc_album_list_free(). */
mc_status mc_library_list_album_group_albums(mc_library *lib, const char *group_uid, mc_album_list **out);

/* Recents: what was last opened, on any device. They're library data and sync with it; an
 * item has one entry everywhere, which the newest open wins. */

typedef enum mc_recent_kind {
  MC_RECENT_ALBUM,
  MC_RECENT_ARTIST,
  MC_RECENT_PLAYLIST,   /* a playlist or a smart playlist */
  MC_RECENT_ALBUM_GROUP /* an album group or a smart album group */
} mc_recent_kind;

/* Notes that item uid, of kind, was opened now, putting it first in the recents (on every
 * device, once it syncs). What counts as opening (say, starting it playing) is the caller's
 * call. MC_ERR_NOT_FOUND if there's no such item of that kind. */
mc_status mc_library_record_opened(mc_library *lib, mc_recent_kind kind, const char *uid);
/* Takes item uid out of the recents until it's next opened. MC_ERR_NOT_FOUND if it isn't in
 * them. */
mc_status mc_library_remove_recent(mc_library *lib, const char *uid);
/* Empties the recents, on every device once it syncs. */
mc_status mc_library_clear_recents(mc_library *lib);

typedef struct mc_recent_info {
  mc_recent_kind kind;
  char uid[MC_UID_STRLEN]; /* the album, artist, playlist or album group */
  int64_t opened_ms;       /* when it was last opened (Unix ms) */
  char *name;              /* its title or name */
  char *artist;            /* an album's artist; NULL for other kinds, or if unknown */
  int is_smart;            /* a smart playlist or smart album group */
} mc_recent_info;

typedef struct mc_recent_list {
  mc_recent_info *items;
  int count;
} mc_recent_list;

/* The recents, most recently opened first, at most limit of them (all if limit <= 0). Items
 * that no longer exist are left out, and with MC_LIST_PLAYABLE in flags so are playlists,
 * album groups and smart lists that are empty only here (as mc_library_list_playlists(),
 * mc_library_list_album_groups() and mc_library_list_smart() leave out; albums and artists
 * are kept). Free with mc_recent_list_free(). */
mc_status mc_library_list_recents(mc_library *lib, int limit, unsigned flags, mc_recent_list **out);
void mc_recent_list_free(mc_recent_list *list);

/* How much of an album or playlist has been played, counting every device's plays of its
 * tracks. A playlist's tracks count however they were played, from an album or anywhere. */
typedef struct mc_played_info {
  mc_recent_kind kind;     /* MC_RECENT_ALBUM or MC_RECENT_PLAYLIST */
  char uid[MC_UID_STRLEN];
  int tracks_played;       /* its tracks with a play, one that's in a playlist twice once */
  int tracks_since;        /* those of them last played at since_ms or later */
  int64_t plays;           /* all their plays, ever */
  int64_t last_played_ms;  /* the newest of them; 0 if only plays too old to have a time */
} mc_played_info;

typedef struct mc_played_list {
  mc_played_info *items;
  int count;
} mc_played_list;

/* Every album and playlist with a play, in no particular order. Read only. Play counts aren't
 * kept by time, so tracks_since says a track's last play was that recent, not how often it's
 * played lately. Free with mc_played_list_free(). */
mc_status mc_library_list_played(mc_library *lib, int64_t since_ms, mc_played_list **out);
void mc_played_list_free(mc_played_list *list);

#ifdef __cplusplus
}
#endif

#endif
