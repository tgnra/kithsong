package ng.kithso.core

// Editing an album's, artist's or track's tags and an album's flags on an open CoreLibrary, and
// matching an album to another MusicBrainz release (musiccore/edit.h), bound in jni_edit.c.
// The rows are what's edited and the files follow, so a set can rewrite and move files, and a
// search or a new release waits for MusicBrainz: keep them all off the main thread. Each throws
// MusicCoreException; the ERR_ statuses below are the ones worth telling apart. "" is no text
// and 0 an unknown number, on the way in and out.

/** mc_album_tags. [mbAlbumId] is the release; "" unlinks the album. */
data class AlbumTags(val title: String, val artist: String, val year: Int, val originalYear: Int, val mbAlbumId: String)

/** mc_artist_tags. [name] can't be empty. */
data class ArtistTags(val name: String, val sortName: String)

/** mc_track_tags. [artist] is the credit as shown, "" for the album's. */
data class TrackTags(
    val title: String,
    val artist: String,
    val trackNo: Int,
    val discNo: Int,
    val year: Int,
    val genre: String,
)

/** mc_album_flags: what an album's tags can't say. */
data class AlbumFlags(val noScrobble: Boolean, val mix: Boolean)

/** mc_release_info: a MusicBrainz release, and in [score] how well it fits the album, 0 to 1. */
data class ReleaseInfo(
    val mbAlbumId: String,
    val title: String,
    val artist: String,
    val year: Int,
    val country: String?,
    val label: String?,
    val format: String?,
    val trackCount: Int,
    val score: Double,
)

object CoreEdit {
    /** MC_ERR_NET: MusicBrainz can't be reached. */
    const val ERR_NET = -6

    /** MC_ERR_NOT_FOUND: the uid has gone. */
    const val ERR_NOT_FOUND = -8

    /** MC_ERR_IN_USE: a file is open, or kept changing. Nothing has changed. */
    const val ERR_IN_USE = -13

    /** A status in words (mc_status_str), in the core's language. */
    fun statusText(status: Int): String = EditNatives.nativeStatusText(status)
}

fun CoreLibrary.albumTags(uid: String): AlbumTags = EditNatives.nativeAlbumTags(handle, uid)

/** Saves [tags]: see mc_album_tags for what a change to each does to the tracks and files. */
fun CoreLibrary.setAlbumTags(uid: String, tags: AlbumTags) = EditNatives.nativeSetAlbumTags(
    handle, uid, tags.title.encodeToByteArray(), tags.artist.encodeToByteArray(), tags.year, tags.originalYear, tags.mbAlbumId,
)

fun CoreLibrary.artistTags(uid: String): ArtistTags = EditNatives.nativeArtistTags(handle, uid)

fun CoreLibrary.setArtistTags(uid: String, tags: ArtistTags) =
    EditNatives.nativeSetArtistTags(handle, uid, tags.name.encodeToByteArray(), tags.sortName.encodeToByteArray())

fun CoreLibrary.trackTags(uid: String): TrackTags = EditNatives.nativeTrackTags(handle, uid)

fun CoreLibrary.setTrackTags(uid: String, tags: TrackTags) = EditNatives.nativeSetTrackTags(
    handle, uid, tags.title.encodeToByteArray(), tags.artist.encodeToByteArray(), tags.trackNo, tags.discNo, tags.year,
    tags.genre.encodeToByteArray(),
)

fun CoreLibrary.albumFlags(uid: String): AlbumFlags =
    EditNatives.nativeAlbumFlags(handle, uid).let { AlbumFlags(it[0] != 0, it[1] != 0) }

fun CoreLibrary.setAlbumFlags(uid: String, flags: AlbumFlags) =
    EditNatives.nativeSetAlbumFlags(handle, uid, flags.noScrobble, flags.mix)

/** The releases for [artist] and [title], best fit for album [uid]'s tracks first; about 6 s. */
fun CoreLibrary.searchReleases(uid: String, artist: String, title: String): List<ReleaseInfo> =
    EditNatives.nativeReleaseSearch(handle, uid, artist.encodeToByteArray(), title.encodeToByteArray()).toList()

/** What album [uid]'s tags would be on release [mbAlbumId], to fill in a form: nothing changes. */
fun CoreLibrary.releaseTags(uid: String, mbAlbumId: String): AlbumTags = EditNatives.nativeReleaseTags(handle, uid, mbAlbumId)

internal object EditNatives {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeAlbumTags(handle: Long, uid: String): AlbumTags
    @JvmStatic external fun nativeSetAlbumTags(
        handle: Long, uid: String, title: ByteArray, artist: ByteArray, year: Int, originalYear: Int, mbAlbumId: String,
    )
    @JvmStatic external fun nativeArtistTags(handle: Long, uid: String): ArtistTags
    @JvmStatic external fun nativeSetArtistTags(handle: Long, uid: String, name: ByteArray, sortName: ByteArray)
    @JvmStatic external fun nativeTrackTags(handle: Long, uid: String): TrackTags
    @JvmStatic external fun nativeSetTrackTags(
        handle: Long, uid: String, title: ByteArray, artist: ByteArray, trackNo: Int, discNo: Int, year: Int, genre: ByteArray,
    )
    @JvmStatic external fun nativeAlbumFlags(handle: Long, uid: String): IntArray
    @JvmStatic external fun nativeSetAlbumFlags(handle: Long, uid: String, noScrobble: Boolean, mix: Boolean)
    @JvmStatic external fun nativeReleaseSearch(handle: Long, uid: String, artist: ByteArray, title: ByteArray): Array<ReleaseInfo>
    @JvmStatic external fun nativeReleaseTags(handle: Long, uid: String, mbAlbumId: String): AlbumTags
    @JvmStatic external fun nativeStatusText(status: Int): String
}
