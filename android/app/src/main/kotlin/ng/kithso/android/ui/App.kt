package ng.kithso.android.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
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
import ng.kithso.android.data.RecentsEditor
import ng.kithso.android.data.SmartListEditor
import ng.kithso.android.importer.Importer
import ng.kithso.android.playback.Playback
import ng.kithso.android.sync.Syncer
import ng.kithso.android.ui.theme.ArtColors
import ng.kithso.android.ui.theme.ArtTheme
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.launch
import java.io.File

/**
 * The app's one screen: the main list, with settings, sync, search or track list screens stacked
 * over it, then the mini player, then the full player. Navigation is one [BackStack] of [Page]s,
 * in the order they opened, so back goes up a page at a time, and the player, always on top,
 * closes first; each layer owns its slide-in, drag-to-dismiss and predictive back. With nothing
 * open back calls [onLeave]. A search result opens its track list over search. The players take
 * their colours from the album art.
 * [onPlay] is called when a track list starts playing, and [onImportStarted] when an import
 * starts; how it's going shows as a card under the main list's header. Pairing with another
 * device shows over everything, as it can start from the other device at any time, and so
 * does the smart list editor ([SmartEditorScreen]) while it's open. [libraryDir] is the library
 * folder, for settings to show its size.
 */
@Composable
fun App(
    source: LibrarySource,
    playback: Playback,
    syncer: Syncer,
    importer: Importer,
    settings: AppSettings,
    libraryDir: File,
    onPlay: () -> Unit = {},
    onImportStarted: () -> Unit = {},
    onLeave: () -> Unit = {},
) {
    val library by source.library.collectAsState()
    val importProgress by importer.progress.collectAsState()
    // Only the merge's state, so sync's other progress doesn't recompose everything.
    val merge by remember(syncer) { syncer.status.map { it.merge }.distinctUntilChanged() }
        .collectAsState(syncer.status.value.merge)
    val inGroup by remember(syncer) { syncer.status.map { it.inGroup }.distinctUntilChanged() }
        .collectAsState(syncer.status.value.inGroup)
    // Tracks waiting for a member this phone can't reach: a card under the main list's header.
    val waiting by remember(syncer) { syncer.status.map { it.waiting }.distinctUntilChanged() }
        .collectAsState(syncer.status.value.waiting)
    // The recycle bin: how many deletes wait for a sync (null outside a group, where deletes
    // don't go there), and the sync button's red dot while one could sync.
    val binCount by remember(syncer) {
        syncer.status.map { s -> s.binCount.takeIf { s.binOffered || it > 0 } }.distinctUntilChanged()
    }.collectAsState(null)
    val binDot by remember(syncer) { syncer.status.map { it.binDot }.distinctUntilChanged() }
        .collectAsState(syncer.status.value.binDot)
    val importPickers = rememberImportPickers(importer, onImportStarted)
    var nav by rememberSaveable(stateSaver = NavSaver) { mutableStateOf(BackStack<Page>()) }
    val track = playback.track
    // The player goes with the track, when a delete empties the queue, say; it doesn't come
    // back open with the next one.
    LaunchedEffect(track == null) { if (track == null) nav = nav.removeAll { it == Page.Player } }

    // Lists end clear of the mini player, which sits above the navigation bar.
    var miniPlayerHeight by remember { mutableIntStateOf(0) }
    val navigationBar = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val bottomPadding = if (track == null) navigationBar
    else navigationBar + with(LocalDensity.current) { miniPlayerHeight.toDp() } + MiniPlayerMargin * 2

    // Opening a list starts finding its art's colours at once, for art that wasn't on screen in
    // the main list, so they're there as its screen slides in rather than fading in after.
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    val open: (CollectionRef) -> Unit = remember {
        { ref ->
            library?.artOf(ref)?.let { art -> scope.launch { ArtColors.find(context, art) } }
            nav = nav.push(Page.Collection(ref))
        }
    }
    // A device's page, over Sync, from wherever it's asked for; the player makes way for it.
    val openDevice: (String) -> Unit = { uid ->
        val under = nav.removeAll { it == Page.Player || it is Page.Device }
        nav = (if (Page.Sync in under.pages) under else under.push(Page.Sync)).push(Page.Device(uid))
    }

    // Menus and Add to playlist, over everything. Their Play and Shuffle, and a track list's,
    // play a whole album or playlist from its start with shuffle off, or with shuffle on from
    // whichever track the core's shuffle starts with, as the desktops do. Deleting an album,
    // artist or track reads open track lists again, and one that's left empty closes itself;
    // one that takes any of the queue's tracks stops playback first.
    val actions = remember(source, playback) {
        // Remove from this phone only makes sense for the real library, which sync fills.
        val deleter = source as? LibraryDeleter
        PlaylistActions(
            source as? PlaylistEditor,
            deleter,
            syncer.takeIf { deleter != null },
            source as? RecentsEditor,
            playback,
            source as? SmartListEditor,
        )
    }
    // The smart list editor covers everything, the players too, until it's closed.
    val smartTarget = actions.smartEditing
    // Once it's sliding out, back is for what's beneath it.
    var smartLeaving by remember(smartTarget) { mutableStateOf(false) }
    val smartOpen = smartTarget != null && !smartLeaving
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

    val top = nav.top
    // Nothing open over the main list: back leaves the app, as the activity sees fit.
    BackHandler(enabled = top == null && !smartOpen, onBack = onLeave)

    // A Surface rather than a background, so text gets the surface's content colour.
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
        Box(Modifier.fillMaxSize()) {
            MainScreen(
                library,
                bottomPadding,
                onOpen = open,
                onSearch = { nav = nav.push(Page.Search) },
                onSync = { nav = nav.push(Page.Sync) },
                onSettings = { nav = nav.push(Page.Settings) },
                importPickers = importPickers,
                importRunning = importProgress?.phase?.isOver == false,
                // How an import is going, the merge after pairing by merging libraries, and
                // tracks waiting for a device.
                banner = if (importProgress == null && merge == null && waiting.tracks == 0) {
                    null
                } else {
                    { modifier ->
                        Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                            importProgress?.let { ImportCard(it, importer) }
                            merge?.let { MergeCard(it, onDismiss = syncer::dismissMerge) }
                            if (waiting.tracks > 0) WaitingCard(waiting)
                        }
                    }
                },
                modifier = Modifier.coveredIf(top != null || smartOpen),
                onRecentMenu = actions::recentMenu,
                onPlaylistMenu = actions::playlistMenu,
                onAlbumMenu = { actions.albumMenu(it) },
                onArtistMenu = actions::artistMenu,
                onAlbumGroupMenu = actions::albumGroupMenu,
                onNewSmart = if (actions.smart != null) actions::newSmart else null,
                syncDot = binDot,
                inGroup = inGroup,
                // This phone's page, over Sync, as Choose what to sync after pairing opens it.
                onChooseWhatToSync = { openDevice(THIS_DEVICE) },
            )
            // Every page but the player, which goes over the mini player.
            nav.layers.forEach { layer ->
                if (layer.page != Page.Player) key(layer.id) {
                    DismissibleScreen(
                        DismissDirection.Right,
                        onLeaving = { nav = nav.leave(layer.id) },
                        onDismissed = { nav = nav.remove(layer.id) },
                        backEnabled = layer.id == top?.id && !smartOpen,
                        modifier = Modifier.coveredIf(layer.id != top?.id),
                    ) { dismiss ->
                        when (val page = layer.page) {
                            Page.Settings -> SettingsScreen(
                                settings, libraryDir, bottomPadding,
                                onLicenses = { nav = nav.push(Page.Licenses) },
                                onBack = dismiss,
                                binCount = binCount,
                                onRecycleBin = { nav = nav.push(Page.RecycleBin) },
                            )
                            Page.Licenses -> LicensesScreen(bottomPadding, onOpen = { nav = nav.push(Page.License(it)) }, onBack = dismiss)
                            is Page.License -> LicenseTextScreen(page.asset, bottomPadding, onBack = dismiss)
                            Page.Sync -> SyncScreen(
                                syncer, bottomPadding, onOpenDevice = { nav = nav.push(Page.Device(it)) }, onBack = dismiss,
                                onOpenBin = { nav = nav.push(Page.RecycleBin) },
                            )
                            Page.RecycleBin -> RecycleBinScreen(syncer, binCount ?: 0, bottomPadding, onBack = dismiss)
                            is Page.Device -> DeviceScreen(syncer, page.uid, bottomPadding, onBack = dismiss, onRename = syncer::rename)
                            Page.Search -> SearchScreen(
                                source,
                                bottomPadding,
                                onOpen = open,
                                onPlayTrack = { found ->
                                    // Its album, from the track on; a single by itself.
                                    val album = found.albumUid?.let { CollectionRef(CollectionRef.Kind.Album, it) }
                                    if (album != null) scope.launch {
                                        val tracks = source.trackList(album)?.tracks.orEmpty()
                                        if (tracks.isEmpty()) return@launch
                                        playback.play(tracks, tracks.indexOfFirst { it.uid == found.uid }.coerceAtLeast(0))
                                        source.notePlayed(album)
                                        onPlay()
                                    } else if (found.track != null) {
                                        playback.play(listOf(found.track), 0)
                                        onPlay()
                                    }
                                },
                                // Its menu, once its artists are looked up, for Go to album and artist.
                                onTrackMenu = { found ->
                                    scope.launch { actions.foundTrackMenu(found, source.trackArtists(found.uid)) }
                                },
                                onBack = dismiss,
                            )
                            is Page.Collection -> DetailScreen(
                                page.ref,
                                source,
                                actions,
                                bottomPadding,
                                onPlay = { tracks, index ->
                                    // From an album, in order from the track, as the desktops do.
                                    if (page.ref.kind == CollectionRef.Kind.Album && settings.albumInOrder) playback.setShuffle(false)
                                    playback.play(tracks, index)
                                    source.notePlayed(page.ref)
                                    onPlay()
                                },
                                onPlayAll = { tracks, shuffle ->
                                    playback.setShuffle(shuffle)
                                    playback.play(tracks, -1)
                                    source.notePlayed(page.ref)
                                    onPlay()
                                },
                                onOpen = open,
                                onBack = dismiss,
                            )
                            Page.Player -> Unit
                        }
                    }
                }
            }
            if (track != null) ArtTheme(track.art) {
                MiniPlayer(
                    playback,
                    track,
                    onOpen = { nav = nav.push(Page.Player) },
                    modifier = Modifier
                        .align(Alignment.BottomCenter)
                        .windowInsetsPadding(
                            WindowInsets.navigationBars.union(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)),
                        )
                        .padding(MiniPlayerMargin)
                        .widthIn(max = 640.dp)
                        .fillMaxWidth()
                        .onSizeChanged { miniPlayerHeight = it.height }
                        .coveredIf(top?.page == Page.Player),
                )
                // Each opening is a layer of its own, so one tapped open while the last is still
                // sliding out doesn't take that one over.
                nav.layers.forEach { layer ->
                    if (layer.page == Page.Player) key(layer.id) {
                        DismissibleScreen(
                            DismissDirection.Down,
                            onLeaving = { nav = nav.leave(layer.id) },
                            onDismissed = { nav = nav.remove(layer.id) },
                            backEnabled = layer.id == top?.id && !smartOpen,
                        ) { dismiss ->
                            FullPlayer(
                                playback,
                                track,
                                onCollapse = dismiss,
                                onAddToPlaylist = if (actions.editor == null) null else {
                                    { actions.addToPlaylist(Addition.Tracks(listOf(track.uid))) }
                                },
                                // The track's menu, for Go to album and artist and Delete from library; it
                                // isn't from a playlist here.
                                onMenu = { actions.trackMenu(track, removable = false) },
                            )
                        }
                    }
                }
            }
            val smart = actions.smart
            if (smartTarget != null && smart != null) key(smartTarget) {
                SmartEditorScreen(
                    smart,
                    smartTarget,
                    // A new list opens once it's made, if it holds something to play here
                    // (else it isn't listed); an edited one's page reads it again itself.
                    onSaved = { uid, created ->
                        if (created) scope.launch {
                            val albums = smartTarget is SmartEditTarget.New && smartTarget.isAlbums
                            val ref = CollectionRef(if (albums) CollectionRef.Kind.AlbumGroup else CollectionRef.Kind.Playlist, uid)
                            if (source.trackList(ref) != null) {
                                nav = nav.removeAll { it == Page.Player }
                                open(ref)
                            }
                        }
                    },
                    onLeaving = { smartLeaving = true },
                    onClose = { if (actions.smartEditing == smartTarget) actions.smartEditing = null },
                )
            }
            PlaylistSheets(
                actions,
                library,
                onPlay = playAll,
                // Closes the deleted playlist's or album group's page, if it's open.
                onDeleted = { ref -> nav = nav.removeAll { it == Page.Collection(ref) } },
                // Go to album or artist: its page, from the player too, which makes way for it.
                onOpen = { ref ->
                    nav = nav.removeAll { it == Page.Player }
                    open(ref)
                },
            )
            // Over everything: another device may ask to pair at any time.
            // Choose What to Sync: the device's page, over Sync as when it's tapped there.
            PairingDialogs(syncer, settings, openDevice)
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

private val NavSaver = BackStack.saver(Page::encode, Page::decode)
