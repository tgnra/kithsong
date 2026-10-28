package ng.kithso.core

// Editing playlists (musiccore/playlist.h) on an open CoreLibrary, bound in jni_playlist_edit.c.
// Entries are a playlist's items: a track can be in one more than once, so moving and
// removing name an entry ([playlistEntries]) rather than a track. Each throws
// MusicCoreException, ERR_NOT_FOUND (-3) when the playlist, track or entry doesn't exist.

/** A new, empty playlist called [name] (not blank); returns its uid. */
fun CoreLibrary.createPlaylist(name: String): String = PlaylistNatives.nativeCreate(handle, name)

fun CoreLibrary.renamePlaylist(playlistUid: String, name: String) = PlaylistNatives.nativeRename(handle, playlistUid, name)

/** Deletes the playlist and its entries; the tracks are untouched. */
fun CoreLibrary.deletePlaylist(playlistUid: String) = PlaylistNatives.nativeDelete(handle, playlistUid)

/** Adds [trackUids] at the end of the playlist, in order, stopping at the first that fails. */
fun CoreLibrary.appendToPlaylist(playlistUid: String, trackUids: List<String>) =
    PlaylistNatives.nativeAppend(handle, playlistUid, trackUids.toTypedArray())

/**
 * The entry uids of [playlistUid] in playlist order, one for each of [CoreLibrary.playlistTracks]
 * (the same order, so the two can be zipped).
 */
fun CoreLibrary.playlistEntries(playlistUid: String): List<String> =
    PlaylistNatives.nativeEntries(handle, playlistUid).asList()

/** Moves [entryUid] to after [afterEntryUid] in its playlist, or to the start if that's null. */
fun CoreLibrary.movePlaylistEntry(entryUid: String, afterEntryUid: String?) =
    PlaylistNatives.nativeMove(handle, entryUid, afterEntryUid)

fun CoreLibrary.removePlaylistEntry(entryUid: String) = PlaylistNatives.nativeRemove(handle, entryUid)

internal object PlaylistNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeCreate(handle: Long, name: String): String
    @JvmStatic external fun nativeRename(handle: Long, uid: String, name: String)
    @JvmStatic external fun nativeDelete(handle: Long, uid: String)
    @JvmStatic external fun nativeAppend(handle: Long, uid: String, trackUids: Array<String>)
    @JvmStatic external fun nativeEntries(handle: Long, uid: String): Array<String>
    @JvmStatic external fun nativeMove(handle: Long, entryUid: String, afterEntryUid: String?)
    @JvmStatic external fun nativeRemove(handle: Long, entryUid: String)
}
