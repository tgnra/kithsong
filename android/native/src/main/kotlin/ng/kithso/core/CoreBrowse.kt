package ng.kithso.core

/** mc_recent_kind: what a recent or a search hit names. */
enum class ItemKind { Album, Artist, Playlist, AlbumGroup }

/**
 * mc_recent_info. [kind] is an album, artist, playlist (manual or smart, [isSmart]) or album
 * group (likewise); [artist] is an album's artist, if known. [openedMs] is Unix ms.
 */
data class RecentInfo(
    val kind: ItemKind,
    val uid: String,
    val openedMs: Long,
    val name: String,
    val artist: String?,
    val isSmart: Boolean,
) {
    internal constructor(kind: Int, uid: String, openedMs: Long, name: String, artist: String?, isSmart: Boolean) :
        this(ItemKind.entries[kind], uid, openedMs, name, artist, isSmart)
}

/**
 * mc_search_hit. [artist] is an album's or a track's, if known; [album] and [albumUid] are a
 * track's album, if it has one. A playlist or album group [isSmart] if it's a smart list.
 */
data class SearchHit(
    val kind: SearchKind,
    val uid: String,
    val name: String,
    val artist: String?,
    val album: String?,
    val albumUid: String?,
    val isSmart: Boolean,
) {
    internal constructor(
        kind: Int, uid: String, name: String, artist: String?, album: String?, albumUid: String?, isSmart: Boolean,
    ) : this(SearchKind.entries[kind], uid, name, artist, album, albumUid, isSmart)
}

/** mc_search_kind, in its order. */
enum class SearchKind { Artist, Album, Track, Playlist, AlbumGroup }

/** An artist's albums: as album artist first, then those they're credited on, then singles. */
fun CoreLibrary.artistAlbums(artistUid: String): List<AlbumInfo> =
    CoreBrowse.nativeListAlbums(handle, CoreBrowse.ALBUMS_ARTIST, artistUid).asList()

/** Every track credited to [artistUid], by title. */
fun CoreLibrary.artistTracks(artistUid: String): List<TrackInfo> =
    CoreBrowse.nativeArtistTracks(handle, artistUid).asList()

/** A manual album group's albums, in the group's order. */
fun CoreLibrary.albumGroupAlbums(groupUid: String): List<AlbumInfo> =
    CoreBrowse.nativeListAlbums(handle, CoreBrowse.ALBUMS_GROUP, groupUid).asList()

/** What smart album group [smartUid] holds now, evaluated afresh. */
fun CoreLibrary.smartAlbums(smartUid: String): List<AlbumInfo> =
    CoreBrowse.nativeListAlbums(handle, CoreBrowse.ALBUMS_SMART, smartUid).asList()

/** The tracks a smart playlist with [rule] (JSON, see docs/schema.md) would hold. */
fun CoreLibrary.previewSmartTracks(rule: String): List<TrackInfo> =
    CoreBrowse.nativePreviewSmartTracks(handle, rule).asList()

/**
 * The image file to show for album group [groupUid] (mc_album_group_art), made as
 * [CoreLibrary.playlistArt] makes a playlist's, into the same [cacheDir]; null for a
 * placeholder. Call it off the main thread.
 */
fun CoreLibrary.albumGroupArt(groupUid: String, cacheDir: String): String? =
    CoreBrowse.nativeAlbumGroupArt(handle, groupUid, cacheDir)

/** Gives a smart list ordered by random a new order (mc_smart_reshuffle). */
fun CoreLibrary.reshuffle(smartUid: String) = CoreBrowse.nativeReshuffle(handle, smartUid)

/** Notes that [uid], a [kind], was opened now, putting it first in the recents everywhere. */
fun CoreLibrary.recordOpened(kind: ItemKind, uid: String) = CoreBrowse.nativeRecordOpened(handle, kind.ordinal, uid)

/** Takes [uid] out of the recents everywhere until it's next opened; nothing if it isn't in them. */
fun CoreLibrary.removeRecent(uid: String) = CoreBrowse.nativeRemoveRecent(handle, uid)

/** Empties the recents, everywhere. */
fun CoreLibrary.clearRecents() = CoreBrowse.nativeClearRecents(handle)

/**
 * The recents, newest first, at most [limit]; if [playableOnly], without playlists and smart
 * lists that have nothing to play here (MC_LIST_PLAYABLE).
 */
fun CoreLibrary.recents(limit: Int, playableOnly: Boolean = false): List<RecentInfo> =
    CoreBrowse.nativeListRecents(handle, limit, if (playableOnly) CoreBrowse.LIST_PLAYABLE else 0).asList()

/** What matches [query], best first, at most [limitPerKind] of each kind (mc_library_search). */
fun CoreLibrary.search(query: String, limitPerKind: Int): List<SearchHit> =
    CoreBrowse.nativeSearch(handle, query, limitPerKind).asList()

/** Starts building the search index in the background if it's out of date. */
fun CoreLibrary.searchPrepare() = CoreBrowse.nativeSearchPrepare(handle)

/** Bindings in android/native/src/main/cpp/jni_browse.c. */
internal object CoreBrowse {
    init {
        System.loadLibrary("kithsong_jni")
    }

    // MC_LIST_PLAYABLE
    const val LIST_PLAYABLE = 1

    // nativeListAlbums' kinds
    const val ALBUMS_ARTIST = 0
    const val ALBUMS_GROUP = 1
    const val ALBUMS_SMART = 2

    @JvmStatic external fun nativeListAlbums(handle: Long, kind: Int, uid: String): Array<AlbumInfo>
    @JvmStatic external fun nativeArtistTracks(handle: Long, uid: String): Array<TrackInfo>
    @JvmStatic external fun nativePreviewSmartTracks(handle: Long, rule: String): Array<TrackInfo>
    @JvmStatic external fun nativeAlbumGroupArt(handle: Long, uid: String, cacheDir: String): String?
    @JvmStatic external fun nativeReshuffle(handle: Long, uid: String)
    @JvmStatic external fun nativeRecordOpened(handle: Long, kind: Int, uid: String)
    @JvmStatic external fun nativeRemoveRecent(handle: Long, uid: String)
    @JvmStatic external fun nativeClearRecents(handle: Long)
    @JvmStatic external fun nativeListRecents(handle: Long, limit: Int, flags: Int): Array<RecentInfo>
    @JvmStatic external fun nativeSearch(handle: Long, query: String, limit: Int): Array<SearchHit>
    @JvmStatic external fun nativeSearchPrepare(handle: Long)
}
