/* Front covers for imported albums, saved as cover.jpg (or .png) in the album's folder.
 * That file is the album's art everywhere: pictures embedded in audio files are never
 * shown, only copied out to it on import (mc_coverart_read_embedded()).
 *
 * Download sources, in order:
 *   1. Cover Art Archive by MusicBrainz release id: exact, no guessing.
 *   2. Cover Art Archive by release group: another edition of the same album.
 *   3. iTunes Search API by artist + album name: wide coverage, but matched by name, so a
 *      result is only used when both names are close. No key needed. */
#ifndef MC_TAGGER_COVERART_H
#define MC_TAGGER_COVERART_H

#include "musiccore/library.h" /* mc_coverart_find() */
#include "musiccore/musiccore.h"
#include "net/http.h"

#define MC_ART_HASH_LEN 65 /* hex SHA-256 plus NUL */

/* A cover whose shorter side is below this many pixels is low resolution: import then
 * downloads one and keeps whichever is bigger. The biggest views draw a cover at about
 * 300-350 logical pixels (the album grids' widest tiles, the phone's full player at screen
 * width), which is 600-1100 device pixels at 2-3x: below 500 a cover is stretched more than
 * twice there and looks soft. 500 and 600 px covers, what most stores embedded for years,
 * pass, so most local art is kept without a request. Downloads are at most 1200 px. */
#define MC_COVER_LOW_RES 500

/* Called from mc_init(). */
void mc_coverart_global_init(void);

/* Downloads a front cover. Any of the ids and names may be NULL. On success *img holds JPEG
 * or PNG bytes (free with mc_buffer_free) and *ext is "jpg" or "png". cancel (NULL: never)
 * gives up on it, as for mc_http_get_cancel(). */
mc_status mc_coverart_fetch(const char *mb_albumid, const char *mb_releasegroupid,
                            const char *artist, const char *album, mc_buffer *img,
                            const char **ext, const mc_http_cancel *cancel);

/* Saves the image as <dir>/cover.<ext>, replacing any older one. */
mc_status mc_coverart_save(const char *dir, const mc_buffer *img, const char *ext);

/* The picture embedded in audio_path, in *img (free with mc_buffer_free) with *ext "jpg"
 * or "png". MC_ERR_NOT_FOUND if it has none, MC_ERR_FORMAT if it's not a JPEG or PNG of
 * sensible size. */
mc_status mc_coverart_read_embedded(const char *audio_path, mc_buffer *img, const char **ext);

/* Reads a cover image file into *img (free with mc_buffer_free). MC_ERR_FORMAT if it's
 * bigger than any cover should be. */
mc_status mc_coverart_read_file(const char *path, mc_buffer *img);

/* The shorter side of a JPEG or PNG, in pixels, from its header (nothing is decoded); 0 if
 * the header can't be read. */
int mc_coverart_short_side(const mc_buffer *img);

/* For tests: mc_coverart_fetch() calls fetch instead of going to the network, until set
 * back to NULL. Set it while no import is running. */
typedef mc_status (*mc_coverart_fetch_fn)(const char *mb_albumid, const char *mb_releasegroupid,
                                          const char *artist, const char *album, mc_buffer *img,
                                          const char **ext);
void mc_coverart_set_fake(mc_coverart_fetch_fn fetch);

/* Hex SHA-256 of a file's contents: what albums.art_hash stores, so devices can tell
 * whether they have the same cover. */
mc_status mc_coverart_hash(const char *path, char hash[MC_ART_HASH_LEN]);

#endif
