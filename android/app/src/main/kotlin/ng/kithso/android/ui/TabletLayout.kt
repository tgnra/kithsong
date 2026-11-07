package ng.kithso.android.ui

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.displayCutout
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.systemBars
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.CompositionLocalProvider
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.runtime.key
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clipToBounds
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.input.InputMode
import androidx.compose.ui.platform.LocalInputModeManager
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.LibrarySource
import ng.kithso.android.playback.Playback
import ng.kithso.android.sync.Syncer
import ng.kithso.android.ui.art.PrefetchArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberOnScreenArtColors
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.launch

/**
 * Draws one of the [BackStack]'s pages, as the phone's layers do: [App] has the one place
 * that knows how to make each, and the tablet layout uses it too. [onOpen] opens what's picked
 * on the page, and dismiss closes it.
 */
typealias PageContent = @Composable (page: Page, bottomPadding: Dp, onOpen: (CollectionRef) -> Unit, dismiss: () -> Unit) -> Unit

/**
 * The narrowest window, less its system bars and cutouts at the sides, for the tablet layout:
 * the sidebar ([SidebarWidth], 240dp) and a content area with room for two album columns at
 * their full width with the gutters (344dp), rounded up to Material's medium width class. A
 * phone held sideways is wider than this, but too short for the sidebar, so it keeps the
 * phone layout.
 */
internal val TabletMinWidth = 600.dp

/** The narrowest the album grid gets beside the track list's pane: two columns, as [TabletMinWidth] counts. */
private val MinGridWidth = 344.dp

/** The track list's pane beside the grid, as wide as the desktops' about. */
private val PaneWidth = 360.dp

/** The narrowest the pane gets when it starts at the fold. */
private val MinPaneWidth = 320.dp

/** An [ArtTile]'s corners, for the mark on one that something's dragged over. */
private val TileShape = RoundedCornerShape(12.dp)

/**
 * Whether [shape] takes the tablet layout: wide enough for the sidebar beside a usable content
 * area ([TabletMinWidth]), and tall enough for the top bar and the whole of the sidebar that
 * doesn't scroll ([sidebarFixedHeight]), at the text size the user has picked. It goes by the
 * window, not the device, so a window resized, folded or unfolded switches as it changes. The
 * keyboard doesn't count against the height, or typing in Search would switch layouts.
 */
@Composable
internal fun useTabletLayout(shape: WindowShape): Boolean {
    val density = LocalDensity.current
    val direction = LocalLayoutDirection.current
    val bars = WindowInsets.systemBars.union(WindowInsets.displayCutout)
    return with(density) {
        val width = shape.width - (bars.getLeft(density, direction) + bars.getRight(density, direction)).toDp()
        val height = shape.height - (bars.getTop(density) + bars.getBottom(density)).toDp()
        width >= TabletMinWidth && height >= TopBarHeight + sidebarFixedHeight(fontScale)
    }
}

/** No insets, for a page whose side or top something else keeps clear of the system bars. */
private val NoInsets = WindowInsets(0, 0, 0, 0)

/** The pages the sidebar opens, which the tablet layout shows in the content area with nothing beneath. */
private fun Page.isSidebarPage(): Boolean =
    this == Page.Search || this == Page.Settings || this == Page.Stats || this == Page.Sync || this is Page.Device

/** The library page that lists what [kind] is, for opening a recent there. */
internal fun sectionOf(kind: CollectionRef.Kind): TabletSection? = when (kind) {
    CollectionRef.Kind.Album -> TabletSection.Albums
    CollectionRef.Kind.Artist -> TabletSection.Artists
    CollectionRef.Kind.Playlist -> TabletSection.Playlists
    CollectionRef.Kind.AlbumGroup -> TabletSection.AlbumGroups
    CollectionRef.Kind.Station -> null
}

/** How wide the sidebar is, and the gap after it the fold takes when the sidebar reaches it. */
@Immutable
private data class SidebarFit(val width: Dp, val gap: Dp)

