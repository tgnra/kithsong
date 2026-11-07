/* Cover art for iPods with a colour screen: iPod_Control/Artwork/ArtworkDB and the
 * F<format>_1.ithmb files it points into (RGB565 little-endian, one picture after another).
 *
 *   mhfd  header, then three mhsd (index 1, 2, 3)
 *   mhsd 1: mhli + mhii*   one per track with a cover; mhii.song_id is the mhit dbid
 *           mhii: one mhod type 2 per size, holding an mhni (format, offset into the .ithmb,
 *           size, width, height) that holds an mhod type 3 (the file name, ":F1017_1.ithmb")
 *   mhsd 2: mhla           photo albums: empty in an ArtworkDB
 *   mhsd 3: mhlf + mhif*   one per size: format id and bytes per picture
 *
 * Covers of tracks the library didn't write are kept (their mhii is copied as it was, and
 * .ithmb files are only ever appended to). Entries for tracks no longer in the iTunesDB are
 * dropped; their pixels stay in the .ithmb as dead space. */
#ifndef KITHIPOD_ARTWORK_H
#define KITHIPOD_ARTWORK_H

#include "kithipod/common.h"
#include "kithipod/itunesdb.h"
#include "kithipod/sysinfo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ipod_artdb ipod_artdb;

/* Reads <mount>/iPod_Control/Artwork/ArtworkDB if there is one. With no formats (a model
 * without cover art) the handle does nothing and writes nothing. */
ipod_artdb *ipod_artdb_load(const char *mount, const ipod_artwork_format *formats, int n_formats,
                            ipod_error *err);
void ipod_artdb_free(ipod_artdb *a);
int ipod_artdb_enabled(const ipod_artdb *a);

/* Sets a track's cover from JPEG or PNG bytes: decoded, centre-cropped to a square and
 * scaled to each size now; written at ipod_artdb_write(). */
int ipod_artdb_set(ipod_artdb *a, uint64_t dbid, const uint8_t *image, size_t len, ipod_error *err);
void ipod_artdb_remove(ipod_artdb *a, uint64_t dbid);
int ipod_artdb_has(const ipod_artdb *a, uint64_t dbid);
int ipod_artdb_dirty(const ipod_artdb *a);

/* Appends new pictures to the .ithmb files and rewrites the ArtworkDB, keeping only
 * entries whose dbid is in db. */
int ipod_artdb_write(ipod_artdb *a, const ipod_db *db, ipod_error *err);

/* Decodes JPEG/PNG bytes into a w x h RGB565 little-endian picture (centre-cropped square
 * source, box-filtered). out holds w*h*2 bytes. */
int ipod_image_to_rgb565(const uint8_t *image, size_t len, int w, int h, uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif
