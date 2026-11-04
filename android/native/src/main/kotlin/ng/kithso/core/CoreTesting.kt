package ng.kithso.core

/**
 * The profiler (musiccore/perf.h) and stress test libraries (musiccore/stress.h). The profiler's
 * functions may be called from any thread, once [setEnabled] has been, and do nothing while
 * it's off; a stress library's job runs on a thread of its own.
 */
object CoreTesting {
    init {
        NativeCore // loads the library
    }

    /** The settings.json key of the profiler's switch (MC_PERF_SETTING). */
    const val SETTING = "perf_profile"
    const val SCROLL_FRAMES = 360 // MC_PERF_SCROLL_FRAMES
    const val SCROLL_DP_PER_FRAME = 40 // MC_PERF_SCROLL_DP_PER_FRAME
    const val PRESETS = 3 // MC_STRESS_PRESETS
    const val PRESET_10K = 1 // MC_STRESS_10K
    const val STATUS_CANCELLED = -8 // MC_ERR_NOT_FOUND, from a cancelled job

    @JvmStatic external fun nowUs(): Long
    @JvmStatic external fun enabled(): Boolean
    @JvmStatic external fun setEnabled(on: Boolean)
    @JvmStatic external fun setRefreshHz(hz: Double)
    @JvmStatic external fun mark(name: String)
    @JvmStatic external fun markAt(name: String, atUs: Long)
    @JvmStatic external fun note(key: String, value: String)
    @JvmStatic external fun frame(series: String)
    @JvmStatic external fun frameBreak(series: String)
    @JvmStatic external fun autoBegin()
    @JvmStatic external fun autoEnd()
    @JvmStatic external fun autoRunning(): Boolean
    @JvmStatic external fun viewBegin(view: String)
    @JvmStatic external fun viewEnd()
    @JvmStatic external fun clear()
    @JvmStatic external fun report(): String

    /** How many tracks [preset] makes. */
    @JvmStatic external fun presetTracks(preset: Int): Int
    /** Where [preset]'s library goes under [configDir]. */
    @JvmStatic external fun stressDir(configDir: String, preset: Int): String
    /** Whether [dir] is a library the generator finished: it has its marker file. */
    @JvmStatic external fun isComplete(dir: String): Boolean
    /** How many stress libraries [configDir] has. */
    @JvmStatic external fun count(configDir: String): Int

    /** Starts generating into [dir]; a job for the calls below, ended by [jobFree]. */
    @JvmStatic external fun start(dir: String, preset: Int): Long
    @JvmStatic external fun jobPercent(job: Long): Int
    @JvmStatic external fun jobDone(job: Long): Boolean
    /** How a finished job ended: 0, [STATUS_CANCELLED], or what went wrong. */
    @JvmStatic external fun jobStatus(job: Long): Int
    @JvmStatic external fun jobCancel(job: Long)
    @JvmStatic external fun jobFree(job: Long)

    /** Deletes the stress libraries under [configDir] but [openDir]; says how many. */
    @JvmStatic external fun deleteAll(configDir: String, openDir: String?): Int
}