/**
 * The sidebar's width beside [fold]. A fold just past the sidebar would leave a strip of the
 * content area before it too narrow for a column of tiles, and that strip would sit empty: so
 * the sidebar reaches to the fold instead, and the content starts after it, as two pages of a
 * book. A fold further along is the grid's to keep clear of ([gridColumns]), and one in the
 * sidebar's own width doesn't matter to a list. [startPx] is the inset at the sidebar's side.
 */
private fun sidebarFit(fold: Fold?, windowWidthPx: Float, startPx: Float, rtl: Boolean, density: Density): SidebarFit {
    if (fold == null || !fold.vertical) return SidebarFit(SidebarWidth, 0.dp)
    return with(density) {
        val start = ((if (rtl) windowWidthPx - fold.bounds.right else fold.bounds.left) - startPx).toDp()
        val end = ((if (rtl) windowWidthPx - fold.bounds.left else fold.bounds.right) - startPx).toDp()
        val beforeFold = start - SidebarWidth - BrowseGutter - FoldClearance
        if (start > SidebarWidth && beforeFold < MinTileWidth) SidebarFit(start, end - start) else SidebarFit(SidebarWidth, 0.dp)
    }
}

/**
 * The layout for a window big enough, as the desktops': the [TopBar] across the top, the
 * [Sidebar] down the side, and the content beside it. It shares the phone's state: [nav] is
 * the same back stack, changed through [updateNav], so turning or folding the device between
 * the two layouts keeps what's open, and the players, editors, menus and dialogs over it are
 * [App]'s, drawn over this as over the phone's.
 *
 * The content is the library page picked in the sidebar ([section]), or a page the sidebar
 * opened (Search, Settings, Library stats, a device), which is then at the bottom of [nav].
 * Pages opened from there come over it, sliding in as on the phone. A track list, though,
 * shows in a pane at the end of the content, as on the desktops, while there's room for it
 * beside a grid of two columns, and over the content otherwise; opening another from the grid
 * or Search replaces it rather than stacking up. If the window has a fold in the content, the
 * pane starts at the fold where both sides have room for that.
 *
 * Tracks and albums can be dragged onto manual playlists and album groups here, as on the
 * desktops ([DragDrop]); the phone layout has no room for both at once, and keeps its long press.
 *
 * [onOpen] is [App]'s, which opens a track list (or plays a station), [onOpenPlayer] opens the
 * full player, showing the queue if asked, and [onTrackMenu] the playing track's menu.
 * [covered] says the player or an editor is over this, which then has neither back nor
 * TalkBack. [banner] is the import's and sync's cards, over the library's pages.
 */
