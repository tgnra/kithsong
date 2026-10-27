package app.musicapp.importer

import android.content.Context
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.Immutable
import app.musicapp.CoreImport
import app.musicapp.MusicCoreException
import app.musicapp.R
import app.musicapp.data.CoreLibrarySource
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch

/** How an import is going, as the import card and the notification show it. */
@Immutable
data class ImportProgress(
    val phase: Phase,
    /** Audio files the scan has found so far. */
    val filesFound: Int = 0,
    /** Albums and singles the files were grouped into; 0 until the scan is done. */
    val items: Int = 0,
    val imported: Int = 0,
    /** Items waiting for a decision: no confident match on MusicBrainz. */
    val review: Int = 0,
    val skipped: Int = 0,
    val failed: Int = 0,
    /** Why the first item waiting for a decision couldn't be matched. */
    val reviewReason: String? = null,
    /** "Name: why" for each item that failed. */
    val problems: List<String> = emptyList(),
) {
    enum class Phase {
        Scanning,
        Matching,
        /** Everything is matched; the items in [review] wait for the user. */
        Review,
        Finished,
        Cancelled;

        val isOver: Boolean get() = this == Finished || this == Cancelled
    }

    val settled: Int get() = imported + review + skipped + failed
}

/**
 * Runs one import at a time through the core ([CoreImport]) into [source]'s library, keeping
 * [ImportProgress] up to date, and reads the library again as albums arrive. It belongs to the
 * process, not the UI, and [ImportService] keeps the process running while it works.
 *
 * Every call on the session runs on one thread ([thread]); the core's events arrive on its
 * import thread and are queued over to that one.
 */
class Importer(
    private val context: Context,
    private val source: CoreLibrarySource,
    private val scope: CoroutineScope,
) {
    private val thread = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow<ImportProgress?>(null)

    /** The current or last import, or null for none (or once [dismiss]ed). */
    val progress: StateFlow<ImportProgress?> = state

    val isRunning: Boolean get() = state.value?.phase?.isOver == false

    // Only touched on [thread].
    private var session: CoreImport? = null
    private var events: Channel<Pair<CoreImport.Event, Int>>? = null
    private var states = emptyArray<CoreImport.State>()
    private var lastRefresh = 0L

    /** Starts importing [paths], folders or files, unless an import is already running. */
    fun start(paths: List<String>, fetchArt: Boolean): Boolean {
        if (isRunning || paths.isEmpty()) return false
        state.value = ImportProgress(ImportProgress.Phase.Scanning)
        ImportService.clearSummary(context)
        ImportService.start(context)
        scope.launch(thread) {
            val events = Channel<Pair<CoreImport.Event, Int>>(Channel.UNLIMITED).also { this@Importer.events = it }
            val started = try {
                source.startImport(paths, fetchArt) { event, arg -> events.trySend(event to arg) }
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't start the import", e)
                null
            }
            if (started == null) {
                state.value = ImportProgress(ImportProgress.Phase.Finished, problems = listOf(context.getString(R.string.library_open_failed)))
                return@launch
            }
            session = started
            states = emptyArray()
            // Until finish() closes the channel. Events still queued then are dropped.
            for ((event, arg) in events) onEvent(session ?: break, event, arg)
        }
        return true
    }

    /** Imports the items waiting for a decision with the tags they have. */
    fun acceptAllAsIs() = resolve(CoreImport::acceptAsIs)

    /** Leaves out the items waiting for a decision. */
    fun skipAll() = resolve(CoreImport::skip)

    /** Stops the import. What has been imported stays. */
    fun cancel() {
        scope.launch(thread) {
            if (session == null) return@launch
            finish(ImportProgress.Phase.Cancelled)
        }
    }

    /** Forgets a finished import, so its summary goes away. */
    fun dismiss() {
        if (state.value?.phase?.isOver != true) return
        state.value = null
        ImportService.clearSummary(context)
    }

    private fun resolve(how: (CoreImport, Int) -> Unit) {
        scope.launch(thread) {
            val session = session ?: return@launch
            states.indices.filter { states[it] == CoreImport.State.Review }.forEach { how(session, it) }
            update(session, ImportProgress.Phase.Matching)
        }
    }

    // On [thread].
    private fun onEvent(session: CoreImport, event: CoreImport.Event, arg: Int) {
        val current = state.value ?: return
        when (event) {
            CoreImport.Event.Scanning -> state.value = current.copy(filesFound = arg)
            CoreImport.Event.Scanned -> {
                states = Array(session.itemCount) { session.itemState(it) }
                if (states.isEmpty()) finish(ImportProgress.Phase.Finished)
                else update(session, ImportProgress.Phase.Matching)
            }
            CoreImport.Event.ItemChanged -> {
                if (arg !in states.indices) return
                val before = states[arg]
                states[arg] = session.itemState(arg)
                if (states[arg] == CoreImport.State.Done && before != CoreImport.State.Done) refreshNowAndThen()
                if (states.all { it.isFinished }) finish(ImportProgress.Phase.Finished)
                else update(session, current.phase)
            }
            CoreImport.Event.Idle -> {
                if (states.all { it.isFinished }) finish(ImportProgress.Phase.Finished)
                else if (states.any { it == CoreImport.State.Review }) update(session, ImportProgress.Phase.Review)
            }
        }
    }

    // On [thread]: the counts from [states], in [phase].
    private fun update(session: CoreImport, phase: ImportProgress.Phase) {
        val counts = states.groupingBy { it }.eachCount()
        val firstReview = states.indexOfFirst { it == CoreImport.State.Review }
        state.value = (state.value ?: return).copy(
            phase = phase,
            items = states.size,
            imported = counts[CoreImport.State.Done] ?: 0,
            review = counts[CoreImport.State.Review] ?: 0,
            skipped = counts[CoreImport.State.Skipped] ?: 0,
            failed = counts[CoreImport.State.Failed] ?: 0,
            reviewReason = if (firstReview >= 0) session.itemError(firstReview) else null,
            problems = states.indices.filter { states[it] == CoreImport.State.Failed }.map { item ->
                session.itemError(item)?.let { context.getString(R.string.import_problem, session.itemName(item), it) }
                    ?: session.itemName(item)
            },
        )
    }

    // On [thread]. Albums show up as they're imported, but reading the library takes a moment,
    // so not after every one.
    private fun refreshNowAndThen() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastRefresh < REFRESH_INTERVAL_MS) return
        lastRefresh = now
        source.refresh()
    }

    // On [thread]. Closing waits for the core's import thread, at most a network timeout.
    private fun finish(phase: ImportProgress.Phase) {
        val session = session ?: return
        update(session, phase)
        this.session = null
        events?.close()
        events = null
        session.close()
        source.refresh()
        source.onChanged()
        val p = state.value
        Log.i(TAG, "Import ${phase.name.lowercase()}: ${p?.imported} imported, ${p?.skipped} skipped, ${p?.failed} failed")
    }

    private companion object {
        const val TAG = "musicapp"
        const val REFRESH_INTERVAL_MS = 3000L
    }
}
