package ng.kithso.android.playback

import android.content.Context
import android.os.Bundle
import androidx.annotation.OptIn
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MediaMetadata
import androidx.media3.common.util.UnstableApi
import androidx.media3.session.MediaConstants
import androidx.media3.session.MediaLibraryService.LibraryParams
import androidx.media3.session.MediaSession.MediaItemsWithStartPosition
import ng.kithso.android.R
import ng.kithso.android.data.Album
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.FoundTrack
import ng.kithso.android.data.Library
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.data.SearchResults
import ng.kithso.android.data.Track
import ng.kithso.core.CorePlayer
import kotlinx.coroutines.flow.filterNotNull
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withTimeoutOrNull
import java.net.URLDecoder
import java.net.URLEncoder

/**
 * The library as Android Auto (or any media browser) sees it, for [PlaybackService]'s library
 * session: four tabs under the root, Home (the phone's Recents), Playlists, Albums and Album
 * groups. Albums, artists, playlists and album groups are browsable and playable too, playing
 * from the start; an album group's children are its albums, the others' their tracks, leaving
 * out those with no file here. Browse items carry their art as an [ArtProvider] URI, which Auto
 * can read, and as an [artRef] for [ArtBitmapLoader].
 *
 * Media ids: `root`; the tabs `tab:home`, `tab:playlists`, `tab:albums` and `tab:groups`;
 * collections `album:<uid>`, `artist:<uid>`, `playlist:<uid>` and `group:<uid>`; an album listed
 * under an album group, which plays on into the group's following albums,
 * `groupalbum:<group uid, URL encoded>:<uid>`; a collection's
 * track `track:<collection id>:<index>`, its index in the collection's [TrackList.tracks]; and a
 * track a search found `found:<track uid>`, known from that search. Uids may hold colons, so a
 * collection id is split at its first and a track id at its last.
 *
 * Grids for browsable items and lists for playable ones, as Auto has them by default, except
 * for the Playlists tab, a list, as the app shows it.
 *
 * The library is read through [source] each time, as the app can swap it. Until it has been
 * read for the first time, which starts with the app, the tree waits for it a moment, then
 * answers as if it were empty. Use it on the main thread.
 */
@OptIn(UnstableApi::class)
internal class BrowseTree(private val context: Context, private val source: () -> LibrarySource) {
    // The tracks the latest searches found, by uid, for their found: ids.
    private val found = HashMap<String, FoundTrack>()

    val root: MediaItem
        get() = folder(ROOT, context.getString(R.string.app_name), MediaMetadata.MEDIA_TYPE_FOLDER_MIXED)

    /** The root's styles, which apply throughout the tree unless an item says otherwise. */
    val rootParams: LibraryParams
        get() = LibraryParams.Builder()
            .setExtras(
                Bundle().apply {
                    putInt(MediaConstants.EXTRAS_KEY_CONTENT_STYLE_BROWSABLE, MediaConstants.EXTRAS_VALUE_CONTENT_STYLE_GRID_ITEM)
                    putInt(MediaConstants.EXTRAS_KEY_CONTENT_STYLE_PLAYABLE, MediaConstants.EXTRAS_VALUE_CONTENT_STYLE_LIST_ITEM)
                },
            )
            .build()

    // A car has nothing to do with an empty list: it can't be played or added to there.
    private val Library.filledPlaylists get() = playlists.filter { it.trackCount > 0 }
    private val Library.filledGroups get() = albumGroups.filter { it.albumCount > 0 }

    /** Each tab's id and how many children it has in [library], to say they've changed. */
    fun tabSizes(library: Library): Map<String, Int> = mapOf(
        HOME to library.recents.size,
        PLAYLISTS to library.filledPlaylists.size,
        ALBUMS to library.albums.size,
        GROUPS to library.filledGroups.size,
    )

    /** The children of [parentId], or null if there's no such item or it isn't browsable. */
    suspend fun children(parentId: String): List<MediaItem>? {
        when (parentId) {
            ROOT -> return TABS.map(::tab)
            HOME -> return library().recents.map { it.toBrowseItem() }
            PLAYLISTS -> return library().filledPlaylists.map { it.toBrowseItem() }
            ALBUMS -> return library().albums.map { it.toBrowseItem() }
            GROUPS -> return library().filledGroups.map { it.toBrowseItem() }
        }
        val ref = parseRef(parentId) ?: return null
        val list = source().trackList(ref) ?: return null
        if (ref.kind == CollectionRef.Kind.AlbumGroup) return list.albums.map { it.toBrowseItem(ref.uid) }
        return list.tracks.mapIndexedNotNull { i, track ->
            track.takeIf { it.path != null }?.toBrowseItem(trackId(ref, i))
        }
    }

