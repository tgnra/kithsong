package ng.kithso.android.archive

import android.content.Context
import android.net.Uri
import android.provider.DocumentsContract
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.core.ArchiveOutcome
import ng.kithso.core.CoreLibrary
import ng.kithso.core.LibraryArchive
import java.io.File

/** An export or restore under way ([result] null), or just over. */
data class ArchiveJob(val restoring: Boolean, val percent: Int = 0, val result: Result? = null, val tooBig: Int = 0) {
    enum class Result { Done, Cancelled, NoSpace, TooBig, Newer, Bad, Failed }

    val isOver: Boolean get() = result != null
}

/**
 * Exports the library as one zip to a document the user picked, and restores such a zip
 * into an empty library (kithcore/library_archive.h), one at a time, with [ArchiveService]
 * keeping the process running meanwhile. The export reads the library through a connection of
 * its own, so the app goes on as usual. The zip to restore is read where it is, through its
 * descriptor, without a copy; once restored, [onRestored] starts the app again, so that nothing
 * holding the empty library open is left.
 */
class LibraryArchiver(
    private val context: Context,
    private val libraryDir: () -> File,
    /** Whether a restore may go ahead: the library has no tracks and isn't in a sync group. */
    private val canRestore: suspend () -> Boolean,
    /** Given how the restore ended, so a restart can still say what it left out. */
    private val onRestored: (ArchiveJob) -> Unit,
    private val scope: CoroutineScope,
) {
    private val state = MutableStateFlow<ArchiveJob?>(null)

    /** The current or last export or restore; null for none, or once [dismiss]ed. */
    val job: StateFlow<ArchiveJob?> = state

    val isRunning: Boolean get() = state.value?.isOver == false

    @Volatile private var stop = false

    // library.db's copy, made while exporting or restoring.
    private val tempDir get() = File(context.cacheDir, "archive")

    /** Exports the library to [uri], a document just made for it; false if one is running. */
    fun export(uri: Uri): Boolean {
        if (isRunning) return false
        begin(restoring = false)
        scope.launch(Dispatchers.IO) {
            val outcome = try {
                context.contentResolver.openFileDescriptor(uri, "w")?.use { pfd ->
                    CoreLibrary(libraryDir().path).use { lib ->
                        LibraryArchive.export(lib, tempDir.path, pfd.fd, ::progress)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Couldn't export the library", e)
                null
            }
            // Half a zip is no use to anyone.
            if (outcome?.ok != true) {
                try {
                    DocumentsContract.deleteDocument(context.contentResolver, uri)
                } catch (e: Exception) {
                    Log.w(TAG, "Couldn't delete the unfinished export", e)
                }
            }
            end(outcome)
        }
        return true
    }

    /**
     * Restores the library from [uri] if it's a library export and a restore can go ahead,
     * returning whether it does; otherwise it's for importing as usual. Reading it needs a
     * file the core can seek in, which a document from a local volume is.
     */
    suspend fun restoreIfExport(uri: Uri): Boolean = withContext(Dispatchers.IO) {
        if (isRunning || !canRestore()) return@withContext false
        val pfd = try {
            context.contentResolver.openFileDescriptor(uri, "r")
        } catch (e: Exception) {
            Log.w(TAG, "Couldn't open $uri", e)
            null
        } ?: return@withContext false
        val path = "/proc/self/fd/${pfd.fd}"
        if (LibraryArchive.exportBytes(path) == null) {
            pfd.close()
            return@withContext false
        }
        begin(restoring = true)
        scope.launch(Dispatchers.IO) {
            val outcome = try {
                pfd.use {
                    CoreLibrary(libraryDir().path).use { lib ->
                        LibraryArchive.restore(lib, path, tempDir.path, ::progress)
                    }
                }
            } catch (e: Exception) {
                Log.e(TAG, "Couldn't restore the library", e)
                null
            }
            end(outcome)
            val job = state.value
            if (outcome?.ok == true && job != null) withContext(Dispatchers.Main) { onRestored(job) }
        }
        true
    }

    fun cancel() {
        stop = true
    }

    /** Forgets a finished export or restore, so its outcome goes away. */
    fun dismiss() {
        if (state.value?.isOver == true) state.value = null
    }

    private fun begin(restoring: Boolean) {
        stop = false
        state.value = ArchiveJob(restoring)
        ArchiveService.start(context)
    }

    // On the core's thread, every megabyte or so: only a new percentage goes out.
    private fun progress(done: Long, total: Long): Boolean {
        val percent = if (total > 0) (done * 100 / total).toInt().coerceIn(0, 100) else 0
        val job = state.value
        if (job != null && job.percent != percent) state.value = job.copy(percent = percent)
        return stop
    }

    private fun end(outcome: ArchiveOutcome?) {
        tempDir.deleteRecursively()
        val result = when {
            outcome == null -> ArchiveJob.Result.Failed
            outcome.ok -> ArchiveJob.Result.Done
            outcome.cancelled -> ArchiveJob.Result.Cancelled
            else -> when (outcome.problem) {
                ArchiveOutcome.NO_SPACE -> ArchiveJob.Result.NoSpace
                ArchiveOutcome.TOO_BIG -> ArchiveJob.Result.TooBig
                ArchiveOutcome.NEWER -> ArchiveJob.Result.Newer
                ArchiveOutcome.BAD -> ArchiveJob.Result.Bad
                else -> ArchiveJob.Result.Failed
            }
        }
        Log.i(TAG, "Library ${if (state.value?.restoring == true) "restore" else "export"}: $result (${outcome?.status}, ${outcome?.problem})")
        state.value = state.value?.copy(result = result, tooBig = outcome?.tooBig ?: 0)
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
