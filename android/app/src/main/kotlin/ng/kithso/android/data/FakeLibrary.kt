package ng.kithso.android.data

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow

/**
 * A made-up library, for previews and for trying the UI without music on the device.
 * MainActivity uses it when started with `--ez fake true`, and with [empty] set when started
 * with `--ez fake_empty true`, to see the empty state.
 */
class FakeLibrary(empty: Boolean = false) : LibrarySource {
    private val full = build()
    private val state = MutableStateFlow(if (empty) Library.Empty else full)

    override val library: StateFlow<Library?> = state

    override suspend fun trackList(ref: CollectionRef): TrackList? {
        val lib = state.value
        return when (ref.kind) {
            CollectionRef.Kind.Album -> lib.albums.firstOrNull { it.uid == ref.uid }?.let { album ->
                TrackList(album.title, album.art, tracksOf(album), album.artist, album.year)
            }
            CollectionRef.Kind.Artist -> lib.artists.firstOrNull { it.uid == ref.uid }?.let { artist ->
                val albums = lib.albums.filter { it.artist == artist.name }
                val singles = singlesOf(artist)
                TrackList(
                    artist.name, artist.art, singles + albums.flatMap(::tracksOf),
                    albumCount = albums.size, albums = albums, singles = singles,
                )
            }
            CollectionRef.Kind.AlbumGroup -> lib.albumGroups.firstOrNull { it.uid == ref.uid }?.let { group ->
                val albums = groupAlbums(lib, group)
                TrackList(group.name, group.art, albums.flatMap(::tracksOf), albumCount = albums.size, albums = albums)
            }
            CollectionRef.Kind.Playlist -> lib.playlists.firstOrNull { it.uid == ref.uid }?.let { playlist ->
                // Every few tracks from across the library, so the list is stable.
                val tracks = lib.albums.flatMap(::tracksOf)
                    .filterIndexed { i, _ -> (i + (playlist.uid.hashCode() and 0x7fffffff)) % 7 == 0 }
                    .take(playlist.trackCount)
                TrackList(playlist.name, playlist.art, tracks)
            }
        }
    }

    // The fake's recents stay as they are.
    override fun notePlayed(ref: CollectionRef) = Unit

    override fun recordPlay(trackUid: String) = Unit

    // Names containing the query, which is enough to try the search screen.
    override suspend fun search(query: String): SearchResults {
        val lib = state.value
        val q = query.trim()
        if (q.isEmpty()) return SearchResults.Empty
        val artists = lib.artists.filter { it.name.contains(q, ignoreCase = true) }
        val albums = lib.albums.filter { it.title.contains(q, ignoreCase = true) }
        val playlists = lib.playlists.filter { it.name.contains(q, ignoreCase = true) }
        val groups = lib.albumGroups.filter { it.name.contains(q, ignoreCase = true) }
        val order = listOfNotNull(
            SearchResults.Kind.Artists.takeIf { artists.isNotEmpty() },
            SearchResults.Kind.Albums.takeIf { albums.isNotEmpty() },
            SearchResults.Kind.Playlists.takeIf { playlists.isNotEmpty() },
            SearchResults.Kind.AlbumGroups.takeIf { groups.isNotEmpty() },
        )
        return SearchResults(order, artists, albums, playlists = playlists, albumGroups = groups)
    }

    override suspend fun reshuffle(ref: CollectionRef) = Unit

    private fun groupAlbums(lib: Library, group: AlbumGroup): List<Album> =
        lib.albums.filterIndexed { i, _ -> (i + (group.uid.hashCode() and 0x7fffffff)) % 5 == 0 }.take(group.albumCount)