    /** The item [id] names, or null if there's none. */
    suspend fun item(id: String): MediaItem? {
        if (id == ROOT) return root
        if (id in TABS) return tab(id)
        if (id.startsWith(TRACK)) {
            val (ref, index) = parseTrack(id) ?: return null
            return source().trackList(ref)?.tracks?.getOrNull(index)?.toBrowseItem(id)
        }
        if (id.startsWith(FOUND)) return found[id.removePrefix(FOUND)]?.toBrowseItem()
        val ref = parseRef(id) ?: return null
        val library = library()
        return when (ref.kind) {
            CollectionRef.Kind.Album -> library.albums.firstOrNull { it.uid == ref.uid }?.toBrowseItem(ref.group)
            CollectionRef.Kind.Artist -> library.artists.firstOrNull { it.uid == ref.uid }?.toBrowseItem()
            CollectionRef.Kind.Playlist -> library.playlists.firstOrNull { it.uid == ref.uid }?.toBrowseItem()
            CollectionRef.Kind.AlbumGroup -> library.albumGroups.firstOrNull { it.uid == ref.uid }?.toBrowseItem()
        } ?: library.recents.firstOrNull { it.ref == ref }?.toBrowseItem()
    }

    /** What [query] finds, each kind best first, the kinds in the order of their best result. */
    suspend fun search(query: String): List<MediaItem> {
        val results = source().search(query)
        results.tracks.forEach { found[it.uid] = it }
        return results.order.flatMap { kind ->
            when (kind) {
                SearchResults.Kind.Artists -> results.artists.map { it.toBrowseItem() }
                SearchResults.Kind.Albums -> results.albums.map { it.toBrowseItem() }
                SearchResults.Kind.Tracks -> results.tracks.map { it.toBrowseItem() }
                SearchResults.Kind.Playlists -> results.playlists.map { it.toBrowseItem() }
                SearchResults.Kind.AlbumGroups -> results.albumGroups.map { it.toBrowseItem() }
            }
        }
    }

    /**
     * The queue to play for [request], an item a browser asked to play, which has only an id or
     * a search query; null if there's nothing to play. A collection plays from the start (the
     * first track, or shuffled, one the player picks), a collection's track plays the
     * collection from it, and a query plays its best result, or with none (as for "play
     * Kithsong"), the first of Recents. An album listed under an album group, or a track of
     * one, plays the whole group from there. A collection played is noted, for Recents.
     */
    suspend fun queue(request: MediaItem): MediaItemsWithStartPosition? {
        val id = request.mediaId
        if (id.isEmpty()) return queueForSearch(request.requestMetadata.searchQuery.orEmpty().trim())
        if (id.startsWith(TRACK)) {
            val (ref, index) = parseTrack(id) ?: return null
            return queueOf(ref, index)
        }
        if (id.startsWith(FOUND)) return queueOf(found[id.removePrefix(FOUND)] ?: return null)
        return queueOf(parseRef(id) ?: return null, null)
    }

    /** Whether [request] is a track of an album's, which plays the album from it. */
    fun isAlbumTrack(request: MediaItem): Boolean =
        request.mediaId.startsWith(TRACK) && parseTrack(request.mediaId)?.first?.kind == CollectionRef.Kind.Album

    private suspend fun queueForSearch(query: String): MediaItemsWithStartPosition? {
        if (query.isEmpty()) {
            val library = library()
            val ref = library.recents.firstOrNull()?.ref
                ?: library.albums.firstOrNull()?.let { CollectionRef(CollectionRef.Kind.Album, it.uid) }
                ?: return null
            return queueOf(ref, null)
        }
        val results = source().search(query)
        return when (results.order.firstOrNull() ?: return null) {
            SearchResults.Kind.Artists -> queueOf(CollectionRef(CollectionRef.Kind.Artist, results.artists[0].uid), null)
            SearchResults.Kind.Albums -> queueOf(CollectionRef(CollectionRef.Kind.Album, results.albums[0].uid), null)
            SearchResults.Kind.Tracks -> queueOf(results.tracks[0])
            SearchResults.Kind.Playlists -> queueOf(CollectionRef(CollectionRef.Kind.Playlist, results.playlists[0].uid), null)
            SearchResults.Kind.AlbumGroups ->
                queueOf(CollectionRef(CollectionRef.Kind.AlbumGroup, results.albumGroups[0].uid), null)
        }
    }

