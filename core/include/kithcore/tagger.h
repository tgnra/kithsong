/* Tag reading/writing and the MusicBrainz autotagger. */
#ifndef KITHCORE_TAGGER_H
#define KITHCORE_TAGGER_H

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tags of one audio file. Strings are NULL when absent; numbers are 0 when absent. */
typedef struct mc_tags {
  char *title;
  char *artist;
  char *album;
  char *album_artist;
  char *genre;
  char *mb_trackid;        /* MusicBrainz recording id */
  char *mb_releasetrackid;
  char *mb_albumid;        /* MusicBrainz release id */
  char *mb_artistid;
  char *mb_albumartistid;
  char *mb_releasegroupid;
  /* Multi-valued tags, values separated by '\n' (Picard's convention): ARTISTS and
   * ALBUMARTISTS, one name per credited artist, and every MUSICBRAINZ_ARTISTID /
   * MUSICBRAINZ_ALBUMARTISTID value. On read mb_artistid and mb_albumartistid stay the first
   * value. On write a non-NULL list replaces that tag's values (an empty string removes it),
   * and mb_artistids / mb_albumartistids win over mb_artistid / mb_albumartistid. */
  char *artists;
  char *album_artists;
  char *mb_artistids;
  char *mb_albumartistids;
  /* Read-only, ignored by mc_tags_write(); clues for the importer's extended search. */
  char *label;   /* LABEL, else ORGANIZATION or PUBLISHER */
  char *catalog; /* CATALOGNUMBER, else CATALOGID, CATALOG or LABELNO */
  int year;
  int original_year;
  int track;
  int track_total;
  int disc;
  int disc_total;
  /* Audio properties; read-only, ignored by mc_tags_write(). */
  int duration_ms;
  int bitrate_kbps;
  int sample_rate;
  int channels;
  int bits_per_sample; /* lossless formats (FLAC, WAV, AIFF); else 0 */
} mc_tags;

/* Reads tags and audio properties. On success *out must be released with mc_tags_clear(). */
mc_status mc_tags_read(const char *path, mc_tags *out);

/* Writes every non-NULL / non-zero field of tags to the file, leaving other tags untouched.
 * An empty string removes that tag, and so does a negative year, original year, track or
 * disc. */
mc_status mc_tags_write(const char *path, const mc_tags *tags);

void mc_tags_clear(mc_tags *tags);

/* Connectivity check: fetches a known artist from the MusicBrainz API over HTTPS and
 * returns its name in *out_name (free with mc_free). */
mc_status mc_musicbrainz_ping(char **out_name);

/* Cheap offline check: opens (and closes) a TCP connection to the MusicBrainz server, to all
 * its addresses at once, waiting at most timeout_ms for the name lookup and the connection
 * together, so broken DNS can't hold it longer. MC_OK if it connected, MC_ERR_NET if not. Blocks.
 * For testing, KITHSONG_MUSICBRAINZ_PROBE=host:port in the environment checks that address
 * instead, e.g. 127.0.0.1:1 to simulate being offline. */
mc_status mc_musicbrainz_reachable(int timeout_ms);

