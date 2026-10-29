/* Tag reading/writing and the MusicBrainz autotagger. */
#ifndef MUSICCORE_TAGGER_H
#define MUSICCORE_TAGGER_H

#include "musiccore/musiccore.h"

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
 * An empty string removes that tag. */
mc_status mc_tags_write(const char *path, const mc_tags *tags);

void mc_tags_clear(mc_tags *tags);

/* Connectivity check: fetches a known artist from the MusicBrainz API over HTTPS and
 * returns its name in *out_name (free with mc_free). */
mc_status mc_musicbrainz_ping(char **out_name);

/* Cheap offline check: opens (and closes) a TCP connection to the MusicBrainz server,
 * waiting at most timeout_ms for the name lookup and the connection together, so broken DNS
 * can't hold it longer. MC_OK if it connected, MC_ERR_NET if not. Blocks.
 * For testing, KITHSONG_MUSICBRAINZ_PROBE=host:port in the environment checks that address
 * instead, e.g. 127.0.0.1:1 to simulate being offline. */
mc_status mc_musicbrainz_reachable(int timeout_ms);

/* --- Import (autotagger) ---
 *
 * An import session scans folders on a core-owned thread and groups the audio files into
 * items: an album (files that belong together), or a single (one loose track). Each item
 * is matched against MusicBrainz and its candidates are scored. If the best one reaches
 * the confidence threshold it is imported straight away: the files are copied into the
 * library folder (moved, if they're already inside it) under Artist/Album (Year)/, tagged,
 * given a cover.jpg, and added to the database. The originals are never modified.
 * Otherwise the item waits in MC_IMPORT_REVIEW until the frontend resolves it. Albums are
 * matched first, then one-file items, so the albums reach the review first.
 *
 * Disc folders. Albums whose album tag (or folder name) ends with a disc ("Title Disc1",
 * "Title CD 2", "Title (Disc 1 of 2)") are grouped one item per disc. When several such items
 * share a title without the disc, in one folder or folders side by side, the session asks
 * MusicBrainz before matching them: if a release of that title has as many discs, and each
 * item's files fit its disc's tracks, they're joined into the first item (album = the title,
 * files in disc order) and the others become MC_IMPORT_SKIPPED with MC_IMPORT_OUTCOME_MOVED.
 * The joined item is never imported straight away: it waits in MC_IMPORT_REVIEW, with error
 * saying it was joined, however high its confidence. Otherwise they're matched one by one.
 *
 * Singles: loose tracks, and releases MusicBrainz classes as singles, go into one album
 * per artist called "<artist> Singles" (albums.kind = 'singles').
 *
 * Offline: before matching, and whenever a lookup fails with a network error, the session
 * checks whether MusicBrainz is reachable (mc_musicbrainz_reachable). While it isn't, items
 * aren't looked up but go straight to MC_IMPORT_REVIEW ("Couldn't reach MusicBrainz"), so an
 * offline import doesn't wait on a timeout per item, and MC_IMPORT_EV_OFFLINE tells the
 * frontend, which can offer mc_import_skip_tagging(). The check is repeated every 30 s; once
 * MusicBrainz answers again the items waiting on it are matched after all.
 *
 * Extended search. Albums often carry a label or circle in the album artist tag rather than
 * the artist ("SHARPNELSOUND"), and then the normal search (album title and album artist)
 * finds nothing, or nothing good. When the best candidate of an album or a one-file item (a
 * single, which may be a whole release such as a DJ mix) is below 75% confidence (or there is
 * none), the session searches more widely by itself before deciding: by the catalogue numbers
 * in its tags (CATALOGNUMBER, CATALOGID, ...) and in [brackets] in the album tag and the
 * folder name; by the album title with the artists found in "Label (Artist) - Title" folder
 * names, the album artist tag, the track artists and the file names; with the label (label
 * tags, or the name standing where the artist would); and by the title alone. At most 4
 * more searches and 6 more release lookups per album. Every release candidate is then
 * scored with the best fitting of those artists and titles; tracks, lengths and the rest
 * are scored as usual, and the same rules decide whether the best one is accepted (a
 * release candidate must be complete, for a single too). Above
 * 75%, mc_import_search_wider() runs it on request. It's skipped while MusicBrainz can't be
 * reached, and a network error ends it early (then searched_wider stays 0).
 *
 * Duplicates. Before an accepted item is written, it is compared with the library (only
 * files from outside the library folder; files inside it are being re-filed, not added):
 *
 *   - The same release already in the library: an album with the same MusicBrainz release
 *     id, else the same release group id, else the same album artist and title (ignoring
 *     case) with at least half of the incoming tracks in its track list. A single: the
 *     artist's singles album holding the same recording (or, without ids, the same title).
 *     Incoming tracks are paired with the album's tracks by release track id, recording id,
 *     disc and track number with the same title, then title alone.
 *   - Quality of one file, best first:
 *       1. lossless beats lossy (lossy: MP3 and Opus; FLAC, WAV and AIFF are lossless);
 *       2. between lossless files: more bits per sample, then a higher sample rate;
 *       3. between lossy files: a higher effective bitrate, i.e. the bitrate weighted by
 *          codec (MP3 1.0, Opus 1.6), and only when it is
 *          more than 10% higher, so VBR noise doesn't count.
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
 *   - The same file again: every file imported from outside the library records the
 *     SHA-256 of its bytes as they were before the import tagged its copy
 *     (files.source_sha256). A file whose bytes hash to a recorded source hash, or to the
 *     SHA-256 of a file in the library, is skipped as already there. Files imported before
 *     this have no source hash: they're only found by their current bytes.
 *
 * mc_import_item.outcome says what happened.
 *
 * Zips. A path given to mc_import_start that is a .zip file is extracted into a staging
 * folder named after the zip without its extension (in a kithsong-import-* folder of its own
 * under mc_import_options.staging_dir), and that folder is imported as any other, cover.jpg
 * and all. Entry names are UTF-8 when the entry's flag bit 11 says so, else the UTF-8 name in
 * its Info-ZIP Unicode Path extra field if it has a matching one, else CP437; entries
 * that are absolute or use ".." (zip slip) are left out, and so are __MACOSX/ and .DS_Store.
 * A zip holding more than 8 GB unzipped, or more than the staging folder's free space, isn't
 * extracted. A zip that can't be extracted becomes an item of its own with no files, in
 * MC_IMPORT_FAILED with the reason in error (folder is the zip's path, album its file name).
 * The staging folder is deleted when the session is freed. Only zips named directly are
 * extracted, not zips found inside a folder.
 *
 * Callbacks run on the import thread; frontends marshal them to their UI thread. Every
 * other function may be called from any thread. */