@Composable
internal fun TabletLayout(
    nav: BackStack<Page>,
    updateNav: ((BackStack<Page>) -> BackStack<Page>) -> Unit,
    section: TabletSection,
    onSection: (TabletSection) -> Unit,
    shape: WindowShape,
    library: Library?,
    source: LibrarySource,
    playback: Playback,
    syncer: Syncer,
    actions: PlaylistActions,
    importPickers: ImportPickers,
    importRunning: Boolean,
    binDot: Boolean,
    banner: (@Composable (Modifier) -> Unit)?,
    covered: Boolean,
    onOpen: (CollectionRef) -> Unit,
    onOpenPlayer: (queue: Boolean) -> Unit,
    onTrackMenu: () -> Unit,
    pageContent: PageContent,
    modifier: Modifier = Modifier,
) {
    val density = LocalDensity.current
    val rtl = LocalLayoutDirection.current == LayoutDirection.Rtl
    val bottomPadding = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()
    val startInset = WindowInsets.safeDrawing.only(WindowInsetsSides.Start)
    val startPx = (if (rtl) startInset.getRight(density, LayoutDirection.Rtl) else startInset.getLeft(density, LayoutDirection.Ltr)).toFloat()
    val windowWidthPx = LocalWindowInfo.current.containerSize.width.toFloat()
    val fit = sidebarFit(shape.fold, windowWidthPx, startPx, rtl, density)

    val open = nav.layers.filter { it.page != Page.Player }
    val root = open.firstOrNull()?.takeIf { it.page.isSidebarPage() }
    val top = nav.top
    // The keyboard's focus, which comes back to where it was when the player or an editor that
    // covered this goes.
    val scope = rememberCoroutineScope()
    val focus = remember(scope) { TabletFocus(scope) }
    val inputMode = LocalInputModeManager.current
    var wasCovered by remember { mutableStateOf(covered) }
    LaunchedEffect(covered) {
        if (wasCovered && !covered && inputMode.inputMode == InputMode.Keyboard) {
            withFrameNanos {}
            focus.window.focusIfThere()
        }
        wasCovered = covered
    }
    // Everything but the player goes: the sidebar starts afresh, as the desktops' does.
    val clear = { stack: BackStack<Page> -> stack.removeAll { it != Page.Player } }
    // From the grid or Search, a track list takes the place of the one in the pane.
    val openFromBase: (CollectionRef) -> Unit = { ref ->
        if (ref.kind != CollectionRef.Kind.Station) updateNav { stack -> stack.removeAll { it is Page.Collection } }
        onOpen(ref)
    }

    // Drag and drop is the tablet layout's, as the phone's long press opens menus.
    val dragDrop = rememberDragDrop(actions.editor, library)
    CompositionLocalProvider(LocalDragDrop provides dragDrop) {
        Column(modifier.fillMaxSize().focusArea(focus.window, covered = covered)) {
            TopBar(
                playback,
                onOpenPlayer = { onOpenPlayer(false) },
                onQueue = { onOpenPlayer(true) },
                onMenu = onTrackMenu,
                modifier = Modifier.coveredIf(covered).focusArea(focus.topBar),
            )
            Row(Modifier.weight(1f).fillMaxWidth()) {
                CompositionLocalProvider(LocalTabExit provides focus.sidebarExit) {
                    Sidebar(
                        section,
                        root?.page,
                        library?.recents.orEmpty(),
                        syncer,
                        binDot,
                        importPickers,
                        importRunning,
                        onSection = { picked ->
                            onSection(picked)
                            updateNav(clear)
                        },
                        onPage = { page ->
                            // Picked again, it stays as it is, with what's typed in Search.
                            if (root?.page != page || open.size > 1) updateNav { clear(it).push(page) }
                        },
                        // On its library page, as the desktops open it; a station plays.
                        onRecent = { recent ->
                            sectionOf(recent.ref.kind)?.let { picked ->
                                onSection(picked)
                                updateNav(clear)
                            }
                            onOpen(recent.ref)
                        },
                        onRecentMenu = actions::recentMenu,
                        onDevice = { uid -> updateNav { clear(it).push(Page.Device(uid)) } },
                        modifier = Modifier
                            .width(fit.width + with(density) { startPx.toDp() })
                            .coveredIf(covered)
                            .focusArea(focus.sidebar),
                    )
                }
                if (fit.gap > 0.dp) Spacer(Modifier.width(fit.gap))
                TabletContent(
                    updateNav, open, root, top, section, shape.fold, rtl, bottomPadding, covered,
                    pageContent, openFromBase, onOpen, banner, library, source, actions, importPickers, focus,
                    Modifier.weight(1f).fillMaxHeight().coveredIf(covered),
                )
            }
        }
    }
}

/**
 * The content beside the sidebar: the library page or the page the sidebar opened at the
 * bottom, what's opened over it, and the track list's pane when there's room. See [TabletLayout].
 */
