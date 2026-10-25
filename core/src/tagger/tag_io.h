/* Parts of the TagLib shim (tag_io.cpp) that are internal to core. The rest is in
 * musiccore/tagger.h. */
#ifndef MC_TAGGER_TAG_IO_H
#define MC_TAGGER_TAG_IO_H

#include <stddef.h>

#include "musiccore/musiccore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The file's embedded front cover, or its first picture if none is marked as the front.
 * On success *data holds the image file's bytes (free with free()). MC_ERR_NOT_FOUND if
 * there's no picture. */
mc_status mc_tags_read_picture(const char *path, unsigned char **data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