    // As the app plays a track a search found: its album from it on, or a single by itself.
    private suspend fun queueOf(track: FoundTrack): MediaItemsWithStartPosition? {
        if (track.albumUid != null) {
            val album = CollectionRef(CollectionRef.Kind.Album, track.albumUid)
            val tracks = source().trackList(album)?.tracks ?: return null
            return queueOf(album, tracks, tracks.indexOfFirst { it.uid == track.uid }.coerceAtLeast(0))
        }
        return queueOf(null, listOf(track.track ?: return null), 0)
    }

    private suspend fun queueOf(ref: CollectionRef, index: Int?): MediaItemsWithStartPosition? {
        val tracks = source().trackList(ref)?.tracks ?: return null
        return queueInGroup(ref, tracks, index) ?: queueOf(ref, tracks, index)
    }

    // The group [ref], an album, was listed under, from tracks[index] or its first track; null
    // if it wasn't, or is no longer in the group.
    private suspend fun queueInGroup(ref: CollectionRef, tracks: List<Track>, index: Int?): MediaItemsWithStartPosition? {
        val group = ref.parentGroup ?: return null
        val track = tracks.getOrNull(index ?: 0) ?: return null
        val queue = source().trackList(group)?.tracks ?: return null
        return queueOf(group, queue, queue.indexOfFirst { it.uid == track.uid }.takeIf { it >= 0 } ?: return null)
    }

    // The tracks of [ref] with a file here, from tracks[index] (or the next with a file) or
    // with no index, from the player's choice of start.
    private fun queueOf(ref: CollectionRef?, tracks: List<Track>, index: Int?): MediaItemsWithStartPosition? {
        val playable = tracks.filter { it.path != null }
        if (playable.isEmpty()) return null
        if (ref != null) source().notePlayed(ref)
        val items = queueItems(context, playable, ref?.origin ?: CorePlayer.Origin(CorePlayer.PlayContext.Manual))
        if (index == null) return MediaItemsWithStartPosition(items, C.INDEX_UNSET, C.TIME_UNSET)
        val start = tracks.take(index).count { it.path != null }.coerceAtMost(playable.lastIndex)
        return MediaItemsWithStartPosition(items, start, 0)
    }

    private suspend fun library(): Library {
        val library = source().library
        return library.value ?: withTimeoutOrNull(LIBRARY_WAIT_MS) { library.filterNotNull().first() } ?: Library.Empty
    }

    private fun tab(id: String): MediaItem = when (id) {
        HOME -> folder(id, context.getString(R.string.section_home), MediaMetadata.MEDIA_TYPE_FOLDER_MIXED)
        PLAYLISTS -> folder(
            id,
            context.getString(R.string.section_playlists),
            MediaMetadata.MEDIA_TYPE_FOLDER_PLAYLISTS,
            MediaConstants.EXTRAS_VALUE_CONTENT_STYLE_LIST_ITEM,
        )
        ALBUMS -> folder(id, context.getString(R.string.section_albums), MediaMetadata.MEDIA_TYPE_FOLDER_ALBUMS)
        else -> folder(id, context.getString(R.string.section_album_groups), MediaMetadata.MEDIA_TYPE_FOLDER_MIXED)
    }

    // A browsable item that isn't played, with [childStyle] for its browsable children.
    private fun folder(id: String, title: String, mediaType: Int, childStyle: Int? = null): MediaItem {
        val extras = childStyle?.let { Bundle().apply { putInt(MediaConstants.EXTRAS_KEY_CONTENT_STYLE_BROWSABLE, it) } }
        return MediaItem.Builder()
            .setMediaId(id)
            .setMediaMetadata(
                MediaMetadata.Builder()
                    .setTitle(title)
                    .setIsBrowsable(true)
                    .setIsPlayable(false)
                    .setMediaType(mediaType)
                    .setExtras(extras)
                    .build(),
            )
            .build()
    }

    private fun collection(ref: CollectionRef, title: String, subtitle: String, art: ArtRef, artist: String? = null): MediaItem {
        val mediaType = when (ref.kind) {
            CollectionRef.Kind.Album -> MediaMetadata.MEDIA_TYPE_ALBUM
            CollectionRef.Kind.Artist -> MediaMetadata.MEDIA_TYPE_ARTIST
            CollectionRef.Kind.Playlist -> MediaMetadata.MEDIA_TYPE_PLAYLIST
            CollectionRef.Kind.AlbumGroup -> MediaMetadata.MEDIA_TYPE_FOLDER_ALBUMS
        }
        return MediaItem.Builder()
            .setMediaId(ref.id)
            .setMediaMetadata(
                MediaMetadata.Builder()
                    .setTitle(title)
                    .setSubtitle(subtitle)
                    .setArtist(artist)
                    .setAlbumTitle(title.takeIf { ref.kind == CollectionRef.Kind.Album })
                    .setArtworkUri(ArtProvider.uriFor(context, art))
                    .setIsBrowsable(true)
                    .setIsPlayable(true)
                    .setMediaType(mediaType)
                    .setExtras(Bundle().apply { putArtRef(art) })
                    .build(),
            )
            .build()
    }

