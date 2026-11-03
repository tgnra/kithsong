package ng.kithso.wear

import android.Manifest
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.compose.setContent
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.platform.LocalContext
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.navigation.SwipeDismissableNavHost
import androidx.wear.compose.navigation.composable
import androidx.wear.compose.navigation.rememberSwipeDismissableNavController
import ng.kithso.core.TrackInfo
import ng.kithso.wear.ui.AlbumScreen
import ng.kithso.wear.ui.AlbumsScreen
import ng.kithso.wear.ui.HeadphonesScreen
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
    val playback = app.playback
    val outputs = app.outputs
    // What has files on the watch, read again as sync brings more.
    val contents by app.library.contents.collectAsState()
    val albums = contents.albums

    // Android 13 on: the playback notification needs leave to show, asked for on the first
    // play and otherwise left to the system, which stops asking after two refusals. Playing
    // doesn't wait for the answer.
    val askNotifications = rememberLauncherForActivityResult(ActivityResultContracts.RequestPermission()) {}
    // What to play once there's an output for it (see AudioOutputs): while there's none, the
    // headphones screen holds it.
    var pendingPlay by remember { mutableStateOf<(() -> Unit)?>(null) }
    fun startPlaying(play: () -> Unit) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU &&
            context.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED
        ) {
            askNotifications.launch(Manifest.permission.POST_NOTIFICATIONS)
        }
        if (outputs.ready) {
            play()
            navController.navigate("player")
        } else {
            pendingPlay = play
            navController.navigate("headphones")
        }
    }

    // AppScaffold shows the time at the top of every screen.
    AppScaffold {
        SwipeDismissableNavHost(navController = navController, startDestination = "home") {
            composable("home") {
                HomeScreen(
                    nowPlaying = playback.track,
                    onNowPlaying = { navController.navigate("player") },
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
                // Read again with the library, as the album's files arrive.
                val tracks by produceState(emptyList<TrackInfo>(), album.uid, contents) {
                    value = app.library.albumTracks(album.uid)
                }
                AlbumScreen(album, tracks, playingUid = playback.track?.uid) { track ->
                    startPlaying { playback.play(tracks, track) }
                }
            }
            composable("headphones") {
                // Headphones connected, or the speaker chosen: play, and put the player where
                // this screen was, so swiping back from it goes to the album.
                val ready = outputs.ready
                LaunchedEffect(ready) {
                    if (!ready) return@LaunchedEffect
                    pendingPlay?.invoke()
                    pendingPlay = null
                    navController.navigate("player") { popUpTo("headphones") { inclusive = true } }
                }
                HeadphonesScreen(
                    hasSpeaker = outputs.hasSpeaker,
                    onBluetoothSettings = outputs::openBluetoothSettings,
                    onPlayOnWatch = { outputs.useSpeaker = true },
                )
            }
            composable("player") {
                val track = playback.track
                PlayerScreen(
                    title = track?.title,
                    artist = track?.artist,
                    isPlaying = playback.isPlaying,
                    hasPrevious = playback.hasPrevious,
                    hasNext = playback.hasNext,
                    positionMs = { playback.positionMs },
                    durationMs = playback.durationMs,
                    onPrevious = playback::previous,
                    onPlayPause = playback::toggle,
                    onNext = playback::next,
                    onSeek = playback::seekTo,
                )
            }
            composable("sync") {
                // Sync runs while the app is on screen (KithsongApp), not just while this
                // screen is open; the screen only shows where it has got to.
                val state by app.syncer.screen.collectAsState()
                SyncScreen(state, onDismissRefusal = app.syncer::dismissRefusal)
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
