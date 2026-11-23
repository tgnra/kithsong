/* Exporting the whole library as one zip, and restoring such a zip into an empty library.
 *
 * The zip holds kithsong-library.json first (what it is, for mc_library_archive_info()), then
 * library.db as it stood when the export began (a consistent copy, made with VACUUM INTO, so
 * the library can stay open and in use), then everything else in the library folder, music
 * and covers, by path. Files go in stored, not compressed: music is compressed already.
 *
 * It's written in one pass, front to back, so it can go to anything that takes a stream (a
 * pipe, a document provider). Each file is read twice, first for its checksum, so its size and
 * checksum are in the header before its data rather than in a descriptor after it, which
 * macOS's Archive Utility doesn't always manage; zip64 records are added only where a size,
 * offset or count needs them, as Info-ZIP and others do, and the file names are UTF-8.
 *
 * Restoring takes such a zip as the whole library, tags and all, with nothing re-tagged: the
 * files go into the library folder as they were, and library.db's contents replace the empty
 * library's, migrated to this build's schema and with a node id of its own, so it never poses
 * as the device it came from. */
#ifndef KITHCORE_LIBRARY_ARCHIVE_H
#define KITHCORE_LIBRARY_ARCHIVE_H

#include <stddef.h>
#include <stdint.h>

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;

/* The name of the zip's first entry, which says it's a library export. */
#define MC_LIBRARY_ARCHIVE_MANIFEST "kithsong-library.json"

/* Why an export or restore failed, beyond its mc_status. */
typedef enum mc_archive_problem {
  MC_ARCHIVE_OK = 0,
  MC_ARCHIVE_NO_SPACE,  /* the destination is full (export), or the library folder's volume
                         * hasn't room for the music (restore) */
  MC_ARCHIVE_TOO_BIG,   /* the destination can't hold a file this big: FAT32's 4 GB */
  MC_ARCHIVE_WRITE,     /* writing failed otherwise */
  MC_ARCHIVE_READ,      /* a library file couldn't be read, or changed while it was exported */
  MC_ARCHIVE_NOT_EMPTY, /* restore: the library has tracks already */
  MC_ARCHIVE_NEWER,     /* restore: the export is from a newer Kithsong, whose library this
                         * build can't open */
  MC_ARCHIVE_BAD        /* restore: not a library export, or damaged */
} mc_archive_problem;

/* Where the export goes: writes len bytes, returning 0, or errno (ENOSPC, EFBIG...) if it
 * couldn't. */
typedef int (*mc_archive_write_fn)(void *user, const void *buf, size_t len);
/* How far it has got, in bytes of total (both only grow, and total may grow a little as the
 * export finds library.db's size); returns non-zero to stop (MC_ERR_CANCELLED). */
typedef int (*mc_archive_progress_fn)(void *user, int64_t done, int64_t total);

/* Writes the library as a zip to write. temp_dir is where library.db's copy is made first
 * (and removed after): somewhere with room for it, outside the library folder. On failure what
 * was written is incomplete, for the caller to delete; problem, if given, says why. Runs on
 * the caller's thread for as long as it takes. */
mc_status mc_library_export(mc_library *lib, const char *temp_dir, mc_archive_write_fn write,
                            mc_archive_progress_fn progress, void *user, mc_archive_problem *problem);

typedef struct mc_archive_info {
  char library_uid[MC_UID_STRLEN];
  int schema;    /* library.db's schema version */
  int files;     /* files besides library.db */
  int64_t bytes; /* what they hold */
} mc_archive_info;

/* Whether the zip at zip_path is a library export, and what's in it: MC_OK, or MC_ERR_FORMAT
 * for any other zip (or not a zip at all). Reads only the zip's directory and manifest. */
mc_status mc_library_archive_info(const char *zip_path, mc_archive_info *out);

/* Restores the export at zip_path into lib, which must have no tracks (MC_ARCHIVE_NOT_EMPTY):
 * its files go into the library folder, and library.db's contents replace lib's, migrated
 * and with a new node id, keeping its library id. lib's handle stays open and sees the
 * restored library; other connections to it (sync's) should be opened again. temp_dir as for
 * export. On failure the library is left empty, as it was, and the files that came out of the
 * zip are removed. Not for a library in a sync group, whose library id it would change: the
 * frontend checks.
 *
 * On 32-bit Android, files of 2 GiB (2^31 bytes) or more can't be played or tagged (a 32-bit
 * off_t), so they're left in the zip rather than failing the restore: their tracks are
 * restored with no file on this device, as a track only another device holds, keeping their
 * plays and places in playlists. too_big, if given, says how many of the library's files were
 * left out (always 0 elsewhere). Anything else in the zip that big is left out unremarked. */
mc_status mc_library_restore(mc_library *lib, const char *zip_path, const char *temp_dir,
                             mc_archive_progress_fn progress, void *user, mc_archive_problem *problem,
                             int *too_big);

#ifdef __cplusplus
}
#endif

#endif
