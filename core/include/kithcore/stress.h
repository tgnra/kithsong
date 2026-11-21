/* Stress test libraries: made-up libraries, in folders of their own, for testing how the UI
 * copes with a lot of music, with the profiler (perf.h) to measure it.
 *
 * Safety. The generator only writes into a new or empty folder, never the user's library, and
 * marks every folder it makes with MC_STRESS_MARKER; only such folders can be deleted from
 * here. mc_sync_new() and mc_scrobbler_new() refuse a stress library (mc_library_is_stress())
 * with MC_ERR_STRESS_LIBRARY, because pairing and the scrobbling account live in the device's
 * sync.db: otherwise it would be offered to the group and its plays sent to Last.fm.
 *
 * Content is fixed per preset (fixed seed) so runs compare: about 12 tracks per album, 40 per
 * artist, 500 per playlist, with awkward data (very long, non-ASCII and right-to-left names,
 * one huge album, one artist with thousands of tracks, multi-artist credits). The changelog
 * stays empty.
 *
 * Every track has a files row, but only the first of each album has a real file (half a
 * second of silence), to keep big presets fast; the player skips the rest as missing files. */
#ifndef KITHCORE_STRESS_H
#define KITHCORE_STRESS_H

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mc_library mc_library;

typedef enum mc_stress_preset {
  MC_STRESS_1K,
  MC_STRESS_10K,
  MC_STRESS_100K,
  MC_STRESS_PRESETS
} mc_stress_preset;

/* In every folder the generator makes. */
#define MC_STRESS_MARKER "kithsong-stress-library.txt"
/* The folder under the device's own (the config dir) they go in. */
#define MC_STRESS_ROOT "stress-libraries"
/* The settings.json key holding the library to go back to while a stress library is open.
 * With it, mc_settings_string() gives the way back. */
#define MC_STRESS_RETURN_SETTING "stress_return_library"

/* How many tracks a preset makes. */
int mc_stress_preset_tracks(mc_stress_preset preset);

/* Where preset's library goes under config_dir. malloc'd, free with mc_free(); NULL if out of
 * memory or preset isn't one. */
char *mc_stress_dir(const char *config_dir, mc_stress_preset preset);

/* Whether dir is a folder the generator finished. It may have been opened since. */
int mc_stress_is_complete(const char *dir);
/* Whether dir holds the marker: the generator made it, finished or not. */
int mc_stress_is_generated(const char *dir);
/* Whether the library is a stress library, wherever it has been moved to. */
int mc_library_is_stress(mc_library *lib);

/* The album with the most tracks, for a scroll test's track list. MC_ERR_NOT_FOUND if there's
 * none. Works on any library. */
mc_status mc_library_biggest_album(mc_library *lib, char uid[MC_UID_STRLEN]);

/* Generating, on a thread of its own, into folder dir: new or empty, or left unfinished by an
 * earlier run. MC_ERR_EXISTS if it holds anything else, a finished stress library included.
 * *out is freed with mc_stress_job_free(). */
typedef struct mc_stress_job mc_stress_job;
mc_status mc_stress_start(const char *dir, mc_stress_preset preset, mc_stress_job **out);
/* 0 to 100. Safe from any thread, as is everything on a job but free. */
int mc_stress_job_percent(const mc_stress_job *job);
/* Whether it has ended, and if it has, *status (may be NULL) says how: MC_OK, or why not. A
 * cancelled job ends with MC_ERR_NOT_FOUND, its folder gone. */
int mc_stress_job_done(const mc_stress_job *job, mc_status *status);
/* Asks it to stop; it removes what it made so far. */
void mc_stress_job_cancel(mc_stress_job *job);
/* Waits for it to end, cancelling nothing, and frees it. NULL is a no-op. */
void mc_stress_job_free(mc_stress_job *job);

/* Deletes stress library dir outright. MC_ERR_INVALID_ARG if it hasn't the marker (nothing is
 * touched then) or is a link, and MC_ERR_IN_USE if it's open_dir, the library open now
 * (may be NULL). */
mc_status mc_stress_delete(const char *dir, const char *open_dir);
/* The same for each stress library under config_dir but open_dir's. *deleted (may be NULL)
 * counts them. Folders without the marker are left alone. */
mc_status mc_stress_delete_all(const char *config_dir, const char *open_dir, int *deleted);
/* How many stress libraries config_dir has. */
int mc_stress_count(const char *config_dir);

#ifdef __cplusplus
}
#endif

#endif
