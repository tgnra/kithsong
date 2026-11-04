package ng.kithso.core

/** mc_recent_kind, in its order, as [RecentInfo] indexes into it: what a recent names. */
enum class ItemKind { Album, Artist, Playlist, AlbumGroup, Station }

/**
 * mc_recent_info. [kind] is an album, artist, playlist (manual or smart, [isSmart]), album group
 * (likewise) or a station, whose [uid] is its seed track's; [artist] is an album's artist, if
 * known. [openedMs] is Unix ms. A station has [seedTitle], its seed's title, and [sub], the line
 * that tells it from others of its name.
 */
data class RecentInfo(
    val kind: ItemKind,
    val uid: String,
    val openedMs: Long,
    val name: String,
    val artist: String?,
    val isSmart: Boolean,
    val seedTitle: String? = null,
    val sub: String? = null,
) {
    internal constructor(
        kind: Int, uid: String, openedMs: Long, name: String, artist: String?, isSmart: Boolean, seedTitle: String?, sub: String?,
    ) : this(ItemKind.entries[kind], uid, openedMs, name, artist, isSmart, seedTitle, sub)
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

/**
 * mc_credit_artist: one artist of a credit, [name] their own name (which the credit may spell
 * otherwise) and [join] what follows them in it ("A feat. B" is A, " feat. ", then B, "").
 */
data class CreditArtist(val uid: String, val name: String, val join: String)

/**
 * The artists track or album [uid] is credited to, in order; a track with no artist of its own
 * has its album's (mc_library_list_credit). Empty if there are none.
 */
fun CoreLibrary.credit(uid: String): List<CreditArtist> =
    CoreBrowse.nativeListCredit(handle, uid).asList()

/** An artist's albums: as album artist first, then those they're credited on. Singles are left out ([artistSingles]). */
fun CoreLibrary.artistAlbums(artistUid: String): List<AlbumInfo> =
    CoreBrowse.nativeListAlbums(handle, CoreBrowse.ALBUMS_ARTIST, artistUid).asList()

/** Every track credited to [artistUid], by title. */
fun CoreLibrary.artistTracks(artistUid: String): List<TrackInfo> =
    CoreBrowse.nativeArtistTracks(handle, artistUid).asList()

/**
 * [artistUid]'s singles: their tracks that are the only one of their album in the library, or
 * on no album, by year then title (mc_library_list_artist_singles).
 */
fun CoreLibrary.artistSingles(artistUid: String): List<TrackInfo> =
    CoreBrowse.nativeArtistSingles(handle, artistUid).asList()

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

/**
 * The image file to show for smart list [smartUid] (mc_smart_art), of either kind: made as
 * [CoreLibrary.playlistArt] or [albumGroupArt] makes its manual counterpart's, from what its
 * rule picks now, into the same [cacheDir]; null for a placeholder, a rule that can't be
 * evaluated included. Call it off the main thread.
 */
fun CoreLibrary.smartArt(smartUid: String, cacheDir: String): String? =
    CoreBrowse.nativeSmartArt(handle, smartUid, cacheDir)

/** Gives a smart list ordered by random a new order (mc_smart_reshuffle). */
fun CoreLibrary.reshuffle(smartUid: String) = CoreBrowse.nativeReshuffle(handle, smartUid)

/** Notes that [uid], a [kind], was opened now, putting it first in the recents everywhere. */
fun CoreLibrary.recordOpened(kind: ItemKind, uid: String) = CoreBrowse.nativeRecordOpened(handle, kind.ordinal, uid)

/** Takes [uid] out of the recents everywhere until it's next opened; nothing if it isn't in them. */
fun CoreLibrary.removeRecent(uid: String) = CoreBrowse.nativeRemoveRecent(handle, uid)

/** Empties the recents, everywhere. */
fun CoreLibrary.clearRecents() = CoreBrowse.nativeClearRecents(handle)

/**
 * The recents, stations among them, newest first, at most [limit]; if [playableOnly], without
 * playlists and smart lists that have nothing to play here, nor stations whose seed has no file
 * here (MC_LIST_PLAYABLE).
 */
fun CoreLibrary.recents(limit: Int, playableOnly: Boolean = false): List<RecentInfo> =
    CoreBrowse.nativeListRecents(
        handle, limit, CoreBrowse.LIST_STATIONS or (if (playableOnly) CoreBrowse.LIST_PLAYABLE else 0),
    ).asList()

/** What matches [query], best first, at most [limitPerKind] of each kind (mc_library_search). */
fun CoreLibrary.search(query: String, limitPerKind: Int): List<SearchHit> =
    CoreBrowse.nativeSearch(handle, query, limitPerKind).asList()

/** Starts building the search index in the background if it's out of date. */
fun CoreLibrary.searchPrepare() = CoreBrowse.nativeSearchPrepare(handle)

/** mc_library_tally: numbers about some of the library's music. */
data class LibraryTally(
    val tracks: Long = 0,
    val albums: Long = 0,
    val singles: Long = 0,
    val artists: Long = 0,
    val genres: Long = 0,
    val durationMs: Long = 0,
)

/**
 * mc_library_stats. [all] is the whole library and [here] what has a file on this device;
 * [files] and [sizeBytes] are this device's. The lists are as saved, smart ones apart; plays and
 * time listened are every device's.
 */
data class LibraryStats(
    val all: LibraryTally = LibraryTally(),
    val here: LibraryTally = LibraryTally(),
    val files: Long = 0,
    val sizeBytes: Long = 0,
    val playlists: Long = 0,
    val smartPlaylists: Long = 0,
    val albumGroups: Long = 0,
    val smartAlbumGroups: Long = 0,
    val plays: Long = 0,
    val tracksPlayed: Long = 0,
    val listenedMs: Long = 0,
)

/** Numbers about the library (mc_library_read_stats). Call it off the main thread. */
fun CoreLibrary.stats(): LibraryStats {
    val n = CoreBrowse.nativeStats(handle)
    fun tally(at: Int) = LibraryTally(n[at], n[at + 1], n[at + 2], n[at + 3], n[at + 4], n[at + 5])
    return LibraryStats(tally(0), tally(6), n[12], n[13], n[14], n[15], n[16], n[17], n[18], n[19], n[20])
}

/** Bindings in android/native/src/main/cpp/jni_browse.c. */
internal object CoreBrowse {
    init {
        System.loadLibrary("kithsong_jni")
    }

    @JvmStatic external fun nativeStats(handle: Long): LongArray

    // MC_LIST_PLAYABLE
    const val LIST_PLAYABLE = 1

    // MC_LIST_STATIONS
    const val LIST_STATIONS = 2

    // nativeListAlbums' kinds
    const val ALBUMS_ARTIST = 0
    const val ALBUMS_GROUP = 1
    const val ALBUMS_SMART = 2

    @JvmStatic external fun nativeListAlbums(handle: Long, kind: Int, uid: String): Array<AlbumInfo>
    @JvmStatic external fun nativeArtistTracks(handle: Long, uid: String): Array<TrackInfo>
    @JvmStatic external fun nativeArtistSingles(handle: Long, uid: String): Array<TrackInfo>
    @JvmStatic external fun nativePreviewSmartTracks(handle: Long, rule: String): Array<TrackInfo>
    @JvmStatic external fun nativeAlbumGroupArt(handle: Long, uid: String, cacheDir: String): String?
    @JvmStatic external fun nativeSmartArt(handle: Long, uid: String, cacheDir: String): String?
    @JvmStatic external fun nativeReshuffle(handle: Long, uid: String)
    @JvmStatic external fun nativeRecordOpened(handle: Long, kind: Int, uid: String)
    @JvmStatic external fun nativeRemoveRecent(handle: Long, uid: String)
    @JvmStatic external fun nativeClearRecents(handle: Long)
    @JvmStatic external fun nativeListRecents(handle: Long, limit: Int, flags: Int): Array<RecentInfo>
    @JvmStatic external fun nativeSearch(handle: Long, query: String, limit: Int): Array<SearchHit>
    @JvmStatic external fun nativeSearchPrepare(handle: Long)
    @JvmStatic external fun nativeListCredit(handle: Long, uid: String): Array<CreditArtist>
}
