package ng.kithso.android.ui

import android.app.Activity
import android.media.AudioManager
import android.util.Log
import android.widget.Toast
import androidx.compose.runtime.withFrameNanos
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.withContext
import ng.kithso.android.NewLibrary
import ng.kithso.android.R
import ng.kithso.core.CoreLibrary
import ng.kithso.core.CoreTesting
import androidx.activity.compose.BackHandler
import androidx.activity.compose.LocalOnBackPressedDispatcherOwner
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.ui.focus.FocusRequester
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
import androidx.compose.runtime.State
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.Saver
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
import ng.kithso.android.data.CoreLibrarySource
import ng.kithso.android.data.LibraryDeleter
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.PlaylistEditor
import ng.kithso.android.data.RecentsEditor
import ng.kithso.android.data.SmartListEditor
import ng.kithso.android.data.TagTarget
import ng.kithso.android.data.Track
import ng.kithso.android.importer.Importer
import ng.kithso.android.importer.ItemKey
import ng.kithso.android.playback.Playback
import ng.kithso.android.playback.origin
import ng.kithso.android.sync.SyncStatus
import ng.kithso.android.sync.Syncer
import ng.kithso.android.ui.theme.ArtColors
import ng.kithso.android.ui.theme.ArtTheme
import ng.kithso.core.CorePlayer
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
 * do the smart list editor ([SmartEditorScreen]) and the tag editor ([TagEditorScreen]) while
 * they're open. [libraryDir] is the library
 * folder, for settings to show its size. [shortcuts] are the keyboard's, which this says what
 * to do for. [newLibrary] is what a library mismatch's dialog offers; [stress] and [demo] say
 * a stress test library or the demo library is open ([ng.kithso.android.Testing]).
 */