@Composable
private fun TabletContent(
    updateNav: ((BackStack<Page>) -> BackStack<Page>) -> Unit,
    open: List<BackLayer<Page>>,
    root: BackLayer<Page>?,
    top: BackLayer<Page>?,
    section: TabletSection,
    fold: Fold?,
    rtl: Boolean,
    bottomPadding: Dp,
    covered: Boolean,
    pageContent: PageContent,
    openFromBase: (CollectionRef) -> Unit,
    onOpen: (CollectionRef) -> Unit,
    banner: (@Composable (Modifier) -> Unit)?,
    library: Library?,
    source: LibrarySource,
    actions: PlaylistActions,
    importPickers: ImportPickers,
    focus: TabletFocus,
    modifier: Modifier,
) {
    BoxWithConstraints(modifier) {
        val width = maxWidth
        val contentFold = rememberGridFold(fold)
        // The fold from the content's start, as the pane goes at its end.
        val span = contentFold.span?.let { if (rtl) FoldSpan(width - it.end, width - it.start) else it }
        val atFold = span != null && span.start >= MinGridWidth && width - span.end >= MinPaneWidth
        val paneLayers = open.filter { it.page is Page.Collection }
        val split = paneLayers.isNotEmpty() && (atFold || width >= MinGridWidth + PaneWidth)
        val baseWidth = if (!split) width else if (atFold) span.start else width - PaneWidth
        val paneStart = if (atFold) span.end else width - PaneWidth
        val end = WindowInsets.safeDrawing.only(WindowInsetsSides.End)
        // Whatever's at the window's edge keeps clear of what's there; the pane, when it's showing, is.
        val baseChrome = PageChrome(top = NoInsets, sides = if (split) NoInsets else end, back = PageBack.None, inTablet = true)
        // The top page over the base, and the pane's top page, which the keyboard's focus goes to.
        val overlay = open.lastOrNull { !it.leaving && it != root && !(split && it.page is Page.Collection) }?.id
        val paneTop = if (split) paneLayers.lastOrNull { !it.leaving }?.id else null
        SideEffect { focus.update(overlay, paneTop, open) }
        val toContent = { focus.later(focus::focusContent) }

        Box(Modifier.fillMaxSize().then(contentFold.modifier)) {
            Box(Modifier.width(baseWidth).fillMaxHeight()) {
                CompositionLocalProvider(LocalPageChrome provides baseChrome, LocalTabExit provides focus.contentExit) {
                    if (root != null) {
                        key(root.id) {
                            val close = { updateNav { it.remove(root.id) } }
                            // Back from it goes to the library page beneath, as nothing slides away.
                            BackHandler(enabled = root.id == top?.id && !covered, onBack = close)
                            Surface(
                                color = MaterialTheme.colorScheme.surface,
                                modifier = Modifier
                                    .fillMaxSize()
                                    .coveredIf(root.id != top?.id)
                                    .focusArea(focus.base, covered = overlay != null, onGone = toContent),
                            ) {
                                pageContent(root.page, bottomPadding, openFromBase, close)
                            }
                        }
                    } else {
                        // Keyed by the page, so the focus on a tile that goes with it goes to the next page.
                        key(section) {
                            LibraryPage(
                                section, library, source, actions, importPickers, fold, bottomPadding, banner, openFromBase, onOpen,
                                Modifier.coveredIf(top != null).focusArea(focus.base, covered = overlay != null, onGone = toContent),
                            )
                        }
                    }
                }
                CompositionLocalProvider(
                    LocalPageChrome provides baseChrome.copy(back = PageBack.Back),
                    LocalTabExit provides focus.contentExit,
                ) {
                    for (layer in open) {
                        if (layer == root || (split && layer.page is Page.Collection)) continue
                        key(layer.id) {
                            // Opened from the keyboard, it takes the focus from the page beneath.
                            val layerFocus = Modifier.focusArea(
                                focus.layer(layer.id),
                                covered = layer.id != overlay,
                                focusWhenShown = true,
                                onGone = toContent,
                            )
                            PageLayer(layer, top, covered, bottomPadding, updateNav, pageContent, onOpen, layerFocus)
                        }
                    }
                }
            }
            if (split) {
                Box(
                    Modifier
                        .align(Alignment.TopStart)
                        .offset(x = paneStart)
                        .width(width - paneStart)
                        .fillMaxHeight()
                        .clipToBounds()
                        .onFocusChanged { focus.paneHasFocus = it.hasFocus },
                ) {
                    CompositionLocalProvider(
                        LocalPageChrome provides PageChrome(top = NoInsets, sides = end, back = PageBack.Close, inTablet = true),
                        LocalTabExit provides focus.paneExit,
                    ) {
                        for (layer in paneLayers) key(layer.id) {
                            // Opened from a tile, the focus stays on the tile; from the pane, it goes to the new page.
                            val layerFocus = Modifier.focusArea(
                                focus.layer(layer.id),
                                covered = layer.id != paneTop,
                                focusWhenShown = focus.paneHasFocus,
                                onGone = { focus.later(focus::focusPane) },
                            )
                            PageLayer(layer, top, covered, bottomPadding, updateNav, pageContent, onOpen, layerFocus)
                        }
                    }
                }
            }
        }
    }
}

