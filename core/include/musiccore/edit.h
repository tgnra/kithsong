/* Editing the library by hand: an album's, artist's or track's tags, an album's flags and
 * cover, and matching an album in the library to another MusicBrainz release.
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
 *   MC_ERR_CANCELLED  stopped by mc_edit_set_watch()'s cancelled. Nothing has changed.
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
 * has makes two artists of that name, listed apart. So before saving a new name, a form asks
 * mc_artist_same_name(), and if there is one offers to merge the two. */
typedef struct mc_artist_tags {
  char *name;
  char *sort_name;
} mc_artist_tags;

mc_status mc_artist_tags_get(mc_library *lib, const char *artist_uid, mc_artist_tags *out);
mc_status mc_artist_tags_set(mc_library *lib, const char *artist_uid, const mc_artist_tags *tags);
void mc_artist_tags_clear(mc_artist_tags *tags);

/* Another artist called name, whatever its case: MC_OK with other_uid set, the closest spelling
 * first, else MC_ERR_NOT_FOUND. */
mc_status mc_artist_same_name(mc_library *lib, const char *artist_uid, const char *name,
                              char other_uid[MC_UID_STRLEN]);
/* Makes artist_uid and other_uid one artist, by other_uid's name and sort name. All that credits
 * either credits it, with files following as for a rename: a _set(), with its statuses and with
 * the free space a rename takes. The row that stays is other_uid's, unless only artist_uid has a
 * MusicBrainz id, so a frontend should open whichever uid has been left in its list again.
 * MC_ERR_INVALID_ARG for one artist given twice. */
mc_status mc_artist_merge(mc_library *lib, const char *artist_uid, const char *other_uid);

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

/* Watching an edit or a release search, which can take minutes: an artist with many albums copies
 * every file first, and MusicBrainz may be busy. Set on the handle before calling, and cleared
 * with NULL after; the struct must last until then. It covers the _set()s above and below, and
 * mc_release_search() and mc_release_tags().
 *
 * progress, if not NULL, is told how far it is: files copied of those to copy, or releases looked
 * up of those found. cancelled, if not NULL, is asked often and returns nonzero to stop: the call
 * then returns MC_ERR_CANCELLED, with nothing changed. Once the copies are made an edit is only
 * renames and can't be stopped, so it may still finish. Both are called on the thread of the
 * call, as often as a file or a request takes, so a file that's copying has to finish first. */
typedef struct mc_edit_watch {
  void (*progress)(void *user, int done, int total);
  int (*cancelled)(void *user);
  void *user;
} mc_edit_watch;

void mc_edit_set_watch(mc_library *lib, const mc_edit_watch *watch);

/* What an album's tags can't say; these are never written to its files. */
typedef struct mc_album_flags {
  /* Its tracks are never scrobbled. Plays are still counted. */
  int no_scrobble;
  /* A continuous mix: one piece of music split into tracks, or in one long track, so the
   * album is the thing to find. Its tracks aren't search results (a match on one finds the
   * album, see search.h), nor on an artist's list of tracks, and smart lists of tracks don't
   * pick them. mc_album_info.mix says the same, for a page to show. One of one track is listed
   * as an album, not a single (library.h). An album nobody has chosen for is a mix if its one
   * track is long; a hand choice, either way, stays whatever the album's length. */
  int mix;
  /* mix is the user's choice, not the album's length. Ignored by _set(): changing mix from
   * what _get() said is what makes a choice, and leaving it leaves it to the length. */
  int mix_by_hand;
  /* The cover is the user's choice, so mc_album_art_reset() has something to undo. Ignored by
   * _set(). */
  int art_chosen;
} mc_album_flags;

mc_status mc_album_flags_get(mc_library *lib, const char *album_uid, mc_album_flags *out);
mc_status mc_album_flags_set(mc_library *lib, const char *album_uid, const mc_album_flags *flags);

