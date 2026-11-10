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
 * order, covers and audio don't. A few albums are opened afterwards, newest last, and a few
 * tracks played a few times, so the recents, Home and Most Played have something in them.
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

/* --- Screenshot states ---
 * For the desktops' --demo-playing and --page import / sync (scripts/store-screenshots). Only
 * ever run with those options, on the demo library. */

/* --demo-playing's value, "<album title>#<track number>@<m:ss>" (e.g. "Copperline#1@0:21"):
 * that track of the album in lib, in *uid, and the position in *position_ms. MC_ERR_INVALID_ARG
 * for a value not in that form, MC_ERR_NOT_FOUND if lib has no such track. */
mc_status mc_demo_playing_track(mc_library *lib, const char *spec, char uid[MC_UID_STRLEN], int *position_ms);

/* An import part-way through, as an mc_import session (kithcore/tagger.h) the import page
 * shows like any other: made-up albums, imported, being written, being matched, waiting, one
 * already in the demo library, and one needing a decision (MC_DEMO_IMPORT_REVIEW, with three
 * candidates and a missing track). Nothing is read, looked up or written: no files, no
 * MusicBrainz, no library. Events come as from a real session's thread (scanned, then each
 * item changed), but never MC_IMPORT_EV_IDLE. Requests (accept, search, ...) are taken and
 * never acted on. Free with mc_import_free(). Declared in kithcore/tagger.h, whose types it
 * takes:
 *   mc_status mc_demo_import_start(mc_import_fn callback, void *user, mc_import **out); */

/* The demo group, for --page sync: writes into config_dir's sync.db (made if missing) a group
 * syncing the demo library in library_dir, of this device (named device_name, NULL for
 * "Living Room PC"; the archive), MC_DEMO_PHONE_NAME (a selection of three albums as Opus
 * 160 kbps, last synced a few minutes ago, most of it still to come) and MC_DEMO_WATCH_NAME
 * (one album as Opus 96 kbps). Neither is real: nothing can connect as them. Does nothing if
 * config_dir's device is in a group already. Before mc_sync_new() for that config_dir. */
mc_status mc_demo_sync_group(const char *library_dir, const char *config_dir, const char *device_name);
#define MC_DEMO_PHONE_NAME "Ivy's phone"
#define MC_DEMO_PHONE_UID "3f9c2d4e-8b1a-4c6f-a2d7-5e0b9c1f7a33"
#define MC_DEMO_WATCH_NAME "Ivy's watch"

/* Shows the phone of mc_demo_sync_group() online and part-way through a sync: sync's on_peer
 * and on_progress report it as a session would (files going to it, 7 of 11, with a speed and
 * time left), and nothing more comes for it. sync runs no automatic sessions or reach checks
 * afterwards. Call once sync has started, from the thread that drives it. */
typedef struct mc_sync mc_sync;
mc_status mc_demo_sync_show(mc_sync *sync);

#ifdef __cplusplus
}
#endif

#endif