/**
 * Where the keyboard's focus goes in the tablet layout. Tab goes from the top bar to the sidebar,
 * the content and the pane, then back to the top bar, and Shift+Tab the other way; a list is
 * one stop, as on the desktops, with the arrows to move about in it (listItemKeys(), [TabExit]),
 * and each part comes back to where the focus was in it. In the content, it goes to the top page
 * opened over the library page, if there is one. A page that goes with the focus in it, closed
 * with Escape say, gives it to what's beneath: the next page down, or the tile it was opened from.
 */
@Stable
private class TabletFocus(private val scope: CoroutineScope) {
    val window = FocusRequester()
    val topBar = FocusRequester()
    val sidebar = FocusRequester()

    /** The library page, or the page the sidebar opened. */
    val base = FocusRequester()

    /** Whether the pane has the focus, so a page opened from it takes it. Nothing draws from it. */
    var paneHasFocus = false

    private val layers = mutableMapOf<Int, FocusRequester>()
    private var overlay: Int? = null
    private var paneTop: Int? = null

    /** What [layer] focuses with. */
    fun layer(id: Int): FocusRequester = layers.getOrPut(id) { FocusRequester() }

    /** The top page over the base and in the pane, now that the pages open are [open]. */
    fun update(overlay: Int?, paneTop: Int?, open: List<BackLayer<Page>>) {
        this.overlay = overlay
        this.paneTop = paneTop
        layers.keys.retainAll(open.map { it.id }.toSet())
    }

    fun focusContent() = (overlay?.let(::layer) ?: base).focusIfThere()

    fun focusPane() {
        val pane = paneTop
        if (pane == null) focusContent() else layer(pane).focusIfThere()
    }

    /** Runs [action] once what's just been composed is there to focus. */
    fun later(action: () -> Unit) {
        scope.launch {
            withFrameNanos {}
            action()
        }
    }

    val sidebarExit = TabExit(next = ::focusContent, previous = { topBar.focusIfThere() })
    val contentExit = TabExit(next = { if (paneTop != null) focusPane() else topBar.focusIfThere() }, previous = { sidebar.focusIfThere() })
    val paneExit = TabExit(next = { topBar.focusIfThere() }, previous = ::focusContent)
}

/** A page over the content or in the pane, which slides in and is dismissed as the phone's are. */
@Composable
private fun PageLayer(
    layer: BackLayer<Page>,
    top: BackLayer<Page>?,
    covered: Boolean,
    bottomPadding: Dp,
    updateNav: ((BackStack<Page>) -> BackStack<Page>) -> Unit,
    pageContent: PageContent,
    onOpen: (CollectionRef) -> Unit,
    modifier: Modifier,
) {
    DismissibleScreen(
        DismissDirection.Right,
        onLeaving = { updateNav { it.leave(layer.id) } },
        onDismissed = { updateNav { it.remove(layer.id) } },
        backEnabled = layer.id == top?.id && !covered,
        modifier = Modifier.coveredIf(layer.id != top?.id).then(modifier),
    ) { dismiss ->
        pageContent(layer.page, bottomPadding, onOpen, dismiss)
    }
}

