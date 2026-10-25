/* Tag reading/writing and the MusicBrainz/AcoustID autotagger. */
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
  int year;
  int track;
  int track_total;
  int disc;
  int disc_total;
  /* Audio properties; read-only, ignored by mc_tags_write(). */
  int duration_ms;
  int bitrate_kbps;
  int sample_rate;
  int channels;
} mc_tags;

/* Reads tags and audio properties. On success *out must be released with mc_tags_clear(). */
mc_status mc_tags_read(const char *path, mc_tags *out);

/* Writes every non-NULL / non-zero field of tags to the file, leaving other tags untouched. */
mc_status mc_tags_write(const char *path, const mc_tags *tags);

void mc_tags_clear(mc_tags *tags);

/* Connectivity check: fetches a known artist from the MusicBrainz API over HTTPS and
 * returns its name in *out_name (free with mc_free). */
mc_status mc_musicbrainz_ping(char **out_name);

/* --- Autotagger (not implemented yet) --- */

typedef struct mc_tagger mc_tagger;

mc_status mc_tagger_new(mc_library *lib, mc_tagger **out);
void mc_tagger_free(mc_tagger *tagger);

/* Scans a directory, groups files into album candidates and looks them up. Candidates
 * are delivered to the frontend for confirmation before anything is written. */
mc_status mc_tagger_import_dir(mc_tagger *tagger, const char *path);

#ifdef __cplusplus
}
#endif

#endif
