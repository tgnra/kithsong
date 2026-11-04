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
import androidx.annotation.StringRes
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.wear.compose.material3.AppScaffold
import androidx.wear.compose.material3.ConfirmationDialog
import androidx.wear.compose.material3.ConfirmationDialogDefaults
import androidx.wear.compose.material3.Icon
import androidx.wear.compose.material3.MaterialTheme
import androidx.wear.compose.material3.Text
import androidx.wear.compose.navigation.SwipeDismissableNavHost
import androidx.wear.compose.navigation.composable
import androidx.wear.compose.navigation.rememberSwipeDismissableNavController
import kotlinx.coroutines.launch
import ng.kithso.core.AlbumInfo
import ng.kithso.core.CorePlayer
import ng.kithso.core.TrackInfo
import ng.kithso.wear.ui.AlbumGroupsScreen
import ng.kithso.wear.ui.AlbumScreen
import ng.kithso.wear.ui.AlbumsScreen
import ng.kithso.wear.ui.Choice
import ng.kithso.wear.ui.ChoiceScreen
import ng.kithso.wear.ui.HeadphonesScreen
import ng.kithso.wear.ui.HomeScreen
import ng.kithso.wear.ui.Icons
import ng.kithso.wear.ui.LicenseTextScreen
import ng.kithso.wear.ui.LicensesScreen
import ng.kithso.wear.ui.PlayerScreen
import ng.kithso.wear.ui.PlaylistScreen
import ng.kithso.wear.ui.PlaylistsScreen
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

/** A passing word after adding to a playlist or album group: what was done, or that it failed. */
private class Notice(val text: String, val success: Boolean)

