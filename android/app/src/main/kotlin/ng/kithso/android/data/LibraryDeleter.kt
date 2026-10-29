package ng.kithso.android.data

import androidx.compose.runtime.Immutable

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
 * alone in their folders) from this phone. No tracks: it's gone already.
 */
@Immutable
data class DeleteSize(val tracks: Int, val files: Int)

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
 * their copies are removed, originals imported there go to that device's trash.
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