typedef struct mc_import mc_import;

typedef enum mc_import_kind {
  MC_IMPORT_ALBUM,  /* a group of files matched to a release */
  MC_IMPORT_SINGLE  /* one loose track matched to a recording */
} mc_import_kind;

typedef enum mc_import_state {
  MC_IMPORT_PENDING,   /* scanned, waiting to be matched */
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
  MC_IMPORT_OUTCOME_MOVED      /* SKIPPED: its files were all assigned to another item's
                                * tracks (mc_import_assign), or it was a disc joined to
                                * another item ("Disc folders" above); it has none left */
} mc_import_outcome;

typedef struct mc_import_options {
  double threshold; /* auto-accept at or above this confidence (0..1); 0 = default 0.9 */
  int fetch_art;    /* download front covers (Cover Art Archive, then iTunes) */
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
  char *album;        /* release title; for a single, the album it lands in */
  char *album_artist;
  char *release;      /* the MusicBrainz release title, e.g. the single's own title */
  char *country;
  char *label;
  char *catalog;
  char *format;       /* "CD", "Digital Media", "2×Vinyl", ... */
  char *release_type; /* "Album", "Single", "EP", ... */
  char *mb_albumid;
  int year;
  int is_single;      /* lands in "<artist> Singles" */
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
  char *error;  /* set when state is MC_IMPORT_FAILED */
  mc_import_outcome outcome;
  int replaced_files;  /* files swapped for higher quality ones */
  int duplicate_files; /* files skipped because the library already has them */
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

/* Fixes a match the tagger got wrong: makes file `file` of item `from_item` release track
 * `track` (an index into matches[match].tracks) of item `item`, then writes `item` with that
 * match, as mc_import_accept() does. So the file is tagged and filed as that track whether the
 * item was still in review or already imported; for an imported item its files are already in
 * the library, so they're re-tagged and moved in place, and the library rows updated (tracks
 * keep their uids).
 *
 * The file may be one of the item's own (one left unmatched, or matched to another track), or
 * belong to another item of the session, e.g. a file whose tags put it in an item of its own.
 * Then it moves into `item`: it's gone from from_item, whose candidates no longer count it,
 * and if from_item had already put it in the library, its library row moves to this album
 * (an album left empty is deleted). A from_item with no files left becomes MC_IMPORT_SKIPPED
 * with MC_IMPORT_OUTCOME_MOVED. A file already on that track stays in the item, unmatched.
 * The match's confidence, extra_files and missing tracks are then recomputed from the files
 * and tracks as they're paired now (no new lookup), every pair counting, so a match that was
 * low only for want of that track goes up. The items' other matches are rescored too, but
 * matches keep their order (and indices), so they may no longer be best first.
 *
 * Neither item may be busy (pending, matching or writing): MC_ERR_INVALID_ARG, as for bad
 * indices. File indices of both items change, so frontends read them again on
 * MC_IMPORT_EV_ITEM_CHANGED. Queued like the calls above. */
mc_status mc_import_assign(mc_import *imp, int item, int match, int track, int from_item, int file);

/* Other items whose folder is item's folder or inside it (a disc folder, say): where files
 * that belong to item but were grouped apart from it are. Fills up to max indices into out and
 * returns how many there are in all. */
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

/* Whether the session last found MusicBrainz unreachable (see MC_IMPORT_EV_OFFLINE). */
int mc_import_offline(mc_import *imp);

#ifdef __cplusplus
}
#endif

#endif
