package ng.kithso.android.importer

import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.SystemClock
import android.util.Log
import androidx.compose.runtime.Immutable
import ng.kithso.android.R
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.core.CoreImport
import ng.kithso.core.MusicCoreException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.channels.Channel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File

/** What to import: a folder the user picked (a document tree), or documents picked or shared. */
sealed interface ImportSource {
    data class Folder(val tree: Uri) : ImportSource
    data class Documents(val uris: List<Uri>) : ImportSource
}

/** An item of one of the import's sessions. */
@Immutable
data class ItemKey(val batch: Int, val item: Int)

/** An item waiting for a decision, as the review list shows it. */
@Immutable
data class ReviewEntry(val key: ItemKey, val name: String, val reason: String?)

/** An item open in the review: what it is and its candidates on MusicBrainz. */
@Immutable
data class ReviewDetails(
    val key: ItemKey,
    val name: String,
    val info: CoreImport.ItemInfo,
    val matches: List<CoreImport.Match>,
)

/** How an import is going, as the import card and the notification show it. */
@Immutable
data class ImportProgress(
    val phase: Phase,
    /** Files copied out of the picked folder or documents so far, of [toCopy], while [Phase.Copying]. */
    val copied: Int = 0,
    val toCopy: Int = 0,
    /** The folder (batch) being worked on, of [batches]; folders are imported one at a time. */
    val batch: Int = 0,
    val batches: Int = 0,
    /** Audio files the scans have found so far. */
    val filesFound: Int = 0,
    /** Albums and singles the files were grouped into so far. */
    val items: Int = 0,
    val imported: Int = 0,
    /** Items waiting for a decision: no confident match on MusicBrainz. */
    val review: Int = 0,
    val skipped: Int = 0,
    val failed: Int = 0,
    /** MusicBrainz can't be reached: items wait for a decision without a lookup. */
    val offline: Boolean = false,
    /** Tagging was skipped, so everything is imported with the tags it has. */
    val taggingSkipped: Boolean = false,
    /** The items waiting for a decision, in order. */
    val reviewItems: List<ReviewEntry> = emptyList(),
    /** "Name: why" for each item that failed, and for what couldn't be copied. */
    val problems: List<String> = emptyList(),
) {
    enum class Phase {
        /** Copying files out of the picked folder or documents into the app's storage. */
        Copying,
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
 * Imports music the user picked with the Storage Access Framework, so the app needs no storage
 * permission. The core imports from file paths, so what was picked is copied into the app's
 * cache first ([root]), with the folders inside it kept (grouping goes by folder), and the core
 * imports the copies into [source]'s library; a zip is extracted by the core itself.
 *
 * A folder is imported a batch at a time, a batch being an album folder (with its disc
 * folders) or a zip: its files are copied, a core session ([CoreImport]) matches them, and
 * once the session has nothing more to do on its own the next batch is copied. A session whose
 * items are all settled is closed and its copies deleted, so the cache holds only the batch
 * being imported and the ones with items waiting for a decision. Documents are all copied up
 * front instead, as access to shared ones can end with the activity that received them.
 *
 * While a folder is being imported the app holds a persistable grant for it, released when
 * the import is over (or, after the process died mid-import, on the next start).
 *
 * It belongs to the process, not the UI, and [ImportService] keeps the process running while
 * it works. Every call on the sessions, and all of the state below, is on one thread
 * ([thread]); the core's events arrive on its import threads and are queued over to it.
 */
class Importer(
    private val context: Context,
    private val source: CoreLibrarySource,
    private val scope: CoroutineScope,
) {
    private val thread = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow<ImportProgress?>(null)
    private val events = Channel<Event>(Channel.UNLIMITED)

    /** Where picked files are copied (b<N>/ a batch each) and zips extracted (unzip/). */
    private val root = File(context.cacheDir, "import")

    /** The current or last import, or null for none (or once [dismiss]ed). */
    val progress: StateFlow<ImportProgress?> = state

    val isRunning: Boolean get() = state.value?.phase?.isOver == false

    private class Event(val batch: Batch, val event: CoreImport.Event, val arg: Int)

    private class Batch(val id: Int, val dir: File, val zip: File?) {
        var session: CoreImport? = null
        var states = emptyArray<CoreImport.State>()
        var found = 0
        var scanned = false
        var offline = false
        var closed = false
        /** Completed once the session has nothing left to do without the user. */
        val idle = CompletableDeferred<Unit>()
        /** Events that arrived before [session] was set. */
        val early = mutableListOf<Event>()
        val names = HashMap<Int, String>()
    }

    // Only touched on [thread].
    private val batches = mutableListOf<Batch>()
    private var runner: Job? = null
    private var runnerDone = false
    private var over = true
    private var grant: Uri? = null
    private var skipTagging = false
    private var copying: Pair<Int, Int>? = null
    private var scanning = false
    private var batchIndex = 0
    private var batchTotal = 0
    private val copyProblems = mutableListOf<String>()
    private var lastRefresh = 0L

    init {
        scope.launch(thread) {
            // Left over from an import the process died in: nothing can resume it.
            cleanUp()
            for (event in events) onEvent(event)
        }
    }

    /** Starts importing [what], unless an import is already running. */
    fun start(what: ImportSource): Boolean {
        if (isRunning) return false
        if (what is ImportSource.Documents && what.uris.isEmpty()) return false
        state.value = ImportProgress(ImportProgress.Phase.Copying)
        ImportService.clearSummary(context)
        ImportService.start(context)
        scope.launch(thread) {
            batches.clear()
            copyProblems.clear()
            runnerDone = false
            over = false
            skipTagging = false
            copying = 0 to 0
            scanning = false
            batchIndex = 0
            batchTotal = 0
            cleanUp()
            if (what is ImportSource.Folder) {
                // For as long as the import runs, so the folder stays readable whatever
                // happens to the activity that picked it.
                try {
                    context.contentResolver.takePersistableUriPermission(what.tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
                    grant = what.tree
                } catch (e: SecurityException) {
                    Log.w(TAG, "No persistable grant for ${what.tree}", e)
                }
            }
            runner = scope.launch(Dispatchers.IO) { run(what) }
        }
        return true
    }

    /** Imports the items waiting for a decision with the tags they have. */
    fun acceptAllAsIs() = resolveAll(CoreImport::acceptAsIs)

    /** Leaves out the items waiting for a decision. */
    fun skipAll() = resolveAll(CoreImport::skip)

    fun accept(key: ItemKey, match: Int) = resolve(key) { session, item -> session.accept(item, match) }
    fun acceptAsIs(key: ItemKey) = resolve(key, CoreImport::acceptAsIs)
    fun skip(key: ItemKey) = resolve(key, CoreImport::skip)
    fun searchWider(key: ItemKey) = resolve(key, CoreImport::searchWider)
    fun search(key: ItemKey, artist: String, title: String) = resolve(key) { session, item ->
        session.search(item, artist.trim().ifEmpty { null }, title.trim().ifEmpty { null })
    }

    /** Offline: imports everything not matched yet with its existing tags, this batch and the rest. */
    fun skipTagging() {
        scope.launch(thread) {
            skipTagging = true
            for (batch in batches) {
                try {
                    batch.session?.skipTagging()
                } catch (e: MusicCoreException) {
                    Log.w(TAG, "Couldn't skip tagging", e)
                }
            }
            publish()
        }
    }

    /** What the review shows of [key], or null once it's gone. */
    suspend fun details(key: ItemKey): ReviewDetails? = withContext(thread) {
        val batch = batches.getOrNull(key.batch) ?: return@withContext null
        val session = batch.session ?: return@withContext null
        try {
            ReviewDetails(key, session.itemName(key.item), session.itemInfo(key.item), session.matches(key.item))
        } catch (e: MusicCoreException) {
            null
        }
    }

    /** Stops the import. What has been imported stays. */
    fun cancel() {
        scope.launch(thread) {
            if (over) return@launch
            runner?.cancel()
            finish(ImportProgress.Phase.Cancelled)
        }
    }

    /** Forgets a finished import, so its summary goes away. */
    fun dismiss() {
        if (state.value?.phase?.isOver != true) return
        state.value = null
        ImportService.clearSummary(context)
    }

    // --- The batches, on Dispatchers.IO ---

    private suspend fun run(what: ImportSource) {
        val files = StagedFiles(context, root)
        try {
            val plan = when (what) {
                is ImportSource.Folder -> files.planFolder(what.tree)
                is ImportSource.Documents -> files.planDocuments(what.uris)
            }
            withContext(thread) {
                batchTotal = plan.size
                copyProblems += files.takeProblems()
                publish()
            }
            if (what is ImportSource.Documents) {
                // All up front: a shared document's grant can end with the activity it came to.
                val total = plan.sumOf { it.files.size }
                var done = 0
                val staged = plan.mapIndexed { i, batch ->
                    files.copy(batch, File(root, "b$i")) { done++; onCopied(done, total) }
                }
                withContext(thread) { copyProblems += files.takeProblems() }
                for ((i, path) in staged.withIndex()) {
                    if (path == null) continue
                    runBatch(i, plan[i], path)
                }
            } else {
                for ((i, batch) in plan.withIndex()) {
                    var done = 0
                    val total = batch.files.size
                    withContext(thread) {
                        batchIndex = i
                        copying = 0 to total
                        publish()
                    }
                    val path = files.copy(batch, File(root, "b$i")) { done++; onCopied(done, total) }
                    withContext(thread) { copyProblems += files.takeProblems() }
                    if (path != null) runBatch(i, batch, path)
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // A provider that went away, say: what was imported stays.
            Log.e(TAG, "Import stopped", e)
            withContext(thread) { copyProblems += context.getString(R.string.import_read_failed) }
        } finally {
            withContext(NonCancellable + thread) {
                copyProblems += files.takeProblems()
                runnerDone = true
                copying = null
                scanning = false
                publish()
                maybeFinish()
            }
        }
    }

    private suspend fun onCopied(done: Int, total: Int) {
        // Not every file: a folder of small files would flood the UI.
        if (done == total || done % 5 == 0) withContext(thread) {
            copying = done to total
            publish()
        }
    }

    // Starts a session for the batch copied to [path] and waits until it has nothing more to
    // do on its own.
    private suspend fun runBatch(index: Int, planned: StagedFiles.Batch, path: File) {
        val batch = withContext(thread) {
            batchIndex = index
            copying = null
            scanning = true
            publish()
            val batch = Batch(batches.size, File(root, "b$index"), if (planned.isZip) path else null)
            batches += batch
            val options = CoreImport.Options(
                fetchArt = true,
                skipTagging = skipTagging,
                stagingDir = File(root, "unzip").apply { mkdirs() }.path,
            )
            val session = try {
                source.startImport(listOf(path.path), options) { event, arg -> events.trySend(Event(batch, event, arg)) }
            } catch (e: MusicCoreException) {
                Log.e(TAG, "Couldn't start the import", e)
                null
            }
            if (session == null) {
                copyProblems += context.getString(R.string.library_open_failed)
                batch.closed = true
                batch.dir.deleteRecursively()
                batch.idle.complete(Unit)
            } else {
                batch.session = session
                for (event in batch.early) onEvent(event)
                batch.early.clear()
            }
            batch
        }
        batch.idle.await()
    }

    // --- Events and decisions, on [thread] ---

    private fun onEvent(e: Event) {
        val batch = e.batch
        if (batch.closed) return
        val session = batch.session ?: run {
            batch.early += e
            return
        }
        when (e.event) {
            CoreImport.Event.Scanning -> batch.found = e.arg
            CoreImport.Event.Scanned -> {
                batch.states = Array(session.itemCount) { session.itemState(it) }
                batch.scanned = true
                scanning = false
                // Extracted by now: the copy of the zip isn't needed.
                batch.zip?.delete()
                if (batch.states.isEmpty() || batch.states.all { it.isFinished }) settle(batch)
            }
            CoreImport.Event.ItemChanged -> {
                if (e.arg in batch.states.indices) {
                    val before = batch.states[e.arg]
                    batch.states[e.arg] = session.itemState(e.arg)
                    if (batch.states[e.arg] == CoreImport.State.Done && before != CoreImport.State.Done) refreshNowAndThen()
                    if (batch.scanned && batch.states.all { it.isFinished }) settle(batch)
                }
            }
            CoreImport.Event.Idle -> batch.idle.complete(Unit)
            CoreImport.Event.Offline -> batch.offline = e.arg != 0
        }
        publish()
        maybeFinish()
    }

    // Every item of the batch is settled: close its session and delete its copies.
    private fun settle(batch: Batch) {
        if (batch.closed) return
        batch.closed = true
        batch.names.clear()
        batch.idle.complete(Unit)
        batch.session?.close()
        batch.session = null
        batch.dir.deleteRecursively()
        source.refresh()
    }

    private fun resolve(key: ItemKey, how: (CoreImport, Int) -> Unit) {
        scope.launch(thread) {
            val session = batches.getOrNull(key.batch)?.session ?: return@launch
            try {
                how(session, key.item)
            } catch (e: MusicCoreException) {
                Log.w(TAG, "Couldn't resolve item $key", e)
            }
        }
    }

    private fun resolveAll(how: (CoreImport, Int) -> Unit) {
        scope.launch(thread) {
            for (batch in batches) {
                val session = batch.session ?: continue
                batch.states.indices.filter { batch.states[it] == CoreImport.State.Review }.forEach {
                    try {
                        how(session, it)
                    } catch (e: MusicCoreException) {
                        Log.w(TAG, "Couldn't resolve item ${batch.id}/$it", e)
                    }
                }
            }
        }
    }

    private fun maybeFinish() {
        if (over || !runnerDone) return
        if (batches.all { it.closed || (it.scanned && it.states.all { s -> s.isFinished }) }) {
            finish(ImportProgress.Phase.Finished)
        }
    }

    // The counts across the batches, and the phase they add up to.
    private fun publish() {
        val current = state.value ?: return
        if (current.phase.isOver) return
        val all = batches.flatMap { it.states.asList() }
        val counts = all.groupingBy { it }.eachCount()
        val review = counts[CoreImport.State.Review] ?: 0
        val busy = all.any { it.isBusy }
        val phase = when {
            copying != null -> ImportProgress.Phase.Copying
            scanning -> ImportProgress.Phase.Scanning
            runnerDone && !busy && review > 0 -> ImportProgress.Phase.Review
            else -> ImportProgress.Phase.Matching
        }
        state.value = current.copy(
            phase = phase,
            copied = copying?.first ?: 0,
            toCopy = copying?.second ?: 0,
            batch = batchIndex,
            batches = batchTotal,
            filesFound = batches.sumOf { it.found },
            items = all.size,
            imported = counts[CoreImport.State.Done] ?: 0,
            review = review,
            skipped = counts[CoreImport.State.Skipped] ?: 0,
            failed = counts[CoreImport.State.Failed] ?: 0,
            offline = !skipTagging && batches.any { !it.closed && it.offline },
            taggingSkipped = skipTagging,
            reviewItems = reviewEntries(),
            problems = copyProblems + failures(),
        )
    }

    private fun reviewEntries(): List<ReviewEntry> = batches.flatMap { batch ->
        val session = batch.session ?: return@flatMap emptyList()
        batch.states.indices.filter { batch.states[it] == CoreImport.State.Review }.map { item ->
            val name = batch.names.getOrPut(item) { session.itemName(item) }
            ReviewEntry(ItemKey(batch.id, item), name, session.itemError(item))
        }
    }

    // Failed items keep their names once their session is closed.
    private val failedNames = mutableMapOf<ItemKey, String>()

    private fun failures(): List<String> {
        for (batch in batches) {
            val session = batch.session ?: continue
            for (item in batch.states.indices) {
                if (batch.states[item] != CoreImport.State.Failed) continue
                val key = ItemKey(batch.id, item)
                if (key in failedNames) continue
                val name = session.itemName(item)
                failedNames[key] = session.itemError(item)?.let { context.getString(R.string.import_problem, name, it) } ?: name
            }
        }
        return failedNames.values.toList()
    }

    // Albums show up as they're imported, but reading the library takes a moment, so not
    // after every one.
    private fun refreshNowAndThen() {
        val now = SystemClock.elapsedRealtime()
        if (now - lastRefresh < REFRESH_INTERVAL_MS) return
        lastRefresh = now
        source.refresh()
    }

    // Closing a session waits for its import thread, at most a network timeout.
    private fun finish(phase: ImportProgress.Phase) {
        if (over) return
        publish()
        over = true
        for (batch in batches) {
            batch.closed = true
            batch.idle.complete(Unit)
            batch.session?.close()
            batch.session = null
        }
        cleanUp()
        source.refresh()
        source.onChanged()
        val p = state.value
        state.value = p?.copy(phase = phase, reviewItems = emptyList(), offline = false)
        failedNames.clear()
        Log.i(TAG, "Import ${phase.name.lowercase()}: ${p?.imported} imported, ${p?.skipped} skipped, ${p?.failed} failed")
    }

    // The copies, and the grant: none is left once an import is over.
    private fun cleanUp() {
        root.deleteRecursively()
        grant = null
        val resolver = context.contentResolver
        for (permission in resolver.persistedUriPermissions) {
            try {
                resolver.releasePersistableUriPermission(permission.uri, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            } catch (e: SecurityException) {
                Log.w(TAG, "Couldn't release ${permission.uri}", e)
            }
        }
    }

    private companion object {
        const val TAG = "kithsong"
        const val REFRESH_INTERVAL_MS = 3000L
    }
}
