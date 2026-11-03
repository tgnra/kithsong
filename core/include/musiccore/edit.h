/* Editing the library by hand: an album's, artist's or track's tags, an album's flags, and
 * matching an album in the library to another MusicBrainz release.
 *
 * The rows are what's edited, so an edit syncs like any other change. Get a struct, change
 * what the user changed and set it whole. Strings are UTF-8, "" or NULL for none; 0 is an
 * unknown number. MC_ERR_NOT_FOUND for a uid that's gone.
 *
 * The _set() calls can rewrite and move files, and mc_release_search(), mc_release_tags() and
 * an album set with a new mb_albumid wait for MusicBrainz, so call them off the UI thread. */
#ifndef MUSICCORE_EDIT_H
#define MUSICCORE_EDIT_H

#include "musiccore/musiccore.h"

/* Defined in library.h, which includes this through musiccore.h before it defines it. */
typedef struct mc_library mc_library;

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_album_tags {
  char *title;
  char *artist; /* the credit as shown ("A & B"); setting it splits it as an import would */
  int year;
  int original_year;
  /* The MusicBrainz release. Set to another release, the album's tracks are matched to that
   * release's too; "" unlinks the album, leaving its tags as they are. */
  char *mb_albumid;
} mc_album_tags;

mc_status mc_album_tags_get(mc_library *lib, const char *album_uid, mc_album_tags *out);
mc_status mc_album_tags_set(mc_library *lib, const char *album_uid, const mc_album_tags *tags);
void mc_album_tags_clear(mc_album_tags *tags);

typedef struct mc_artist_tags {
  char *name;
  char *sort_name;
} mc_artist_tags;

mc_status mc_artist_tags_get(mc_library *lib, const char *artist_uid, mc_artist_tags *out);
mc_status mc_artist_tags_set(mc_library *lib, const char *artist_uid, const mc_artist_tags *tags);
void mc_artist_tags_clear(mc_artist_tags *tags);

typedef struct mc_track_tags {
  char *title;
  char *artist; /* the credit as shown; "" for the album's */
  int track_no;
  int disc_no;
  int year;
  char *genre;
} mc_track_tags;

mc_status mc_track_tags_get(mc_library *lib, const char *track_uid, mc_track_tags *out);
mc_status mc_track_tags_set(mc_library *lib, const char *track_uid, const mc_track_tags *tags);
void mc_track_tags_clear(mc_track_tags *tags);

/* What an album's tags can't say; these are never written to its files. */
typedef struct mc_album_flags {
  /* Its tracks are never scrobbled. Plays are still counted. */
  int no_scrobble;
  /* A continuous mix: one piece of music split into tracks, so the album is the thing to
   * find. Its tracks aren't search results (a match on one finds the album, see search.h)
   * and smart lists of tracks don't pick them. */
  int mix;
} mc_album_flags;

mc_status mc_album_flags_get(mc_library *lib, const char *album_uid, mc_album_flags *out);
mc_status mc_album_flags_set(mc_library *lib, const char *album_uid, const mc_album_flags *flags);

typedef struct mc_release_info {
  char mb_albumid[MC_UID_STRLEN];
  char *title;
  char *artist;
  int year;
  char *country;    /* NULL if unknown */
  char *label;      /* NULL if unknown */
  char *format;     /* "CD", "Digital Media"...; NULL if unknown */
  int track_count;
  double score;     /* how well it fits the album's tracks, 0 to 1 */
} mc_release_info;

typedef struct mc_release_list {
  mc_release_info *items;
  int count;
} mc_release_list;

/* The MusicBrainz releases for artist and title, best fit for album_uid's tracks first.
 * MC_ERR_NET if MusicBrainz can't be reached. Free with mc_release_list_free(). */
mc_status mc_release_search(mc_library *lib, const char *album_uid, const char *artist,
                            const char *title, mc_release_list **out);
void mc_release_list_free(mc_release_list *list);

/* What album_uid's tags would be on release mb_albumid, to fill in a form: nothing changes
 * until mc_album_tags_set(). Release with mc_album_tags_clear(). */
mc_status mc_release_tags(mc_library *lib, const char *album_uid, const char *mb_albumid,
                          mc_album_tags *out);

#ifdef __cplusplus
}
#endif

#endif
