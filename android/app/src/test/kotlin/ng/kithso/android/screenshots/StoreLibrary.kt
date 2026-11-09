package ng.kithso.android.screenshots

import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import ng.kithso.android.data.Album
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.FoundTrack
import ng.kithso.android.data.Home
import ng.kithso.android.data.Library
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.data.SearchResults
import ng.kithso.android.data.Track
import ng.kithso.android.data.TrackList
import ng.kithso.android.playback.Playback
import ng.kithso.core.CorePlayer
import ng.kithso.core.LibraryStats
import ng.kithso.core.LibraryTally
import java.io.File

/**
 * The store's made-up library: the same six albums and 34 tracks as the Play listing's demo
 * zip (albums.py in the listing's scripts), with its covers. Names are kept identical to it so
 * every platform's screenshots show the same library. Copperline's and Nightjar Season's track
 * lengths are the demo zip's, as the listing shows them; the other albums' are made up.
 */
object StoreLibrary : LibrarySource {
    private val covers = File(StoreLibrary::class.java.getResource("/store-demo/0.jpg")!!.toURI()).parentFile

    private class Def(val i: Int, val artist: String, val title: String, val year: Int, val tracks: List<String>, val seconds: List<Int>? = null)

    private val defs = listOf(
        Def(0, "Marrowfield Choir", "Salt on the Sill", 2021, listOf("Salt on the Sill", "The Long Tide Home", "Grey Gulls", "Mending Nets", "Windward", "A Lamp for the Boats")),
        Def(1, "Oona Vesper", "Low Country Radio", 2023, listOf("Low Country Radio", "Dust on the Dial", "Halfway to Morning", "Porchlight", "Cicada Static", "Last Call for Rain")),
        Def(2, "Pale Orchard", "Nightjar Season", 2019, listOf("Nightjar Season", "Windfall", "Soft Machinery", "Pollen Count", "Blue Hour Bicycle"), listOf(179, 163, 249, 204, 290)),
        Def(3, "Tomas Quillfeather", "Small Hours, Big Rooms", 2022, listOf("Small Hours", "Big Rooms", "Coat Check Blues", "Third Set", "Elevator to the Lobby", "Goodnight, Mezzanine")),
        Def(4, "Kestrel Bay Ensemble", "Field Notes for Late Trains", 2020, listOf("Platform Four", "Sleeper Car", "Night Freight", "Field Notes", "Terminus")),
        Def(5, "Ivy Sunderholt", "Copperline", 2024, listOf("Copperline", "Good Bones", "Paper Towns Burn Bright", "Ladder Song", "Keep the Engine Running", "Copperline (Reprise)"), listOf(228, 264, 224, 194, 210, 224)),
    )

    private fun art(i: Int, label: String) = ArtRef("cover-$i", label, File(covers, "$i.jpg").path)

    val albums = defs.map { Album("album-${it.i}", it.title, it.artist, it.year, art(it.i, it.title)) }.sortedBy { it.title }
    private val artists = defs.map { Artist("artist-${it.i}", it.artist, 1, art(it.i, it.artist), it.tracks.size) }.sortedBy { it.name }
    val tracks: Map<String, List<Track>> = defs.associate { d ->
        "album-${d.i}" to d.tracks.mapIndexed { n, t ->
            val seconds = d.seconds?.get(n) ?: (148 + (n * 37 + d.i * 11) % 140)
            Track(
                "t${d.i}-$n", t, d.artist, d.title, n + 1, seconds * 1000L, art(d.i, d.title),
                path = "/demo/${d.i}/$n.mp3", albumUid = "album-${d.i}", artistUid = "artist-${d.i}",
            )
        }
    }
    private val allTracks = tracks.values.flatten()

    const val COPPERLINE = "album-5"
    const val NIGHTJAR = "album-2"

