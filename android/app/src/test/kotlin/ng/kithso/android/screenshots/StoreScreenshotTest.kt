package ng.kithso.android.screenshots

import android.content.Context
import androidx.activity.ComponentActivity
import androidx.annotation.StringRes
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.ui.semantics.SemanticsActions
import androidx.compose.ui.semantics.SemanticsProperties
import androidx.compose.ui.semantics.getOrNull
import androidx.compose.ui.test.SemanticsMatcher
import androidx.compose.ui.test.hasAnyDescendant
import androidx.compose.ui.test.hasClickAction
import androidx.compose.ui.test.hasContentDescription
import androidx.compose.ui.test.hasScrollToKeyAction
import androidx.compose.ui.test.hasSetTextAction
import androidx.compose.ui.test.hasText
import androidx.compose.ui.test.junit4.createAndroidComposeRule
import androidx.compose.ui.test.onFirst
import androidx.compose.ui.test.onRoot
import androidx.compose.ui.test.performScrollToKey
import androidx.compose.ui.test.performSemanticsAction
import androidx.compose.ui.test.performTextInput
import androidx.core.graphics.Insets
import androidx.core.view.ViewCompat
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.test.core.app.ApplicationProvider
import coil3.SingletonImageLoader
import coil3.annotation.DelicateCoilApi
import com.github.takahirom.roborazzi.captureRoboImage
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import ng.kithso.android.AppSettings
import ng.kithso.android.KithsongApp
import ng.kithso.android.NewLibrary
import ng.kithso.android.R
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.importer.ImportProgress
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.ui.App
import ng.kithso.android.ui.LocalNow
import ng.kithso.android.ui.art.FileArt
import ng.kithso.android.ui.art.artImageLoader
import ng.kithso.android.ui.theme.KithsongTheme
import ng.kithso.core.CoreSettings
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
import java.util.TimeZone
import javax.imageio.ImageIO