/**
 * One of the library's pages, under [banner]: Home, or a grid of albums, playlists or album
 * groups, or the list of artists. Opening something calls [onOpen]; [onPlay] plays a station.
 * Long-pressing something opens its menu, as on the phone's main screen; an album's tile can be
 * dragged, and a manual playlist's or album group's tile takes a track or album dropped on it
 * ([DragDrop]). Playlists' and Album groups' headings have the + that makes a new one.
 */
@Composable
private fun LibraryPage(
    section: TabletSection,
    library: Library?,
    source: LibrarySource,
    actions: PlaylistActions,
    importPickers: ImportPickers,
    fold: Fold?,
    bottomPadding: Dp,
    banner: (@Composable (Modifier) -> Unit)?,
    onOpen: (CollectionRef) -> Unit,
    onPlay: (CollectionRef) -> Unit,
    modifier: Modifier,
) {
    val sides = pageSides()
    val canMake = actions.editor != null
    Surface(color = MaterialTheme.colorScheme.surface, modifier = modifier.fillMaxSize()) {
        Column(Modifier.fillMaxSize()) {
            banner?.invoke(Modifier.windowInsetsPadding(sides).padding(start = BrowseGutter, end = BrowseGutter, top = 12.dp))
            val page = Modifier.weight(1f).fillMaxWidth()
            when (section) {
                TabletSection.Home -> HomePage(
                    source,
                    actions,
                    onOpen = onOpen,
                    onPlayStation = onPlay,
                    importPickers = importPickers,
                    modifier = page.windowInsetsPadding(sides),
                    contentPadding = PaddingValues(top = 8.dp, bottom = bottomPadding),
                )
                TabletSection.Albums -> GridPage(
                    stringResource(R.string.section_albums),
                    library?.albums,
                    key = { it.uid },
                    art = { it.art },
                    fold, bottomPadding,
                    empty = stringResource(R.string.no_albums),
                    modifier = page,
                ) { album ->
                    ArtTile(
                        album.title,
                        album.artist,
                        album.art,
                        onLongClick = { actions.albumMenu(album) },
                        onClick = { onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid)) },
                        modifier = Modifier,
                        drag = DragItem.Album(album.uid, album.title, album.artist),
                    )
                }
                TabletSection.Playlists -> GridPage(
                    stringResource(R.string.section_playlists),
                    library?.playlists,
                    key = { it.uid },
                    art = { it.art },
                    fold, bottomPadding,
                    empty = stringResource(R.string.no_playlists_title),
                    onNew = if (canMake) ({ actions.newList(false) }) else null,
                    newLabel = stringResource(R.string.new_playlist_menu),
                    modifier = page,
                ) { playlist ->
                    ArtTile(
                        playlist.name,
                        pluralStringResource(R.plurals.tracks, playlist.trackCount, playlist.trackCount),
                        playlist.art,
                        onLongClick = { actions.playlistMenu(playlist) },
                        onClick = { onOpen(CollectionRef(CollectionRef.Kind.Playlist, playlist.uid)) },
                        modifier = Modifier.dropTarget(CollectionRef(CollectionRef.Kind.Playlist, playlist.uid), playlist.name, TileShape),
                    )
                }
                TabletSection.AlbumGroups -> GridPage(
                    stringResource(R.string.section_album_groups),
                    library?.albumGroups,
                    key = { it.uid },
                    art = { it.art },
                    fold, bottomPadding,
                    empty = stringResource(R.string.no_album_groups_title),
                    onNew = if (canMake) ({ actions.newList(true) }) else null,
                    newLabel = stringResource(R.string.new_album_group_menu),
                    modifier = page,
                ) { group ->
                    ArtTile(
                        group.name,
                        pluralStringResource(R.plurals.albums, group.albumCount, group.albumCount),
                        group.art,
                        onLongClick = { actions.albumGroupMenu(group) },
                        onClick = { onOpen(CollectionRef(CollectionRef.Kind.AlbumGroup, group.uid)) },
                        modifier = Modifier.dropTarget(CollectionRef(CollectionRef.Kind.AlbumGroup, group.uid), group.name, TileShape),
                    )
                }
                TabletSection.Artists -> {
                    val artists = library?.artists
                    LazyColumn(
                        contentPadding = PaddingValues(bottom = bottomPadding),
                        modifier = page.windowInsetsPadding(sides),
                    ) {
                        item(key = "heading") { PageHeading(stringResource(R.string.section_artists)) }
                        if (artists != null && artists.isEmpty()) item(key = "empty") { EmptyNote(stringResource(R.string.no_artists)) }
                        items(artists.orEmpty(), key = { it.uid }) { artist ->
                            LibraryRow(
                                artist.name,
                                artist.subtitle(),
                                artist.art,
                                CircleShape,
                                onLongClick = { actions.artistMenu(artist) },
                            ) { onOpen(CollectionRef(CollectionRef.Kind.Artist, artist.uid)) }
                        }
                    }
                }
            }
        }
    }
}

