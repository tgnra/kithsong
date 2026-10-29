package ng.kithso.wear

import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.util.Log
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.navigation.SwipeDismissableNavHost
import androidx.wear.compose.navigation.composable
import androidx.wear.compose.navigation.rememberSwipeDismissableNavController
import ng.kithso.core.AlbumInfo
import ng.kithso.core.MusicCoreException
import ng.kithso.core.TrackInfo
import ng.kithso.wear.ui.AlbumScreen
import ng.kithso.wear.ui.AlbumsScreen
import ng.kithso.wear.ui.HomeScreen
import ng.kithso.wear.ui.LicenseTextScreen
import ng.kithso.wear.ui.LicensesScreen
import ng.kithso.wear.ui.PlayerScreen
import ng.kithso.wear.ui.SettingsScreen
import ng.kithso.wear.ui.SyncScreen

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            MaterialTheme {
                WearApp()
            }
        }
    }
}

@Composable
private fun WearApp() {
    val navController = rememberSwipeDismissableNavController()
    val context = LocalContext.current
    val app = context.applicationContext as KithsongApp
    val player = app.player
    // Only albums with files on the watch; the library lists everything it knows about.
    var albums by remember { mutableStateOf(emptyList<AlbumInfo>()) }
    LaunchedEffect(Unit) {
        albums = libraryCall(emptyList()) { app.library.albums().filter { it.folder != null } }
    }
    val sync = remember {
        val name = Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
        SampleSync(name ?: Build.MODEL, libraryCall(0) { app.library.trackCount.toInt() })
    }

    // AppScaffold shows the time at the top of every screen.
    AppScaffold {
        SwipeDismissableNavHost(navController = navController, startDestination = "home") {
            composable("home") {
                HomeScreen(
                    onAlbums = { navController.navigate("albums") },
                    onSync = { navController.navigate("sync") },
                    onSettings = { navController.navigate("settings") },
                )
            }
            composable("albums") {
                AlbumsScreen(albums) { navController.navigate("album/${it.uid}") }
            }
            composable("album/{id}") { entry ->
                val uid = entry.arguments?.getString("id")
                val album = albums.firstOrNull { it.uid == uid } ?: return@composable
                val tracks = remember(uid) {
                    libraryCall(emptyList<TrackInfo>()) {
                        app.library.albumTracks(album.uid).filter { it.path != null }
                    }
                }
                AlbumScreen(album, tracks, playingUid = player.track?.uid) {
                    player.play(album, tracks, it)
                    navController.navigate("player")
                }
            }
            composable("player") {
                PlayerScreen(
                    title = player.track?.title,
                    artist = player.album?.artist,
                    isPlaying = player.isPlaying,
                    hasPrevious = player.hasPrevious,
                    hasNext = player.hasNext,
                    onPrevious = player::skipPrevious,
                    onPlayPause = player::togglePlayPause,
                    onNext = player::skipNext,
                )
            }
            composable("sync") {
                // Discoverable, and syncing, only while this screen is open.
                LaunchedEffect(Unit) { sync.run() }
                SyncScreen(sync.state, onDismissRefusal = sync::dismissRefusal)
            }
            composable("settings") {
                SettingsScreen(onLicenses = { navController.navigate("licenses") })
            }
            composable("licenses") {
                // The asset's path has slashes: encoded here, decoded by the route.
                LicensesScreen { navController.navigate("license/${Uri.encode(it)}") }
            }
            composable("license/{asset}") { entry ->
                LicenseTextScreen(entry.arguments?.getString("asset") ?: return@composable)
            }
        }
    }
}

/** [block]'s result, or [fallback] if the library can't be read. */
private inline fun <T> libraryCall(fallback: T, block: () -> T): T =
    try {
        block()
    } catch (e: MusicCoreException) {
        Log.e("kithsong", "Library read failed", e)
        fallback
    }
