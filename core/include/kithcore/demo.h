/* The demo library: six made-up albums (34 tracks) for store screenshots and demos, the same
 * ones every platform's listing shows. The artists, albums and tracks are fictional, picked to
 * be absent from MusicBrainz, so nothing real appears in a screenshot.
 *
 * What's in it. Each track is a real FLAC file, tagged (title, artist, album artist, album,
 * year, genre, track and disc numbers), holding silence of a plausible length (2½ to 5
 * minutes) in a few kilobytes. Each album folder has a cover.jpg, drawn here from simple
 * shapes. Everything comes from fixed data, so the files are the same every run.
 *
 * mc_demo_create() puts it in a library through the importer, with tagging skipped: the rows
 * are what an import of tagged files leaves, with no MusicBrainz lookup and no network. The
 * library's uids and times are an import's, so those differ between runs; names, numbers,
 * order, covers and audio don't. A few albums are opened afterwards, newest last, so the
 * recents and Home have something in them.
 *
 * It's an ordinary library otherwise: nothing stops it syncing, so frontends open it only in
 * a config folder of its own (desktop --demo-library, Android's debug store_demo extra). */
#ifndef KITHCORE_DEMO_H
#define KITHCORE_DEMO_H

#include <stddef.h>

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;

#define MC_DEMO_ALBUMS 6
#define MC_DEMO_TRACKS 34
/* In a library folder mc_demo_create() made: "generating" while it works, then "complete". */
#define MC_DEMO_MARKER "kithsong-demo-library.txt"
/* The folder under a config folder the desktop apps keep the demo library in. */
#define MC_DEMO_DIR_NAME "demo-library"

/* Album i's (0 ..< MC_DEMO_ALBUMS) artist and title, static strings; NULL for a bad i. */
const char *mc_demo_album_artist(int i);
const char *mc_demo_album_title(int i);

/* Album i's cover as a JPEG of size x size pixels (64 to 2000), malloc'd into *data (free it
 * with mc_free()). */
mc_status mc_demo_cover_jpeg(int i, int size, unsigned char **data, size_t *len);

/* Writes the albums as files into dir, as Artist/Album/NN Title.flac with a cover.jpg in each
 * album folder. dir is made if missing; MC_ERR_EXISTS if it holds anything. */
mc_status mc_demo_write_folder(const char *dir);

/* Makes the demo library in library_dir, which must be missing or empty (MC_ERR_EXISTS
 * otherwise), unless it's a demo library cut off before it was complete: that's deleted and
 * made again. The files are written to a folder made in staging_dir (NULL for the system's
 * temporary folder; not inside library_dir), imported, and the folder deleted. Blocks until
 * done, a few seconds; needs mc_init(). On failure the half-made library is deleted. */
mc_status mc_demo_create(const char *library_dir, const char *staging_dir);

/* Whether dir holds a demo library mc_demo_create() finished. */
int mc_demo_is_complete(const char *dir);

/* Exposed for tests: the FLAC of n_samples of 44.1 kHz mono silence, untagged, malloc'd into
 * *data (free with mc_free()). */
mc_status mc_demo_silent_flac(uint64_t n_samples, unsigned char **data, size_t *len);

#ifdef __cplusplus
}
#endif

#endif
