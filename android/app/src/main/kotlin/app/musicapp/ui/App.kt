package app.musicapp.ui

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
import app.musicapp.AppSettings
import app.musicapp.data.CollectionRef
import app.musicapp.data.LibrarySource
import app.musicapp.importer.Importer
import app.musicapp.playback.Playback
import app.musicapp.sync.Syncer
import app.musicapp.ui.theme.ArtTheme
import java.io.File

/**
 * The app's one screen: the main list, with settings or track list screens stacked over it,
 * then the mini player, then the full player. Navigation is this stack of [CollectionRef]s
 * and flags for settings and the player, so each layer can own its slide-in, drag-to-dismiss
 * and predictive back. The players take their colours from the album art. [onPlay] is called
 * when a track list starts playing, and [onImportStarted] when an import starts. Pairing with
 * another device shows over everything, as it can start from the other device at any time. [libraryDir]
 * is the library folder, for settings to show its size.
 */
@Composable
fun App(
    source: LibrarySource,
    playback: Playback,
    importer: Importer,
    syncer: Syncer,
    settings: AppSettings,
    libraryDir: File,
    onPlay: () -> Unit = {},
    onImportStarted: () -> Unit = {},
) {
    val library by source.library.collectAsState()
    val importProgress by importer.progress.collectAsState()
    val stack = rememberSaveable(saver = StackSaver) { SnapshotStateList<CollectionRef>() }
    var settingsOpen by rememberSaveable { mutableStateOf(false) }
    var playerOpen by rememberSaveable { mutableStateOf(false) }
    var importOpen by rememberSaveable { mutableStateOf(false) }
    val track = playback.track

    // Lists end clear of the mini player, which sits above the navigation bar.
    var miniPlayerHeight by remember { mutableIntStateOf(0) }
    val navigationBar = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val bottomPadding = if (track == null) navigationBar
    else navigationBar + with(LocalDensity.current) { miniPlayerHeight.toDp() } + MiniPlayerMargin * 2

    val open: (CollectionRef) -> Unit = remember { { stack.add(it) } }
    val pop: () -> Unit = remember { { if (stack.isNotEmpty()) stack.removeAt(stack.lastIndex) } }

    // A Surface rather than a background, so text gets the surface's content colour.
    Surface(Modifier.fillMaxSize(), color = MaterialTheme.colorScheme.surface) {
        Box(Modifier.fillMaxSize()) {
            MainScreen(
                library,
                bottomPadding,
                onOpen = open,
                onImport = { importOpen = true },
                onSettings = { settingsOpen = true },
                banner = importProgress?.let { progress -> { modifier -> ImportCard(progress, importer, modifier) } },
                modifier = Modifier.coveredIf(stack.isNotEmpty() || playerOpen || settingsOpen),
            )
            if (settingsOpen) {
                DismissibleScreen(
                    DismissDirection.Right,
                    onDismissed = { settingsOpen = false },
                    backEnabled = !playerOpen,
                    modifier = Modifier.coveredIf(playerOpen),
                ) { dismiss ->
                    SettingsScreen(settings, syncer, libraryDir, bottomPadding, onBack = dismiss)
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
                            bottomPadding,
                            onPlay = { tracks, index ->
                                playback.play(tracks, index)
                                source.notePlayed(ref)
                                onPlay()
                            },
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
                        FullPlayer(playback, track, onCollapse = dismiss)
                    }
                }
            }
            if (importOpen) {
                ImportFlow(importer, settings, onStarted = onImportStarted, onClose = { importOpen = false })
            }
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