    private fun track(id: String, title: String, artist: String, album: String, art: ArtRef, durationMs: Long?): MediaItem =
        MediaItem.Builder()
            .setMediaId(id)
            .setMediaMetadata(
                MediaMetadata.Builder()
                    .setTitle(title)
                    .setSubtitle(artist)
                    .setArtist(artist)
                    .setAlbumTitle(album)
                    .setDurationMs(durationMs)
                    .setArtworkUri(ArtProvider.uriFor(context, art))
                    .setIsBrowsable(false)
                    .setIsPlayable(true)
                    .setMediaType(MediaMetadata.MEDIA_TYPE_MUSIC)
                    .setExtras(Bundle().apply { putArtRef(art) })
                    .build(),
            )
            .build()

    // An album's artist, or what kind of thing it is, as the app's Recents has it.
    private fun Recent.toBrowseItem(): MediaItem {
        val kind = when (ref.kind) {
            CollectionRef.Kind.Artist -> R.string.artist
            CollectionRef.Kind.AlbumGroup -> R.string.album_group
            else -> R.string.playlist
        }
        return collection(ref, title, artist ?: context.getString(kind), art, artist)
    }

    private fun Album.toBrowseItem(group: String? = null) =
        collection(CollectionRef(CollectionRef.Kind.Album, uid, group), title, artist, art, artist)

    private fun Artist.toBrowseItem() = collection(
        CollectionRef(CollectionRef.Kind.Artist, uid),
        name,
        if (albumCount > 0) plural(R.plurals.albums, albumCount) else plural(R.plurals.tracks, trackCount),
        art,
    )

    private fun Playlist.toBrowseItem() =
        collection(CollectionRef(CollectionRef.Kind.Playlist, uid), name, plural(R.plurals.tracks, trackCount), art)

    private fun AlbumGroup.toBrowseItem() =
        collection(CollectionRef(CollectionRef.Kind.AlbumGroup, uid), name, plural(R.plurals.albums, albumCount), art)

    private fun Track.toBrowseItem(id: String) = track(id, title, artist, album, art, durationMs.takeIf { it > 0 })

    private fun FoundTrack.toBrowseItem() = track(FOUND + uid, title, artist, album, art, track?.durationMs)

    private fun plural(id: Int, n: Int) = context.resources.getQuantityString(id, n, n)

    internal companion object {
        const val ROOT = "root"
        const val HOME = "tab:home"
        const val PLAYLISTS = "tab:playlists"
        const val ALBUMS = "tab:albums"
        const val GROUPS = "tab:groups"
        const val TRACK = "track:"
        const val FOUND = "found:"
        const val GROUP_ALBUM = "groupalbum"
        val TABS = listOf(HOME, PLAYLISTS, ALBUMS, GROUPS)

        // How long to wait for the library's first read, which takes a moment at most; a
        // browser asking before then is likely to have started the app itself.
        const val LIBRARY_WAIT_MS = 3000L

        val CollectionRef.id: String
            get() = when {
                group != null -> "$GROUP_ALBUM:${URLEncoder.encode(group, "UTF-8")}"
                kind == CollectionRef.Kind.Album -> "album"
                kind == CollectionRef.Kind.Artist -> "artist"
                kind == CollectionRef.Kind.Playlist -> "playlist"
                else -> "group"
            } + ":" + uid

        fun parseRef(id: String): CollectionRef? {
            if (id.startsWith("$GROUP_ALBUM:")) {
                val rest = id.substringAfter(':')
                if (':' !in rest) return null
                return CollectionRef(CollectionRef.Kind.Album, rest.substringAfter(':'), URLDecoder.decode(rest.substringBefore(':'), "UTF-8"))
            }
            val kind = when (id.substringBefore(':', "")) {
                "album" -> CollectionRef.Kind.Album
                "artist" -> CollectionRef.Kind.Artist
                "playlist" -> CollectionRef.Kind.Playlist
                "group" -> CollectionRef.Kind.AlbumGroup
                else -> return null
            }
            return CollectionRef(kind, id.substringAfter(':'))
        }

        fun trackId(ref: CollectionRef, index: Int) = "$TRACK${ref.id}:$index"

        fun parseTrack(id: String): Pair<CollectionRef, Int>? {
            val rest = id.removePrefix(TRACK)
            val index = rest.substringAfterLast(':', "").toIntOrNull()?.takeIf { it >= 0 } ?: return null
            return (parseRef(rest.substringBeforeLast(':')) ?: return null) to index
        }
    }
}
