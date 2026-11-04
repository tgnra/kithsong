/* Stress test libraries: made-up libraries, in folders of their own, for testing how the UI
 * copes with a lot of music, with the profiler (perf.h) to measure it.
 *
 * Safety. The generator only ever writes into a new folder, never one with anything in it, and
 * nothing here touches the library the user keeps their music in. Every folder it makes holds
 * the marker file MC_STRESS_MARKER, and only such a folder can be deleted from here. The
 * library inside says what it is (mc_library_is_stress()): mc_sync_new() and mc_scrobbler_new()
 * refuse it with MC_ERR_STRESS_LIBRARY. They have to, since a new library doesn't start clean:
 * pairing and the scrobbling account are in the device's sync.db, not in the library, so
 * without this a stress library would be offered to the group, and its plays would go to
 * Last.fm.
 *
 * What's in one. The same every time for a preset, with a fixed seed, so runs can be compared:
 * about one album to 12 tracks, an artist to 40, a playlist to 500, with awkward data among
 * them: names hundreds of characters long, non-ASCII and right-to-left names, one album far
 * bigger than the rest, one artist with thousands of tracks at the top preset, compilations and
 * credits with several artists. The changelog stays empty, as the library never syncs.
 *
 * Audio. Every track has a row in files, as if it were here, but only the first track of each
 * album has a real file: half a second of silence. Generating a file for each would make a big
 * preset far slower, and UI speed doesn't need them. Playing a track without one skips it, as
 * for any file that's gone: the player reports an error (MC_PLAYER_EVENT_ERROR) and goes on to
 * the next. Every file written is in library.db, the covers apart (cover.jpg, as usual). */
#ifndef MUSICCORE_STRESS_H
#define MUSICCORE_STRESS_H

#include "musiccore/musiccore.h"

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
