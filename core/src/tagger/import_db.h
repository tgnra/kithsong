/* Library rows for an accepted import item. Separate from the import thread so it can be
 * tested without the network. */
#ifndef MC_TAGGER_IMPORT_DB_H
#define MC_TAGGER_IMPORT_DB_H

#include <stdint.h>

#include "musiccore/library.h"

typedef struct mc_db_artist {
  const char *name;
  const char *sort_name;
  const char *mb_artistid;
} mc_db_artist;

/* One artist of a credit, with the join phrase that follows it (" feat. ", " & ", or NULL
 * after the last). See docs/tagging-rules.md, "Artist credits". */
typedef struct mc_db_credit_part {
  mc_db_artist artist;
  const char *join;
} mc_db_credit_part;

typedef struct mc_db_album {
  /* Tracks with no album: no album row is written, and title, years, ids, art and
   * existing_uid are ignored. The artist is still the tracks' fallback. */
  int no_album;
  const char *title;
  mc_db_artist artist; /* the whole credit as credited ("A feat. B"); its id only if one artist */
  /* The credit's artists in order: album_artists rows, and albums.artist_uid is the first.
   * With credit_count 0 the credit is artist alone, one part. */
  const mc_db_credit_part *credit;
  int credit_count;
  int year;
  int original_year;
  int disc_total;
  const char *mb_albumid;
  const char *mb_releasegroupid;
  const char *art_hash; /* NULL keeps the current cover */
  /* An album already in the library to write into (a duplicate, see dupes.h): its row is
   * left as it is, apart from art_hash filling a missing cover. */
  const char *existing_uid;
  /* With existing_uid: its row takes this album's title, artist, years and ids instead (a
   * whole release swapped for a better one, see mc_dupe_plan.whole). Its uid stays. */
  int update_existing;
} mc_db_album;

typedef struct mc_db_track {
  const char *path;     /* relative to the library folder */
  const char *old_path; /* where the file was in the library before this import, if it moved */
  const char *format;
  int64_t size_bytes;
  int64_t mtime_ms;
  int bitrate_kbps;
  const char *title;
  mc_db_artist artist; /* the whole credit, as for the album; name NULL: the album's */
  const mc_db_credit_part *credit; /* track_artists rows; as for the album */
  int credit_count;
  int track_no;
  int disc_no;
  int duration_ms;
  int year;
  const char *genre;
  const char *mb_trackid;
  const char *mb_releasetrackid;
  const char *mb_albumid;
  /* A track already in the library that this file belongs to (see dupes.h). Its metadata
   * is kept; only the duration follows the new file. With replace_file_uid the file swaps
   * out that files row in place: the row gets a new uid (so nothing cached for the old
   * file, such as transcodes, is taken for the new one), the track a new file_rev. Without
   * it, the file is added to the track. */
  const char *track_uid;
  const char *replace_file_uid;
  /* With track_uid: the track's metadata follows this row after all (title, artist, numbers,
   * year, ids, and the genre if set); its uid and album stay. For a whole release. */
  int update_track;
  /* The SHA-256 (hex) of the bytes the file was imported from, before it was tagged:
   * files.source_sha256. NULL if unknown; a file updated in place then keeps its own. */
  const char *source_sha256;
} mc_db_track;

/* Inserts or updates the album, its artists, tracks and files in one transaction. Artists
 * are matched by MusicBrainz id, then by name; albums by release id, then by artist and
 * title; tracks by file path (old_path if set), so re-importing a file updates it instead of
 * duplicating it. Albums left empty by a re-import are deleted. */
mc_status mc_import_db_write(mc_library *lib, const mc_db_album *album, const mc_db_track *tracks,
                             int count);

/* Replaces the artist credit of track uid (is_album 0) or album uid (1) in place, as an
 * import writes one: artist and credit/credit_count as in mc_db_album (credit_count 0: the
 * artist alone), each artist upserted as an import's, artist_uid the first, then
 * artist_credit and the credit rows, the rows past a shorter credit deleted. Nothing else
 * about the row changes, and artists left with nothing aren't deleted here. For the refresh
 * after upgrading (docs/schema.md). One transaction; MC_ERR_NOT_FOUND if there's no such
 * row. */
mc_status mc_import_db_set_credit(mc_library *lib, int is_album, const char *uid,
                                  const mc_db_artist *artist, const mc_db_credit_part *credit,
                                  int credit_count);

/* Changes the path of the files row at old_path (both relative). */
mc_status mc_import_db_move_file(mc_library *lib, const char *old_path, const char *new_path);

/* Whether the file at path (relative) is in the library as the given release track, i.e.
 * importing that track again to the same place would replace a copy of itself. */
int mc_import_db_is_track_at(mc_library *lib, const char *path, const char *mb_releasetrackid);

#endif
