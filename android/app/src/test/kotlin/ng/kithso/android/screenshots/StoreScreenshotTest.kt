package ng.kithso.android.screenshots

import android.app.Application
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.unit.dp
import androidx.test.core.app.ApplicationProvider
import coil3.SingletonImageLoader
import com.github.takahirom.roborazzi.captureRoboImage
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import ng.kithso.android.data.Album
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Home
import ng.kithso.android.data.Library
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.data.SearchResults
import ng.kithso.android.data.Track
import ng.kithso.android.data.TrackList
import ng.kithso.android.playback.FakePlayback
import ng.kithso.android.ui.DetailScreen
import ng.kithso.android.ui.ImportPickers
import ng.kithso.android.ui.MainScreen
import ng.kithso.android.ui.MiniPlayer
import ng.kithso.android.ui.PlaylistActions
import ng.kithso.android.ui.art.FileArt
import ng.kithso.android.ui.art.artImageLoader
import ng.kithso.android.ui.theme.KithsongTheme
import ng.kithso.core.CorePlayer
import ng.kithso.core.LibraryStats
import ng.kithso.core.LibraryTally
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.io.File

/** The store's made-up library: the same six albums as the Play listing's demo zip. */
object StoreLibrary : LibrarySource {
    private val covers = File(StoreLibrary::class.java.getResource("/store-demo/0.jpg")!!.toURI()).parentFile

    private class Def(val i: Int, val artist: String, val title: String, val year: Int, val tracks: List<String>)

    private val defs = listOf(
        Def(0, "Marrowfield Choir", "Salt on the Sill", 2021, listOf("Salt on the Sill", "The Long Tide Home", "Grey Gulls", "Mending Nets", "Windward", "A Lamp for the Boats")),
        Def(1, "Oona Vesper", "Low Country Radio", 2023, listOf("Low Country Radio", "Dust on the Dial", "Halfway to Morning", "Porchlight", "Cicada Static", "Last Call for Rain")),
        Def(2, "Pale Orchard", "Nightjar Season", 2019, listOf("Nightjar Season", "Windfall", "Soft Machinery", "Pollen Count", "Blue Hour Bicycle")),
        Def(3, "Tomas Quillfeather", "Small Hours, Big Rooms", 2022, listOf("Small Hours", "Big Rooms", "Coat Check Blues", "Third Set", "Elevator to the Lobby", "Goodnight, Mezzanine")),
        Def(4, "Kestrel Bay Ensemble", "Field Notes for Late Trains", 2020, listOf("Platform Four", "Sleeper Car", "Night Freight", "Field Notes", "Terminus")),
        Def(5, "Ivy Sunderholt", "Copperline", 2024, listOf("Copperline", "Good Bones", "Paper Towns Burn Bright", "Ladder Song", "Keep the Engine Running", "Copperline (Reprise)")),
    )

    private fun art(i: Int, label: String) = ArtRef("cover-$i", label, File(covers, "$i.jpg").path)

    val albums = defs.map { Album("album-${it.i}", it.title, it.artist, it.year, art(it.i, it.title)) }.sortedBy { it.title }
    private val artists = defs.map { Artist("artist-${it.i}", it.artist, 1, art(it.i, it.artist), it.tracks.size) }.sortedBy { it.name }
    val tracks: Map<String, List<Track>> = defs.associate { d ->
        "album-${d.i}" to d.tracks.mapIndexed { n, t ->
            Track(
                "t${d.i}-$n", t, d.artist, d.title, n + 1, (148 + (n * 37 + d.i * 11) % 140) * 1000L, art(d.i, d.title),
                path = "/demo/${d.i}/$n.mp3", albumUid = "album-${d.i}", artistUid = "artist-${d.i}",
            )
        }
    }
    private val recents = listOf(5, 2, 0).map { i ->
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
    override suspend fun tracks(uids: List<String>): List<Track?> = uids.map { null }
    override fun notePlayed(ref: CollectionRef) = Unit
    override suspend fun search(query: String) = SearchResults.Empty
    override suspend fun trackArtists(trackUid: String): List<Artist> = emptyList()
    override suspend fun reshuffle(ref: CollectionRef) = Unit
    override fun recordPlay(trackUid: String) = Unit
    override fun logPlay(event: CorePlayer.PlayEvent) = Unit
    override suspend fun stats(): LibraryStats {
        val t = LibraryTally(tracks = 34, albums = 6, artists = 6, durationMs = 34 * 210_000L)
        return LibraryStats(all = t, here = t, playlists = 1)
    }
}

/**
 * Prototype: phone store screenshots drawn on the JVM (Robolectric native graphics and
 * Roborazzi), at the listing's phone size, 1080x1920. Writes build/outputs/roborazzi/phone-*.png.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [36], application = Application::class, qualifiers = "w411dp-h731dp-port-420dpi")
class StoreScreenshotTest {
    @get:Rule val compose = createComposeRule()

    @Before fun images() {
        val context = ApplicationProvider.getApplicationContext<Application>()
        SingletonImageLoader.setUnsafe(artImageLoader(context, FileArt()))
    }

    private fun shoot(name: String, content: @Composable () -> Unit) {
        compose.setContent {
            KithsongTheme { Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) { content() } }
        }
        // Coil decodes off the main thread: let it, then let compose settle.
        repeat(30) {
            Thread.sleep(50)
            compose.waitForIdle()
        }
        compose.onRoot().captureRoboImage("build/outputs/roborazzi/phone-$name.png")
    }

    private fun playing(): FakePlayback = FakePlayback(MainScope()).also {
        it.load(StoreLibrary.tracks.getValue("album-5"), 2)
    }

    @Test fun library() {
        val playback = playing()
        shoot("1-library") {
            Box(Modifier.fillMaxSize()) {
                MainScreen(
                    StoreLibrary.library.value, bottomPadding = 72.dp, onOpen = {}, onSearch = {}, onSync = {},
                    onSettings = {}, onStats = {}, importPickers = ImportPickers({}, {}), importRunning = false, banner = null,
                )
                MiniPlayer(playback, playback.track!!, onOpen = {}, onMenu = {}, modifier = Modifier.align(Alignment.BottomCenter).padding(8.dp))
            }
        }
    }

    @Test fun albumDark() {
        org.robolectric.RuntimeEnvironment.setQualifiers("+night")
        val playback = playing()
        shoot("3-album-dark") {
            Box(Modifier.fillMaxSize()) {
                DetailScreen(
                    CollectionRef(CollectionRef.Kind.Album, "album-2"), StoreLibrary, PlaylistActions(editor = null),
                    bottomPadding = 72.dp, onPlay = { _, _ -> }, onPlayAll = { _, _ -> }, onOpen = {}, onBack = {},
                )
                MiniPlayer(playback, playback.track!!, onOpen = {}, onMenu = {}, modifier = Modifier.align(Alignment.BottomCenter).padding(8.dp))
            }
        }
    }

    @Test fun album() {
        val playback = playing()
        shoot("2-album") {
            Box(Modifier.fillMaxSize()) {
                DetailScreen(
                    CollectionRef(CollectionRef.Kind.Album, "album-5"), StoreLibrary, PlaylistActions(editor = null),
                    bottomPadding = 72.dp, onPlay = { _, _ -> }, onPlayAll = { _, _ -> }, onOpen = {}, onBack = {},
                )
                MiniPlayer(playback, playback.track!!, onOpen = {}, onMenu = {}, modifier = Modifier.align(Alignment.BottomCenter).padding(8.dp))
            }
        }
    }
}
