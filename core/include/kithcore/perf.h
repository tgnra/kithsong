/* A small profiler for UI speed: how long startup takes, how smoothly lists scroll and how
 * long a view takes to show. Frontends feed it, since only they know when something was
 * painted; the timing, the statistics and the report are here, so the numbers mean the same on
 * every platform.
 *
 * It's off until mc_perf_set_enabled(), and every feed returns at once while it's off, so the
 * hooks can stay in release builds. Settings' Testing section turns it on (the "perf_profile"
 * setting, MC_PERF_SETTING); KITHSONG_PERF=1 in the environment does too. To measure a cold
 * launch, a frontend reads the setting first thing and calls mc_perf_set_enabled(): the start
 * time is taken from the system, so nothing before that call is lost.
 *
 * All of this may be called from any thread, once mc_perf_set_enabled() has been called, which
 * every frontend does at startup, on or off. Names passed in are copied. */
#ifndef KITHCORE_PERF_H
#define KITHCORE_PERF_H

#include <stdint.h>

#include "kithcore/kithcore.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The settings.json key for "profile on the next start and from now on". */
#define MC_PERF_SETTING "perf_profile"

/* The fixed script of a scroll test, the same on every platform so that runs can be compared:
 * each view scrolls down this many frames, this far each frame (density-independent pixels:
 * 96 per inch on the desktops, 160 on Android). A view that runs out stops there. */
#define MC_PERF_SCROLL_FRAMES 360
#define MC_PERF_SCROLL_DP_PER_FRAME 40

/* Monotonic time in microseconds, finer than mc_mono_ms(). Not related to any other clock. */
uint64_t mc_perf_now_us(void);

int mc_perf_enabled(void);
/* Turning it on the first time asks the system when the process started, which is where
 * startup's times count from. */
void mc_perf_set_enabled(int on);
/* Whether KITHSONG_PERF asks for it. */
int mc_perf_env_enabled(void);

/* The display's refresh rate, which makes a frame late (dropped); 60 until set. */
void mc_perf_set_refresh_hz(double hz);

/* Startup: a phase ended now, or at at_us (mc_perf_now_us()) for something timed before the
 * setting could be read. Only a name's first mark counts, so a repeat is harmless. The report
 * lists them by time, each with the time since the last. */
void mc_perf_mark(const char *name);
void mc_perf_mark_at(const char *name, uint64_t at_us);

/* A line for the report's [run] section, about what was measured: the library's size, say.
 * Setting a key again replaces it. */
void mc_perf_note(const char *key, const char *value);
void mc_perf_note_int(const char *key, int64_t value);

/* Scrolling: a frame of series ("albums", say) was just painted while it scrolled. The time
 * between one and the next is the frame time; work_us is what painting took, or 0 if unknown.
 * Frames more than half a second apart count as a pause (the "pauses" line), not as frames,
 * except in a scroll test. */
void mc_perf_frame(const char *series, uint64_t work_us);
/* Series stopped scrolling: the next frame starts a new run of frames. */
void mc_perf_frame_break(const char *series);

/* A scroll test runs from begin to end. Its frames go into series of their own, named
 * "auto_<series>", which begin clears, and a long frame counts however long it is. */
void mc_perf_auto_begin(void);
void mc_perf_auto_end(void);
int mc_perf_auto_running(void);

/* A view switch: the click happened (view is what it opens), and then the view was painted.
 * begin again before end replaces the switch under way. */
void mc_perf_view_begin(const char *view);
void mc_perf_view_end(void);
/* Whether a switch is waiting for its mc_perf_view_end(). */
int mc_perf_view_pending(void);

/* Forgets the frames and view switches, keeping startup's marks and the notes. */
void mc_perf_clear(void);

/* The report: plain text, "key value" lines under [section] headings, in a fixed order, so
 * that `diff` between two runs shows what changed. Free with mc_free(); NULL if out of
 * memory. */
char *mc_perf_report(void);
/* Writes the report to path, replacing it. */
mc_status mc_perf_save(const char *path);

#ifdef __cplusplus
}
#endif

#endif
