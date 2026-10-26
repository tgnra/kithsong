package app.musicapp

/** A musiccore failure; [status] is the negative mc_status code. */
class MusicCoreException(val status: Int) : RuntimeException("musiccore status $status")

/** mc_album_info. [folder] is null when none of the album's files are on this device. */
data class AlbumInfo(
    val uid: String,
    val title: String,
    val artist: String?,
    val year: Int,
    val isSingles: Boolean,
    val hasArt: Boolean,
    val folder: String?,
)

/** mc_track_info. [path] is null when the track has no file on this device. */
data class TrackInfo(
    val uid: String,
    val title: String,
    val trackNo: Int,
    val discNo: Int,
    val durationMs: Int,
    val path: String?,
)

/** An open library (mc_library), in folder [dir]. Not thread-safe; close it when done. */
class CoreLibrary(dir: String) : AutoCloseable {
    private var handle = nativeOpen(dir)

    val trackCount: Long get() = nativeCount(handle, ENTITY_TRACKS)

    /** Every album, in display order. */
    fun albums(): List<AlbumInfo> = nativeListAlbums(handle).asList()

    /** [albumUid]'s tracks in order. */
    fun albumTracks(albumUid: String): List<TrackInfo> =
        nativeListAlbumTracks(handle, albumUid).asList()

    override fun close() {
        if (handle != 0L) nativeClose(handle)
        handle = 0
    }

    private companion object {
        init {
            System.loadLibrary("musicapp_jni")
        }

        // mc_entity
        const val ENTITY_TRACKS = 2

        @JvmStatic external fun nativeOpen(dir: String): Long
        @JvmStatic external fun nativeClose(handle: Long)
        @JvmStatic external fun nativeCount(handle: Long, entity: Int): Long
        @JvmStatic external fun nativeListAlbums(handle: Long): Array<AlbumInfo>
        @JvmStatic external fun nativeListAlbumTracks(handle: Long, albumUid: String): Array<TrackInfo>
    }
}
