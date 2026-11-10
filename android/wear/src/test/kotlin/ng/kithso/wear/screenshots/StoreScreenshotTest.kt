package ng.kithso.wear.screenshots

import androidx.compose.runtime.Composable
import androidx.compose.ui.test.junit4.createComposeRule
import androidx.compose.ui.test.hasScrollToIndexAction
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.semantics.SemanticsActions
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.TimeSource
import androidx.wear.compose.material3.TimeText
import com.github.takahirom.roborazzi.captureRoboImage
import kotlinx.coroutines.Dispatchers
import ng.kithso.core.AlbumInfo
import ng.kithso.core.ConfirmState
import ng.kithso.core.TrackInfo
import ng.kithso.wear.FileProgress
import ng.kithso.wear.SyncState
import ng.kithso.wear.ui.AlbumScreen
import ng.kithso.wear.ui.AlbumsScreen
import ng.kithso.wear.ui.HomeScreen
import ng.kithso.wear.ui.PlayerScreen
import ng.kithso.wear.ui.SyncScreen
import ng.kithso.wear.ui.coverDispatcher
import ng.kithso.wear.ui.coverFileOf
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import org.robolectric.shadows.ShadowLooper
import java.awt.AlphaComposite
import java.awt.Color
import java.awt.RenderingHints
import java.awt.geom.Ellipse2D
import java.awt.image.BufferedImage
import java.io.File
import java.util.concurrent.TimeUnit
import javax.imageio.ImageIO