/* --- Import (autotagger) ---
 *
 * An import session scans folders on a core-owned thread and groups the audio files into
 * items: an album, or a single (one loose track). Each item is matched against MusicBrainz;
 * if the best candidate reaches the confidence threshold it is imported straight away
 * (copied into the library folder, or moved if already inside it, under Artist/Album (Year)/,
 * tagged, given a cover.jpg and added to the database). The originals are never modified.
 * Otherwise the item waits in MC_IMPORT_REVIEW. Albums are matched before one-file items.
 * The full rules (grouping, disc folders, singles, extended search, zips) are in
 * docs/tagging-rules.md; what frontends see:
 *
 * Disc folders and other discs. Items that turn out to be discs of one release are joined
 * into the first; the others become MC_IMPORT_SKIPPED with MC_IMPORT_OUTCOME_MOVED. An album
 * missing discs that later items may supply stays in MC_IMPORT_PENDING (error says it is
 * waiting) until they're matched. A joined item always goes to review, with error saying so.
 *
 * Singles. A matched loose track is filed as its track on the release it's credited to,
 * looked up in full when written. An album with one track in the library is a single:
 * album listings leave it out, mc_library_list_artist_singles() lists it (library.h).
 *
 * Offline. Before matching, and after a network error, the session checks
 * mc_musicbrainz_reachable(). While unreachable, items go straight to review with
 * unreachable set (no per-item timeout) and MC_IMPORT_EV_OFFLINE is raised; the check
 * repeats every 30 s and waiting items are matched once it connects. Items whose lookups
 * failed while the check still connects stay in review until mc_import_retry_unreachable().
 * Transient failures are retried (2 s, backing off to 30 s, for up to 10 minutes) while the
 * item stays in MC_IMPORT_MATCHING. mc_import_free() cancels a lookup or download in progress.
 *
 * Extended search. When the best candidate of an album or one-file item is below 75% (or
 * there is none), the session searches more widely by itself (catalogue numbers, other
 * artists, the label, the title alone; at most 4 searches and 6 lookups per album). Above
 * 75% mc_import_search_wider() runs it on request. A network error ends it early and leaves
 * searched_wider 0.
 *
 * Duplicates. Before an accepted item is written, it is compared with the library (only
 * files from outside the library folder; files inside it are being re-filed, not added):
 *
 *   - The same release already in the library: an album with the same MusicBrainz release
 *     id, else the same release group id, else the same album artist and title (ignoring
 *     case) with at least half of the incoming tracks in its track list. A track with no
 *     album: the artist's tracks with no album, holding the same recording (or, without
 *     ids, the same title).
 *     Incoming tracks are paired with the album's tracks by release track id, recording id,
 *     disc and track number with the same title, then title alone.
 *   - Quality of one file, best first:
 *       1. lossless beats lossy (lossy: MP3 and Opus; FLAC, WAV and AIFF are lossless);
 *       2. between lossless files: more bits per sample, then a higher sample rate;
 *       3. between lossy files: a higher effective bitrate (bitrate weighted by codec:
 *          MP3 1.0, Opus 1.6), only when more than 10% higher, so VBR noise doesn't count.
 *     Anything else is equal quality.
 *   - The incoming release replaces the library's when, taken as a whole, it is better: it
 *     has a counterpart for every track the library holds a file of here (never a partial
 *     release for a full one), none of its tracks is worse, and more than half are better.
 *     Then every paired track's file is swapped: the new file is written next to the old
 *     one, the library rows are updated in place (the album, tracks and their uids stay, so
 *     playlists, album groups, play counts, recents and sync profiles keep them), and only
 *     then the old file goes to the Recycle Bin / trash (see mc_trash). The track gets a new
 *     tracks.file_rev, which sync uses to replace other devices' copies.
 *   - A swapped-in file becomes a better copy of the track the library has: it is tagged as
 *     the library's track (its title, artist, numbers and MusicBrainz ids, and its album's
 *     title, album artist, year and ids; ids the library lacks are removed from the file) and
 *     takes the old file's name with its own extension, whatever its own match said. So is
 *     a file attached to a track that has no file on this device, named for that track.
 *   - Except for the whole release: when the album was found by release or release group
 *     id, every one of its tracks is paired, and every file of it on this device is
 *     swapped, the item's MusicBrainz match wins instead. The new files keep the match's
 *     tags and names (the old folder goes if that leaves it empty, its cover moving along
 *     unless the item brings one), and the album and track rows take the match's title,
 *     artists, numbers, years and ids, keeping their uids. Not for an album found by name,
 *     a singles album, or an item imported with its existing tags.
 *   - Otherwise the tracks already in the library are skipped: no "(2)" copies.
 *   - Tracks of the release the library lacks are added to the existing album either way.
 *     Short of a whole release, a matched one takes the album's title, album artist, year
 *     and ids rather than its match's, which can be another release of the group, and has
 *     no release track id, as its own is that release's.
 *   - The same file again: every file new to the library records the SHA-256 of its bytes
 *     as they were before the import tagged its copy (files.source_sha256). A file whose
 *     bytes hash to a recorded source hash, or to the SHA-256 of a file in the library, is
 *     skipped as already there. Older rows have no source hash and match by current bytes.
 *
 * mc_import_item.outcome says what happened.
 *
 * Zips. A .zip given to mc_import_start, or found in a given folder, is extracted (one
 * level deep) into a staging folder under mc_import_options.staging_dir and imported like a
 * folder; the staging folder is deleted when the session is freed. A zip that can't be
 * extracted (size limits, free space, bad archive) becomes an item with no files in
 * MC_IMPORT_FAILED, folder = the zip's path, album = its file name, error = the reason.
 *
 * Long paths. An unreadable audio file is normally passed over as not audio, but one whose
 * path is 260 characters or more becomes a failed item the same way, with error
 * MC_STR_IMPORT_ERROR_LONG_PATH, so the frontend can explain Windows' long-path setting.
 *
 * Too big. On 32-bit Android an audio file of 2 GiB (2^31 bytes) or more can't be tagged or
 * played (a 32-bit off_t), so it isn't imported: it becomes a failed item the same way, with
 * error MC_STR_IMPORT_ERROR_TOO_BIG, before its tags are read; one in a zip isn't extracted,
 * and its item's folder is the zip's path, '/', and its path in the zip. Other files that big
 * aren't imported anyway, so they go unremarked (and unextracted). Other builds have no
 * such limit.
 *
 * Callbacks run on the import thread; frontends marshal them to their UI thread. Every
 * other function may be called from any thread. */

