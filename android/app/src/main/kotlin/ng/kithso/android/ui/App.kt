package ng.kithso.android.ui

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
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.listSaver
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshots.SnapshotStateList
import androidx.compose.runtime.toMutableStateList
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.unit.dp
import ng.kithso.android.AppSettings
import ng.kithso.android.data.Addition
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.LibraryDeleter
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.PlaylistEditor
import ng.kithso.android.playback.Playback
import ng.kithso.android.sync.Syncer
import ng.kithso.android.ui.theme.ArtTheme
import kotlinx.coroutines.launch
import java.io.File

/**
 * The app's one screen: the main list, with settings, sync, search or track list screens stacked
 * over it, then the mini player, then the full player. Navigation is this stack of
 * [CollectionRef]s and flags for settings, sync, the device open from sync, search and the
 * player, so each layer can own its slide-in, drag-to-dismiss and predictive back. A search
 * result opens its track list over search. The players take their colours from the album art.
 * [onPlay] is called when a track list starts playing. Pairing with another device shows over
 * everything, as it can start from the other device at any time. [libraryDir] is the library
 * folder, for settings to show its size.
 */
@Composable
fun App(
    source: LibrarySource,
    playback: Playback,
    syncer: Syncer,
    settings: AppSettings,
    libraryDir: File,
    onPlay: () -> Unit = {},
) {
    val library by source.library.collectAsState()
    val stack = rememberSaveable(saver = StackSaver) { SnapshotStateList<CollectionRef>() }
    var settingsOpen by rememberSaveable { mutableStateOf(false) }
    var syncOpen by rememberSaveable { mutableStateOf(false) }
    var licensesOpen by rememberSaveable { mutableStateOf(false) }
    // The licence text open over the licences: its asset's path.
    var licenseOpen by rememberSaveable { mutableStateOf<String?>(null) }
    // The device whose page is open over sync: a member's uid, or THIS_DEVICE.
    var deviceOpen by rememberSaveable { mutableStateOf<String?>(null) }
    var playerOpen by rememberSaveable { mutableStateOf(false) }
    var searchOpen by rememberSaveable { mutableStateOf(false) }
    val track = playback.track

    // Lists end clear of the mini player, which sits above the navigation bar.
    var miniPlayerHeight by remember { mutableIntStateOf(0) }
    val navigationBar = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val bottomPadding = if (track == null) navigationBar
    else navigationBar + with(LocalDensity.current) { miniPlayerHeight.toDp() } + MiniPlayerMargin * 2

    val open: (CollectionRef) -> Unit = remember { { stack.add(it) } }
    val pop: () -> Unit = remember { { if (stack.isNotEmpty()) stack.removeAt(stack.lastIndex) } }

    // Menus and Add to playlist, over everything. Their Play and Shuffle, and a track list's,
    // play a whole album or playlist from its start with shuffle off, or with shuffle on from
    // whichever track the core's shuffle starts with, as the desktops do. Deleting an album,
    // artist or track reads open track lists again, and one that's left empty closes itself.
    val actions = remember(source) { PlaylistActions(source as? PlaylistEditor, source as? LibraryDeleter) }
    val scope = rememberCoroutineScope()
    val playAll: (CollectionRef, Boolean) -> Unit = { ref, shuffle ->
        scope.launch {
            val tracks = source.trackList(ref)?.tracks ?: return@launch
            if (tracks.none { it.path != null }) return@launch
            playback.setShuffle(shuffle)
            playback.play(tracks, -1)
            source.notePlayed(ref)
            onPlay()
        }
    }

    // A Surface rather than a background, so text gets the surface's content colour.
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
        Box(Modifier.fillMaxSize()) {
            MainScreen(
                library,
                bottomPadding,
                onOpen = open,
                onSearch = { searchOpen = true },
                onSync = { syncOpen = true },
                onSettings = { settingsOpen = true },
                modifier = Modifier.coveredIf(stack.isNotEmpty() || playerOpen || settingsOpen || syncOpen || searchOpen),
                onPlaylistMenu = actions::playlistMenu,
                onAlbumMenu = { actions.albumMenu(it) },
                onArtistMenu = actions::artistMenu,
                onAlbumGroupMenu = actions::albumGroupMenu,
            )
            if (settingsOpen) {
                DismissibleScreen(
                    DismissDirection.Right,
                    onDismissed = { settingsOpen = false },
                    backEnabled = !playerOpen && !syncOpen && !licensesOpen,
                    modifier = Modifier.coveredIf(playerOpen || syncOpen || licensesOpen),
                ) { dismiss ->
                    SettingsScreen(
                        settings, libraryDir, bottomPadding,
                        onSync = { syncOpen = true },
                        onLicenses = { licensesOpen = true },
                        onBack = dismiss,
                    )
                }
            }
            if (licensesOpen) {
                DismissibleScreen(
                    DismissDirection.Right,
                    onDismissed = { licensesOpen = false },
                    backEnabled = !playerOpen && licenseOpen == null,
                    modifier = Modifier.coveredIf(playerOpen || licenseOpen != null),
                ) { dismiss ->
                    LicensesScreen(bottomPadding, onOpen = { licenseOpen = it }, onBack = dismiss)
                }
            }
            licenseOpen?.let { asset ->
                key(asset) {
                    DismissibleScreen(
                        DismissDirection.Right,
                        onDismissed = { licenseOpen = null },
                        backEnabled = !playerOpen,
                        modifier = Modifier.coveredIf(playerOpen),
                    ) { dismiss ->
                        LicenseTextScreen(asset, bottomPadding, onBack = dismiss)
                    }
                }
            }
            if (syncOpen) {
                DismissibleScreen(
                    DismissDirection.Right,
                    onDismissed = { syncOpen = false },
                    backEnabled = !playerOpen && deviceOpen == null,
                    modifier = Modifier.coveredIf(playerOpen || deviceOpen != null),
                ) { dismiss ->
                    SyncScreen(syncer, bottomPadding, onOpenDevice = { deviceOpen = it }, onBack = dismiss)
                }
            }
            deviceOpen?.let { uid ->
                key(uid) {
                    DismissibleScreen(
                        DismissDirection.Right,
                        onDismissed = { deviceOpen = null },
                        backEnabled = !playerOpen,
                        modifier = Modifier.coveredIf(playerOpen),
                    ) { dismiss ->
                        DeviceScreen(syncer, uid, bottomPadding, onBack = dismiss, onRename = syncer::rename)
                    }
                }
            }
            // Under the track lists, which its results open over it.
            if (searchOpen) {
                DismissibleScreen(
                    DismissDirection.Right,
                    onDismissed = { searchOpen = false },
                    backEnabled = stack.isEmpty() && !playerOpen,
                    modifier = Modifier.coveredIf(stack.isNotEmpty() || playerOpen),
                ) { dismiss ->
                    SearchScreen(
                        source,
                        bottomPadding,
                        onOpen = open,
                        onPlayTrack = { found ->
                            // Its album, from the track on.
                            val album = found.albumUid?.let { CollectionRef(CollectionRef.Kind.Album, it) }
                            if (album != null) scope.launch {
                                val tracks = source.trackList(album)?.tracks.orEmpty()
                                if (tracks.isEmpty()) return@launch
                                playback.play(tracks, tracks.indexOfFirst { it.uid == found.uid }.coerceAtLeast(0))
                                source.notePlayed(album)
                                onPlay()
                            }
                        },
                        onBack = dismiss,
                    )
                }
            }
            stack.forEachIndexed { i, ref ->
                key(i, ref) {
                    val top = i == stack.lastIndex
                    DismissibleScreen(
                        DismissDirection.Right,
                        onDismissed = pop,
                        backEnabled = top && !playerOpen,
                        modifier = Modifier.coveredIf(!top || playerOpen),
                    ) { dismiss ->
                        DetailScreen(
                            ref,
                            source,
                            actions,
                            bottomPadding,
                            onPlay = { tracks, index ->
                                playback.play(tracks, index)
                                source.notePlayed(ref)
                                onPlay()
                            },
                            onPlayAll = { tracks, shuffle ->
                                playback.setShuffle(shuffle)
                                playback.play(tracks, -1)
                                source.notePlayed(ref)
                                onPlay()
                            },
                            onOpen = open,
                            onBack = dismiss,
                        )
                    }
                }
            }
            if (track != null) ArtTheme(track.art) {
                MiniPlayer(
                    playback,
                    track,
                    onOpen = { playerOpen = true },
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .windowInsetsPadding(
                            WindowInsets.navigationBars.union(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)),
                        )
                        .padding(MiniPlayerMargin)
                        .widthIn(max = 640.dp)
                        .fillMaxWidth()
                        .onSizeChanged { miniPlayerHeight = it.height }
                        .coveredIf(playerOpen),
                )
                if (playerOpen) {
                    DismissibleScreen(DismissDirection.Down, onDismissed = { playerOpen = false }) { dismiss ->
                        FullPlayer(
                            playback,
                            track,
                            onCollapse = dismiss,
                            onAddToPlaylist = if (actions.editor == null) null else {
                                { actions.addToPlaylist(Addition.Tracks(listOf(track.uid))) }
                            },
                        )
                    }
                }
            }
            PlaylistSheets(
                actions,
                onPlay = playAll,
                // Closes the deleted playlist's or album group's page, if it's open.
                onDeleted = { ref -> stack.removeAll { it == ref } },
            )
            // Over everything: another device may ask to pair at any time.
            PairingDialogs(syncer)
        }
    }
}

private val MiniPlayerMargin = 8.dp

/**
 * Whether the window is short, as a phone is in landscape: screens then put their headers'
 * parts side by side, to leave room for the content beneath.
 */
@Composable
fun isShortWindow(): Boolean =
    with(LocalDensity.current) { LocalWindowInfo.current.containerSize.height.toDp() } < ShortWindowHeight

private val ShortWindowHeight = 480.dp

/** Hides a layer from accessibility services while another covers it. */
private fun Modifier.coveredIf(covered: Boolean) = if (covered) clearAndSetSemantics {} else this

private val StackSaver = listSaver<SnapshotStateList<CollectionRef>, String>(
    save = { stack -> stack.map { "${it.kind.name}:${it.uid}" } },
    restore = { saved ->
        saved.map { CollectionRef(CollectionRef.Kind.valueOf(it.substringBefore(':')), it.substringAfter(':')) }
            .toMutableStateList()
    },
)
