/* Front covers for imported albums, saved as cover.jpg (or .png) in the album's folder.
 *
 * Sources, in order:
 *   1. Cover Art Archive by MusicBrainz release id: exact, no guessing.
 *   2. Cover Art Archive by release group: another edition of the same album.
 *   3. iTunes Search API by artist + album name: wide coverage, but matched by name, so a
 *      result is only used when both names are close. No key needed. */
#ifndef MC_TAGGER_COVERART_H
#define MC_TAGGER_COVERART_H

#include "musiccore/musiccore.h"
#include "net/http.h"

#define MC_ART_HASH_LEN 65 /* hex SHA-256 plus NUL */

/* Called from mc_init(). */
void mc_coverart_global_init(void);

/* Downloads a front cover. Any argument may be NULL. On success *img holds JPEG or PNG
 * bytes (free with mc_buffer_free) and *ext is "jpg" or "png". */
mc_status mc_coverart_fetch(const char *mb_albumid, const char *mb_releasegroupid,
                            const char *artist, const char *album, mc_buffer *img,
                            const char **ext);

/* Finds an existing cover image in dir: cover, folder or front (.jpg, .jpeg or .png, any
 * case), in that order. Returns a malloc'd full path, or NULL. */
char *mc_coverart_find(const char *dir);

/* Saves the image as <dir>/cover.<ext>, replacing any older one. */
mc_status mc_coverart_save(const char *dir, const mc_buffer *img, const char *ext);

/* Hex SHA-256 of a file's contents: what albums.art_hash stores, so devices can tell
 * whether they have the same cover. */
mc_status mc_coverart_hash(const char *path, char hash[MC_ART_HASH_LEN]);

#endif