typedef struct mc_import mc_import;

typedef enum mc_import_kind {
  MC_IMPORT_ALBUM,  /* a group of files matched to a release */
  MC_IMPORT_SINGLE  /* one loose track matched to a recording */
} mc_import_kind;

typedef enum mc_import_state {
  MC_IMPORT_PENDING,   /* scanned, waiting to be matched; or waiting for its other discs */
  MC_IMPORT_MATCHING,  /* searching MusicBrainz */
  MC_IMPORT_REVIEW,    /* needs the user: low confidence or no candidates */
  MC_IMPORT_WRITING,   /* accepted; tags, art and library rows being written */
  MC_IMPORT_DONE,
  MC_IMPORT_SKIPPED,
  MC_IMPORT_FAILED
} mc_import_state;

typedef enum mc_import_event {
  MC_IMPORT_EV_SCANNING,     /* arg: audio files found so far */
  MC_IMPORT_EV_SCANNED,      /* arg: number of items; items can be read from now on */
  MC_IMPORT_EV_ITEM_CHANGED, /* arg: item index; its state or candidates changed */
  MC_IMPORT_EV_IDLE,         /* every item is matched and no work is queued */
  MC_IMPORT_EV_OFFLINE       /* arg: 1 when MusicBrainz can't be reached, 0 when it can again */
} mc_import_event;

typedef void (*mc_import_fn)(void *user, mc_import_event event, int arg);

/* What writing an item did, beyond its state. See "Duplicates" above. */
typedef enum mc_import_outcome {
  MC_IMPORT_OUTCOME_NONE,      /* not written yet, or written as new music */
  MC_IMPORT_OUTCOME_REPLACED,  /* DONE: replaced the library's copy with higher quality */
  MC_IMPORT_OUTCOME_ADDED,     /* DONE: added the tracks the library lacked; the rest were
                                * already there in equal or better quality */
  MC_IMPORT_OUTCOME_DUPLICATE, /* SKIPPED: already in the library in equal or better quality */
  MC_IMPORT_OUTCOME_MOVED,     /* SKIPPED: its files were all assigned to another item's
                                * tracks (mc_import_assign), or it was a disc joined to
                                * another item ("Disc folders" above); it has none left */
  MC_IMPORT_OUTCOME_REMOVED    /* SKIPPED: the user removed all its files from the import
                                * (mc_import_remove); putting one back brings it to review */
} mc_import_outcome;

