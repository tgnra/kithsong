package ng.kithso.android.data

import androidx.compose.runtime.Immutable
import ng.kithso.core.BinEntry

/**
 * An album or track to delete from the library, with the [name] to ask about. An artist can't
 * be deleted as such, only their albums and tracks.
 */
@Immutable
data class Deletion(val kind: Kind, val uid: String, val name: String) {
    enum class Kind { Track, Album }
}

/**
 * What a [Deletion] would take: [tracks] from the library, and [files] (theirs, and covers left
 * alone in their folders) from this phone. No tracks: it's gone already. [toBin]: in a sync
 * group, where the delete waits in the recycle bin until the next sync and can be restored
 * until then ([RecycleBin]).
 */
@Immutable
data class DeleteSize(val tracks: Int, val files: Int, val toBin: Boolean = false)

/** How a delete went. */
sealed interface DeleteOutcome {
    data object Deleted : DeleteOutcome

    /** [tracks] stayed in the library, as one of their files couldn't be removed. */
    data class Kept(val tracks: Int) : DeleteOutcome

    /** The library couldn't be written; some files may have gone all the same. */
    data object Failed : DeleteOutcome
}

/**
 * Deleting albums and tracks from the library, as the desktops do, for a
 * [LibrarySource] that can ([CoreLibrarySource]; the made-up library can't, and the UI then
 * offers no delete). The desktops move the files to the Recycle Bin or Trash, from which they
 * can be restored; Android has none, so here the files are removed for good, which the UI says
 * before it asks. Either way the delete is logged, so sync deletes the tracks on every device:
 * their copies are removed, originals imported there go to that device's trash. In a sync
 * group the delete waits in the core's recycle bin until the next sync ([RecycleBin]), and
 * restoring it from there keeps it in the library, removed from this phone only.
 *
 * A file that's playing could still be removed here, but the track would play on to its end
 * and the queue's other deleted tracks be skipped, so as on Windows the UI stops playback
 * first, emptying the queue, if the delete takes any of its tracks ([takesAny]).
 * [LibrarySource.library] follows the delete, and an open track list is read again
 * ([PlaylistEditor.edits]).
 */
interface LibraryDeleter {
    /** What deleting [what] would take, to ask first; null if that couldn't be found out. */
    suspend fun deleteSize(what: Deletion): DeleteSize?

    /** Whether deleting [what] would take any of [trackUids]; false if that couldn't be found out. */
    suspend fun takesAny(what: Deletion, trackUids: List<String>): Boolean

    /** Deletes [what]. It goes on even if the caller is cancelled. */
    suspend fun delete(what: Deletion): DeleteOutcome
}

/**
 * The recycle bin (kithcore/delete.h): in a sync group, what was deleted here since the last
 * sync. Its files are gone from this phone already; until a sync sends the deletes, one can be
 * restored, which keeps it in the library and on the other devices, removed from this phone
 * (its sync profile excludes it). While it holds anything this phone doesn't sync by itself:
 * the user presses Sync, which makes the deletes permanent. [ng.kithso.android.sync.Syncer]
 * has it.
 */
interface RecycleBin {
    /** What's in it, newest first; empty if that couldn't be read. */
    suspend fun binEntries(): List<BinEntry>

    /**
     * Restores [entry]; how many entries came back (more than one where a later delete took
     * something it needs, such as its album), or null if it couldn't be (a sync sent it).
     */
    suspend fun binRestore(entry: BinEntry): Int?
}
