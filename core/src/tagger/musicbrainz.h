/* MusicBrainz client used by the importer. Every call blocks, and calls are rate-limited
 * process-wide to MusicBrainz's 1 request/second. */
#ifndef MC_TAGGER_MUSICBRAINZ_H
#define MC_TAGGER_MUSICBRAINZ_H

#include "musiccore/musiccore.h"
#include "net/http.h"

/* One entry of a MusicBrainz artist-credit array. */
typedef struct mb_credit_part {
  char *name; /* as credited on this release ("name"), not the artist's own name */
  char *id;
  char *sort_name;
  char *join; /* "joinphrase"; NULL if empty */
  char *artist_name; /* the artist's own name, when it isn't the credited one; else NULL */
} mb_credit_part;

typedef struct mb_credit {
  mb_credit_part *parts;
  int count;
} mb_credit;

void mb_credit_clear(mb_credit *c);

typedef struct mb_track {
  char *id; /* release track id */
  char *recording_id;
  char *title;
  char *artist;    /* as credited, e.g. "Jay-Z feat. Rihanna" */
  char *artist_id; /* only when a single artist is credited */
  mb_credit credit; /* every credited artist, in order */
  int disc;
  int number;
  int length_ms;
  int video; /* a video: its recording is flagged as one, or it's on a video medium (DVD-Video) */
} mb_track;

/* A release with its tracklist. A recording found for a loose track is represented as a
 * one-track release with is_single set: the release it's credited to (id, title, release
 * group, year), with the recording as its one track (numbered as on that release when the
 * search says), and the recording's artist credit for the release's, since the search
 * doesn't give the release's own. */
typedef struct mb_release {
  char *id;
  char *title;
  char *artist;      /* as credited */
  char *artist_id;   /* id and sort name only when a single artist is credited */
  char *artist_sort;
  char *primary_artist; /* first credited artist: the album artist of a recording whose
                         * release can't be looked up */
  char *primary_artist_id;
  char *primary_artist_sort;
  mb_credit credit; /* every credited artist, in order */
  char *releasegroup_id;
  char *release_type; /* release group primary type: "Album", "Single", "EP", ... */
  char *country;
  char *label;   /* the first label */
  char **labels; /* every label's name, in order (label is the first) */
  int label_count;
  char *catalog;
  char *format;
  int year;
  int original_year;
  int disc_total;
  int is_single; /* a recording found on its own (above), not a release looked up */
  mb_track *tracks;
  int track_count;
} mb_release;

void mb_release_clear(mb_release *rel);

/* Called from mc_init(). */
void mc_musicbrainz_global_init(void);

/* What a caller's lookups share, passed as call to the functions below (NULL: nothing).
 * cancel (NULL: never) gives a lookup up, waiting for the rate limit and retries included,
 * as if offline (MC_ERR_NET). A lookup that fails for a reason that passes (a dropped
 * connection, MusicBrainz busy) is retried, for minutes if need be, unless MusicBrainz
 * can't be reached at all (musicbrainz.c). When a lookup fails with MC_ERR_NET other than by
 * being cancelled, why receives the reason in English: "HTTP 503, MusicBrainz is busy", or
 * what went wrong with the connection. */
typedef struct mb_call {
  const mc_http_cancel *cancel;
  char why[160];
} mb_call;

/* mc_musicbrainz_reachable() that cancel (NULL: never) can give up on, and that, when
 * MusicBrainz can't be reached, puts why in why (if why_len isn't 0), in English. */
mc_status mb_reachable_why(int timeout_ms, const mc_http_cancel *cancel, char *why, size_t why_len);

/* With KITHSONG_TAGGER_TRACE set (and not "0"), what the importer does is written to stderr:
 * every MusicBrainz request, and each item's tags, candidates and decision. For working out
 * why a release was matched as it was. mb_trace() writes one line when it's on. */
int mb_tracing(void);
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void mb_trace(const char *fmt, ...);

/* Release ids for an artist/album search, most relevant first. Releases whose track count
 * equals track_count (if > 0) are moved to the front. artist may be NULL. The album is
 * searched without its edition notes (mc_search_title). */
mc_status mb_search_releases(const char *artist, const char *album, int track_count,
                             char (*ids)[MC_UID_STRLEN], int max, int *n, mb_call *call);

/* A release search by any of: title, artist, label names (any of them) and catalogue
 * numbers (any of them). Fields left NULL / 0 aren't searched; at least one must be set. */
typedef struct mb_release_query {
  const char *album;
  const char *artist;
  const char *const *labels;
  int label_count;
  const char *const *catnos;
  int catno_count;
} mb_release_query;

/* Like mb_search_releases, with the fields of q. */
mc_status mb_search_releases_by(const mb_release_query *q, int track_count,
                                char (*ids)[MC_UID_STRLEN], int max, int *n, mb_call *call);

/* Full release with tracklist, artist credits, label and release group. */
mc_status mb_lookup_release(const char *id, mb_release *out, mb_call *call);

/* Recordings matching artist/title (artist may be NULL), each as a one-track release. When
 * a recording has several releases, the earliest official single is preferred, else the
 * earliest release. *n receives the number filled; free each with mb_release_clear(). */
mc_status mb_search_recordings(const char *artist, const char *title, mb_release *out, int max,
                               int *n, mb_call *call);

/* One recording by id, as a one-track release (see mb_search_recordings). */
mc_status mb_lookup_recording(const char *id, mb_release *out, mb_call *call);

/* For tests: answers the lookups above instead of musicbrainz.org, without the rate limit,
 * until set back to NULL. A NULL function finds nothing. While a fake is set,
 * mc_musicbrainz_reachable() says MusicBrainz is reachable. Not thread-safe: set it while
 * no import is running. */
typedef struct mb_fake {
  mc_status (*search_releases)(const char *artist, const char *album, int track_count,
                               char (*ids)[MC_UID_STRLEN], int max, int *n);
  mc_status (*lookup_release)(const char *id, mb_release *out);
  mc_status (*search_recordings)(const char *artist, const char *title, mb_release *out, int max,
                                 int *n);
  mc_status (*lookup_recording)(const char *id, mb_release *out);
  mc_status (*search_releases_by)(const mb_release_query *q, int track_count,
                                  char (*ids)[MC_UID_STRLEN], int max, int *n);
} mb_fake;

void mb_set_fake(const mb_fake *fake);

/* For tests: what mb_lookup_release and mb_search_recordings make of a response body. */
mc_status mb_release_parse(const char *json, size_t len, mb_release *out);
mc_status mb_recordings_parse(const char *json, size_t len, mb_release *out, int max, int *n);

#endif