    private val recents = listOf(5, 2).map { i ->
        val a = albums.first { it.uid == "album-$i" }
        Recent(CollectionRef(CollectionRef.Kind.Album, a.uid), a.title, a.artist, a.art)
    }
    private val playlists = listOf(Playlist("pl-0", "Late night", 9, art(2, "Late night")))
    private val lib = Library(recents, playlists, albums, artists)

    override val library: StateFlow<Library?> = MutableStateFlow(lib)
    override val home: StateFlow<Home?> = MutableStateFlow(Home(emptyList(), hasMusic = true, hasPlays = true))
    override suspend fun trackList(ref: CollectionRef): TrackList? = albums.firstOrNull { it.uid == ref.uid }?.let {
        TrackList(it.title, it.art, tracks.getValue(it.uid), it.artist, it.year)
    }
    override suspend fun stationSeed(seedUid: String): Track? = null
    override suspend fun tracks(uids: List<String>): List<Track?> = uids.map { uid -> allTracks.firstOrNull { it.uid == uid } }
    override fun notePlayed(ref: CollectionRef) = Unit

    // Albums, artists and tracks whose names hold the query, as the core's search finds them.
    override suspend fun search(query: String): SearchResults {
        val q = query.trim()
        fun String.has() = contains(q, ignoreCase = true)
        val foundArtists = artists.filter { it.name.has() }
        val foundAlbums = albums.filter { it.title.has() || it.artist.has() }
        val foundTracks = allTracks.filter { it.title.has() || it.album.has() || it.artist.has() }
            // Those named for it first, by title, then those on an album or by an artist named for it.
            .sortedWith(compareBy({ !it.title.has() }, { if (it.title.has()) it.title else "" }, { it.album }, { it.trackNo }))
            .map { FoundTrack(it.uid, it.title, it.artist, it.album, it.albumUid, it.art, it) }
        val order = listOfNotNull(
            SearchResults.Kind.Artists.takeIf { foundArtists.isNotEmpty() },
            SearchResults.Kind.Albums.takeIf { foundAlbums.isNotEmpty() },
            SearchResults.Kind.Tracks.takeIf { foundTracks.isNotEmpty() },
        )
        return SearchResults(order, artists = foundArtists, albums = foundAlbums, tracks = foundTracks)
    }
    override suspend fun trackArtists(trackUid: String): List<Artist> = emptyList()
    override suspend fun reshuffle(ref: CollectionRef) = Unit
    override fun recordPlay(trackUid: String) = Unit
    override fun logPlay(event: CorePlayer.PlayEvent) = Unit
    override suspend fun stats(): LibraryStats {
        val t = LibraryTally(tracks = allTracks.size.toLong(), albums = 6, artists = 6, durationMs = allTracks.sumOf { it.durationMs })
        return LibraryStats(all = t, here = t, playlists = 1)
    }
}

/**
 * A player stopped in time: [track] of [queue] playing at [positionMs], which never moves, so
 * every shot comes out the same.
 */
class StorePlayback(private val queue: List<Track>, private val index: Int, position: Long) : Playback {
    override var track by mutableStateOf<Track?>(queue[index])
        private set
    override val isPlaying = true
    override var positionMs by mutableLongStateOf(position)
        private set
    override val hasNext get() = index < queue.lastIndex
    override val hasPrevious get() = index > 0
    override val nextTrack get() = queue.getOrNull(index + 1)
    override val previousTrack get() = queue.getOrNull(index - 1)
    override val shuffle = false
    override val queueUids get() = queue.map { it.uid }
    override val queueTracks get() = queue

    override fun play(queue: List<Track>, index: Int, origin: CorePlayer.Origin) = Unit
    override fun toggle() = Unit
    override fun seekTo(positionMs: Long) = Unit
    override fun next() = Unit
    override fun previous() = Unit
    override fun skipToPrevious() = Unit
    override fun setShuffle(on: Boolean) = Unit
    override fun stop() = Unit
    override fun clear() = Unit
}
