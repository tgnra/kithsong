package ng.kithso.core

// One run of the audio scanner over a library (musiccore/analysis.h), bound in jni_analysis.c.

/**
 * Analyses the tracks of the library in [dir] that have a file here and no result yet, on a
 * core thread and connection of its own, from now until it has been through them or is
 * [stop]ped. It stands aside during sync sessions and imports by itself. What it has done stays
 * whenever it stops, and the next run goes on from there. Throws MusicCoreException if it can't
 * start.
 */
class CoreAnalysis(dir: String) {
    enum class State {
        Running,

        /** Nothing's left that it could do. */
        Finished,

        /** The library couldn't be opened or written: try another time. */
        GaveUp,
    }

    private var handle = AnalysisNatives.nativeStart(dir)

    val state: State
        get() = when (AnalysisNatives.nativeState(handle)) {
            0 -> State.Running
            1 -> State.Finished
            else -> State.GaveUp
        }

    /** Stops it within a moment, whatever the file. Call once, whatever the [state]. */
    fun stop() {
        AnalysisNatives.nativeStop(handle)
        handle = 0
    }
}

/** The tracks a run would go through now: those with a file here and no valid result. */
fun CoreLibrary.analysisPending(): Int = AnalysisNatives.nativePending(handle)

internal object AnalysisNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeStart(dir: String): Long
    @JvmStatic external fun nativePending(library: Long): Int
    @JvmStatic external fun nativeState(handle: Long): Int
    @JvmStatic external fun nativeStop(handle: Long)
}
