/* MusicBrainz client used by the importer. Every call blocks, and calls are rate-limited
 * process-wide to MusicBrainz's 1 request/second. */
#ifndef MC_TAGGER_MUSICBRAINZ_H
#define MC_TAGGER_MUSICBRAINZ_H

#include "musiccore/musiccore.h"

typedef struct mb_track {
  char *id; /* release track id */
  char *recording_id;
  char *title;
  char *artist;    /* as credited, e.g. "Jay-Z feat. Rihanna" */
  char *artist_id; /* only when a single artist is credited */
  int disc;
  int number;
  int length_ms;
} mb_track;

/* A release with its tracklist. A recording found for a loose track is represented as a
 * one-track release with is_single set. */
typedef struct mb_release {
  char *id;
  char *title;
  char *artist;      /* as credited */
  char *artist_id;   /* id and sort name only when a single artist is credited */
  char *artist_sort;
  char *primary_artist; /* first credited artist: whose "<artist> Singles" a single joins */
  char *primary_artist_id;
  char *primary_artist_sort;
  char *releasegroup_id;
  char *release_type; /* release group primary type: "Album", "Single", "EP", ... */
  char *country;
  char *label;
  char *catalog;
  char *format;
  int year;
  int original_year;
  int disc_total;
  int is_single;
  mb_track *tracks;
  int track_count;
} mb_release;

void mb_release_clear(mb_release *rel);

/* Called from mc_init(). */
void mc_musicbrainz_global_init(void);

/* Release ids for an artist/album search, most relevant first. Releases whose track count
 * equals track_count (if > 0) are moved to the front. artist may be NULL. */
mc_status mb_search_releases(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n);

/* Full release with tracklist, artist credits, label and release group. */
mc_status mb_lookup_release(const char *id, mb_release *out);

/* Recordings matching artist/title (artist may be NULL), each as a one-track release. When
 * a recording has several releases, the earliest official single is preferred, else the
 * earliest release. *n receives the number filled; free each with mb_release_clear(). */
mc_status mb_search_recordings(const char *artist, const char *title, mb_release *out, int max,
                               int *n);

/* One recording by id, as a one-track release (see mb_search_recordings). */
mc_status mb_lookup_recording(const char *id, mb_release *out);

#endif