    private fun tracksOf(album: Album): List<Track> {
        val count = 6 + (album.uid.hashCode() and 0x7fffffff) % 9
        return (1..count).map { n ->
            Track(
                uid = "${album.uid}-$n",
                title = TRACK_WORDS[(n * 7 + album.title.length) % TRACK_WORDS.size] +
                    " " + TRACK_WORDS[(n * 3 + album.year) % TRACK_WORDS.size].lowercase(),
                artist = album.artist,
                album = album.title,
                trackNo = n,
                durationMs = (150 + (n * 37 + album.year) % 180) * 1000L,
                art = album.art,
                albumUid = album.uid,
                artistUid = "artist-${ARTISTS.indexOf(album.artist)}",
            )
        }
    }

    // Two singles each: one released on its own, one on no album.
    private fun singlesOf(artist: Artist): List<Track> = (0..1).map { n ->
        val title = TRACK_WORDS[(artist.name.length * 5 + n * 11) % TRACK_WORDS.size]
        val album = if (n == 0) title else ""
        val uid = "${artist.uid}-single-$n"
        Track(
            uid = uid,
            title = title,
            artist = artist.name,
            album = album,
            trackNo = 1,
            durationMs = (190 + n * 23) * 1000L,
            art = ArtRef(album.ifEmpty { uid }, album.ifEmpty { title }),
            artistUid = artist.uid,
        )
    }

    private companion object {
        val ARTISTS = listOf(
            "Aurora Lane", "The Paper Kites", "Neon Harbour", "Juniper Hollow", "Mara Vey",
            "Glass Pilots", "Owl & Ember", "Saltwater Choir", "Kite Theory", "Low Orbit",
            "Fennel", "Hollis Gray", "Velvet Static", "North Ward", "Cassia Bloom", "Tidelines",
        )
        val ALBUM_WORDS = listOf(
            "Northern", "Lights", "Paper", "Houses", "Afterglow", "Static", "Summer", "Rivers",
            "Quiet", "Machines", "Golden", "Hour", "Midnight", "Garden", "Small", "Weather",
            "Distant", "Signals", "Hollow", "Bones", "Silver", "Linings", "Late", "Bloom",
        )
        val TRACK_WORDS = listOf(
            "Harbour", "Lanterns", "Undertow", "Wildfire", "Monday", "Satellite", "Echoes",
            "Driftwood", "Paperweight", "Fever", "Comets", "Overgrown", "Lullaby", "Arrows",
            "Ferris", "Copper", "Glasshouse", "Sundown", "Tremor", "Velvet", "Ivory", "Atlas",
        )

        fun build(): Library {
            val albums = (0 until 42).map { i ->
                // Distinct for i < 2 * ALBUM_WORDS.size.
                val title = ALBUM_WORDS[i % ALBUM_WORDS.size] + " " +
                    ALBUM_WORDS[(i * 7 + i / ALBUM_WORDS.size + 3) % ALBUM_WORDS.size].lowercase()
                val uid = "album-$i"
                Album(uid, title, ARTISTS[i % ARTISTS.size], 1994 + (i * 7) % 31, ArtRef(uid, title))
            }.sortedBy { it.title }
            val artists = ARTISTS.mapIndexed { i, name ->
                val uid = "artist-$i"
                Artist(uid, name, albums.count { it.artist == name }, ArtRef(uid, name))
            }.sortedBy { it.name }
            val playlists = listOf("Morning", "Running", "Focus", "Road trip", "Rainy days").mapIndexed { i, name ->
                val uid = "playlist-$i"
                Playlist(uid, name, 12 + i * 5, ArtRef(uid, name))
            }
            val recents = listOf(albums[3], albums[17], albums[29]).map {
                Recent(CollectionRef(CollectionRef.Kind.Album, it.uid), it.title, it.artist, it.art)
            } + playlists[2].let {
                Recent(CollectionRef(CollectionRef.Kind.Playlist, it.uid), it.name, null, it.art)
            }
            val groups = listOf("Desert island", "Autumn").mapIndexed { i, name ->
                val uid = "group-$i"
                AlbumGroup(uid, name, 4 + i * 2, ArtRef(uid, name))
            }
            return Library(recents, playlists, albums, artists, groups)
        }
    }
}
