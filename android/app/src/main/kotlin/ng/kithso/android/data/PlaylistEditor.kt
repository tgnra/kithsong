package ng.kithso.android.data

import androidx.compose.runtime.Immutable
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow

/** What Add to playlist adds: some tracks, or an album's tracks in order. */
@Immutable
sealed interface Addition {
    data class Tracks(val uids: List<String>) : Addition
    data class Album(val uid: String) : Addition
}

/** A manual playlist, as Add to playlist lists it, or a manual album group, as Add to album group does. */
@Immutable
data class PlaylistChoice(val uid: String, val name: String)

/** A change to a playlist or album group that failed, as [PlaylistEditor.failures] reports it. */
enum class EditFailure {
    PlaylistAdd, PlaylistCreate, PlaylistRename, PlaylistDelete, PlaylistRemove, PlaylistMove,
    GroupAdd, GroupCreate, GroupRename, GroupDelete, GroupRemove, GroupMove,
}

/**
 * Editing manual playlists and album groups, for a [LibrarySource] that can ([CoreLibrarySource]; the made-up
 * library can't, and the UI then offers no editing). The changes are made in the background;
 * [LibrarySource.library] follows, and [edits] counts them, so an open track list can be read
 * again even when the library looks no different (after a move, say). A change that fails is
 * logged and comes to nothing, and [failures] says so, for the UI to show. Smart playlists can't be edited, nor can they be added to.
 *
 * A playlist with nothing to play on this phone isn't listed (see [CoreLibrarySource]), so a
 * new one is only made with something added to it.
 */
interface PlaylistEditor {
    val edits: StateFlow<Int>

    /** Each change that failed, as it fails; none are kept for later collectors. */
    val failures: SharedFlow<EditFailure>

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

    // Album groups, the playlists of albums, are edited likewise. An album is in a group at
    // most once. As with playlists, a group with no album on this phone isn't listed, so a new
    // one is only made with an album in it; smart groups can't be edited or added to.

    /** Every manual album group by name, those with nothing here too; null on failure. */
    suspend fun albumGroupChoices(): List<PlaylistChoice>?

    /** Whether [groupUid] is a manual album group, which can be edited. */
    suspend fun isEditableGroup(groupUid: String): Boolean

    /** Adds album [albumUid] to the end of album group [groupUid], unless it's in it already. */
    fun addToAlbumGroup(groupUid: String, albumUid: String)

    /** Makes an album group called [name] and adds album [albumUid] to it. */
    fun addToNewAlbumGroup(name: String, albumUid: String)

    fun renameAlbumGroup(groupUid: String, name: String)

    /** Deletes the album group; its albums stay in the library. */
    fun deleteAlbumGroup(groupUid: String)

    /** Takes album [albumUid] out of album group [groupUid]. */
    fun removeFromAlbumGroup(groupUid: String, albumUid: String)

    /** Moves album [albumUid] to after [afterAlbumUid] in album group [groupUid], or to the start if null. */
    fun moveInAlbumGroup(groupUid: String, albumUid: String, afterAlbumUid: String?)
}
