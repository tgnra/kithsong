package app.kithsong

import android.Manifest
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.splashscreen.SplashScreen.Companion.installSplashScreen
import app.kithsong.ui.App
import app.kithsong.ui.theme.KithsongTheme

class MainActivity : ComponentActivity() {
    private val askForNotifications = registerForActivityResult(ActivityResultContracts.RequestPermission()) {}

    override fun onCreate(savedInstanceState: Bundle?) {
        installSplashScreen()
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()

        val app = application as KithsongApp
        // `adb shell am start -n app.kithsong/.MainActivity --ez fake true` shows made-up
        // music instead of the library, and `--ez fake_empty true` the empty state. Only on a
        // fresh start, so turning the phone doesn't start the fake over.
        val empty = intent.getBooleanExtra("fake_empty", false)
        if (savedInstanceState == null && (empty || intent.getBooleanExtra("fake", false))) {
            app.useFakeLibrary(empty)
        }
        Log.i(TAG, "musiccore ${NativeCore.version()}")

        setContent {
            KithsongTheme {
                App(
                    app.library,
                    app.playback,
                    app.syncer,
                    app.settings,
                    app.libraryDir,
                    onPlay = ::askForNotificationsOnce,
                )
            }
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
    // play: that's when there is one to show. Settings can ask again. The playback controls
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