/* The same for a single, from its track, which may have no album to hold the flag. */
typedef struct mc_track_mix {
  int can_set; /* it's alone on its album or has none; a longer album's flag is the album's */
  int mix;
  int mix_by_hand;
} mc_track_mix;

mc_status mc_track_mix_get(mc_library *lib, const char *track_uid, mc_track_mix *out);
/* Makes a track that can_set a mix or not, by hand; MC_ERR_INVALID_ARG for one that can't. A
 * mix needs an album, so a track with none gets one of its title and is filed there, with the
 * statuses and file changes of any _set() above. Made no mix again, here or by
 * mc_album_flags_set(), such a track goes back to having no album, unless the album is flagged
 * no_scrobble, which needs one. */
mc_status mc_track_mix_set(mc_library *lib, const char *track_uid, int mix);

/* An album's cover, chosen by the user. It's kept as given, as cover.jpg or cover.png in every
 * folder of this device holding the album's files, with albums.art_hash following, and reaches
 * other devices as a change of art (see "Covers" in docs/sync-protocol.md). Files' tags aren't
 * touched: no art is embedded, on this device or any. The cover replaced goes to the Recycle
 * Bin or trash, as "replaced cover.jpg", or is deleted where there's none, as on Android.
 *
 * The album remembers the choice: import and cover downloads leave it alone, a later choice
 * being the only thing that replaces it. See "Cover art" in docs/tagging-rules.md.
 *
 * An image is JPEG or PNG, of up to MC_ALBUM_ART_MAX_BYTES and MC_ALBUM_ART_MAX_SIDE pixels each
 * way, and decodes. */
#define MC_ALBUM_ART_MAX_BYTES (16 * 1024 * 1024)
#define MC_ALBUM_ART_MAX_SIDE 8192

/* The cover this device shows for album_uid, for a form: *path is its file (mc_free()), or NULL
 * if the album's folders here have none. MC_ERR_NOT_FOUND if the album has no file on this device
 * or is gone: a new cover has nowhere to go, so don't offer one. */
mc_status mc_album_art_get(mc_library *lib, const char *album_uid, char **path);

/* Whether an image would do, for a form to say so at once: MC_OK, or MC_ERR_FORMAT if it can't
 * be a cover. It's decoded, so a big one takes a moment. check_file() returns MC_ERR_IO, or
 * MC_ERR_NOT_FOUND, for a file it can't read. */
mc_status mc_album_art_check(const void *image, size_t len);
mc_status mc_album_art_check_file(const char *path);

/* Makes the image album_uid's cover, from bytes (a content URI, say) or a file. It's copied, so
 * the caller's can go afterwards. As for the other _set()s, it's all or nothing, on a connection
 * of its own, and MC_ERR_IN_USE and MC_ERR_IO mean the same: what's open is a cover being sent to
 * a peer, so not for long. Also MC_ERR_FORMAT, as mc_album_art_check(), and MC_ERR_NOT_FOUND for
 * an album with no file on this device, as a cover has nowhere to go without one. Call it after
 * mc_album_tags_set() if both are being saved, as that can move the album's folder. */
mc_status mc_album_art_set(mc_library *lib, const char *album_uid, const void *image, size_t len);
mc_status mc_album_art_set_file(mc_library *lib, const char *album_uid, const char *path);

/* Undoes the choice: the album goes back to the cover an import would give it, the picture in its
 * files or one downloaded, whichever is bigger, and import and downloads may change it again. If
 * there's none to be had it has no cover, and the chosen one goes as a replaced one does. Waits
 * for the network, so call it off the UI thread. MC_OK with nothing changed if no cover was
 * chosen; MC_ERR_NET, also with nothing changed, if a cover may be there but can't be reached;
 * otherwise as mc_album_art_set(). Other devices have the album's new cover only if the folder
 * has none, as for a cover that isn't chosen: see "Covers" in docs/sync-protocol.md. */
mc_status mc_album_art_reset(mc_library *lib, const char *album_uid);

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
