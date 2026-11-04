/* Inside the scanner (musiccore/analysis.h). Internal to core. */
#ifndef MC_ANALYSIS_SCANNER_H
#define MC_ANALYSIS_SCANNER_H

#include <stdint.h>

#include "musiccore/analysis.h"

/* Holds the scanner off, for as long as the work that calls it runs: it writes nothing and
 * reads no file until the last hold is released. Sync needs this. A row logged during a session
 * moves this device's version vector under it, so the session ends in a digest mismatch and
 * sends every row of this device's origin again; scanning writes rows every few seconds for
 * hours. Holds count, and may come from any thread, before or without any scanner. */
void mc_analysis_hold(void);
void mc_analysis_release(void);

typedef enum mc_scan_step {
  MC_SCAN_NONE,      /* nothing left after the cursor */
  MC_SCAN_DONE,      /* a track analysed and stored */
  MC_SCAN_FAILED,    /* a file that won't decode, recorded so it isn't tried again */
  MC_SCAN_SKIPPED,   /* not now: its file is missing, or changed while it was read */
  MC_SCAN_CANCELLED
} mc_scan_step;

/* Does the next track after *cursor (a uid; "" to start) that needs it, moving the cursor on,
 * and says what became of it in *step. cancel is as for mc_analysis_file(), and is also asked
 * once more before writing, so it may wait out a hold. */
mc_status mc_analysis_scan_one(mc_library *lib, char cursor[MC_UID_STRLEN],
                               mc_analysis_cancel_fn cancel, void *user, mc_scan_step *step);

/* For tests: how long a scanner waits after starting, between passes over the library, and
 * whether it rests as it works. (30 s, 5 minutes and yes by default.) */
void mc_analysis_scanner_set_timing(uint32_t start_ms, uint32_t idle_ms, int rests);

#endif
