/* The artist refresh after upgrading to library v13: splits the credits imported before
 * (docs/tagging-rules.md, "Refresh after upgrading"). Runs by itself on a thread of its own,
 * started by mc_library_open() and stopped by mc_library_close(). */
#ifndef MC_TAGGER_REFRESH_H
#define MC_TAGGER_REFRESH_H

#include <stdint.h>

#include "musiccore/library.h"

typedef struct mc_refresh mc_refresh;

/* Called from mc_init(). Until then no refresh starts. */
void mc_refresh_global_init(void);

/* Starts the refresh of lib's library if meta.artist_refresh is 'pending' and no other
 * handle in this process is refreshing the same folder. NULL if none was started. */
mc_refresh *mc_refresh_start(mc_library *lib);

/* Stops it and waits for its thread, which stops at its next step (a MusicBrainz request or
 * a busy wait is given up); progress is kept, so the next open goes on from there. NULL is
 * fine. */
void mc_refresh_stop(mc_refresh *r);

/* 1 while it runs, with how many albums and tracks with no album it has done of those it
 * found to do; else 0. NULL is fine. */
int mc_refresh_progress(mc_refresh *r, int *done, int *total);

/* For tests: how long a refresh waits after the library opens before it starts (3 s by
 * default, so a handle opened for a moment does nothing). */
void mc_refresh_set_delay_ms(uint32_t ms);

/* For tests: waits until lib's refresh, if it has one, has finished or stopped. */
void mc_refresh_wait(mc_library *lib);

#endif
