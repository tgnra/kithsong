package ng.kithso.android.screenshots

import android.content.Context
import androidx.activity.ComponentActivity
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.onAllNodesWithText
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.test.performTextInput
import androidx.compose.ui.unit.dp
import androidx.core.graphics.Insets
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.test.core.app.ApplicationProvider
import coil3.SingletonImageLoader
import com.github.takahirom.roborazzi.captureRoboImage
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.sync.Syncer
import ng.kithso.android.ui.BackStack
import ng.kithso.android.ui.DetailScreen
import ng.kithso.android.ui.FullPlayer
import ng.kithso.android.ui.ImportPickers
import ng.kithso.android.ui.MainScreen
import ng.kithso.android.ui.MiniPlayer
import ng.kithso.android.ui.Page
import ng.kithso.android.ui.PageContent
import ng.kithso.android.ui.PlaylistActions
import ng.kithso.android.ui.SearchScreen
import ng.kithso.android.ui.TabletLayout
import ng.kithso.android.ui.TabletSection
import ng.kithso.android.ui.art.FileArt
import ng.kithso.android.ui.art.artImageLoader
import ng.kithso.android.ui.rememberQueueSlide
import ng.kithso.android.ui.rememberWindowShape
import ng.kithso.android.ui.theme.ArtTheme
import ng.kithso.android.ui.theme.KithsongTheme
import org.junit.After
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode
import java.awt.image.BufferedImage
import java.io.File
import javax.imageio.ImageIO

/**
 * The phone's and tablets' Play Store screenshots, drawn on the JVM by Robolectric from the
 * store's made-up library ([StoreLibrary]), at the listing's sizes: phone 1080x1920 at 420 dpi,
 * 7" tablet 1080x1920 at 288 dpi in portrait, 10" tablet 2560x1440 at 288 dpi in landscape. The
 * set is the listing's: albums, an album and the player in light and dark, and Search in light.
 *
 * The screens are put together here much as App puts them together: the main list, a page over
 * it and the mini player on the phone; the tablet layout itself (top bar, sidebar, content) on
 * tablets; the full player over either. App itself needs the core (settings, the perf marks), so
 * it isn't drawn whole. The status and navigation bars are given as insets, blank, as the
 * listing's shots have them painted out.
 *
 * Runs only with -PkithsongScreenshots; the PNGs go to build/outputs/store-screenshots/<size>,
 * named as in the listing, as 24-bit PNGs without alpha, as Play wants them.
 */