@Composable
private fun WearApp() {
    val navController = rememberSwipeDismissableNavController()
    val context = LocalContext.current
    val app = context.applicationContext as KithsongApp
    val playback = app.playback
    val outputs = app.outputs
    val scope = rememberCoroutineScope()
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

    // The track whose menu is open, kept here because the library has no lookup of one track:
    // the menu and the picker under it read it, checking the uid in their route against it.
    var menuTrack by remember { mutableStateOf<TrackInfo?>(null) }
    // The notice stays while the dialog fades out, so it's separate from whether it shows.
    var notice by remember { mutableStateOf<Notice?>(null) }
    var noticeShown by remember { mutableStateOf(false) }

    /**
     * Adds through [add] (a WatchLibrary helper) and says so, then leaves the menu and its
     * picker at once, back to the list the long press was on: [menuRoute] is the menu's.
     */
    fun addAndLeave(menuRoute: String, name: String, @StringRes added: Int, @StringRes failed: Int, add: suspend () -> Boolean) {
        scope.launch {
            val ok = add()
            notice = Notice(if (ok) context.getString(added, name) else context.getString(failed), ok)
            noticeShown = true
        }
        navController.popBackStack(menuRoute, inclusive = true)
    }

    // AppScaffold shows the time at the top of every screen.
    AppScaffold {
        SwipeDismissableNavHost(navController = navController, startDestination = "home") {
            composable("home") {
                HomeScreen(
                    nowPlaying = playback.track,
                    onNowPlaying = { navController.navigate("player") },
                    onAlbums = { navController.navigate("albums") },
                    onPlaylists = { navController.navigate("playlists") },
                    onAlbumGroups = { navController.navigate("groups") },
                    onSync = { navController.navigate("sync") },
                    onSettings = { navController.navigate("settings") },
                )
            }
            composable("albums") {
                AlbumsScreen(
                    albums,
                    onAlbum = { navController.navigate("album/${it.uid}") },
                    onAlbumMenu = { navController.navigate("album/${it.uid}/menu") },
                )
            }
            composable("album/{id}") { entry ->
                val uid = entry.arguments?.getString("id")
                val album = albums.firstOrNull { it.uid == uid } ?: return@composable
                // Read again with the library, as the album's files arrive.
                val tracks by produceState(emptyList<TrackInfo>(), album.uid, contents) {
                    value = app.library.albumTracks(album.uid)
                }
                AlbumScreen(
                    album,
                    tracks,
                    playingUid = playback.track?.uid,
                    onTrack = { track ->
                        startPlaying { playback.play(tracks, track, CorePlayer.Origin(CorePlayer.PlayContext.Album, album.uid)) }
                    },
                    onAlbumMenu = { navController.navigate("album/${album.uid}/menu") },
                    onTrackMenu = { track ->
                        menuTrack = track
                        navController.navigate("track/${track.uid}/menu")
                    },
                )
            }
            composable("album/{id}/menu") { entry ->
                val uid = entry.arguments?.getString("id")
                val album = albums.firstOrNull { it.uid == uid } ?: return@composable
                ChoiceScreen(
                    album.title,
                    listOf(
                        Choice("playlist", stringResource(R.string.add_to_playlist_menu)) {
                            navController.navigate("album/${album.uid}/add-to-playlist")
                        },
                        Choice("group", stringResource(R.string.add_to_album_group_menu)) {
                            navController.navigate("album/${album.uid}/add-to-group")
                        },
                    ),
                )
            }
            composable("album/{id}/add-to-playlist") { entry ->
                // Only playlists with music here are offered, like everywhere on the watch:
                // there's no New playlist, since the watch can't name one.
                val uid = entry.arguments?.getString("id") ?: return@composable
                ChoiceScreen(
                    stringResource(R.string.add_to_playlist),
                    contents.playlists.map { playlist ->
                        Choice(playlist.uid, playlist.name) {
                            addAndLeave("album/$uid/menu", playlist.name, R.string.playlist_added, R.string.playlist_add_failed) {
                                app.library.appendAlbumToPlaylist(playlist.uid, uid)
                            }
                        }
                    },
                    emptyText = stringResource(R.string.watch_no_playlists),
                )
            }
            composable("album/{id}/add-to-group") { entry ->
                val uid = entry.arguments?.getString("id") ?: return@composable
                ChoiceScreen(
                    stringResource(R.string.add_to_album_group),
                    contents.albumGroups.map { group ->
                        Choice(group.uid, group.name) {
                            addAndLeave("album/$uid/menu", group.name, R.string.album_group_added, R.string.album_group_add_failed) {
                                app.library.addToAlbumGroup(group.uid, uid)
                            }
                        }
                    },
                    emptyText = stringResource(R.string.watch_no_album_groups),
                )
            }
            composable("track/{id}/menu") { entry ->
                val track = menuTrack?.takeIf { it.uid == entry.arguments?.getString("id") } ?: return@composable
                ChoiceScreen(
                    track.title,
                    listOf(
                        Choice("playlist", stringResource(R.string.add_to_playlist_menu)) {
                            navController.navigate("track/${track.uid}/add-to-playlist")
                        },
                    ),
                )
            }
            composable("track/{id}/add-to-playlist") { entry ->
                val track = menuTrack?.takeIf { it.uid == entry.arguments?.getString("id") } ?: return@composable
                ChoiceScreen(
                    stringResource(R.string.add_to_playlist),
                    contents.playlists.map { playlist ->
                        Choice(playlist.uid, playlist.name) {
                            addAndLeave("track/${track.uid}/menu", playlist.name, R.string.playlist_added, R.string.playlist_add_failed) {
                                app.library.appendToPlaylist(playlist.uid, listOf(track.uid))
                            }
                        }
                    },
                    emptyText = stringResource(R.string.watch_no_playlists),
                )
            }
            composable("playlists") {
                PlaylistsScreen(contents.playlists) { navController.navigate("playlist/${it.uid}") }
            }
            composable("playlist/{id}") { entry ->
                val uid = entry.arguments?.getString("id")
                val playlist = contents.playlists.firstOrNull { it.uid == uid } ?: return@composable
                // Read again as sync brings more of it, or something is added here.
                val tracks by produceState(emptyList<TrackInfo>(), playlist.uid, contents) {
                    value = app.library.playlistTracks(playlist.uid)
                }
                PlaylistScreen(
                    playlist,
                    tracks,
                    playingUid = playback.track?.uid,
                    // The queue is the playlist's tracks that are here, from the one tapped.
                    onTrack = { track ->
                        startPlaying { playback.play(tracks, track, CorePlayer.Origin(CorePlayer.PlayContext.Playlist, playlist.uid)) }
                    },
                    onTrackMenu = { track ->
                        menuTrack = track
                        navController.navigate("track/${track.uid}/menu")
                    },
                )
            }
            composable("groups") {
                AlbumGroupsScreen(contents.albumGroups) { navController.navigate("group/${it.uid}") }
            }
            composable("group/{id}") { entry ->
                val uid = entry.arguments?.getString("id")
                val group = contents.albumGroups.firstOrNull { it.uid == uid } ?: return@composable
                val groupAlbums by produceState(emptyList<AlbumInfo>(), group.uid, contents) {
                    value = app.library.albumGroupAlbums(group.uid)
                }
                // The group is listed only while an album of it is here, so no empty text.
                AlbumsScreen(
                    groupAlbums,
                    title = group.name,
                    emptyText = null,
                    onAlbum = { navController.navigate("album/${it.uid}") },
                    onAlbumMenu = { navController.navigate("album/${it.uid}/menu") },
                )
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
        // "Added to Favourites", with a tick, for a moment over whatever screen is up.
        val current = notice
        ConfirmationDialog(
            visible = noticeShown,
            onDismissRequest = { noticeShown = false },
            text = { Text(current?.text.orEmpty()) },
            colors = if (current?.success != false) ConfirmationDialogDefaults.successColors() else ConfirmationDialogDefaults.colors(),
        ) {
            if (current?.success != false) {
                ConfirmationDialogDefaults.SuccessIcon(Modifier.size(ConfirmationDialogDefaults.SmallIconSize))
            } else {
                Icon(Icons.Close, contentDescription = null, modifier = Modifier.size(ConfirmationDialogDefaults.SmallIconSize))
            }
        }
    }
}
