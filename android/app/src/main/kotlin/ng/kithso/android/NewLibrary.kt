package ng.kithso.android

import android.net.Uri
import android.util.Log
import java.io.File
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.android.data.ExportProgress
import ng.kithso.android.data.ExportReadException
import ng.kithso.android.data.MAX_ZIP_BYTES
import ng.kithso.android.data.exportReleases
import ng.kithso.android.data.splitReleases
import ng.kithso.android.data.writeZips
import ng.kithso.core.CoreLibrary

/**
 * A new, empty library in place of this phone's, the phone's answer to the desktops' Create a
 * new library: pairing is refused when both devices have music, and a device can only join with
 * an empty library. A desktop opens one in another folder and keeps the old; a phone has one
 * library folder, in the app's storage, so the new library replaces it. The library's music can
 * be exported first, as zip files Import takes back ([export], see data/LibraryExport.kt); only
 * once that's finished is the library replaced. Playlists, plays and the like don't survive.
 *
 * As [Testing] does for a stress library, [start] saves the request and starts the process
 * again, as nothing here can swap the library under the player, sync, imports and the art
 * caches. The next launch sets the old library aside before anything opens it
 * ([KithsongApp]), and the folder set aside is deleted in the background ([deleteReplaced]).
 * Sync then starts on the new library with the same sync.db, as on the desktops. [pairAgain]
 * then offers to pair with the device the pairing failed with.
 */
class NewLibrary(private val app: KithsongApp, private val settings: AppSettings, private val scope: CoroutineScope) {
    /**
     * The device a pairing failed with, to ask again: by [uid] if discovery sees it, else at
     * [host] and [port] (none: ""). [name] is what the dialogs call it.
     */
    data class Target(val uid: String, val name: String, val host: String, val port: Int) {
        /** As [decode] reads it, for [AppSettings.pairAgain]. */
        fun encode(): String = listOf(uid, name.replace('\n', ' '), host, port.toString()).joinToString("\n")

        companion object {
            fun decode(text: String): Target? {
                val parts = text.split('\n')
                if (parts.size != 4) return null
                return Target(parts[0], parts[1], parts[2], parts[3].toIntOrNull() ?: 0)
            }
        }
    }

    sealed interface State {
        data object Idle : State

        /** Writing the zips for [target]'s pairing; [progress] is null while the files are listed. */
        data class Exporting(val target: Target?, val progress: ExportProgress? = null) : State

        /**
         * Finished: [zips] written (0 if none of the music is on this phone), [tooBig] of them
         * albums over the importer's limit. The new library waits for the user to [start] it.
         */
        data class Exported(val target: Target?, val zips: Int, val tooBig: Int) : State

        /** The export stopped, and its zips were deleted: [readPath] is a file it couldn't read, if that's why. */
        data class Failed(val readPath: String?) : State
    }

    private val mutableState = MutableStateFlow<State>(State.Idle)
    val state: StateFlow<State> = mutableState
    private var job: Job? = null

    // Not while the old library is still there: setting it aside failed, and is tried again.
    private val mutablePairAgain = MutableStateFlow(settings.pairAgain?.takeIf { !settings.newLibrary }?.let(Target::decode))

    /** The device to offer to pair with again, after a new library was started for it. */
    val pairAgain: StateFlow<Target?> = mutablePairAgain

    init {
        scope.launch(Dispatchers.IO) { deleteReplaced() }
    }

    /**
     * Exports the library's music into the folder [tree] (from OpenDocumentTree), with
     * [ExportService] keeping the process going, then waits in [State.Exported].
     */
    fun export(tree: Uri, target: Target?) {
        if (mutableState.value is State.Exporting) return
        mutableState.value = State.Exporting(target)
        ExportService.start(app)
        val dir = app.realLibraryDir
        job = scope.launch {
            val next = try {
                withContext(Dispatchers.IO) {
                    // A connection of its own: the library's is busy with the app.
                    val releases = CoreLibrary(dir.path).use { exportReleases(it, dir) }
                    val zips = splitReleases(releases, MAX_ZIP_BYTES) { it.bytes }
                    Log.i(TAG, "Exporting ${releases.size} releases in ${zips.size} zips")
                    val written = writeZips(app.contentResolver, tree, app.getString(R.string.export_zip_name), zips) {
                        mutableState.value = State.Exporting(target, it)
                    }
                    State.Exported(target, written, releases.count { it.bytes > MAX_ZIP_BYTES })
                }
            } catch (e: CancellationException) {
                mutableState.value = State.Idle
                throw e
            } catch (e: ExportReadException) {
                Log.e(TAG, "Couldn't export the library", e)
                State.Failed(e.path)
            } catch (e: Exception) {
                // IOException and KithCoreException, or a provider's SecurityException and the like.
                Log.e(TAG, "Couldn't export the library", e)
                State.Failed(null)
            }
            mutableState.value = next
        }
    }

    /** Stops the export; what it wrote is deleted, and the library stays. */
    fun cancel() {
        job?.cancel()
    }

    /** Forgets how the export ended, keeping the library. */
    fun dismiss() {
        if (mutableState.value !is State.Exporting) mutableState.value = State.Idle
    }

    /**
     * Replaces the library with a new, empty one: restarts the app, whose next launch sets this
     * one aside, then offers to pair with [target] again.
     */
    fun start(target: Target?) {
        if (mutableState.value is State.Exporting) return
        settings.pairAgain = target?.encode()
        settings.newLibrary = true
        Log.i(TAG, "Restarting for a new library")
        app.restart()
    }

    /** Pair again was answered, either way. */
    fun pairAgainAnswered() {
        settings.pairAgain = null
        mutablePairAgain.value = null
    }

    // Libraries set aside by earlier launches, which may be big: deleted off the main thread,
    // and again next launch if this one is cut short.
    private fun deleteReplaced() {
        val replaced = app.noBackupFilesDir.listFiles { file -> file.name.startsWith(REPLACED_PREFIX) }.orEmpty()
        for (dir in replaced) {
            if (!dir.deleteRecursively()) Log.w(TAG, "Couldn't delete all of ${dir.name}")
        }
    }

    companion object {
        private const val TAG = "kithsong"

        /** The start of the name of a library set aside for a new one, beside [KithsongApp.realLibraryDir]. */
        const val REPLACED_PREFIX = "library-replaced-"

        /** Where [KithsongApp] sets the library aside: a new name, so nothing is deleted on the main thread. */
        fun replacedDir(app: KithsongApp): File = File(app.noBackupFilesDir, REPLACED_PREFIX + System.currentTimeMillis())
    }
}