/**
 * A library page's grid of [items] (null while the library is being read), under its heading,
 * with [onNew]'s + ([newLabel] for TalkBack) at the heading's end, if given, and [empty] said
 * when there's nothing. Its columns keep clear of the window's [fold] ([gridColumns]), wherever
 * the grid is in the window, and over it the background takes its colours from the art on
 * screen, as the phone's main screen's does.
 */
@Composable
private fun <T> GridPage(
    title: String,
    items: List<T>?,
    key: (T) -> String,
    art: (T) -> ArtRef,
    fold: Fold?,
    bottomPadding: Dp,
    empty: String,
    modifier: Modifier,
    onNew: (() -> Unit)? = null,
    newLabel: String = "",
    tile: @Composable (T) -> Unit,
) {
    BoxWithConstraints(modifier) {
        val density = LocalDensity.current
        val direction = LocalLayoutDirection.current
        val sides = pageSides()
        val gridFold = rememberGridFold(fold)
        val width = maxWidth - with(density) { (sides.getLeft(density, direction) + sides.getRight(density, direction)).toDp() }
        val columns = gridColumns(width, gridFold.span)
        val rows = remember(items, columns.count) { items.orEmpty().chunked(columns.count) }
        val listState = rememberLazyListState()
        // The heading's item comes before the rows.
        val artAt = { index: Int -> rows.getOrNull(index - 1)?.map(art).orEmpty() }
        PrefetchArt(listState, with(density) { columns.tileWidth.roundToPx() }, artAt = artAt)
        ArtBackdrop(rememberOnScreenArtColors(listState, artAt), Modifier.matchParentSize())
        LazyColumn(
            state = listState,
            contentPadding = PaddingValues(bottom = bottomPadding),
            modifier = Modifier.fillMaxSize().windowInsetsPadding(sides).then(gridFold.modifier),
        ) {
            item(key = "heading", contentType = "heading") { PageHeading(title, onNew, newLabel) }
            if (items != null && items.isEmpty()) item(key = "empty", contentType = "empty") { EmptyNote(empty) }
            items(rows, key = { "row:" + key(it.first()) }, contentType = { "row" }) { row ->
                GridRow(columns, row.size) { i -> tile(row[i]) }
            }
        }
    }
}

/** A library page's heading, with [onNew]'s + at its end if there's one. */
@Composable
private fun PageHeading(title: String, onNew: (() -> Unit)? = null, newLabel: String = "") {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 64.dp).padding(start = BrowseGutter, end = 4.dp, top = 8.dp, bottom = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            title,
            style = MaterialTheme.typography.headlineMedium,
            modifier = Modifier.weight(1f).semantics { heading() },
        )
        if (onNew != null) {
            IconButton(onClick = onNew) { Icon(Icons.Add, contentDescription = newLabel) }
        }
    }
}

/** What a library page with nothing on it says. */
@Composable
private fun EmptyNote(text: String) {
    Text(
        text,
        style = MaterialTheme.typography.bodyLarge,
        color = MaterialTheme.colorScheme.onSurfaceVariant,
        modifier = Modifier.padding(horizontal = BrowseGutter, vertical = 24.dp),
    )
}