@Composable
fun App(
    source: LibrarySource,
    playback: Playback,
    syncer: Syncer,
    importer: Importer,
    settings: AppSettings,
    libraryDir: File,
    newLibrary: NewLibrary,
    stress: Boolean,
    demo: Boolean,
    onPlay: () -> Unit = {},
    onImportStarted: () -> Unit = {},
    onLeave: (() -> Unit)? = null,
    shortcuts: KeyboardShortcuts = remember { KeyboardShortcuts() },
) {
    val library by source.library.collectAsState()
    val importProgress by importer.progress.collectAsState()
    // Only the merge's state, so sync's other progress doesn't recompose everything.
    val merge by syncer.statusPart { it.merge }
    val inGroup by syncer.statusPart { it.inGroup }
    // Tracks waiting for a member this phone can't reach: a card under the main list's header.
    val waiting by syncer.statusPart { it.waiting }
    // What's wrong with the files here: a card above that one.
    val problems by syncer.statusPart { it.problems }
    // The recycle bin: how many deletes wait for a sync (null outside a group, where deletes
    // don't go there), and the red dot on the main menu and its Sync while one could sync.
    val binCount by syncer.statusPart { s -> s.binCount.takeIf { s.binOffered || it > 0 } }
    val binDot by syncer.statusPart { it.binDot }
    val importPickers = rememberImportPickers(importer, onImportStarted)
    var nav by rememberSaveable(stateSaver = NavSaver) { mutableStateOf(BackStack<Page>()) }
    // What the tag editor is open on, which outlives a rotation as what's typed there does.
    var tagTarget by rememberSaveable(stateSaver = TagTargetSaver) { mutableStateOf<TagTarget?>(null) }
    val track = playback.track
    // The player goes with the track, when a delete empties the queue, say; it doesn't come
    // back open with the next one.
    LaunchedEffect(track == null) { if (track == null) nav = nav.removeAll { it == Page.Player } }
    // Tabletop opens the player, when there's a track for it, laid out around the fold, and
    // leaving tabletop closes it again if that's what opened it. One that was open already, or
    // that's been closed and opened again since, was opened by hand, and stays.
    val tabletop = rememberWindowShape().tabletop && track != null
    var tabletopPlayer by rememberSaveable { mutableStateOf<Int?>(null) }
    LaunchedEffect(tabletop) {
        val opened = tabletopPlayer
        if (tabletop && Page.Player !in nav.pages) {
            nav = nav.push(Page.Player)
            tabletopPlayer = nav.top?.id
        } else if (!tabletop && opened != null) {
            nav = nav.remove(opened)
            tabletopPlayer = null
        }
    }
    LaunchedEffect(nav.layers.none { it.id == tabletopPlayer }) {
        if (nav.layers.none { it.id == tabletopPlayer }) tabletopPlayer = null
    }
    // The tablet layout, while the window has room for it (useTabletLayout), and the library
    // page its sidebar has picked, which outlives a turn to the phone layout and back. Both
    // share the back stack, so what's open stays open across the switch.
    val windowShape = rememberWindowShape()
    val tablet = useTabletLayout(windowShape)
    var tabletSection by rememberSaveable { mutableStateOf(TabletSection.Albums) }
    // The tablet layout's queue button opens the player at its queue.
    var playerOnQueue by remember { mutableStateOf(false) }

    // Lists end clear of the mini player, which sits above the navigation bar. The tablet
    // layout has none.
    var miniPlayerHeight by remember { mutableIntStateOf(0) }
    val navigationBar = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val bottomPadding = if (track == null || tablet) navigationBar
    else navigationBar + with(LocalDensity.current) { miniPlayerHeight.toDp() } + MiniPlayerMargin * 2

    // Opening a list starts finding its art's colours at once, for art that wasn't on screen in
    // the main list, so they're there as its screen slides in rather than fading in after.
    val context = LocalContext.current
    val scope = rememberCoroutineScope()
    // A station starts from a track of its kind or its seed, which one from Recents, synced from another device,
    // may not have here: that's asked at each tap, and said if it can't.
    val playStation: (CollectionRef) -> Unit = remember(source, playback) {
        { ref ->
            scope.launch {
                val seed = source.stationSeed(ref.uid)
                if (seed == null) {
                    Toast.makeText(context, R.string.station_unavailable, Toast.LENGTH_LONG).show()
                } else {
                    playback.play(listOf(seed), 0, ref.origin)
                    source.notePlayed(ref)
                    onPlay()
                }
            }
        }
    }
    // A station has no page, so it never reaches the back stack: opening it plays it.
    val open: (CollectionRef) -> Unit = remember {
        { ref ->
            if (ref.kind == CollectionRef.Kind.Station) {
                playStation(ref)
            } else {
                library?.artOf(ref)?.let { art -> scope.launch { ArtColors.find(context, art) } }
                nav = nav.push(Page.Collection(ref))
            }
        }
    }
    // A device's page, over Sync, from wherever it's asked for; the player makes way for it.
    // The tablet layout lists the devices in its sidebar, so there it's the page by itself.
    val openDevice: (String) -> Unit = { uid ->
        if (tablet) {
            nav = nav.removeAll { true }.push(Page.Device(uid))
        } else {
            val under = nav.removeAll { it == Page.Player || it is Page.Device }
            nav = (if (Page.Sync in under.pages) under else under.push(Page.Sync)).push(Page.Device(uid))
        }
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
            onEdit = (source as? CoreLibrarySource)?.let { { target -> tagTarget = target } },
        )
    }
    // The smart list and tag editors cover everything, the players too, until they're closed.
    val smartTarget = actions.smartEditing
    // Once it's sliding out, back is for what's beneath it.
    var smartLeaving by remember(smartTarget) { mutableStateOf(false) }
    val smartOpen = (smartTarget != null && !smartLeaving) || tagTarget != null
    // A track list's tracks from tracks[index]: in an album opened from an album group, the
    // whole group as the queue, from there, so it goes on into the albums after it and previous
    // goes back into those before. An album no longer in its group plays by itself.
    val playFrom: (CollectionRef, List<Track>, Int) -> Unit = { ref, tracks, index ->
        val play = { queue: List<Track>, start: Int, from: CollectionRef ->
            playback.play(queue, start, from.origin)
            source.notePlayed(from)
            onPlay()
        }
        val group = ref.parentGroup
        if (group == null) {
            play(tracks, index, ref)
        } else {
            scope.launch {
                val queue = source.trackList(group)?.tracks.orEmpty()
                val start = queue.indexOfFirst { it.uid == tracks[index].uid }
                if (start >= 0) play(queue, start, group) else play(tracks, index, ref)
            }
        }
    }
    // A whole track list, in order or shuffled. An album's own Shuffle stays in the album, even
    // from a group's page: the group has Shuffle tracks.
    val playList: (CollectionRef, List<Track>, Boolean) -> Unit = { ref, tracks, shuffle ->
        playback.setShuffle(shuffle)
        if (ref.group != null && !shuffle) {
            playFrom(ref, tracks, 0)
        } else {
            playback.play(tracks, -1, ref.origin)
            source.notePlayed(ref)
            onPlay()
        }
    }
    val playAll: (CollectionRef, Boolean) -> Unit = { ref, shuffle ->
        if (ref.kind == CollectionRef.Kind.Station) {
            playStation(ref)
        } else {
            scope.launch {
                val tracks = source.trackList(ref)?.tracks ?: return@launch
                if (tracks.none { it.path != null }) return@launch
                playList(ref, tracks, shuffle)
            }
        }
    }

    val top = nav.top
    // A view switch is over once a frame has been drawn with the new page, and the frame after
    // has begun.
    LaunchedEffect(top?.id) {
        withFrameNanos {}
        withFrameNanos {}
        CoreTesting.viewEnd()
    }
    // Startup's last marks, and what's being measured.
    LaunchedEffect(Unit) {
        withFrameNanos {}
        CoreTesting.mark("window_shown")
    }
    val libraryRead = library != null
    LaunchedEffect(libraryRead) {
        if (!libraryRead) return@LaunchedEffect
        CoreTesting.mark("library_loaded")
        withFrameNanos {}
        withFrameNanos {}
        CoreTesting.mark("first_content_painted")
        CoreTesting.note("library_albums", library?.albums?.size.toString())
        CoreTesting.note("library_stress", if (stress) "yes" else "no")
        val tracks = withContext(Dispatchers.IO) {
            runCatching { CoreLibrary(libraryDir.path).use { it.trackCount } }.getOrNull()
        }
        if (tracks != null) CoreTesting.note("library_tracks", tracks.toString())
        if (PerfRun.requested) {
            PerfRun.requested = false
            delay(2000)
            ScrollTest.start()
        }
    }
    LaunchedEffect(Unit) {
        ScrollTest.step.collect { step ->
            when (step) {
                ScrollTest.Step.OpenTracks -> {
                    val uid = withContext(Dispatchers.IO) {
                        runCatching { CoreLibrary(libraryDir.path).use { it.biggestAlbum() } }.getOrNull()
                    }
                    if (uid == null) {
                        ScrollTest.step.value = ScrollTest.Step.Done
                    } else {
                        ScrollTest.step.value = ScrollTest.Step.Tracks
                        open(CollectionRef(CollectionRef.Kind.Album, uid))
                    }
                }
                ScrollTest.Step.Done -> {
                    CoreTesting.autoEnd()
                    ScrollTest.step.value = ScrollTest.Step.Idle
                    TestingOpen.next = true
                    nav = nav.removeAll { it is Page.Collection }.push(Page.Settings)
                    CoreTesting.report().lines().forEach { Log.i("kithsong-perf", it) }
                }
                else -> {}
            }
        }
    }
    // Nothing open over the main list: back leaves the app, as the activity sees fit, if it
    // doesn't leave that to the system, which then shows where back goes as it's swiped.
    BackHandler(enabled = onLeave != null && top == null && !smartOpen) { onLeave?.invoke() }

    // The keyboard shortcuts (see Shortcuts.kt), in either layout. Playback's work from
    // anywhere; those that open a page wait while an editor covers everything. Escape goes back
    // only from something open, so it never leaves the app.
    val backDispatcher = LocalOnBackPressedDispatcherOwner.current?.onBackPressedDispatcher
    val audio = remember(context) { context.getSystemService(AudioManager::class.java) }
    // A page the sidebar opens, as picking it there does: by itself in the tablet layout,
    // where picked again it stays as it is; on top of what's open on the phone.
    val showPage = { page: Page ->
        nav = if (tablet) {
            if (nav.pages == listOf(page)) nav else nav.removeAll { true }.push(page)
        } else {
            if (nav.top?.page == page) nav else nav.removeAll { it == Page.Player || it == page }.push(page)
        }
    }
    val showSection = { section: TabletSection ->
        tabletSection = section
        nav = nav.removeAll { true }
    }
    SideEffect {
        shortcuts.hasPages = tablet
        shortcuts.perform = { shortcut ->
            val playing = playback.track
            val volume = { direction: Int -> audio?.adjustStreamVolume(AudioManager.STREAM_MUSIC, direction, AudioManager.FLAG_SHOW_UI) }
            val free = !smartOpen
            when (shortcut) {
                Shortcut.PlayPause -> if (playing != null) playback.toggle()
                Shortcut.Next -> if (playback.hasNext) playback.next()
                Shortcut.Previous -> if (playing != null) playback.previous()
                Shortcut.SeekForward, Shortcut.SeekBack -> if (playing != null) {
                    val step = if (shortcut == Shortcut.SeekForward) SEEK_STEP_MS else -SEEK_STEP_MS
                    playback.seekTo((playback.positionMs + step).coerceIn(0L, playing.durationMs))
                }
                Shortcut.VolumeUp -> volume(AudioManager.ADJUST_RAISE)
                Shortcut.VolumeDown -> volume(AudioManager.ADJUST_LOWER)
                Shortcut.Mute -> volume(AudioManager.ADJUST_TOGGLE_MUTE)
                // A station has its own order, as on the players; said, as the desktops flash it.
                Shortcut.Shuffle -> if (playing != null && !playback.station) {
                    playback.setShuffle(!playback.shuffle)
                    Toast.makeText(context, if (playback.shuffle) R.string.shuffle_on else R.string.shuffle_off, Toast.LENGTH_SHORT).show()
                }
                Shortcut.Search -> if (free) showPage(Page.Search)
                Shortcut.Settings -> if (free) showPage(Page.Settings)
                Shortcut.Albums -> if (free && tablet) showSection(TabletSection.Albums)
                Shortcut.Artists -> if (free && tablet) showSection(TabletSection.Artists)
                Shortcut.Playlists -> if (free && tablet) showSection(TabletSection.Playlists)
                Shortcut.AlbumGroups -> if (free && tablet) showSection(TabletSection.AlbumGroups)
                Shortcut.Import -> if (free) importPickers.pickFolder()
                // The Playlists page's +, as on the desktops.
                Shortcut.NewPlaylist -> if (free && actions.editor != null) {
                    if (tablet) showSection(TabletSection.Playlists)
                    actions.newList(false)
                }
                Shortcut.Shortcuts -> (context as? Activity)?.requestShowKeyboardShortcuts()
                Shortcut.Back -> if (top != null || smartOpen) backDispatcher?.onBackPressed()
            }
        }
    }
    DisposableEffect(shortcuts) { onDispose { shortcuts.perform = null } }

    // Each page but the player, made the same way in either layout: the phone's layers below,
    // and the tablet layout's content and pane. [pagePadding] is what keeps a page's end clear,
    // and [pageOpen] opens what's picked on it.
    val pageContent: PageContent = { page, pagePadding, pageOpen, dismiss ->
        when (page) {
            Page.Settings -> SettingsScreen(
                settings, libraryDir, pagePadding,
                onLicenses = { nav = nav.push(Page.Licenses) },
                onBack = dismiss,
                binCount = binCount,
                onRecycleBin = { nav = nav.push(Page.RecycleBin) },
                onRunScrollTest = {
                    nav = nav.removeAll { it == Page.Settings }
                    ScrollTest.start()
                },
            )
            Page.Stats -> StatsScreen(source, pagePadding, onBack = dismiss)
            Page.Licenses -> LicensesScreen(pagePadding, onOpen = { nav = nav.push(Page.License(it)) }, onBack = dismiss)
            is Page.License -> LicenseTextScreen(page.asset, pagePadding, onBack = dismiss)
            Page.Sync -> SyncScreen(
                syncer, pagePadding, onOpenDevice = { nav = nav.push(Page.Device(it)) }, onBack = dismiss,
                onOpenBin = { nav = nav.push(Page.RecycleBin) },
            )
            Page.RecycleBin -> RecycleBinScreen(syncer, binCount ?: 0, pagePadding, onBack = dismiss)
            is Page.Device -> DeviceScreen(
                syncer, page.uid, pagePadding, onBack = dismiss, onRename = syncer::rename,
                // With no Sync page in the tablet layout, this phone's page has what's on it for the whole group.
                onOpenBin = if (tablet) ({ nav = nav.push(Page.RecycleBin) }) else null,
            )
            Page.Search -> SearchScreen(
                source,
                pagePadding,
                onOpen = pageOpen,
                onPlayTrack = { found ->
                    // Its album, from the track on; a single by itself.
                    val album = found.albumUid?.let { CollectionRef(CollectionRef.Kind.Album, it) }
                    if (album != null) scope.launch {
                        val tracks = source.trackList(album)?.tracks.orEmpty()
                        if (tracks.isEmpty()) return@launch
                        playback.play(tracks, tracks.indexOfFirst { it.uid == found.uid }.coerceAtLeast(0), album.origin)
                        source.notePlayed(album)
                        onPlay()
                    } else if (found.track != null) {
                        playback.play(listOf(found.track), 0, CorePlayer.Origin(CorePlayer.PlayContext.Manual))
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
                pagePadding,
                onPlay = { tracks, index ->
                    // From an album, in order from the track, as the desktops do.
                    if (page.ref.kind == CollectionRef.Kind.Album && settings.albumInOrder) playback.setShuffle(false)
                    playFrom(page.ref, tracks, index)
                },
                onPlayAll = { tracks, shuffle -> playList(page.ref, tracks, shuffle) },
                onOpen = pageOpen,
                onBack = dismiss,
            )
            Page.Import -> ImportScreen(importer, pagePadding, onOpen = { nav = nav.push(Page.ImportItem(it.batch, it.item)) }, onBack = dismiss)
            is Page.ImportItem -> ImportItemScreen(importer, ItemKey(page.batch, page.item), pagePadding, onBack = dismiss)
            Page.Player -> Unit
        }
    }
    // How an import is going, the merge after pairing by merging libraries, what's wrong with
    // the files here, and tracks waiting for a device.
    val banner: (@Composable (Modifier) -> Unit)? =
        if (importProgress == null && merge == null && waiting.tracks == 0 && problems.text.isEmpty() && !stress) {
            null
        } else {
            { modifier ->
                Column(modifier, verticalArrangement = Arrangement.spacedBy(8.dp)) {
                    if (stress) StressCard()
                    importProgress?.let { ImportCard(it, importer, onReview = { nav = nav.push(Page.Import) }) }
                    merge?.let { MergeCard(it, onDismiss = syncer::dismissMerge) }
                    if (problems.text.isNotEmpty() && !stress) ProblemsCard(problems, onRemove = syncer::removeLost)
                    if (waiting.tracks > 0) WaitingCard(waiting)
                }
            }
        }

    // A Surface rather than a background, so text gets the surface's content colour.
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
        Box(Modifier.fillMaxSize()) {
            if (tablet) {
                TabletLayout(
                    nav,
                    updateNav = { change -> nav = change(nav) },
                    section = tabletSection,
                    onSection = { tabletSection = it },
                    shape = windowShape,
                    library = library,
                    source = source,
                    playback = playback,
                    syncer = syncer,
                    actions = actions,
                    importPickers = importPickers,
                    importRunning = importProgress?.phase?.isOver == false,
                    binDot = binDot,
                    banner = banner,
                    covered = top?.page == Page.Player || smartOpen,
                    onOpen = open,
                    onOpenPlayer = { queue ->
                        playerOnQueue = queue
                        nav = nav.push(Page.Player)
                    },
                    onTrackMenu = { track?.let { actions.trackMenu(it, removable = false) } },
                    pageContent = pageContent,
                )
            } else {
                MainScreen(
                    library,
                    bottomPadding,
                    onOpen = open,
                    onSearch = { nav = nav.push(Page.Search) },
                    onSync = { nav = nav.push(Page.Sync) },
                    onSettings = { nav = nav.push(Page.Settings) },
                    onStats = { nav = nav.push(Page.Stats) },
                    importPickers = importPickers,
                    importRunning = importProgress?.phase?.isOver == false,
                    banner = banner,
                    modifier = Modifier.coveredIf(top != null || smartOpen),
                    onStationMenu = actions::stationMenu,
                    onRecentMenu = actions::recentMenu,
                    onPlaylistMenu = actions::playlistMenu,
                    onAlbumMenu = { actions.albumMenu(it) },
                    onArtistMenu = actions::artistMenu,
                    onAlbumGroupMenu = actions::albumGroupMenu,
                    onNewList = if (actions.editor != null) actions::newList else null,
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
                            pageContent(layer.page, bottomPadding, open, dismiss)
                        }
                    }
                }
            }
            if (track != null) ArtTheme(track.art) {
                // The track's menu, for Go to album and artist and Delete from library; it isn't
                // from a playlist here.
                val trackMenu = { actions.trackMenu(track, removable = false) }
                // The tablet layout's top bar takes the mini player's place.
                if (!tablet) MiniPlayer(
                    playback,
                    track,
                    onOpen = { nav = nav.push(Page.Player) },
                    onMenu = trackMenu,
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
                        // Back and dragging down are the queue's while it's open.
                        val queue = rememberQueueSlide()
                        // Opened by the tablet layout's queue button: on to the queue at once.
                        LaunchedEffect(queue) {
                            if (playerOnQueue) {
                                playerOnQueue = false
                                queue.show()
                            }
                        }
                        val backHere = layer.id == top?.id && !smartOpen
                        DismissibleScreen(
                            DismissDirection.Down,
                            onLeaving = { nav = nav.leave(layer.id) },
                            onDismissed = { nav = nav.remove(layer.id) },
                            backEnabled = backHere && !queue.open,
                            draggable = !queue.shown,
                            // Opened from the keyboard, the keys move about in it, not beneath it.
                            modifier = Modifier.focusArea(remember { FocusRequester() }, focusWhenShown = true),
                        ) { dismiss ->
                            FullPlayer(
                                playback,
                                track,
                                queue,
                                queueBackEnabled = backHere,
                                onCollapse = dismiss,
                                onAddToPlaylist = if (actions.editor == null) null else {
                                    { actions.addToPlaylist(Addition.Tracks(listOf(track.uid))) }
                                },
                                onMenu = trackMenu,
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
            val tagEditor = (source as? CoreLibrarySource)?.tagEditor
            tagTarget?.let { target ->
                if (tagEditor != null) key(target) {
                    TagEditorScreen(tagEditor, playback, target, onClose = { if (tagTarget == target) tagTarget = null })
                }
            }
            PlaylistSheets(
                actions,
                library,
                onPlay = playAll,
                // Closes the deleted playlist's or album group's page, if it's open.
                onDeleted = { ref -> nav = nav.removeAll { it == Page.Collection(ref) } },
                // Go to album or artist: its page, from the player too, which makes way for it.
                // The tablet layout opens it on its library page, as the desktops do.
                onOpen = { ref ->
                    val page = if (tablet) sectionOf(ref.kind) else null
                    if (page != null) {
                        tabletSection = page
                        nav = nav.removeAll { true }
                    } else {
                        nav = nav.removeAll { it == Page.Player }
                    }
                    open(ref)
                },
            )
            // Over everything: another device may ask to pair at any time.
            // Choose What to Sync: the device's page, over Sync as when it's tapped there.
            PairingDialogs(syncer, settings, newLibrary, ownLibrary = !stress && !demo, openDevice)
        }
    }
}

private val MiniPlayerMargin = 8.dp

/** One part of [Syncer.status] as state, so a change elsewhere in it doesn't recompose what reads this. */
@Composable
private fun <T> Syncer.statusPart(part: (SyncStatus) -> T): State<T> {
    val parts = remember(this) { status.map(part).distinctUntilChanged() }
    return parts.collectAsState(remember(this) { part(status.value) })
}

/**
 * Whether the window is short, as a phone is in landscape: screens then put their headers'
 * parts side by side, to leave room for the content beneath.
 */
@Composable
fun isShortWindow(): Boolean =
    with(LocalDensity.current) { LocalWindowInfo.current.containerSize.height.toDp() } < ShortWindowHeight

private val ShortWindowHeight = 480.dp

/** Hides a layer from accessibility services while another covers it. */
internal fun Modifier.coveredIf(covered: Boolean) = if (covered) clearAndSetSemantics {} else this

private val NavSaver = BackStack.saver(Page::encode, Page::decode)

private val TagTargetSaver = Saver<TagTarget?, String>(
    save = { target -> target?.let { "${it.kind.name}:${it.uid}" } },
    restore = { saved -> TagTarget(TagTarget.Kind.valueOf(saved.substringBefore(':')), saved.substringAfter(':')) },
)

/** A test script's request to run the scroll test after startup. */
object PerfRun {
    var requested = false
}
