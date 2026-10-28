package ng.kithso.android.data

import androidx.compose.runtime.Immutable
import kotlinx.coroutines.flow.StateFlow

/**
 * Album art to load: [uid] names the album (or artist or playlist) whose cover it is, and
 * [label] gives the placeholder its letter and colour when there is no cover. [file] is the
 * cover image (never an audio file: embedded art isn't shown), and [version] its modification
 * time, so a changed cover isn't served from the cache; with no [file], art is a placeholder.
 */
@Immutable
data class ArtRef(val uid: String, val label: String, val file: String? = null, val version: Long = 0)

@Immutable
data class Album(
    val uid: String,
    val title: String,
    val artist: String,
    val year: Int,
    val art: ArtRef,
)

/**
 * An artist with something on this phone: [albumCount] albums here as album artist, and
 * [trackCount] tracks here credited to them.
 */
@Immutable
data class Artist(val uid: String, val name: String, val albumCount: Int, val art: ArtRef, val trackCount: Int = 0)

@Immutable
data class Playlist(val uid: String, val name: String, val trackCount: Int, val art: ArtRef)

/** An album group, manual or smart, with [albumCount] albums on this phone. */
@Immutable
data class AlbumGroup(val uid: String, val name: String, val albumCount: Int, val art: ArtRef)

@Immutable
data class Track(
    val uid: String,
    val title: String,
    val artist: String,
    val album: String,
    val trackNo: Int,
    val durationMs: Long,
    val art: ArtRef,
    /** The file to play, or null when the track has none on this device. */
    val path: String? = null,
    /** Its entry in the playlist it was listed from, for moving and removing it; else null. */
    val entryUid: String? = null,
)

/**
 * What a track list screen shows: an album, an artist, a playlist or an album group. A smart
 * playlist is a Playlist and a smart album group an AlbumGroup, as the core's recents have them.
 */
@Immutable
data class CollectionRef(val kind: Kind, val uid: String) {
    enum class Kind { Album, Artist, Playlist, AlbumGroup }
}

/**
 * Something played recently, shown under Recents. [artist] is an album's artist; artists,
 * playlists and album groups have none, and are labelled by their kind instead.
 */
@Immutable
data class Recent(val ref: CollectionRef, val title: String, val artist: String?, val art: ArtRef)

/** The library as the main screen shows it. */
@Immutable
data class Library(
    val recents: List<Recent>,
    val playlists: List<Playlist>,
    val albums: List<Album>,
    val artists: List<Artist>,
    val albumGroups: List<AlbumGroup> = emptyList(),
) {
    val isEmpty: Boolean
        get() = recents.isEmpty() && playlists.isEmpty() && albums.isEmpty() && artists.isEmpty() && albumGroups.isEmpty()

    companion object {
        val Empty = Library(emptyList(), emptyList(), emptyList(), emptyList())
    }
}

/**
 * A track list screen's contents. An album has its [artist] and [year] (0 if unknown), an
 * artist its [albumCount]. An artist and an album group have their [albums] here too; a
 * group's [tracks] are its albums' in turn, for playing it. [canReshuffle] is a smart list
 * ordered by random, which [LibrarySource.reshuffle] gives a new order.
 */
@Immutable
data class TrackList(
    val title: String,
    val art: ArtRef,
    val tracks: List<Track>,
    val artist: String = "",
    val year: Int = 0,
    val albumCount: Int = 0,
    val albums: List<Album> = emptyList(),
    val canReshuffle: Boolean = false,
)

/** A track search found; [albumUid] is its album, null if it has none on this phone. */
@Immutable
data class FoundTrack(
    val uid: String,
    val title: String,
    val artist: String,
    val album: String,
    val albumUid: String?,
    val art: ArtRef,
)

/**
 * What a search found on this phone, each kind best first. [order] is the kinds by their best
 * result, the order to show their sections in.
 */
@Immutable
data class SearchResults(
    val order: List<Kind>,
    val artists: List<Artist> = emptyList(),
    val albums: List<Album> = emptyList(),
    val tracks: List<FoundTrack> = emptyList(),
    val playlists: List<Playlist> = emptyList(),
    val albumGroups: List<AlbumGroup> = emptyList(),
) {
    enum class Kind { Artists, Albums, Tracks, Playlists, AlbumGroups }

    val isEmpty: Boolean get() = order.isEmpty()

    companion object {
        val Empty = SearchResults(emptyList())
    }
}

/**
 * Something a sync profile can select: an album (with its [artist] and [year], 0 if unknown),
 * an artist or an album group (with its [albumCount]), or a playlist.
 */
@Immutable
data class ProfileChoice(
    val uid: String,
    val name: String,
    val artist: String? = null,
    val year: Int = 0,
    val albumCount: Int = 0,
)

/**
 * What a sync profile can select, from the whole library rather than just what's on this
 * phone: every album, artist, manual playlist and manual album group, by the core's uids.
 */
@Immutable
data class ProfileChoices(
    val albums: List<ProfileChoice>,
    val artists: List<ProfileChoice>,
    val playlists: List<ProfileChoice>,
    val albumGroups: List<ProfileChoice>,
)

/** Where the UI gets the library from: the core ([CoreLibrarySource]) or [FakeLibrary]. */
interface LibrarySource {
    /** The library, or null until it has been read for the first time. */
    val library: StateFlow<Library?>

    /** The tracks of [ref], or null if it no longer exists. */
    suspend fun trackList(ref: CollectionRef): TrackList?

    /** Notes that [ref] was played, putting it first under Recents. */
    fun notePlayed(ref: CollectionRef)

    /** What matches [query] (see the core's search.h), among what's on this phone. */
    suspend fun search(query: String): SearchResults

    /** Gives smart list [ref], which is ordered by random, a new order. */
    suspend fun reshuffle(ref: CollectionRef)

    /**
     * Counts a play of track [trackUid] in its play count and last played time, which sync and
     * which smart playlists and Recents read. The player calls it once a track has played
     * long enough to count.
     */
    fun recordPlay(trackUid: String)
}
