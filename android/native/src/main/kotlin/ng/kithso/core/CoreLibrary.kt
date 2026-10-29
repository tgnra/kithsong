package ng.kithso.core

/** A musiccore failure; [status] is the negative mc_status code. */
class MusicCoreException(val status: Int) : RuntimeException("musiccore status $status")

/**
 * mc_album_info. [folder] is null when none of the album's files are on this device;
 * [artistUid] is [artist]'s uid, null when it has none.
 */
data class AlbumInfo(
    val uid: String,
    val title: String,
    val artist: String?,
    val year: Int,
    val isSingles: Boolean,
    val hasArt: Boolean,
    val folder: String?,
    val artistUid: String? = null,
)

/**
 * mc_track_info. [path] is null when the track has no file on this device. [artist] is the
 * track's, else its album's, and [artistUid] its uid; [coverDir] is the folder holding its
 * album's cover, if any.
 */
data class TrackInfo(
    val uid: String,
    val title: String,
    val trackNo: Int,
    val discNo: Int,
    val durationMs: Int,
    val path: String?,
    val artist: String? = null,
    val album: String? = null,
    val coverDir: String? = null,
    val artistUid: String? = null,
)

/** mc_playlist_info. [playableCount] is the entries whose track has a file on this device. */
data class PlaylistInfo(val uid: String, val name: String, val trackCount: Int, val playableCount: Int)

/**
 * mc_smart_info: a smart playlist, or a smart album group if [isAlbums]. [rule] is its JSON;
 * [isRandom] if it's ordered by random, so it can be reshuffled.
 */
data class SmartInfo(val uid: String, val name: String, val isAlbums: Boolean, val rule: String, val isRandom: Boolean)

/**
 * mc_artist_info or mc_album_group_info. [albumCount] is an artist's albums as album artist (0
 * if only credited on tracks), or an album group's albums.
 */
data class NamedInfo(val uid: String, val name: String, val albumCount: Int)

/**
 * mc_artist_info from mc_library_list_artists_here(): [albumCount] is the artist's albums
 * with a file on this device, [trackCount] the tracks credited to them with one.
 */
data class ArtistInfo(val uid: String, val name: String, val albumCount: Int, val trackCount: Int)

/** An open library (mc_library), in folder [dir]. Not thread-safe; close it when done. */
class CoreLibrary(dir: String) : AutoCloseable {
    internal var handle = nativeOpen(dir)
        private set

    val trackCount: Long get() = nativeCount(handle, ENTITY_TRACKS)

    /** Every album, in display order. */
    fun albums(): List<AlbumInfo> = nativeListAlbums(handle).asList()

    /** [albumUid]'s tracks in order. */
    fun albumTracks(albumUid: String): List<TrackInfo> =
        nativeListTracks(handle, TRACKS_ALBUM, albumUid).asList()

    /**
     * Every playlist, by name; if [playableOnly], only those with a track that has a file on
     * this device (MC_LIST_PLAYABLE), as the views for playing show. Pickers list them all.
     */
    fun playlists(playableOnly: Boolean = false): List<PlaylistInfo> =
        nativeListPlaylists(handle, if (playableOnly) LIST_PLAYABLE else 0).asList()

    /** [playlistUid]'s tracks in playlist order; a track in it twice is listed twice. */
    fun playlistTracks(playlistUid: String): List<TrackInfo> =
        nativeListTracks(handle, TRACKS_PLAYLIST, playlistUid).asList()

    /**
     * The image file to show for [playlistUid] (mc_playlist_art): a 2x2 grid of its album covers
     * cached in [cacheDir], or one album's cover if it has fewer than four, or null for a
     * placeholder. The path changes whenever the picture does. A cache miss decodes four covers,
     * so call it off the main thread.
     */
    fun playlistArt(playlistUid: String, cacheDir: String): String? =
        nativePlaylistArt(handle, playlistUid, cacheDir)

    /** Deletes the pictures in [cacheDir] of playlists that have gone (mc_playlist_art_prune). */
    fun playlistArtPrune(cacheDir: String) = nativePlaylistArtPrune(handle, cacheDir)

    /** Every artist, album artists and those credited only on tracks alike, by sort name. */
    fun artists(): List<NamedInfo> = nativeListArtists(handle).asList()

    /** The artists with something to play on this device, in the same order, told apart by uid. */
    fun artistsHere(): List<ArtistInfo> = nativeListArtistsHere(handle).asList()

    /** Every manual album group, by name. */
    fun albumGroups(): List<NamedInfo> = nativeListAlbumGroups(handle).asList()

    /**
     * Every smart list of either kind, by name; if [playableOnly], only those holding something
     * with a file on this device (MC_LIST_PLAYABLE).
     */
    fun smartLists(playableOnly: Boolean = false): List<SmartInfo> =
        nativeListSmart(handle, if (playableOnly) LIST_PLAYABLE else 0).asList()

    /** What smart playlist [smartUid] holds now, evaluated afresh. */
    fun smartTracks(smartUid: String): List<TrackInfo> =
        nativeListTracks(handle, TRACKS_SMART, smartUid).asList()

    /** The albums a smart album group with [rule] (JSON, see docs/schema.md) would hold. */
    fun previewSmartAlbums(rule: String): List<AlbumInfo> = nativePreviewSmartAlbums(handle, rule).asList()

    /** Counts a play of [trackUid] on this device and makes now its last play. */
    fun recordPlay(trackUid: String) = nativeRecordPlay(handle, trackUid)

    override fun close() {
        if (handle != 0L) nativeClose(handle)
        handle = 0
    }

    private companion object {
        init {
            System.loadLibrary("kithsong_jni")
        }

        // mc_entity
        const val ENTITY_TRACKS = 2

        // MC_LIST_PLAYABLE
        const val LIST_PLAYABLE = 1

        // nativeListTracks' kinds
        const val TRACKS_ALBUM = 0
        const val TRACKS_PLAYLIST = 1
        const val TRACKS_SMART = 2

        @JvmStatic external fun nativeOpen(dir: String): Long
        @JvmStatic external fun nativeClose(handle: Long)
        @JvmStatic external fun nativeCount(handle: Long, entity: Int): Long
        @JvmStatic external fun nativeListAlbums(handle: Long): Array<AlbumInfo>
        @JvmStatic external fun nativeListTracks(handle: Long, kind: Int, uid: String): Array<TrackInfo>
        @JvmStatic external fun nativeListPlaylists(handle: Long, flags: Int): Array<PlaylistInfo>
        @JvmStatic external fun nativePlaylistArt(handle: Long, uid: String, cacheDir: String): String?
        @JvmStatic external fun nativePlaylistArtPrune(handle: Long, cacheDir: String)
        @JvmStatic external fun nativeListArtists(handle: Long): Array<NamedInfo>
        @JvmStatic external fun nativeListArtistsHere(handle: Long): Array<ArtistInfo>
        @JvmStatic external fun nativeListAlbumGroups(handle: Long): Array<NamedInfo>
        @JvmStatic external fun nativeListSmart(handle: Long, flags: Int): Array<SmartInfo>
        @JvmStatic external fun nativePreviewSmartAlbums(handle: Long, rule: String): Array<AlbumInfo>
        @JvmStatic external fun nativeRecordPlay(handle: Long, trackUid: String)
    }
}
