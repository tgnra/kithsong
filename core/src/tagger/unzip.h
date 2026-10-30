/* Extracting .zip files for the importer (miniz). A zip given to an import, or found in a
 * folder given to it, is extracted into a staging folder named after it, which the session
 * then scans like any folder. */
#ifndef MC_TAGGER_UNZIP_H
#define MC_TAGGER_UNZIP_H

#include <stdint.h>

#include "musiccore/musiccore.h"

/* The most a zip may hold, uncompressed: a big lossless discography fits, a zip bomb doesn't. */
#define MC_UNZIP_MAX_BYTES ((int64_t)8 * 1024 * 1024 * 1024)
/* The most all the zips of one import session may hold together: a folder of Bandcamp
 * downloads fits, a folder of zip bombs that each stay under the limit above doesn't. A
 * variable so tests can lower it; it's read when a session starts. */
#define MC_UNZIP_SESSION_MAX_BYTES ((int64_t)32 * 1024 * 1024 * 1024)
extern int64_t mc_unzip_session_max_bytes;

typedef enum mc_unzip_result {
  MC_UNZIP_OK,
  MC_UNZIP_BAD,       /* not a zip, damaged, encrypted, or compressed in a way miniz can't read */
  MC_UNZIP_TOO_BIG,   /* holds more than max_bytes uncompressed */
  MC_UNZIP_SESSION_FULL, /* fits on its own, but not with the zips extracted before it (budget) */
  MC_UNZIP_NO_SPACE,  /* not enough free space where it would be extracted */
  MC_UNZIP_WRITE,     /* couldn't write a file */
  MC_UNZIP_CANCELLED,
  MC_UNZIP_NOMEM
} mc_unzip_result;

typedef struct mc_unzip_stats {
  int files;    /* files extracted */
  int unsafe;   /* entries left out: absolute, or with ".." in the path (zip slip) */
  int64_t bytes; /* bytes written */
} mc_unzip_stats;

/* What the zips of one import session may hold together. mc_unzip adds what it wrote to used. */
typedef struct mc_unzip_budget {
  int64_t max;  /* the most, uncompressed */
  int64_t used; /* extracted so far */
} mc_unzip_budget;

/* Whether path names a .zip file (by its extension, ignoring case). */
int mc_unzip_is_zip(const char *path);

/* Extracts the zip at zip_path into dest (created if missing), keeping its folders. Entry
 * names are UTF-8 when the entry says so (general purpose flag bit 11); else they're taken
 * from its Info-ZIP Unicode Path extra field when it has one that matches, else CP437. Entries
 * that are absolute or climb out with ".." are left out and counted in stats->unsafe;
 * __MACOSX/ folders and .DS_Store files are left out silently. On Windows, characters a file
 * name can't hold become '_'. An entry whose path is already taken (a repeated name) is left
 * out. Nothing is written if the zip holds more than max_bytes uncompressed or more than the
 * free space at dest; the sizes are checked again while writing, so a zip whose headers lie
 * stops there. With a budget, nothing is written either if the zip would take the session past
 * budget->max (MC_UNZIP_SESSION_FULL), or if the free space wouldn't also cover what earlier
 * zips extracted (budget->used): those files are still to be copied into the library.
 * cancel, if set, is polled while extracting. On failure what was written stays in dest for
 * the caller to remove. */
mc_unzip_result mc_unzip(const char *zip_path, const char *dest, int64_t max_bytes,
                         mc_unzip_budget *budget, int (*cancel)(void *user), void *user,
                         mc_unzip_stats *stats);

/* An entry's name as UTF-8, from its raw bytes: decoded as UTF-8 if utf8 is set and the
 * bytes are valid UTF-8, else as CP437. malloc'd, or NULL. For tests too. */
char *mc_unzip_name(const char *raw, size_t len, int utf8);

/* The name in an Info-ZIP Unicode Path field (0x7075) among an entry's extra fields, if
 * there's one of version 1 whose CRC-32 matches raw (the name as stored, raw_len bytes) and
 * whose name is valid UTF-8; else NULL, and the stored name stands. malloc'd. For tests too. */
char *mc_unzip_unicode_path(const unsigned char *extra, size_t len, const char *raw, size_t raw_len);

/* The entry name's path inside the zip made safe, with '/' between its parts, or NULL if it
 * must be left out (*unsafe set when that's because it's absolute or uses ".."). Empty and "."
 * parts are dropped. malloc'd. For tests too. */
char *mc_unzip_safe_path(const char *name, int *unsafe);

#endif
