package ng.kithso.wear.ui

import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.onRoot
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.MaterialTheme
import com.github.takahirom.roborazzi.captureRoboImage
import ng.kithso.core.AlbumInfo
import ng.kithso.core.TrackInfo
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.RobolectricTestRunner
import java.io.File

/**
 * Prototype: the watch's store screenshots, drawn on the JVM by Robolectric at Wear OS small
 * round (384x384), from the made-up store library. Writes build/outputs/roborazzi/wear-*.png.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [36], qualifiers = "w192dp-h192dp-round-watch-xhdpi", application = android.app.Application::class)
class StoreScreenshotTest {
    @get:Rule val compose = createComposeRule()

    private val covers: File = File(javaClass.getResource("/store-demo/0.jpg")!!.toURI()).parentFile

    private fun album(i: Int, title: String, artist: String, year: Int) =
        AlbumInfo("a$i", title, artist, year, isSingle = false, hasArt = true, folder = File(covers, "$i.jpg").path)

    private val albums = listOf(
        album(5, "Copperline", "Ivy Sunderholt", 2024),
        album(4, "Field Notes for Late Trains", "Kestrel Bay Ensemble", 2020),
        album(1, "Low Country Radio", "Oona Vesper", 2023),
        album(2, "Nightjar Season", "Pale Orchard", 2019),
        album(0, "Salt on the Sill", "Marrowfield Choir", 2021),
        album(3, "Small Hours, Big Rooms", "Tomas Quillfeather", 2022),
    )

    private val copperline = listOf(
        "Copperline", "Good Bones", "Paper Towns Burn Bright", "Ladder Song", "Keep the Engine Running", "Copperline (Reprise)",
    ).mapIndexed { i, t -> TrackInfo("t$i", t, i + 1, 1, 180_000 + i * 17_000, "/x/$i.mp3", artist = "Ivy Sunderholt") }

    init {
        // The "folder" is the cover file itself here.
        coverFileOf = { it }
        coverDispatcher = kotlinx.coroutines.Dispatchers.Unconfined
    }

    private fun shoot(name: String, content: @androidx.compose.runtime.Composable () -> Unit) {
        compose.setContent { MaterialTheme { AppScaffold { content() } } }
        // Covers decode on Dispatchers.IO; give them a moment, then let compose settle.
        repeat(40) {
            Thread.sleep(50)
            org.robolectric.shadows.ShadowLooper.idleMainLooper(16, java.util.concurrent.TimeUnit.MILLISECONDS)
            compose.mainClock.advanceTimeByFrame()
            compose.waitForIdle()
        }
        compose.onRoot().captureRoboImage("build/outputs/roborazzi/wear-$name.png")
    }

    @Test fun home() = shoot("1-home") { HomeScreen(copperline[2].copy(album = "Copperline"), onNowPlaying = {}, onAlbums = {}, onPlaylists = {}, onAlbumGroups = {}, onSync = {}, onSettings = {}) }

    @Test fun albums() = shoot("2-albums") { AlbumsScreen(albums, onAlbum = {}, onAlbumMenu = {}) }

    @Test fun album() = shoot("3-album") {
        AlbumScreen(albums[0], copperline, playingUid = "t2", onTrack = {}, onAlbumMenu = {}, onTrackMenu = {})
    }

    @Test fun player() = shoot("4-player") {
        PlayerScreen(
            "Paper Towns Burn Bright", "Ivy Sunderholt", isPlaying = true, hasPrevious = true, hasNext = true,
            positionMs = { 83_000 }, durationMs = 214_000,
            onPrevious = {}, onPlayPause = {}, onNext = {}, onSeek = {},
        )
    }
}
