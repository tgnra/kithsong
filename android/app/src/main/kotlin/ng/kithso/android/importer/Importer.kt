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
import ng.kithso.core.KithCoreException
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Deferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.async
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
    val files: List<CoreImport.ItemFile>,
    /** Other items from its folder, not busy, whose files may be its missing tracks. */
    val related: List<RelatedItem>,
)

/** Another item from an item's folder, in the review: its album, or a single's title. */
@Immutable
data class RelatedItem(val item: Int, val name: String, val files: List<CoreImport.ItemFile>)

/** A file picked for a missing track: [path] of item [fromItem], for [track] of the match. */
@Immutable
data class TrackPick(val track: Int, val fromItem: Int, val path: String)

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
    /** Items waiting for a decision because MusicBrainz couldn't be reached, offline or not. */
    val unreachable: Int = 0,
    /** Tagging was skipped, so everything is imported with the tags it has. */
    val taggingSkipped: Boolean = false,
    /** The items waiting for a decision, in order. */
    val reviewItems: List<ReviewEntry> = emptyList(),
    /** "Name: why" for each item that failed, and for what couldn't be copied. */
    val problems: List<String> = emptyList(),
    /** "Name: what happened" for each item the library already had some of. */
    val outcomes: List<String> = emptyList(),
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
 * What's shared or picked while an import runs is imported after everything it was already
 * doing ([add]): documents are copied straight away, and a folder a batch at a time as ever.
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
        /** Picks being applied, one at a time, as the core renumbers files at each. */
        val assigns = ArrayDeque<Assign>()
    }

    private class Assign(val item: Int, val match: Int, val pick: TrackPick) {
        var sent = false
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
    /** What was [add]ed, waiting for the batches before it, in order. */
    private val added = ArrayDeque<Added>()
    /** The import's own batches; those [add]ed are numbered after them, in the order they run. */
    private var sourceBatches = 0
    private var addedRun = 0
    private var addedCount = 0

    // A batch of documents [add]ed, copied into [dir]; [path] is what the core gets, or null.
    private class Staged(val batch: StagedFiles.Batch, val dir: File, val path: File?)

    private sealed interface Added {
        // Documents, being copied.
        class Documents(val staged: Deferred<List<Staged>>) : Added
        class Folder(val tree: Uri) : Added
    }

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
            added.clear()
            sourceBatches = 0
            addedRun = 0
            addedCount = 0
            cleanUp()
            if (what is ImportSource.Folder) holdGrant(what.tree)
            runner = scope.launch(Dispatchers.IO) { run(what) }
        }
        return true
    }

    /**
     * Adds [what], shared or picked while an import runs, to the end of it, or starts one with
     * it if it's over by now. Documents are copied straight away, as their grant can end with
     * the activity they came to. A core session takes its paths up front, so these get sessions
     * of their own, as the import's batches do, started once the batches before have nothing
     * more to do on their own.
     */
    fun add(what: ImportSource) {
        if (what is ImportSource.Documents && what.uris.isEmpty()) return
        scope.launch(thread) {
            if (over) {
                start(what)
                return@launch
            }
            added += when (what) {
                is ImportSource.Documents -> {
                    val n = addedCount++
                    Added.Documents(scope.async(Dispatchers.IO) { copyAdded(what.uris, File(root, "a$n")) })
                }
                is ImportSource.Folder -> {
                    holdGrant(what.tree)
                    Added.Folder(what.tree)
                }
            }
            // Everything so far is waiting for the user: a runner for these.
            if (runnerDone) {
                runnerDone = false
                runner = scope.launch(Dispatchers.IO) { runAdded() }
            }
        }
    }

    // For as long as the import runs, so the folder stays readable whatever happens to the
    // activity that picked it.
    private fun holdGrant(tree: Uri) {
        try {
            context.contentResolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION)
            grant = tree
        } catch (e: SecurityException) {
            Log.w(TAG, "No persistable grant for $tree", e)
        }
    }

    /** Imports the items waiting for a decision with the tags they have. */
    fun acceptAllAsIs() = resolveAll(CoreImport::acceptAsIs)

    /** Leaves out the items waiting for a decision. */
    fun skipAll() = resolveAll(CoreImport::skip)

    fun accept(key: ItemKey, match: Int) = resolve(key) { session, item -> session.accept(item, match) }

    /**
     * Imports [key] with candidate [match], its missing tracks filled by [picks]. The first
     * assign writes the item, and those after it re-file it, so its batch stays open until the
     * last has been seen through.
     */
    fun accept(key: ItemKey, match: Int, picks: List<TrackPick>) {
        if (picks.isEmpty()) return accept(key, match)
        scope.launch(thread) {
            val batch = batches.getOrNull(key.batch) ?: return@launch
            picks.mapTo(batch.assigns) { Assign(key.item, match, it) }
            nextAssign(batch)
        }
    }
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
                } catch (e: KithCoreException) {
                    Log.w(TAG, "Couldn't skip tagging", e)
                }
            }
            publish()
        }
    }

    /** Looks up again everything that couldn't reach MusicBrainz, in every batch. */
    fun retryUnreachable() {
        scope.launch(thread) {
            for (batch in batches) {
                try {
                    batch.session?.retryUnreachable()
                } catch (e: KithCoreException) {
                    Log.w(TAG, "Couldn't retry MusicBrainz", e)
                }
            }
        }
    }

    /** What the review shows of [key], or null once it's gone. */
    suspend fun details(key: ItemKey): ReviewDetails? = withContext(thread) {
        val batch = batches.getOrNull(key.batch) ?: return@withContext null
        val session = batch.session ?: return@withContext null
        try {
            val related = session.related(key.item).filterNot { session.itemState(it).isBusy }.map { item ->
                val info = session.itemInfo(item)
                RelatedItem(item, info.album, session.files(item))
            }
            ReviewDetails(
                key,
                session.itemName(key.item),
                session.itemInfo(key.item),
                session.matches(key.item),
                session.files(key.item),
                related,
            )
        } catch (e: KithCoreException) {
            null
        }
    }

    /** Stops the import. What has been imported stays. */
    fun cancel() {
        scope.launch(thread) {
            if (over) return@launch
            runner?.cancel()
            added.forEach { (it as? Added.Documents)?.staged?.cancel() }
            added.clear()
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
                sourceBatches = plan.size
                batchTotal += plan.size
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
                for ((i, batch) in plan.withIndex()) copyAndRun(files, i, batch)
            }
            runAddedBatches()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            // A provider that went away, say: what was imported stays.
            Log.e(TAG, "Import stopped", e)
            withContext(thread) { copyProblems += context.getString(R.string.import_read_failed) }
        } finally {
            withContext(NonCancellable + thread) {
                copyProblems += files.takeProblems()
                runnerFinished()
            }
        }
    }

    // One batch of a folder: copied, then imported.
    private suspend fun copyAndRun(files: StagedFiles, index: Int, batch: StagedFiles.Batch) {
        var done = 0
        val total = batch.files.size
        withContext(thread) {
            batchIndex = index
            copying = 0 to total
            publish()
        }
        val path = files.copy(batch, File(root, "b$index")) { done++; onCopied(done, total) }
        withContext(thread) { copyProblems += files.takeProblems() }
        if (path != null) runBatch(index, batch, path)
    }

    // A runner for what was [add]ed once the last one was done, or as it stopped.
    private suspend fun runAdded() {
        try {
            runAddedBatches()
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Log.e(TAG, "Import stopped", e)
            withContext(thread) { copyProblems += context.getString(R.string.import_read_failed) }
        } finally {
            withContext(NonCancellable + thread) { runnerFinished() }
        }
    }

    private suspend fun runAddedBatches() {
        while (true) {
            when (val next = withContext(thread) { added.removeFirstOrNull() } ?: return) {
                is Added.Documents -> for (staged in next.staged.await()) {
                    if (staged.path == null) continue
                    val index = withContext(thread) { sourceBatches + addedRun++ }
                    runBatch(index, staged.batch, staged.path, staged.dir)
                }
                is Added.Folder -> runAddedFolder(next.tree)
            }
        }
    }

    private suspend fun runAddedFolder(tree: Uri) {
        val files = StagedFiles(context, root)
        try {
            val plan = files.planFolder(tree)
            withContext(thread) {
                batchTotal += plan.size
                publish()
            }
            for (batch in plan) copyAndRun(files, withContext(thread) { sourceBatches + addedRun++ }, batch)
        } finally {
            withContext(NonCancellable + thread) { copyProblems += files.takeProblems() }
        }
    }

    // Copies documents [add]ed under [dir] (a folder each for their batches), as [run] does.
    private suspend fun copyAdded(uris: List<Uri>, dir: File): List<Staged> {
        val files = StagedFiles(context, root)
        try {
            val plan = files.planDocuments(uris)
            withContext(thread) {
                batchTotal += plan.size
                publish()
            }
            return plan.mapIndexed { i, batch ->
                val to = File(dir, "$i")
                Staged(batch, to, files.copy(batch, to) {})
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: Exception) {
            Log.e(TAG, "Couldn't copy the shared files", e)
            withContext(thread) { copyProblems += context.getString(R.string.import_read_failed) }
            return emptyList()
        } finally {
            withContext(NonCancellable + thread) {
                copyProblems += files.takeProblems()
                publish()
            }
        }
    }

    // As a runner stops, on [thread]: what was added meanwhile gets one of its own,
    // otherwise the import finishes once its items are settled.
    private fun runnerFinished() {
        copying = null
        scanning = false
        if (!over && added.isNotEmpty()) {
            runner = scope.launch(Dispatchers.IO) { runAdded() }
        } else {
            runnerDone = true
        }
        publish()
        maybeFinish()
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
    private suspend fun runBatch(index: Int, planned: StagedFiles.Batch, path: File, dir: File = File(root, "b$index")) {
        val batch = withContext(thread) {
            batchIndex = index
            copying = null
            scanning = true
            publish()
            val batch = Batch(batches.size, dir, if (planned.isZip) path else null)
            batches += batch
            val options = CoreImport.Options(
                fetchArt = true,
                skipTagging = skipTagging,
                stagingDir = File(root, "unzip").apply { mkdirs() }.path,
            )
            val session = try {
                source.startImport(listOf(path.path), options) { event, arg -> events.trySend(Event(batch, event, arg)) }
            } catch (e: KithCoreException) {
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
                batch.states.indices.filter { batch.states[it].isFinished }.forEach { noteOutcome(batch, session, it) }
                batch.scanned = true
                scanning = false
                // Extracted by now: the copy of the zip isn't needed.
                batch.zip?.delete()
                if (batch.states.isEmpty() || batch.states.all { it.isFinished }) settle(batch)
            }
            CoreImport.Event.ItemChanged -> {
                nextAssign(batch)
                if (e.arg in batch.states.indices) {
                    val before = batch.states[e.arg]
                    batch.states[e.arg] = session.itemState(e.arg)
                    if (batch.states[e.arg] == CoreImport.State.Done && before != CoreImport.State.Done) refreshNowAndThen()
                    if (batch.states[e.arg].isFinished) noteOutcome(batch, session, e.arg)
                    if (batch.scanned && batch.states.all { it.isFinished }) settle(batch)
                }
            }
            CoreImport.Event.Idle -> batch.idle.complete(Unit)
            CoreImport.Event.Offline -> batch.offline = e.arg != 0
        }
        publish()
        maybeFinish()
    }

    // Sends the next of the batch's picks once the one before has been written.
    private fun nextAssign(batch: Batch) {
        val session = batch.session ?: return
        while (true) {
            val next = batch.assigns.firstOrNull() ?: return
            try {
                val state = session.itemState(next.item)
                if (state == CoreImport.State.Failed) {
                    batch.assigns.clear()
                    return
                }
                if (state.isBusy) return
                val tracks = session.matches(next.item).getOrNull(next.match)?.tracks.orEmpty()
                if (tracks.getOrNull(next.pick.track)?.let { it.file >= 0 } != false) {
                    batch.assigns.removeFirst() // it's on the track, or the track has gone
                    continue
                }
                if (next.sent) return
                val file = session.files(next.pick.fromItem).indexOfFirst { it.path == next.pick.path }
                if (file < 0) {
                    batch.assigns.removeFirst()
                    continue
                }
                session.assign(next.item, next.match, next.pick.track, next.pick.fromItem, file)
                next.sent = true
                return
            } catch (e: KithCoreException) {
                Log.w(TAG, "Couldn't use a file as a track", e)
                batch.assigns.removeFirst()
            }
        }
    }

    // What's said of an item the library already had some of, kept for when its session is closed.
    private fun noteOutcome(batch: Batch, session: CoreImport, item: Int) {
        val text = when (session.itemInfo(item).outcome) {
            CoreImport.Outcome.Moved -> {
                moved += ItemKey(batch.id, item)
                return
            }
            CoreImport.Outcome.Replaced -> R.string.import_state_replaced
            CoreImport.Outcome.Added -> R.string.import_state_added_missing
            CoreImport.Outcome.Duplicate -> R.string.import_state_duplicate
            else -> return
        }
        outcomes[ItemKey(batch.id, item)] = context.getString(R.string.import_problem, session.itemName(item), context.getString(text))
    }

    // Every item of the batch is settled: close its session and delete its copies.
    private fun settle(batch: Batch) {
        if (batch.closed || batch.assigns.isNotEmpty()) return
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
            } catch (e: KithCoreException) {
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
                    } catch (e: KithCoreException) {
                        Log.w(TAG, "Couldn't resolve item ${batch.id}/$it", e)
                    }
                }
            }
        }
    }

    private fun maybeFinish() {
        if (over || !runnerDone) return
        if (batches.all { it.closed || (it.scanned && it.assigns.isEmpty() && it.states.all { s -> s.isFinished }) }) {
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
            items = all.size - moved.size,
            imported = counts[CoreImport.State.Done] ?: 0,
            review = review,
            skipped = (counts[CoreImport.State.Skipped] ?: 0) - moved.size,
            failed = counts[CoreImport.State.Failed] ?: 0,
            offline = !skipTagging && batches.any { !it.closed && it.offline },
            unreachable = if (skipTagging) 0 else unreachableCount(),
            taggingSkipped = skipTagging,
            reviewItems = reviewEntries(),
            problems = copyProblems + failures(),
            outcomes = outcomes.values.toList(),
        )
    }

    private fun unreachableCount(): Int = batches.sumOf { batch ->
        val session = batch.session ?: return@sumOf 0
        batch.states.indices.count { batch.states[it] == CoreImport.State.Review && session.itemUnreachable(it) }
    }

    private fun reviewEntries(): List<ReviewEntry> = batches.flatMap { batch ->
        val session = batch.session ?: return@flatMap emptyList()
        batch.states.indices.filter { batch.states[it] == CoreImport.State.Review }.map { item ->
            val name = batch.names.getOrPut(item) { session.itemName(item) }
            ReviewEntry(ItemKey(batch.id, item), name, session.itemError(item))
        }
    }

    private val outcomes = LinkedHashMap<ItemKey, String>()
    // Items whose files all went to another item: nothing to count them as.
    private val moved = HashSet<ItemKey>()

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
        outcomes.clear()
        moved.clear()
        Log.i(TAG, "Import ${phase.name.lowercase()}: ${p?.imported} imported, ${p?.skipped} skipped, ${p?.failed} failed")
        // Even if cancelled: what was imported stays.
        if ((p?.imported ?: 0) > 0) AnalysisWorker.schedule(context)
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
