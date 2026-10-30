package ng.kithso.android

import ng.kithso.core.*
import android.Manifest
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.net.Uri
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.IntentCompat
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import ng.kithso.android.importer.ImportSource
import ng.kithso.android.ui.App
import ng.kithso.android.ui.theme.KithsongTheme
import kotlinx.coroutines.MainScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull

class MainActivity : ComponentActivity() {
    private val askForNotifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val app = application as KithsongApp
        // `adb shell am start -n ng.kithso.android/.MainActivity --ez fake true` shows made-up
        // music instead of the library, and `--ez fake_empty true` the empty state. Only on a
        // fresh start, so turning the phone doesn't start the fake over.
        val empty = intent.getBooleanExtra("fake_empty", false)
        if (savedInstanceState == null && (empty || intent.getBooleanExtra("fake", false))) {
            app.useFakeLibrary(empty)
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
        Log.i(TAG, "musiccore ${NativeCore.version()}")
        // A zip or audio files opened with or shared to the app: import them. Only on a fresh
        // start, so turning the phone doesn't import them again.
        if (savedInstanceState == null) importShared(intent)

        setContent {
            KithsongTheme {
                App(
                    app.library,
                    app.playback,
                    app.syncer,
                    app.importer,
                    app.settings,
                    app.libraryDir,
                    onPlay = ::askForNotificationsOnce,
                    onImportStarted = ::askForNotificationsOnce,
                )
            }
        }
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        importShared(intent)
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
            importer.add(uris)
            val added = resources.getQuantityString(R.plurals.import_added, uris.size, uris.size)
            Toast.makeText(this, added, Toast.LENGTH_SHORT).show()
        }
    }

    override fun onStart() {
        super.onStart()
        (application as KithsongApp).isVisible = true
    }

    override fun onStop() {
        (application as KithsongApp).isVisible = false
        super.onStop()
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
