package ng.kithso.android

import ng.kithso.core.*
import android.Manifest
import android.app.SearchManager
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.net.Uri
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import android.view.KeyEvent
import android.view.KeyboardShortcutGroup
import android.view.Menu
import android.util.Log
import android.widget.Toast
import android.window.OnBackInvokedDispatcher
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.CompositionLocalProvider
import androidx.core.content.IntentCompat
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import ng.kithso.android.importer.ImportSource
import ng.kithso.android.ui.App
import ng.kithso.android.ui.KeyboardShortcuts
import ng.kithso.android.ui.LocalShortcuts
import ng.kithso.android.ui.theme.KithsongTheme
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

class MainActivity : ComponentActivity() {
    private val askForNotifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) {}

    // The keyboard shortcuts (ui/Shortcuts.kt), which App says what to do for.
    private val shortcuts = KeyboardShortcuts()

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.BAKLAVA) {
            onBackInvokedDispatcher.registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_SYSTEM_NAVIGATION_OBSERVER,
                ::afterSystemBack,
            )
        }

        val app = application as KithsongApp
        CoreTesting.mark("activity_created")
        window.decorView.display?.refreshRate?.let { CoreTesting.setRefreshHz(it.toDouble()) }
        // `adb shell am start -n ng.kithso.android/.MainActivity --ez fake true` shows made-up
        // music instead of the library, and `--ez fake_empty true` the empty state. Only on a
        // fresh start, so turning the phone doesn't start the fake over.
        val empty = intent.getBooleanExtra("fake_empty", false)
        if (savedInstanceState == null && (empty || intent.getBooleanExtra("fake", false))) {
            app.useFakeLibrary(empty)
        }
        // Debug builds, for test scripts: `--ei stress_preset 1` turns the profiler on and generates
        // and opens that stress library, and `--ez perf_run true` runs the scroll test after
        // startup. Every test writes its report to the log (tag kithsong-perf).
        if (applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0 && savedInstanceState == null) {
            if (intent.hasExtra("stress_preset")) {
                app.testing.setProfiling(true)
                app.testing.generate(intent.getIntExtra("stress_preset", CoreTesting.PRESET_10K))
            }
            if (intent.getBooleanExtra("perf_run", false)) ng.kithso.android.ui.PerfRun.requested = true
            // `--ez store_demo true` makes and opens the demo library (kithcore/demo.h), six
            // made-up albums for store screenshots, restarting the app into it; `false` goes
            // back to the real library.
            if (intent.hasExtra("store_demo")) app.testing.storeDemo(intent.getBooleanExtra("store_demo", true))
        }
        // Debug builds: `--es fake_merge <scenario>` shows one of the merge's screens with
        // made-up data (see Syncer.debugShowMerge).
        val debuggable = applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0
        val fakeMerge = intent.getStringExtra("fake_merge")
        if (fakeMerge != null && debuggable) app.syncer.debugShowMerge(fakeMerge)
        // Debug builds, for test scripts: `--es debug_pair <host>:<port>` asks that device to
        // pair once sync runs, as Add device by address does (the desktops' KITHSONG_DEBUG_PAIR).
        val debugPair = intent.getStringExtra("debug_pair")?.split(':')
        if (debuggable && savedInstanceState == null && debugPair?.size == 2) {
            val port = debugPair[1].toIntOrNull() ?: 0
            MainScope().launch {
                withTimeoutOrNull(10_000) { app.syncer.status.first { it.running } }
                app.syncer.pair(debugPair[0], port)
            }
        }
        Log.i(TAG, "kithcore ${NativeCore.version()}")
        // A zip or audio files opened with or shared to the app: import them. Only on a fresh
        // start, so turning the phone doesn't import them again.
        if (savedInstanceState == null) {
            importShared(intent)
            playSearch(intent)
        }

        setContent {
            KithsongTheme {
                CompositionLocalProvider(LocalShortcuts provides shortcuts) {
                    App(
                        app.library,
                        app.playback,
                        app.syncer,
                        app.importer,
                        app.settings,
                        app.libraryDir,
                        onPlay = ::askForNotificationsOnce,
                        onImportStarted = ::askForNotificationsOnce,
                        onLeave = if (systemBack) null else ::leave,
                        shortcuts = shortcuts,
                    )
                }
            }
        }
    }

    // From Android 16 an app can hear of a back it leaves to the system, so back on the main list
    // needs no handler of the app's, and the system's preview of going home can play.
    private val systemBack = Build.VERSION.SDK_INT >= Build.VERSION_CODES.BAKLAVA

    // Back from the main list. While music plays the app only goes out of sight, and playback
    // carries on; with nothing playing it quits, leaving no player behind: the notification of
    // a paused track goes too, and PlaybackService keeps its place for next time. Opened from
    // another app (a share, say) the activity isn't its task's root, and goes back there.
    private fun leave() {
        val playback = (application as KithsongApp).playback
        // A paused cast is left to end itself after a while (the cast package's endAfterPaused).
        val playing = playback.isPlaying || playback.cast.casting
        if (!playing) playback.stop()
        when {
            !isTaskRoot -> finish()
            playing -> moveTaskToBack(false)
            else -> finishAndRemoveTask()
        }
    }

    // [leave] when the system has done the going: out of sight, or back to the other app.
    private fun afterSystemBack() {
        val playback = (application as KithsongApp).playback
        if (playback.isPlaying) return
        playback.stop()
        if (isTaskRoot) finishAndRemoveTask()
    }

    // Every key, for the shortcuts: most before the focused element sees it, Space after, if
    // nothing used it. Whether a text field has the focus is whether the window is taking text.
    override fun dispatchKeyEvent(event: KeyEvent): Boolean {
        val text = window.decorView.findFocus()?.onCheckIsTextEditor() == true
        return shortcuts.beforeFocused(event, text) || super.dispatchKeyEvent(event) || shortcuts.afterFocused(event, text)
    }

    // The shortcuts in the system's list of them, which Meta+/ shows (Ctrl+? too, as on the desktops).
    override fun onProvideKeyboardShortcuts(data: MutableList<KeyboardShortcutGroup>, menu: Menu?, deviceId: Int) {
        super.onProvideKeyboardShortcuts(data, menu, deviceId)
        data.addAll(shortcuts.groups(this))
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        importShared(intent)
        playSearch(intent)
    }

    // A voice assistant asking for something to be played.
    private fun playSearch(intent: Intent?) {
        if (intent?.action != MediaStore.INTENT_ACTION_MEDIA_PLAY_FROM_SEARCH) return
        (application as KithsongApp).playback.playSearch(intent.getStringExtra(SearchManager.QUERY).orEmpty())
    }

    // ACTION_VIEW of a zip ("Open with"), ACTION_SEND of a zip or audio file, ACTION_SEND_MULTIPLE
    // of audio files (the share sheet). The intent's grant covers reading them; the importer
    // copies them in straight away. While an import runs they're added to the end of it.
    private fun importShared(intent: Intent?) {
        val uris: List<Uri> = when (intent?.action) {
            Intent.ACTION_VIEW -> listOfNotNull(intent.data)
            Intent.ACTION_SEND -> listOfNotNull(IntentCompat.getParcelableExtra(intent, Intent.EXTRA_STREAM, Uri::class.java))
            Intent.ACTION_SEND_MULTIPLE ->
                IntentCompat.getParcelableArrayListExtra(intent, Intent.EXTRA_STREAM, Uri::class.java).orEmpty()
            else -> emptyList()
        }
        if (uris.isEmpty()) return
        val importer = (application as KithsongApp).importer
        if (importer.start(ImportSource.Documents(uris))) {
            askForNotificationsOnce()
        } else {
            importer.add(ImportSource.Documents(uris))
            val added = resources.getQuantityString(R.plurals.import_added, uris.size, uris.size)
            Toast.makeText(this, added, Toast.LENGTH_SHORT).show()
        }
    }

    // From Android 13 notifications need permission, asked for once, unprompted, on the first
    // play or import: that's when there is one to show. Settings can ask again. The playback controls
    // show without it, being a media session's, but not in the list of notifications.
    private fun askForNotificationsOnce() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return
        if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) == PackageManager.PERMISSION_GRANTED) return
        val settings = (application as KithsongApp).settings
        if (settings.askedForNotifications) return
        settings.askedForNotifications = true
        askForNotifications.launch(Manifest.permission.POST_NOTIFICATIONS)
    }

    private companion object {
        const val TAG = "kithsong"
    }
}
