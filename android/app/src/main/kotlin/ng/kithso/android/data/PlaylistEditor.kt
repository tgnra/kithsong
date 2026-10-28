package ng.kithso.android.data

import androidx.compose.runtime.Immutable
import kotlinx.coroutines.flow.StateFlow

/** What Add to playlist adds: some tracks, or an album's tracks in order. */
@Immutable
sealed interface Addition {
    data class Tracks(val uids: List<String>) : Addition
    data class Album(val uid: String) : Addition
}

/** A manual playlist, as Add to playlist lists it. */
@Immutable
data class PlaylistChoice(val uid: String, val name: String)

/**
 * Editing manual playlists, for a [LibrarySource] that can ([CoreLibrarySource]; the made-up
 * library can't, and the UI then offers no editing). The changes are made in the background;
 * [LibrarySource.library] follows, and [edits] counts them, so an open track list can be read
 * again even when the library looks no different (after a move, say). A change that fails is
 * logged and comes to nothing. Smart playlists can't be edited, nor can they be added to.
 *
 * A playlist with nothing to play on this phone isn't listed (see [CoreLibrarySource]), so a
 * new one is only made with something added to it.
 */
interface PlaylistEditor {
    val edits: StateFlow<Int>

    /** Every manual playlist by name, those with nothing to play here too; null on failure. */
    suspend fun playlistChoices(): List<PlaylistChoice>?

    /** Whether [playlistUid] is a manual playlist, which can be edited. */
    suspend fun isEditable(playlistUid: String): Boolean

    /** Adds [what] to the end of playlist [playlistUid]. */
    fun addToPlaylist(playlistUid: String, what: Addition)

    /** Makes a playlist called [name] and adds [what] to it. */
    fun addToNewPlaylist(name: String, what: Addition)

    fun renamePlaylist(playlistUid: String, name: String)

    /** Deletes the playlist; its tracks stay in the library. */
    fun deletePlaylist(playlistUid: String)

    /** Takes entry [entryUid] (see [Track.entryUid]) out of its playlist. */
    fun removeEntry(entryUid: String)

    /** Moves entry [entryUid] to after [afterEntryUid] in its playlist, or to the start if null. */
    fun moveEntry(entryUid: String, afterEntryUid: String?)
}