/* Once every item is settled and the import ran to completion, the import thread sweeps the
 * library folder for orphans ("Files" in docs/sync-protocol.md), unless the import itself
 * left unknown files there (e.g. a skipped file of a source inside the library folder). */
typedef struct mc_import_options {
  double threshold; /* auto-accept at or above this confidence (0..1); 0 = default 0.9 */
  int fetch_art;    /* download a front cover (Cover Art Archive, then iTunes) for albums
                     * whose own art is missing or low resolution (docs/tagging-rules.md) */
  int skip_tagging; /* no MusicBrainz lookups: import every item with its existing tags */
  /* Where zips are extracted (see "Zips" above); NULL for the system's temporary folder.
   * Must not be inside the library folder. Copied. */
  const char *staging_dir;
} mc_import_options;

/* One track of a candidate, in release order. */
typedef struct mc_import_match_track {
  char *title;
  char *artist;
  int disc;
  int number;
  int length_ms;
  int file; /* index into mc_import_item.files, or -1 if no file was matched to it */
} mc_import_match_track;

typedef struct mc_import_match {
  char *album;        /* release title, the album it lands in (for a single too) */
  char *album_artist; /* for a single, its first credited artist (see is_single) */
  char *release;      /* the MusicBrainz release title; the same as album */
  char *country;
  char *label;
  char *catalog;
  char *format;       /* "CD", "Digital Media", "2×Vinyl", ... */
  char *release_type; /* "Album", "Single", "EP", ... */
  char *mb_albumid;
  int year;
  /* A recording found on its own, credited to the release album: tracks holds just it.
   * The release is looked up in full only when it's written, which may give it another
   * album artist and its place in the tracklist. */
  int is_single;
  double confidence;  /* 0..1 */
  int extra_files;    /* files left over after matching */
  mc_import_match_track *tracks;
  int track_count;
} mc_import_match;

typedef struct mc_import_file {
  char *path;   /* the original until the item is imported, then the copy in the library */
  char *title;  /* existing tags */
  char *artist;
  int track;
  int disc;
  int duration_ms;
} mc_import_file;

/* Snapshot of an item; free with mc_import_item_free(). */
typedef struct mc_import_item {
  mc_import_kind kind;
  mc_import_state state;
  char *folder;
  char *artist; /* best guess from the existing tags, for display */
  char *album;  /* for a single, the track title */
  mc_import_file *files;
  int file_count;
  mc_import_match *matches; /* best first */
  int match_count;
  int chosen;   /* index of the applied match; -1 if imported as-is or not yet decided */
  int searched_wider; /* the matches include an extended search's (see above) */
  int unreachable; /* in review because MusicBrainz couldn't be reached (see "Offline") */
  char *error;  /* set when state is MC_IMPORT_FAILED; may say why it's in review or pending */
  mc_import_outcome outcome;
  int replaced_files;  /* files swapped for higher quality ones */
  int duplicate_files; /* files skipped because the library already has them */
  mc_import_file *removed; /* files removed from the import (mc_import_remove), in the order
                            * they were removed; not in files */
  int removed_count;
} mc_import_item;

/* Starts importing the given folders and/or files. lib must stay open until the session is
 * freed; the import thread uses its own connection to the same database. */
mc_status mc_import_start(mc_library *lib, const char *const *paths, int path_count,
                          const mc_import_options *opts, mc_import_fn callback, void *user,
                          mc_import **out);

/* Stops the session, waits for the import thread and frees everything. Work already
 * written stays written. Don't call it from the callback. */
void mc_import_free(mc_import *imp);

int mc_import_item_count(mc_import *imp);
mc_status mc_import_item_get(mc_import *imp, int item, mc_import_item **out);
void mc_import_item_free(mc_import_item *item);

/* Resolving an item that needs review. These queue work for the import thread; progress is
 * reported through MC_IMPORT_EV_ITEM_CHANGED. */
