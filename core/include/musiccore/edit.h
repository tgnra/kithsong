/* Editing the library by hand: an album's, artist's or track's tags, an album's flags, and
 * matching an album in the library to another MusicBrainz release.
 *
 * The rows are what's edited, so an edit syncs like any other change. Get a struct, change
 * what the user changed and set it whole. Strings are UTF-8, "" or NULL for none; 0 is an
 * unknown number. MC_ERR_NOT_FOUND for a uid that's gone.
 *
 * The _set() calls can rewrite and move files, and mc_release_search(), mc_release_tags() and
 * an album set with a new mb_albumid wait for MusicBrainz, so call them off the UI thread.
 *
 * An edit is all or nothing, rows and this device's files together. As well as the usual
 * statuses a _set() can return:
 *   MC_ERR_IN_USE  a file it would rewrite or move is open: on Windows, usually the track
 *                  playing, or the next one queued to play. Nothing has changed: say so, and
 *                  it can be tried again once playback has moved on or stopped.
 *   MC_ERR_EXISTS  (an album's new mb_albumid) another album has that release, or once had:
 *                  sync would merge the two. Nothing has changed.
 *   MC_ERR_IO      a file couldn't be copied or written, a full disk say. Nothing has changed.
 *
 * What an edit does to files: every original of this device that the changed tags are in is
 * rewritten, those tags only, with its files row following (size, mtime, sha256); its file_rev
 * doesn't change, so no peer fetches it again, and peers' copies keep their old tags, the rows
 * being what they show. A file moves if the edit changes where organize.c files it. Transcodes
 * move but aren't rewritten. A file that's missing, read-only, outside the library, or isn't
 * what its row or hash says is left as it is, and that doesn't stop the edit.
 *
 * An edit can take as long as copying the files it touches, and needs that much free space
 * while it runs: one file for a track's, every file credited for an artist's rename. See "Hand
 * edits" in docs/tagging-rules.md. */
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
  /* The credit as shown ("A & B"). Changed, it's split as an import would split it, and the
   * tracks credited as the album was follow it; left as it is, it isn't split again. */
  char *artist;
  int year; /* tracks with the album's year follow it */
  int original_year;
  /* The MusicBrainz release. Set to another release, the album's tracks are matched to that
   * release's too, and those that match take its titles, credits, numbers and ids; the others
   * keep their tags. The title, artist and years take the release's too, unless they differ
   * from what the album has now, as the user's own or mc_release_tags()'s do. A cover is
   * fetched if the album has none. "" unlinks the album, leaving its tags as they are but for
   * the release's ids (its tracks keep their recordings'). */
  char *mb_albumid;
} mc_album_tags;

mc_status mc_album_tags_get(mc_library *lib, const char *album_uid, mc_album_tags *out);
mc_status mc_album_tags_set(mc_library *lib, const char *album_uid, const mc_album_tags *tags);
void mc_album_tags_clear(mc_album_tags *tags);

/* name can't be empty (MC_ERR_INVALID_ARG). Renamed, credits that spell the name out follow, as
 * do the files; one that credits the artist otherwise stays as it was. A name another artist
 * has makes two artists of that name, listed apart: they're merged only as any two are, by
 * sharing a MusicBrainz id. */
typedef struct mc_artist_tags {
  char *name;
  char *sort_name;
} mc_artist_tags;

mc_status mc_artist_tags_get(mc_library *lib, const char *artist_uid, mc_artist_tags *out);
mc_status mc_artist_tags_set(mc_library *lib, const char *artist_uid, const mc_artist_tags *tags);
void mc_artist_tags_clear(mc_artist_tags *tags);

typedef struct mc_track_tags {
  char *title;
  char *artist; /* the credit as shown; "" for the album's, also read for one that says the same */
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

/* The MusicBrainz releases for artist and title, best fit for album_uid's tracks first: up to
 * 5, each a request, so about 6 s at MusicBrainz's rate limit, which an import running
 * meanwhile shares. title is required (MC_ERR_INVALID_ARG). MC_ERR_NET if MusicBrainz can't be
 * reached. Free with mc_release_list_free(). */
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
