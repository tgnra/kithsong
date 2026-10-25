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

typedef struct mc_db_album {
  /* The artist's "<artist> Singles" album; title, years and ids are then ignored. */
  int singles;
  const char *title;
  mc_db_artist artist;
  int year;
  int original_year;
  int disc_total;
  const char *mb_albumid;
  const char *mb_releasegroupid;
  const char *art_hash; /* NULL keeps the current cover */
} mc_db_album;

typedef struct mc_db_track {
  const char *path;     /* relative to the library folder */
  const char *old_path; /* where the file was in the library before this import, if it moved */
  const char *format;
  int64_t size_bytes;
  int64_t mtime_ms;
  int bitrate_kbps;
  const char *title;
  mc_db_artist artist;
  int track_no;
  int disc_no;
  int duration_ms;
  int year;
  const char *genre;
  const char *mb_trackid;
  const char *mb_releasetrackid;
  const char *mb_albumid;
} mc_db_track;

/* Inserts or updates the album, its artists, tracks and files in one transaction. Artists
 * are matched by MusicBrainz id, then by name; albums by release id, then by artist and
 * title; tracks by file path (old_path if set), so re-importing a file updates it instead of
 * duplicating it. Albums left empty by a re-import are deleted. */
mc_status mc_import_db_write(mc_library *lib, const mc_db_album *album, const mc_db_track *tracks,
                             int count);

/* Whether the file at path (relative) is in the library as the given release track, i.e.
 * importing that track again to the same place would replace a copy of itself. */
int mc_import_db_is_track_at(mc_library *lib, const char *path, const char *mb_releasetrackid);

#endif