@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
abstract class StoreScreenshots(
    private val folder: String,
    private val width: Int,
    private val height: Int,
    private val tablet: Boolean,
    private val statusBarPx: Int,
    private val navigationBarPx: Int,
) {
    @get:Rule val compose = createAndroidComposeRule<ComponentActivity>()

    private val context: Context get() = ApplicationProvider.getApplicationContext()

    // For the tablet sidebar's Devices: a Syncer that never starts, over a core library source
    // that's never read. Neither can reach the core on the JVM; what they try fails quietly here.
    private val idle = CoroutineScope(SupervisorJob() + Dispatchers.Default + CoroutineExceptionHandler { _, _ -> })
    private val syncer: Syncer by lazy {
        val dir = File("build/tmp/store-screenshots/library").apply { mkdirs() }
        val core = CoreLibrarySource(dir, dir, dir, context.getSharedPreferences("store", 0), idle) { "" }
        Syncer(context, core, dir, "Tablet", idle)
    }

    @Before fun images() {
        SingletonImageLoader.setUnsafe(artImageLoader(context, FileArt()))
    }

    @After fun stop() = idle.cancel()

    // The light shots play Copperline's first track, the dark ones Nightjar Season's.
    private fun light() = StorePlayback(StoreLibrary.tracks.getValue(StoreLibrary.COPPERLINE), 0, 21_000)
    // The dark tests run with "+night" (@Config), set before the activity starts.
    private fun dark() = StorePlayback(StoreLibrary.tracks.getValue(StoreLibrary.NIGHTJAR), 0, 8_000)

    private fun album(uid: String) = CollectionRef(CollectionRef.Kind.Album, uid)

    @Test fun albumsLight() = albums("albums-light", light())
    @Test fun albumLight() = album("album-light", light(), StoreLibrary.COPPERLINE)
    @Test fun playerLight() = player("player-light", light())
    @Config(qualifiers = "+night")
    @Test fun albumsDark() = albums("albums-dark", dark())
    @Config(qualifiers = "+night")
    @Test fun playerDark() = player("player-dark", dark())
    @Config(qualifiers = "+night")
    @Test fun albumDark() = album("album-dark", dark(), StoreLibrary.NIGHTJAR)
    @Test fun searchLight() {
        val playback = light()
        shoot(numbered("search-light"), prepare = {
            compose.onNode(hasSetTextAction()).performTextInput("copper")
            // Past the search's wait for more typing.
            compose.mainClock.advanceTimeBy(1_000)
        }) {
            if (tablet) Tablet(playback, BackStack<Page>().push(Page.Search)) else Phone(playback, Page.Search)
        }
    }

    // On the phone the main list scrolls under the status bar, which is painted over, as the
    // listing's shots have it.
    private fun albums(name: String, playback: StorePlayback) = shoot(numbered(name), clearStatusBar = !tablet, prepare = {
        // The phone's main list scrolled down to its Albums, as the listing has it.
        if (!tablet) {
            val top = compose.onAllNodesWithText("Albums").onFirst().fetchSemanticsNode().boundsInRoot.top
            compose.onNode(SemanticsMatcher.keyIsDefined(SemanticsProperties.VerticalScrollAxisRange)).performSemanticsAction(SemanticsActions.ScrollBy) {
                it(0f, top - statusBarPx - 40f)
            }
        }
    }) {
        if (tablet) Tablet(playback, BackStack()) else Phone(playback, null)
    }

    private fun album(name: String, playback: StorePlayback, uid: String) = shoot(numbered(name)) {
        val page = Page.Collection(album(uid))
        if (tablet) Tablet(playback, BackStack<Page>().push(page)) else Phone(playback, page)
    }

    private fun player(name: String, playback: StorePlayback) = shoot(numbered(name)) {
        if (tablet) Tablet(playback, BackStack()) else Phone(playback, null, mini = false)
        val track = playback.track!!
        ArtTheme(track.art) {
            FullPlayer(
                playback, track, rememberQueueSlide(), queueBackEnabled = false, onCollapse = {}, onAddToPlaylist = {}, onMenu = {},
            )
        }
    }

    /** The listing's file name: its place in the listing's order, then [name]. */
    private fun numbered(name: String): String {
        val order = if (tablet) {
            listOf("album-light", "player-light", "albums-light", "album-dark", "player-dark", "albums-dark", "search-light")
        } else {
            listOf("albums-light", "album-light", "player-light", "albums-dark", "player-dark", "album-dark", "search-light")
        }
        return "${order.indexOf(name) + 1}-$name"
    }

    /** Each page as App makes it. */
    private fun pageContent(playback: StorePlayback): PageContent = { page, padding, onOpen, dismiss ->
        when (page) {
            is Page.Collection -> DetailScreen(
                page.ref, StoreLibrary, PlaylistActions(editor = null, playback = playback), padding,
                onPlay = { _, _ -> }, onPlayAll = { _, _ -> }, onOpen = onOpen, onBack = dismiss,
            )
            Page.Search -> SearchScreen(StoreLibrary, padding, onOpen = onOpen, onPlayTrack = {}, onTrackMenu = {}, onBack = dismiss)
            else -> Unit
        }
    }

    /** The phone layout: the main list, [page] over it, and the mini player unless not [mini]. */
    @Composable
    private fun Phone(playback: StorePlayback, page: Page?, mini: Boolean = true) {
        val track = playback.track!!
        var miniHeight by remember { mutableIntStateOf(0) }
        val navigationBar = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
        val bottomPadding = navigationBar + with(LocalDensity.current) { miniHeight.toDp() } + 16.dp
        Box(Modifier.fillMaxSize()) {
            MainScreen(
                StoreLibrary.library.value, bottomPadding, onOpen = {}, onSearch = {}, onSync = {},
                onSettings = {}, onStats = {}, importPickers = ImportPickers({}, {}), importRunning = false, banner = null,
            )
            if (page != null) pageContent(playback)(page, bottomPadding, {}, {})
            if (mini) ArtTheme(track.art) {
                MiniPlayer(
                    playback, track, onOpen = {}, onMenu = {},
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .windowInsetsPadding(WindowInsets.navigationBars.union(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)))
                        .padding(8.dp)
                        .widthIn(max = 640.dp)
                        .fillMaxWidth()
                        .onSizeChanged { miniHeight = it.height },
                )
            }
        }
    }

    /** The tablet layout, on its Albums page with [nav] open over it. */
    @Composable
    private fun Tablet(playback: StorePlayback, nav: BackStack<Page>) {
        TabletLayout(
            nav,
            updateNav = {},
            section = TabletSection.Albums,
            onSection = {},
            shape = rememberWindowShape(),
            library = StoreLibrary.library.value,
            source = StoreLibrary,
            playback = playback,
            syncer = syncer,
            actions = PlaylistActions(editor = null, playback = playback),
            importPickers = ImportPickers({}, {}),
            importRunning = false,
            binDot = false,
            banner = null,
            covered = false,
            onOpen = {},
            onOpenPlayer = {},
            onTrackMenu = {},
            pageContent = pageContent(playback),
        )
    }

    private fun shoot(name: String, clearStatusBar: Boolean = false, prepare: () -> Unit = {}, content: @Composable () -> Unit) {
        compose.setContent {
            KithsongTheme { Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) { content() } }
        }
        systemBars()
        settle()
        prepare()
        settle()
        val raw = File("build/tmp/store-screenshots/$folder/$name.png")
        compose.onRoot().captureRoboImage(raw.path)
        crop(raw, File(outDir, "$folder/$name.png"), clearStatusBar)
    }

    /** The status and navigation bars as insets, as edge to edge on a device. */
    private fun systemBars() = compose.runOnUiThread {
        val window = compose.activity.window
        WindowCompat.setDecorFitsSystemWindows(window, false)
        val insets = WindowInsetsCompat.Builder()
            .setInsets(WindowInsetsCompat.Type.statusBars(), Insets.of(0, statusBarPx, 0, 0))
            .setInsets(WindowInsetsCompat.Type.navigationBars(), Insets.of(0, 0, 0, navigationBarPx))
            .build()
        ViewCompat.dispatchApplyWindowInsets(window.decorView, insets)
    }

    // Art decodes and its colours are found off the main thread: give them time, then let
    // Compose settle.
    private fun settle() = repeat(30) {
        Thread.sleep(50)
        compose.mainClock.advanceTimeByFrame()
        compose.waitForIdle()
    }

    /**
     * [raw] cut to the listing's size, as a 24-bit PNG in [out]. Robolectric's window is a dp
     * wide, so it can come out a pixel or two over; anything short of the size is an error.
     * With [clearStatusBar], what scrolled under the status bar is painted over with the row
     * below it, as the listing's shots were.
     */
    private fun crop(raw: File, out: File, clearStatusBar: Boolean) {
        val shot = ImageIO.read(raw)
        check(shot.width >= width && shot.height >= height && shot.width - width <= 3 && shot.height - height <= 3) {
            "${raw.name} is ${shot.width}x${shot.height}, not about ${width}x$height"
        }
        val rgb = BufferedImage(width, height, BufferedImage.TYPE_INT_RGB)
        rgb.createGraphics().apply {
            drawImage(shot, 0, 0, null)
            dispose()
        }
        if (clearStatusBar) {
            val clean = rgb.getRGB(0, statusBarPx + 2, width, 1, null, 0, width)
            for (y in 0 until statusBarPx) rgb.setRGB(0, y, width, 1, clean, 0, width)
        }
        out.parentFile.mkdirs()
        check(ImageIO.write(rgb, "png", out))
    }

    private companion object {
        val outDir = File(System.getProperty("kithsong.screenshots.out") ?: "build/outputs/store-screenshots")
    }
}

// 1080 / 2.625 isn't a whole number of dp, so the window is a dp over and cropped.
@Config(sdk = [36], application = android.app.Application::class, instrumentedPackages = ["ng.kithso.core"], qualifiers = "w412dp-h732dp-port-420dpi")
class PhoneScreenshotTest : StoreScreenshots("phone", 1080, 1920, tablet = false, statusBarPx = 137, navigationBarPx = 63)

@Config(sdk = [36], application = android.app.Application::class, instrumentedPackages = ["ng.kithso.core"], qualifiers = "w600dp-h1067dp-port-288dpi")
class Tablet7ScreenshotTest : StoreScreenshots("tablet-7in", 1080, 1920, tablet = true, statusBarPx = 54, navigationBarPx = 43)

@Config(sdk = [36], application = android.app.Application::class, instrumentedPackages = ["ng.kithso.core"], qualifiers = "w1423dp-h800dp-land-288dpi")
class Tablet10ScreenshotTest : StoreScreenshots("tablet-10in", 2560, 1440, tablet = true, statusBarPx = 54, navigationBarPx = 43)