/**
 * The phone's and tablets' Play Store screenshots, drawn on the JVM by Robolectric from the
 * store's made-up library ([StoreLibrary]), at the listing's sizes: phone 1080x1920 at 420 dpi,
 * 7" tablet 1080x1920 at 288 dpi in portrait, 10" tablet 2560x1440 at 288 dpi in landscape. The
 * set is the listing's (albums, an album and the player in light and dark, and Search in
 * light), then an import part way with an album waiting for a decision between candidates, and
 * a sync part way with Desk, as the Sync page lists it and as this device's page shows it, with
 * its selection and budget.
 *
 * Each is the real [App], the phone's layout or the tablet layout as the window's size picks, so
 * the header, the mini player or top bar and their seekbars are there as on a device. Its pages
 * are opened by tapping, as App's navigation is its own. The native core is made inert
 * (instrumentedPackages); the library, the player, the import and sync are made up behind the
 * real classes ([StoreLibrary], [StorePlayback], [StoreImporter], [StoreSyncer]). The status and
 * navigation bars are given as insets, blank, as the listing's shots have them painted out.
 *
 * Runs only with -PkithsongScreenshots; the PNGs go to build/outputs/store-screenshots/<size>,
 * named in the listing's order, as 24-bit PNGs without alpha, as Play wants them.
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

    // What App's parts start in the background: none of it can reach the core on the JVM, and
    // what they try fails quietly here.
    private val idle = CoroutineScope(SupervisorJob() + Dispatchers.Default + CoroutineExceptionHandler { _, _ -> })
    private val dir = File("build/tmp/store-screenshots/library").apply { mkdirs() }
    private val core by lazy { CoreLibrarySource(dir, dir, dir, context.getSharedPreferences("store", 0), idle) { "" } }
    private val settings by lazy { AppSettings(CoreSettings(dir.path), context.getSharedPreferences("store-app", 0)) }

    // What another device calls this one.
    private val deviceName = if (tablet) "Tablet" else "Phone"

    // The zone the shots' dates and times are written in, wherever they're run.
    private val zone = TimeZone.getDefault()

    @OptIn(DelicateCoilApi::class)
    @Before fun images() {
        SingletonImageLoader.setUnsafe(artImageLoader(context, FileArt()))
        TimeZone.setDefault(TimeZone.getTimeZone("UTC"))
    }

    @After fun stop() {
        idle.cancel()
        TimeZone.setDefault(zone)
    }

    // The light shots play Copperline's first track, the dark ones Nightjar Season's.
    private fun light() = StorePlayback(StoreLibrary.tracks.getValue(StoreLibrary.COPPERLINE), 0, 21_000)
    // The dark tests run with "+night" (@Config), set before the activity starts.
    private fun dark() = StorePlayback(StoreLibrary.tracks.getValue(StoreLibrary.NIGHTJAR), 0, 8_000)

    // The light one with the phone's header (the wordmark and the menu) at the top, the dark
    // one scrolled down to the albums, as the listing has it.
    @Test fun albumsLight() = albums("albums-light", light(), scrolled = false)
    @Test fun albumLight() = album("album-light", light(), "Copperline")
    @Test fun playerLight() = player("player-light", light())
    @Config(qualifiers = "+night")
    @Test fun albumsDark() = albums("albums-dark", dark())
    @Config(qualifiers = "+night")
    @Test fun playerDark() = player("player-dark", dark())
    @Config(qualifiers = "+night")
    @Test fun albumDark() = album("album-dark", dark(), "Nightjar Season")

    @Test fun searchLight() = shoot("search-light", light()) {
        if (tablet) tap(hasText(string(R.string.search)) and hasClickAction()) else menu(R.string.search)
        settle()
        compose.onNode(hasSetTextAction()).performTextInput("copper")
        // Past the search's wait for more typing.
        compose.mainClock.advanceTimeBy(1_000)
    }

    // The import's card on the main list, its Review, then the album waiting for a decision. The
    // phone's page is too short for its header, Search again and a candidate with the mini player
    // under them: it has nothing playing, and starts at the album's name, which goes under the
    // status bar. The tablets' show all of it as it opens.
    @Test fun importLight() = shoot("import-light", if (tablet) light() else StorePlayback.NOTHING, import = StoreImport.progress) {
        tap(hasText(string(R.string.import_review_open)) and hasClickAction())
        settle()
        tap(hasText(StoreImport.details.name) and hasClickAction())
        settle()
        if (!tablet) {
            val title = compose.onAllNodes(hasText(StoreImport.details.name)).onFirst().fetchSemanticsNode().boundsInRoot.top
            scrollBy(title - statusBarPx - 8f)
        }
    }

    // The Sync page, part way through a session with Desk. The tablet layout has none: its
    // sidebar lists the devices, and Desk's page is shown, with the session.
    @Config(qualifiers = "+night")
    @Test fun syncDark() = shoot("sync-dark", dark(), sync = StoreSync.syncing(deviceName)) {
        if (tablet) tap(hasText("Desk") and hasClickAction()) else menu(R.string.sync)
        // On the 10" held sideways the sidebar's Recents and Devices don't all fit, and scroll (see
        // Sidebar): to its end, so the group shows whole.
        if (wide) {
            settle()
            compose.onAllNodes(hasScrollToKeyAction() and hasAnyDescendant(hasText(string(R.string.section_recents))))
                .onFirst().performScrollToKey("member:watch")
        }
    }

    // This device's page: the session, its files, and its selection within a budget.
    @Test fun deviceLight() = shoot("device-light", light(), sync = StoreSync.syncing(deviceName)) {
        if (tablet) {
            tap(hasText(deviceName) and hasClickAction())
        } else {
            menu(R.string.sync)
            settle()
            tap(hasText(deviceName) and hasClickAction())
        }
        settle()
        // Its profile, with the session over it (on the 10" only the group's syncing: the Desk shot
        // has the session). The tablet layout's page has the group's Sync
        // now and Leave group between them as well.
        scrollTo("profile")
        scrollBy(if (wide) -360f else if (tablet) -598f else -580f)
    }

    // On the phone the main list scrolls under the status bar, which is painted over, as the
    // listing's shots have it.
    private fun albums(name: String, playback: StorePlayback, scrolled: Boolean = true) = shoot(name, playback) {
        if (!tablet && scrolled) {
            val top = compose.onAllNodes(hasText(string(R.string.section_albums))).onFirst().fetchSemanticsNode().boundsInRoot.top
            scrollBy(top - statusBarPx - 40f)
        }
    }

    private fun album(name: String, playback: StorePlayback, title: String) = shoot(name, playback) {
        settle()
        tap(hasContentDescription(title, substring = true) and hasClickAction())
    }

    private fun player(name: String, playback: StorePlayback) = shoot(name, playback) {
        tap(SemanticsMatcher("opens the player") { node ->
            node.config.getOrNull(SemanticsActions.OnClick)?.label == string(R.string.open_player)
        })
    }

    // Whether pages have room for two panes (the import item's TwoPaneWidth): only the 10".
    private val wide get() = width > 2000

    private fun string(@StringRes id: Int) = context.getString(id)

    private fun tap(matcher: SemanticsMatcher) {
        compose.onAllNodes(matcher, useUnmergedTree = false).onFirst().performSemanticsAction(SemanticsActions.OnClick)
    }

    // The phone's main menu, then [item] in it.
    private fun menu(@StringRes item: Int) {
        tap(hasContentDescription(string(R.string.main_menu)) and hasClickAction())
        settle()
        tap(hasText(string(item)) and hasClickAction())
    }

    // The topmost page's list scrolled down by [px].
    private fun scrollBy(px: Float) {
        val vertical = SemanticsMatcher.keyIsDefined(SemanticsProperties.VerticalScrollAxisRange)
        val lists = compose.onAllNodes(vertical).fetchSemanticsNodes().size
        compose.onAllNodes(vertical)[lists - 1].performSemanticsAction(SemanticsActions.ScrollBy) { it(0f, px) }
    }

    // The topmost page's lazy list scrolled to the item with [key] at its top.
    private fun scrollTo(key: String) {
        val lists = compose.onAllNodes(hasScrollToKeyAction()).fetchSemanticsNodes().size
        compose.onAllNodes(hasScrollToKeyAction())[lists - 1].performScrollToKey(key)
    }

    /**
     * The listing's file name: its place in the listing's order, then [name]. Play takes up to
     * eight per kind of device, so the listing has to leave some of these out.
     */
    private fun numbered(name: String): String {
        val order = if (tablet) {
            listOf("album-light", "player-light", "albums-light", "album-dark", "player-dark", "albums-dark", "search-light")
        } else {
            listOf("albums-light", "album-light", "player-light", "albums-dark", "player-dark", "album-dark", "search-light")
        } + listOf("import-light", "sync-dark", "device-light")
        return "${order.indexOf(name) + 1}-$name"
    }

    /**
     * App with [playback] playing, [import] under way and [sync] as sync has it, then [prepare]
     * (opening a page, say), as the shot [name].
     */
    private fun shoot(
        name: String,
        playback: StorePlayback,
        import: ImportProgress? = null,
        sync: SyncStatus = StoreSync.alone(deviceName),
        prepare: () -> Unit = {},
    ) {
        val importer = StoreImporter(context, core, idle, import)
        val syncer = StoreSyncer(context, core, dir, idle, sync)
        // Its export and restart are never asked for here; what it starts in the background
        // (deleting set-aside libraries) fails quietly.
        val newLibrary = NewLibrary(KithsongApp(), settings, idle)
        compose.setContent {
            KithsongTheme {
                CompositionLocalProvider(LocalNow provides { StoreSync.NOW }) {
                    App(StoreLibrary, playback, syncer, importer, settings, dir, newLibrary, stress = false, demo = true)
                }
            }
        }
        systemBars()
        settle()
        prepare()
        settle()
        val file = numbered(name)
        val raw = File("build/tmp/store-screenshots/$folder/$file.png")
        compose.onRoot().captureRoboImage(raw.path)
        crop(raw, File(outDir, "$folder/$file.png"), clearStatusBar = !tablet)
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
        out.parentFile!!.mkdirs()
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
