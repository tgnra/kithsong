package ng.kithso.android

import android.util.Log
import java.io.File
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.core.CoreTesting
import ng.kithso.core.KithCoreException

/**
 * Settings' Testing section: the profiler's switch, and stress test libraries in folders of their
 * own under [configDir]. A phone has one library folder, so a stress library is opened by saving
 * its folder ([AppSettings.stressLibrary]) and starting the process again, as nothing here can
 * swap the library under the player and sync; going back clears it and does the same. Anything
 * doubtful at startup (see [KithsongApp]) falls back to the real library.
 */
class Testing(
    private val app: KithsongApp,
    private val configDir: File,
    private val settings: AppSettings,
    private val scope: CoroutineScope,
) {
    /** How generating or deleting is going. */
    data class State(
        val percent: Int? = null,
        val deleting: Boolean = false,
        /** Of the last generating or deleting: a status code, or null. */
        val failure: Int? = null,
        /** How many libraries the last delete removed, or null. */
        val deleted: Int? = null,
        val libraries: Int = 0,
    )

    private val mutableState = MutableStateFlow(State(libraries = CoreTesting.count(configDir.path)))
    val state: StateFlow<State> = mutableState
    @Volatile private var job = 0L

    /** Whether a stress library is open. */
    val inStress: Boolean get() = app.libraryDir != app.realLibraryDir && !app.demoLibrary

    /**
     * Whether the demo library (kithcore/demo.h) is open, for store screenshots. It's opened as
     * a stress library is, but nothing says so on screen: it's there to look like a real one.
     */
    val inDemo: Boolean get() = app.demoLibrary

    /**
     * Debug builds' `--ez store_demo true`: makes the demo library under [configDir], unless
     * it's there from before, and opens it; `false` goes back to the real library.
     */
    fun storeDemo(on: Boolean) {
        if (!on) {
            if (inDemo) back()
            return
        }
        if (inDemo || mutableState.value.percent != null) return
        val dir = File(configDir, CoreTesting.DEMO_DIR_NAME).path
        if (CoreTesting.isDemoComplete(dir)) {
            open(dir)
            return
        }
        mutableState.value = mutableState.value.copy(percent = 0, failure = null, deleted = null)
        scope.launch {
            val status = withContext(Dispatchers.IO) { CoreTesting.demoCreate(dir, app.cacheDir.path) }
            mutableState.value = mutableState.value.copy(percent = null, failure = status.takeIf { it != 0 })
            if (status == 0) open(dir) else Log.e(TAG, "Couldn't make the demo library: $status")
        }
    }

    fun setProfiling(on: Boolean) {
        CoreTesting.setEnabled(on)
        settings.perfProfile = on
    }

    /** Makes [preset]'s library, unless it's there from before, and opens it. */
    fun generate(preset: Int) {
        if (mutableState.value.percent != null) return
        val dir = CoreTesting.stressDir(configDir.path, preset)
        if (CoreTesting.isComplete(dir)) {
            open(dir)
            return
        }
        mutableState.value = mutableState.value.copy(percent = 0, failure = null, deleted = null)
        scope.launch {
            val status = withContext(Dispatchers.IO) {
                try {
                    job = CoreTesting.start(dir, preset)
                    while (!CoreTesting.jobDone(job)) {
                        mutableState.value = mutableState.value.copy(percent = CoreTesting.jobPercent(job))
                        delay(POLL_MS)
                    }
                    CoreTesting.jobStatus(job).also {
                        CoreTesting.jobFree(job)
                        job = 0
                    }
                } catch (e: KithCoreException) {
                    e.status
                }
            }
            mutableState.value = mutableState.value.copy(
                percent = null,
                failure = status.takeIf { it != 0 && it != CoreTesting.STATUS_CANCELLED },
                libraries = CoreTesting.count(configDir.path),
            )
            if (status == 0) open(dir)
        }
    }

    fun cancel() {
        val running = job
        if (running != 0L) CoreTesting.jobCancel(running)
    }

    fun back() {
        settings.stressLibrary = null
        restart()
    }

    fun deleteAll() {
        if (mutableState.value.deleting) return
        mutableState.value = mutableState.value.copy(deleting = true, failure = null, deleted = null)
        val open = app.libraryDir.path
        scope.launch {
            var failure: Int? = null
            var deleted: Int? = null
            withContext(Dispatchers.IO) {
                try {
                    deleted = CoreTesting.deleteAll(configDir.path, open)
                } catch (e: KithCoreException) {
                    failure = e.status
                }
            }
            mutableState.value = mutableState.value.copy(
                deleting = false,
                failure = failure,
                deleted = deleted,
                libraries = CoreTesting.count(configDir.path),
            )
        }
    }

    private fun open(dir: String) {
        settings.stressLibrary = dir
        restart()
    }

    private fun restart() {
        Log.i(TAG, "Restarting for ${settings.stressLibrary ?: "the real library"}")
        app.restart()
    }

    private companion object {
        const val TAG = "kithsong"
        const val POLL_MS = 150L
    }
}