/**
 * The watch's Play Store screenshots, drawn on the JVM by Robolectric at Wear OS small round
 * (384x384) from the store's made-up library: the same six albums as the phone's shots and the
 * listing's demo zip (albums.py in the listing's scripts). Robolectric draws a square screen,
 * so each shot is masked to the round display afterwards, black outside it as on a watch, and
 * saved without alpha, as Play wants. The clock always says 12:45.
 *
 * Runs only with -PkithsongScreenshots; the PNGs go to build/outputs/store-screenshots/wear,
 * named as in the listing; 6-sync is new, not in the listing yet.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
@Config(sdk = [36], qualifiers = "w192dp-h192dp-round-watch-xhdpi", application = android.app.Application::class)
class StoreScreenshotTest {
    @get:Rule val compose = createComposeRule()

    private val covers: File = File(javaClass.getResource("/store-demo/0.jpg")!!.toURI()).parentFile

    private fun album(i: Int, title: String, artist: String, year: Int) =
        AlbumInfo("a$i", title, artist, year, isSingle = false, hasArt = true, folder = File(covers, "$i.jpg").path)

    // Sorted by title, as the watch lists them.
    private val albums = listOf(
        album(5, "Copperline", "Ivy Sunderholt", 2024),
        album(4, "Field Notes for Late Trains", "Kestrel Bay Ensemble", 2020),
        album(1, "Low Country Radio", "Oona Vesper", 2023),
        album(2, "Nightjar Season", "Pale Orchard", 2019),
        album(0, "Salt on the Sill", "Marrowfield Choir", 2021),
        album(3, "Small Hours, Big Rooms", "Tomas Quillfeather", 2022),
    )

    private fun tracks(artist: String, titles: List<String>, seconds: List<Int>) = titles.mapIndexed { i, t ->
        TrackInfo("$artist-$i", t, i + 1, 1, seconds[i] * 1000, "/x/$i.mp3", artist = artist)
    }

    private val copperline = tracks(
        "Ivy Sunderholt",
        listOf("Copperline", "Good Bones", "Paper Towns Burn Bright", "Ladder Song", "Keep the Engine Running", "Copperline (Reprise)"),
        listOf(228, 264, 224, 194, 210, 224),
    )
    private val nightjar = tracks(
        "Pale Orchard",
        listOf("Nightjar Season", "Windfall", "Soft Machinery", "Pollen Count", "Blue Hour Bicycle"),
        listOf(179, 163, 249, 204, 290),
    )

    init {
        // The "folder" is the cover file itself here, and covers decode in place, so every one
        // is there when the shot is taken.
        coverFileOf = { it }
        coverDispatcher = Dispatchers.Unconfined
    }

    private fun shoot(name: String, prepare: () -> Unit = {}, content: @Composable () -> Unit) {
        compose.setContent {
            MaterialTheme { AppScaffold(timeText = { TimeText(timeSource = FixedTime) }) { content() } }
        }
        settle()
        prepare()
        settle()
        val raw = File("build/tmp/store-screenshots/wear/$name.png")
        compose.onRoot().captureRoboImage(raw.path)
        roundOff(raw, File(outDir, "$name.png"))
    }

    private fun settle() = repeat(40) {
        ShadowLooper.idleMainLooper(16, TimeUnit.MILLISECONDS)
        compose.mainClock.advanceTimeByFrame()
        compose.waitForIdle()
    }

    @Test fun player() = shoot("1-player") {
        PlayerScreen(
            "Copperline", "Ivy Sunderholt", isPlaying = true, hasPrevious = true, hasNext = true,
            positionMs = { 35_000 }, durationMs = 228_000,
            onPrevious = {}, onPlayPause = {}, onNext = {}, onSeek = {},
        )
    }

    @Test fun albums() = shoot("2-albums") { AlbumsScreen(albums, onAlbum = {}, onAlbumMenu = {}) }

    @Test fun album() = shoot("3-album") {
        AlbumScreen(albums[0], copperline, playingUid = null, onTrack = {}, onAlbumMenu = {}, onTrackMenu = {})
    }

    // An album scrolled down to its tracks.
    @Test fun tracks() = shoot("4-tracks", prepare = {
        compose.onNode(hasScrollToIndexAction()).performSemanticsAction(SemanticsActions.ScrollBy) { it(0f, 170f) }
    }) {
        AlbumScreen(albums[3], nightjar, playingUid = null, onTrack = {}, onAlbumMenu = {}, onTrackMenu = {})
    }

    @Test fun home() = shoot("5-home") {
        HomeScreen(null, onNowPlaying = {}, onAlbums = {}, onPlaylists = {}, onAlbumGroups = {}, onSync = {}, onSettings = {})
    }

    // Sync part way: files coming in from the group, as the watch's Sync screen shows them.
    @Test fun sync() = shoot("6-sync") {
        SyncScreen(
            SyncState.Paired(
                devices = listOf("Bob's laptop", "Bob's phone"),
                confirm = ConfirmState.Confirmed,
                synced = 38,
                wanted = 62,
                hasStatus = true,
                progress = FileProgress(24, 62, "1.4 MB/s · about 3 min left"),
            ),
            onDismissRefusal = {},
        )
    }

    private object FixedTime : TimeSource {
        @Composable override fun currentTime() = "12:45"
    }

    private companion object {
        val outDir = File(System.getProperty("kithsong.screenshots.out") ?: "build/outputs/store-screenshots", "wear")

        /** [raw] masked to the round screen, black outside, as a 24-bit PNG in [out]. */
        fun roundOff(raw: File, out: File) {
            val shot = ImageIO.read(raw)
            check(shot.width == 384 && shot.height == 384) { "${raw.name} is ${shot.width}x${shot.height}, not 384x384" }
            // The circle as an antialiased alpha mask, the shot drawn through it, then onto black.
            val masked = BufferedImage(shot.width, shot.height, BufferedImage.TYPE_INT_ARGB)
            masked.createGraphics().apply {
                setRenderingHint(RenderingHints.KEY_ANTIALIASING, RenderingHints.VALUE_ANTIALIAS_ON)
                color = Color.WHITE
                fill(Ellipse2D.Double(0.0, 0.0, shot.width.toDouble(), shot.height.toDouble()))
                composite = AlphaComposite.SrcIn
                drawImage(shot, 0, 0, null)
                dispose()
            }
            val rgb = BufferedImage(shot.width, shot.height, BufferedImage.TYPE_INT_RGB)
            rgb.createGraphics().apply {
                color = Color.BLACK
                fillRect(0, 0, shot.width, shot.height)
                drawImage(masked, 0, 0, null)
                dispose()
            }
            out.parentFile.mkdirs()
            check(ImageIO.write(rgb, "png", out))
        }
    }
}