mc_status mc_import_accept(mc_import *imp, int item, int match);
mc_status mc_import_accept_as_is(mc_import *imp, int item); /* keep existing tags */
mc_status mc_import_skip(mc_import *imp, int item);
/* Searches again with user-supplied terms. title is the album, or the track for a single. */
mc_status mc_import_search(mc_import *imp, int item, const char *artist, const char *title);

/* Fixes a wrong pairing: makes file `file` of item `from_item` release track `track` (index
 * into matches[match].tracks) of item `item`, then writes `item` with that match as
 * mc_import_accept() does. Works on items in review or already imported (an imported item's
 * files are re-tagged and moved in place; tracks keep their uids).
 *
 * The file may come from another item of the session; it then leaves from_item (and, if
 * already imported, its library row moves to this album; an album left empty is deleted). A
 * from_item left with no files becomes MC_IMPORT_SKIPPED with MC_IMPORT_OUTCOME_MOVED. A file
 * already on that track stays in the item, unmatched. Both items' matches are rescored
 * without a new lookup, but keep their order and indices, so may no longer be best first.
 *
 * MC_ERR_INVALID_ARG if either item is busy (pending, matching or writing) or an index is
 * bad. File indices change, so frontends re-read on MC_IMPORT_EV_ITEM_CHANGED. Queued. */
mc_status mc_import_assign(mc_import *imp, int item, int match, int track, int from_item, int file);

/* mc_import_remove takes file `file` of an item in review out of the import: it is neither
 * tagged nor imported and moves to mc_import_item.removed. Matches are rescored from the
 * pairs left (its track becomes missing, so the match is no longer complete); order and
 * indices are kept. The item stays in review; one left with no files becomes
 * MC_IMPORT_SKIPPED with MC_IMPORT_OUTCOME_REMOVED.
 *
 * mc_import_put_back undoes it for removed[removed]: the file returns in disc/track order and
 * each match pairs it with its old track if still free, else the closest free track, else
 * none. A skipped-for-removal item returns to review.
 *
 * Both return MC_ERR_INVALID_ARG unless the item is in review (or, for put_back, skipped
 * with MC_IMPORT_OUTCOME_REMOVED) and indices are valid. Queued; file indices change. */
mc_status mc_import_remove(mc_import *imp, int item, int file);
mc_status mc_import_put_back(mc_import *imp, int item, int removed);

/* Other items whose folder is item's folder or inside it (a disc folder, say): where files
 * that belong to item but were grouped apart from it are. Fills up to max indices into out and
 * returns how many there are in all. Items with no files are left out, except those skipped
 * with MC_IMPORT_OUTCOME_REMOVED, whose removed files can be put back. */
int mc_import_related(mc_import *imp, int item, int *out, int max);

/* Searches more widely for an album or a single (see "Extended search" above); the candidates
 * found join the item's, and the item stays in review. While offline it waits for the next
 * request instead, so frontends offer it only while online. */
mc_status mc_import_search_wider(mc_import *imp, int item);

/* Stops tagging for the rest of the session, as if it had been started with skip_tagging:
 * items not matched yet, and items in review because MusicBrainz couldn't be reached, are
 * imported with their existing tags. Items that already have candidates stay in review, and
 * mc_import_search() still searches. For an offline import. */
mc_status mc_import_skip_tagging(mc_import *imp);

/* Matches again every item in review because MusicBrainz couldn't be reached (unreachable
 * set), checking first whether MusicBrainz is back if the session found it offline. Those
 * that still can't reach it go back to review with the new reason. Does nothing once tagging
 * is skipped. Queued like the calls above. */
mc_status mc_import_retry_unreachable(mc_import *imp);

/* Whether the session last found MusicBrainz unreachable (see MC_IMPORT_EV_OFFLINE). */
int mc_import_offline(mc_import *imp);

/* The demo import, for the import page's store screenshots: see kithcore/demo.h. Here for
 * the types it takes. MC_DEMO_IMPORT_REVIEW is its item that needs a decision. */
mc_status mc_demo_import_start(mc_import_fn callback, void *user, mc_import **out);
#define MC_DEMO_IMPORT_REVIEW 2

#ifdef __cplusplus
}
#endif

#endif
