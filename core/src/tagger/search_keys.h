/* Clues for the importer's extended search (see "Extended search" in docs/tagging-rules.md):
 * for an album whose album artist tag names a label or circle rather than the artist, the
 * real artist, the label and the catalogue number often show up elsewhere, in the folder
 * name ("Label (Artist) - Title [CAT-001] (Event)"), the track artists, the file names
 * ("03. Artist — Title.flac") and catalogue-number tags. Pure functions, no I/O. */
#ifndef MC_TAGGER_SEARCH_KEYS_H
#define MC_TAGGER_SEARCH_KEYS_H

#include "musiccore/tagger.h"

#define MC_KEYS_MAX 4 /* of each kind of key */

/* A folder name taken apart. Every string may be NULL. */
typedef struct mc_folder_name {
  char *prefix; /* before " - ", without a parenthesised part: "Label" */
  char *paren;  /* the parenthesised part of the prefix, without "and V.A.": "Artist" */
  char *title;  /* after " - ", up to the first [bracketed] group or a trailing "(2004)" */
  int various;  /* the parenthesised part said "V.A." or "Various Artists" */
  char *catnos[MC_KEYS_MAX]; /* catalogue numbers in [brackets] */
  int catno_count;
} mc_folder_name;

void mc_folder_name_parse(const char *name, mc_folder_name *out);
void mc_folder_name_clear(mc_folder_name *f);

/* The disc in a title that ends with one: "危ないベースライン Disc1", "Title - Disc 2",
 * "Title CD1", "Title (Disc 1 of 2)", "Title [CD 2/2]". Returns the disc number and sets
 * *base_len to the length of the title without it (and the separators before it), or
 * returns 0 when there's no disc suffix or nothing before it ("Disc 1"). */
int mc_disc_suffix(const char *title, size_t *base_len);

/* Whether text (one bracket group or tag value, already split at "/") looks like a
 * catalogue number: 3 to 24 characters of ASCII letters, digits, spaces and "-._", with a
 * letter and at least three digits, and not a format or quality note like "24bit" or
 * "FLAC". "SRPC-0014" yes, "2002.08.11", "C89" (an event) and "V0" no. */
int mc_is_catno(const char *s, size_t len);

/* Adds the catalogue numbers in text (split at "/", "／", ",", ";") to list, skipping
 * ones already there; for one with a short letter suffix ("SRPC-0008LTD") the base
 * ("SRPC-0008") is added too. Returns the new count. */
int mc_catnos_add(char **list, int count, int max, const char *text);

/* The artist in a file name like "03. Artist — Title.flac" or "Artist - Title.mp3", or NULL.
 * With a title tag, the side that isn't the title is the artist; without one, the left side.
 * Free with free(). */
char *mc_filename_artist(const char *path, const char *title_tag);

/* The artist in a title tag like "Artist - Title", or NULL. Free with free(). */
char *mc_title_artist(const char *title);

/* An artist credit without its featured artists: "A feat. B" -> "A". Free with free(). */
char *mc_main_artist(const char *artist);

typedef struct mc_search_keys {
  char *album;        /* release title to search for: the album tag, else the folder's */
  char *folder_album; /* the folder's title, when it differs from album */
  char *artists[MC_KEYS_MAX]; /* likely release artists, most likely first */
  int artist_count;
  char *labels[MC_KEYS_MAX];
  int label_count;
  char *catnos[MC_KEYS_MAX];
  int catno_count;
  int various; /* no artist has most of the tracks: likely a compilation */
} mc_search_keys;

/* Gathers the keys for an album item. folder is the album folder (its last component is
 * parsed), files and paths the item's files, album/artist the item's album and artist (from
 * the tags), which aren't repeated among the keys. */
mc_status mc_search_keys_build(const char *folder, const mc_tags *files, const char *const *paths,
                               int n, const char *album, const char *artist, mc_search_keys *out);
void mc_search_keys_clear(mc_search_keys *k);

#endif
