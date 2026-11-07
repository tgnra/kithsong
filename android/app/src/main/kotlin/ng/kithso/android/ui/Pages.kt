package ng.kithso.android.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.key
import androidx.compose.ui.Modifier
import androidx.compose.ui.unit.Dp
import ng.kithso.android.AppSettings
import ng.kithso.android.data.Addition
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.FoundTrack
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.data.Track
import ng.kithso.android.playback.Playback
import ng.kithso.android.sync.Syncer
import java.io.File

/**
 * What pages need from [App]: its one copy of the library, the actions and the callbacks that
 * open things and play them, the same whichever layout a page is drawn in.
 */
internal class PageHost(
    val source: LibrarySource,
    val playback: Playback,
    val syncer: Syncer,
    val settings: AppSettings,
    val libraryDir: File,
    val actions: PlaylistActions,
    val binCount: Int?,
    val push: (Page) -> Unit,
    val open: (CollectionRef) -> Unit,
    val playFrom: (CollectionRef, List<Track>, Int) -> Unit,
    val playList: (CollectionRef, List<Track>, Boolean) -> Unit,
    val playFound: (FoundTrack) -> Unit,
    val foundTrackMenu: (FoundTrack) -> Unit,
    val runScrollTest: () -> Unit,
)

/**
 * A page's content, wherever it's placed: it fills what it's given, and [LocalPane] tells it the
 * rest. [bottomPadding] is what lists end clear of, and [dismiss] closes the page. The player
 * isn't here, as it needs a track: see [PlayerLayers].
 */
@Composable
internal fun PageContent(page: Page, host: PageHost, bottomPadding: Dp, dismiss: () -> Unit) {
    when (page) {
        Page.Settings -> SettingsScreen(
            host.settings, host.libraryDir, bottomPadding,
            onLicenses = { host.push(Page.Licenses) },
            onBack = dismiss,
            binCount = host.binCount,
            onRecycleBin = { host.push(Page.RecycleBin) },
            onRunScrollTest = host.runScrollTest,
        )
        Page.Stats -> StatsScreen(host.source, bottomPadding, onBack = dismiss)
        Page.Licenses -> LicensesScreen(bottomPadding, onOpen = { host.push(Page.License(it)) }, onBack = dismiss)
        is Page.License -> LicenseTextScreen(page.asset, bottomPadding, onBack = dismiss)
        Page.Sync -> SyncScreen(
            host.syncer, bottomPadding, onOpenDevice = { host.push(Page.Device(it)) }, onBack = dismiss,
            onOpenBin = { host.push(Page.RecycleBin) },
        )
        Page.RecycleBin -> RecycleBinScreen(host.syncer, host.binCount ?: 0, bottomPadding, onBack = dismiss)
        is Page.Device -> DeviceScreen(host.syncer, page.uid, bottomPadding, onBack = dismiss, onRename = host.syncer::rename)
        Page.Search -> SearchScreen(
            host.source,
            bottomPadding,
            onOpen = host.open,
            onPlayTrack = host.playFound,
            onTrackMenu = host.foundTrackMenu,
            onBack = dismiss,
        )
        is Page.Collection -> DetailScreen(
            page.ref,
            host.source,
            host.actions,
            bottomPadding,
            onPlay = { tracks, index ->
                // From an album, in order from the track, as the desktops do.
                if (page.ref.kind == CollectionRef.Kind.Album && host.settings.albumInOrder) host.playback.setShuffle(false)
                host.playFrom(page.ref, tracks, index)
            },
            onPlayAll = { tracks, shuffle -> host.playList(page.ref, tracks, shuffle) },
            onOpen = host.open,
            onBack = dismiss,
        )
        Page.Player -> Unit
    }
}

/**
 * Every open page but the player, which goes over the mini player, each sliding in over the last
 * in the pane this is called in. [backEnabled] is false while something over them all has back.
 * They slide in from [from], and [slides] follows each, for what moves with them. [onShown] is
 * called with a layer's id once it's fully there.
 */
@Composable
internal fun PageLayers(
    nav: BackStack<Page>,
    backEnabled: Boolean,
    onLeaving: (Int) -> Unit,
    onDismissed: (Int) -> Unit,
    onShown: (Int) -> Unit,
    host: PageHost,
    bottomPadding: Dp,
    slides: PageSlides,
    from: DismissDirection,
) {
    val top = nav.top
    nav.layers.forEachIndexed { index, layer ->
        if (layer.page != Page.Player) key(layer.id) {
            DismissibleScreen(
                from,
                onLeaving = { onLeaving(layer.id) },
                onDismissed = { onDismissed(layer.id) },
                backEnabled = layer.id == top?.id && backEnabled,
                modifier = Modifier.coveredIf(layer.id != top?.id),
                // Beneath a page shifting home is the column moving away from it, not a page it's over.
                scrim = !layer.shifted || index > 0,
                onShown = { onShown(layer.id) },
                onSlide = { slides.set(layer.id, it) },
            ) { dismiss ->
                PageContent(layer.page, host, bottomPadding, dismiss)
            }
        }
    }
}

/**
 * The full player, in the pane this is called in. Each opening is a layer of its own, so one
 * tapped open while the last is still sliding out doesn't take that one over.
 */
@Composable
internal fun PlayerLayers(
    nav: BackStack<Page>,
    backEnabled: Boolean,
    onLeaving: (Int) -> Unit,
    onDismissed: (Int) -> Unit,
    playback: Playback,
    track: Track,
    actions: PlaylistActions,
    onMenu: () -> Unit,
) {
    val top = nav.top
    nav.layers.forEach { layer ->
        if (layer.page == Page.Player) key(layer.id) {
            // Back and dragging down are the queue's while it's open.
            val queue = rememberQueueSlide()
            val backHere = layer.id == top?.id && backEnabled
            DismissibleScreen(
                DismissDirection.Down,
                onLeaving = { onLeaving(layer.id) },
                onDismissed = { onDismissed(layer.id) },
                backEnabled = backHere && !queue.open,
                draggable = !queue.shown,
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
                    onMenu = onMenu,
                )
            }
        }
    }
}
