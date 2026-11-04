package ng.kithso.wear

import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import ng.kithso.core.AlbumInfo
import ng.kithso.core.CoreLibrary
import ng.kithso.core.CoreSync
import ng.kithso.core.MusicCoreException
import ng.kithso.core.NamedInfo
import ng.kithso.core.PlaylistInfo
import ng.kithso.core.TrackInfo
import ng.kithso.core.addToAlbumGroup
import ng.kithso.core.albumGroupAlbums
import ng.kithso.core.appendToPlaylist
import java.io.File

/**
 * The library as musiccore has it, in folder [dir], seen as the watch can play it. Sync brings
 * the whole group's library data but only the files the watch's profile wants, so only what
 * has a file here is listed: albums with a folder here, playlists with a track that has a file
 * here, manual album groups with such an album, and of an album's or playlist's tracks only
 * those with a file. Smart lists and artists aren't built on the watch.
 *
 * A library handle isn't thread-safe, so every core call runs on one background thread
 * ([core]); [contents] is read whole when this is created and again on [refresh], after every
 * sync that brought something. The lists are small on a watch, so that's one query for the
 * albums, one per album for its tracks (the count), one for the playlists and one per album
 * group.
 *
 * Playlists and album groups can be added to here ([appendToPlaylist], [appendAlbumToPlaylist],
 * [addToAlbumGroup]); each change is followed by a [refresh] and told to sync through
 * [onChanged].
 */
class WatchLibrary(private val dir: File, private val scope: CoroutineScope) {
    /** What the watch holds, as the screens list it. */
    data class Contents(
        /** Albums with a file here, in display order. */
        val albums: List<AlbumInfo> = emptyList(),
        /** Manual playlists with a track that has a file here, by name. */
        val playlists: List<PlaylistInfo> = emptyList(),
        /** Manual album groups with an album that has a file here, by name, counting those albums. */
        val albumGroups: List<NamedInfo> = emptyList(),
        /** The tracks with a file here. */
        val trackCount: Int = 0,
    )

    private val core = Dispatchers.IO.limitedParallelism(1)
    private val state = MutableStateFlow(Contents())
    val contents: StateFlow<Contents> = state

    /** Called after a change made here, for sync to pass on soon. */
    var onChanged: () -> Unit = {}

    // Only touched on [core].
    private var lib: CoreLibrary? = null

    init {
        refresh()
    }

    /** Reads the library again, e.g. after a sync. */
    fun refresh(): Job = scope.launch(core) {
        val lib = open() ?: return@launch
        try {
            val albums = lib.albums().filter { it.folder != null }
            val tracks = albums.sumOf { album -> lib.albumTracks(album.uid).count { it.path != null } }
            // MC_LIST_PLAYABLE keeps empty playlists too, as a place for tracks to go; the
            // watch shows only what's present, so only ones with something to play.
            val playlists = lib.playlists(playableOnly = true).filter { it.playableCount > 0 }
            // Each group's count is of its albums here, as a playlist's playableCount is of
            // its tracks: the watch shows only what's present.
            val groups = lib.albumGroups().mapNotNull { group ->
                val here = lib.albumGroupAlbums(group.uid).count { it.folder != null }
                if (here > 0) group.copy(albumCount = here) else null
            }
            state.value = Contents(albums, playlists, groups, tracks)
            Log.i(TAG, "Read ${albums.size} albums, $tracks tracks, ${playlists.size} playlists, ${groups.size} album groups here")
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't read the library", e)
        }
    }

    /** [albumUid]'s tracks with a file here, in order. */
    suspend fun albumTracks(albumUid: String): List<TrackInfo> =
        read(emptyList()) { it.albumTracks(albumUid).here() }

    /** [playlistUid]'s tracks with a file here, in playlist order. */
    suspend fun playlistTracks(playlistUid: String): List<TrackInfo> =
        read(emptyList()) { it.playlistTracks(playlistUid).here() }

    /** [groupUid]'s albums' tracks with a file here, one album after another in the group's order. */
    suspend fun albumGroupTracks(groupUid: String): List<TrackInfo> =
        read(emptyList()) { lib -> lib.albumGroupAlbums(groupUid).flatMap { lib.albumTracks(it.uid).here() } }

    /** [groupUid]'s albums with a file here, in the group's order. */
    suspend fun albumGroupAlbums(groupUid: String): List<AlbumInfo> =
        read(emptyList()) { it.albumGroupAlbums(groupUid).filter { album -> album.folder != null } }

    /** Adds [trackUids] at the end of [playlistUid], for every device; false if the core refused. */
    suspend fun appendToPlaylist(playlistUid: String, trackUids: List<String>): Boolean =
        change("add to playlist $playlistUid") { it.appendToPlaylist(playlistUid, trackUids) }

    /**
     * Adds all of [albumUid]'s tracks at the end of [playlistUid], in album order, for every
     * device: the album is one thing to the person adding it, so the tracks not on the watch
     * go too, and other devices see the whole album in the playlist. False if the core refused.
     */
    suspend fun appendAlbumToPlaylist(playlistUid: String, albumUid: String): Boolean =
        change("add album $albumUid to playlist $playlistUid") {
            it.appendToPlaylist(playlistUid, it.albumTracks(albumUid).map { track -> track.uid })
        }

    /** Adds [albumUid] at the end of album group [groupUid], for every device; false if the core refused. */
    suspend fun addToAlbumGroup(groupUid: String, albumUid: String): Boolean =
        change("add to album group $groupUid") { it.addToAlbumGroup(groupUid, albumUid) }

    /**
     * Runs [block] on the core's thread with the open library, for sync, which is opened on
     * it. Throws MusicCoreException (ERR_NOT_FOUND) if the library can't be opened.
     */
    suspend fun <T> onCore(block: (CoreLibrary) -> T): T = withContext(core) {
        block(open() ?: throw MusicCoreException(CoreSync.ERR_NOT_FOUND))
    }

    private suspend fun <T> read(fallback: T, block: (CoreLibrary) -> T): T = withContext(core) {
        val lib = open() ?: return@withContext fallback
        try {
            block(lib)
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Library read failed", e)
            fallback
        }
    }

    private suspend fun change(what: String, block: (CoreLibrary) -> Unit): Boolean {
        val done = withContext(core) {
            val lib = open() ?: return@withContext false
            try {
                block(lib)
                true
            } catch (e: MusicCoreException) {
                Log.w(TAG, "Couldn't $what", e)
                false
            }
        }
        if (done) {
            refresh().join()
            onChanged()
        }
        return done
    }

    // On [core]. Opens the library once. Sync makes the folder when it first writes to it;
    // before that, opening makes an empty library there.
    private fun open(): CoreLibrary? {
        lib?.let { return it }
        return try {
            dir.mkdirs()
            CoreLibrary(dir.path).also { lib = it }
        } catch (e: MusicCoreException) {
            Log.e(TAG, "Couldn't open the library in $dir", e)
            null
        }
    }

    private companion object {
        const val TAG = "kithsong"
    }
}

private fun List<TrackInfo>.here() = filter { it.path != null }
