package ng.kithso.android.ui

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.BoxWithConstraintsScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.pager.HorizontalPager
import androidx.compose.foundation.pager.PagerState
import androidx.compose.foundation.pager.rememberPagerState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PrimaryScrollableTabRow
import androidx.compose.material3.Tab
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.launch
import ng.kithso.android.R
import ng.kithso.android.data.Album
import ng.kithso.android.data.AlbumGroup
import ng.kithso.android.data.ArtRef
import ng.kithso.android.data.Artist
import ng.kithso.android.data.CollectionRef
import ng.kithso.android.data.Library
import ng.kithso.android.data.Playlist
import ng.kithso.android.data.Recent
import ng.kithso.android.data.Station
import ng.kithso.android.ui.art.PrefetchArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.ArtColor
import ng.kithso.android.ui.theme.rememberOnScreenArtColors

// The home screen of a window too small for the main list: a page across for each of the
// library's sections, each a grid going down, and last the page of what the main list's header
// has in its menu. Which of this and the main list a window gets is decided in [HomeFrame].

/** A page of [SmallHome]. */
@Immutable
internal sealed interface HomePage {
    val key: String

    /** A section of the library, as a grid. */
    data class Category(val section: Section) : HomePage {
        override val key get() = "section:$section"
    }

    /** No music here yet, and where it can come from. */
    data object Welcome : HomePage {
        override val key get() = "welcome"
    }

    /** What the main list's header has in its menu, as a list. */
    data object Utilities : HomePage {
        override val key get() = "utilities"
    }
}

/** The pages for [library]: those of its sections with something in them, in the main list's order, then the utilities. */
internal fun homePages(library: Library): List<HomePage> = buildList {
    if (library.isEmpty) add(HomePage.Welcome)
    if (library.stations.isNotEmpty()) add(HomePage.Category(Section.Radio))
    if (library.recents.isNotEmpty()) add(HomePage.Category(Section.Recents))
    if (library.playlists.isNotEmpty()) add(HomePage.Category(Section.Playlists))
    if (library.albumGroups.isNotEmpty()) add(HomePage.Category(Section.AlbumGroups))
    if (library.albums.isNotEmpty()) add(HomePage.Category(Section.Albums))
    if (library.artists.isNotEmpty()) add(HomePage.Category(Section.Artists))
    add(HomePage.Utilities)
}

private val Gutter = 8.dp
private val GridSpacing = 8.dp
private val MinTileWidth = 104.dp

/** How many tiles fit across [width] on a page. */
internal fun smallColumns(width: Dp): Int =
    ((width - Gutter * 2 + GridSpacing) / (MinTileWidth + GridSpacing)).toInt().coerceAtLeast(2)

/** A tile's title and artist, under its art and with the space below the row. */
private val TileLabelsHeight = 52.dp

/** How many tiles a page has before the fast scroll strip has letters to go by. */
private const val LETTERS_FROM = 12

/**
 * The home screen for [LocalWindowLayout]: [small] in a [WindowLayoutClass.Small] window, else
 * [list]. Each keeps what it's saved while the other shows, so a phone folded to its cover
 * screen and opened again finds the list where it was.
 */
@Composable
internal fun HomeFrame(modifier: Modifier, small: @Composable () -> Unit, list: @Composable BoxWithConstraintsScope.() -> Unit) {
    val homes = rememberSaveableStateHolder()
    if (LocalWindowLayout.current.layoutClass == WindowLayoutClass.Small) {
        homes.SaveableStateProvider("small", small)
    } else {
        homes.SaveableStateProvider("list") { BoxWithConstraints(modifier.fillMaxSize(), content = list) }
    }
}

/** What [SmallHome]'s pages do, as [MainScreen] takes them. */
internal class SmallHomeActions(
    val onOpen: (CollectionRef) -> Unit,
    val onSearch: () -> Unit,
    val onSync: () -> Unit,
    val onSettings: () -> Unit,
    val onStats: () -> Unit,
    val onChooseWhatToSync: () -> Unit,
    val onStationMenu: (Station) -> Unit,
    val onRecentMenu: (Recent) -> Unit,
    val onPlaylistMenu: (Playlist) -> Unit,
    val onAlbumMenu: (Album) -> Unit,
    val onArtistMenu: (Artist) -> Unit,
    val onAlbumGroupMenu: (AlbumGroup) -> Unit,
    val onNewList: ((isAlbums: Boolean) -> Unit)?,
)

/**
 * The pages, under a strip of their titles. As in [MainScreen], [bottomPadding] keeps what's last
 * on a page clear of the mini player, [banner] goes at the top of every page, and [syncDot] is the
 * red dot, here on the strip's last title and on Sync. Nothing shows while the library is being
 * read, so the pages, which are only known then, find their saved place. Back isn't this
 * screen's: it leaves the app from any page, as it does from the main list.
 */
@Composable
internal fun SmallHome(
    library: Library?,
    bottomPadding: Dp,
    actions: SmallHomeActions,
    importPickers: ImportPickers,
    importRunning: Boolean,
    banner: (@Composable (Modifier) -> Unit)?,
    inGroup: Boolean,
    syncDot: Boolean,
    modifier: Modifier,
) {
    // The scroll test has no one list to run on here, so it goes on to the track list.
    LaunchedEffect(Unit) {
        ScrollTest.step.collect { step -> if (step == ScrollTest.Step.Main) ScrollTest.step.value = ScrollTest.Step.OpenTracks }
    }
    Box(modifier.fillMaxSize()) {
        if (library == null) return@Box
        val pages = remember(library) { homePages(library) }
        val pager = rememberPagerState { pages.size }
        // The page in view's, kept while a page with no art of its own shows.
        var colors by remember { mutableStateOf(emptyList<ArtColor>()) }
        ArtBackdrop(colors, Modifier.matchParentSize())
        Column(Modifier.fillMaxSize()) {
            PageTabs(pages, pager, syncDot)
            HorizontalPager(
                pager,
                key = { pages[it].key },
                modifier = Modifier
                    .weight(1f)
                    .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal).inPane()),
            ) { index ->
                val frame = PageFrame(bottomPadding, banner, actions.onSearch) { found ->
                    if (index == pager.currentPage && found.isNotEmpty()) colors = found
                }
                when (val page = pages[index]) {
                    is HomePage.Category -> CategoryPage(page.section, library, frame, actions)
                    HomePage.Welcome -> WelcomePage(frame, actions, importPickers, inGroup)
                    HomePage.Utilities -> UtilitiesPage(library, frame, actions, importPickers, importRunning, syncDot)
                }
            }
        }
    }
}

/** What's the same on every page: see [SmallHome]. [onColors] is told the colours of the art on it. */
private class PageFrame(
    val bottomPadding: Dp,
    val banner: (@Composable (Modifier) -> Unit)?,
    val onSearch: () -> Unit,
    val onColors: (List<ArtColor>) -> Unit,
)

/**
 * The pages' titles, in a strip as tall as a touch target that scrolls to keep the current one in
 * view. Tapping one goes there.
 */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun PageTabs(pages: List<HomePage>, pager: PagerState, syncDot: Boolean) {
    val scope = rememberCoroutineScope()
    PrimaryScrollableTabRow(
        selectedTabIndex = pager.currentPage.coerceAtMost(pages.lastIndex),
        containerColor = Color.Transparent,
        edgePadding = 0.dp,
        minTabWidth = 0.dp,
        divider = {},
        modifier = Modifier
            .windowInsetsPadding(WindowInsets.statusBars.inPane())
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal).inPane()),
    ) {
        pages.forEachIndexed { index, page ->
            Tab(
                selected = index == pager.currentPage,
                onClick = { scope.launch { pager.animateScrollToPage(index) } },
                unselectedContentColor = MaterialTheme.colorScheme.onSurfaceVariant,
                text = {
                    val title = pageTitle(page)
                    if (page == HomePage.Utilities && syncDot) {
                        val waiting = stringResource(R.string.main_menu_sync_waiting)
                        BadgedBox(badge = { Badge() }, modifier = Modifier.semantics { contentDescription = waiting }) {
                            Text(title, maxLines = 1)
                        }
                    } else {
                        Text(title, maxLines = 1)
                    }
                },
            )
        }
    }
}

@Composable
private fun pageTitle(page: HomePage): String = stringResource(
    when (page) {
        is HomePage.Category -> page.section.title
        HomePage.Welcome -> R.string.section_home
        HomePage.Utilities -> R.string.main_menu
    },
)

/** One section of the library as a grid. Tapping and long-pressing do what they do in the main list. */
@Composable
private fun CategoryPage(section: Section, library: Library, frame: PageFrame, actions: SmallHomeActions) {
    val new = actions.onNewList
    when (section) {
        Section.Radio -> GridPage(library.stations, { it.seedUid }, { it.art }, frame) { station, modifier ->
            val subtitle = station.subtitle()
            ArtTile(
                station.name,
                subtitle,
                station.art,
                onLongClick = { actions.onStationMenu(station) },
                onClick = { actions.onOpen(CollectionRef(CollectionRef.Kind.Station, station.seedUid)) },
                modifier = modifier,
                badge = { StationBadge() },
                description = stringResource(R.string.station_label, station.name, subtitle, station.seedTitle),
            )
        }
        Section.Recents -> GridPage(library.recents, { "${it.ref.kind}:${it.ref.uid}" }, { it.art }, frame) { recent, modifier ->
            val subtitle = recent.subtitle()
            val station = recent.ref.kind == CollectionRef.Kind.Station
            ArtTile(
                recent.title,
                subtitle,
                recent.art,
                onLongClick = { actions.onRecentMenu(recent) },
                onClick = { actions.onOpen(recent.ref) },
                modifier = modifier,
                badge = if (station) ({ StationBadge() }) else null,
                description = if (station) {
                    stringResource(R.string.station_label, recent.title, subtitle, recent.seedTitle.orEmpty())
                } else {
                    null
                },
            )
        }
        Section.Playlists -> GridPage(
            library.playlists,
            { it.uid },
            { it.art },
            frame,
            sortKey = { it.name },
            onNew = new?.let { make -> NewRow(R.string.new_playlist_menu) { make(false) } },
        ) { playlist, modifier ->
            ArtTile(
                playlist.name,
                pluralStringResource(R.plurals.tracks, playlist.trackCount, playlist.trackCount),
                playlist.art,
                onLongClick = { actions.onPlaylistMenu(playlist) },
                onClick = { actions.onOpen(CollectionRef(CollectionRef.Kind.Playlist, playlist.uid)) },
                modifier = modifier,
            )
        }
        Section.AlbumGroups -> GridPage(
            library.albumGroups,
            { it.uid },
            { it.art },
            frame,
            onNew = new?.let { make -> NewRow(R.string.new_album_group_menu) { make(true) } },
        ) { group, modifier ->
            ArtTile(
                group.name,
                pluralStringResource(R.plurals.albums, group.albumCount, group.albumCount),
                group.art,
                onLongClick = { actions.onAlbumGroupMenu(group) },
                onClick = { actions.onOpen(CollectionRef(CollectionRef.Kind.AlbumGroup, group.uid)) },
                modifier = modifier,
            )
        }
        Section.Albums -> GridPage(library.albums, { it.uid }, { it.art }, frame, sortKey = { it.title }) { album, modifier ->
            ArtTile(
                album.title,
                album.artist,
                album.art,
                onLongClick = { actions.onAlbumMenu(album) },
                onClick = { actions.onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid)) },
                modifier = modifier,
            )
        }
        Section.Artists -> GridPage(library.artists, { it.uid }, { it.art }, frame, sortKey = { it.sortName }) { artist, modifier ->
            ArtTile(
                artist.name,
                artist.subtitle(),
                artist.art,
                onLongClick = { actions.onArtistMenu(artist) },
                onClick = { actions.onOpen(CollectionRef(CollectionRef.Kind.Artist, artist.uid)) },
                modifier = modifier,
                artShape = CircleShape,
            )
        }
    }
}

/** The way to make a new playlist or album group, at the top of their page. */
private class NewRow(val title: Int, val onClick: () -> Unit)

/**
 * [things] in a grid going down. A list of rows rather than a lazy grid, as the main list's album
 * rows are, for what works on a list here: pull to search, the fast scroll strip, which goes by
 * [sortKey] on a page in that order, and finding the art's colours.
 */
@Composable
private fun <T> GridPage(
    things: List<T>,
    keyOf: (T) -> Any,
    artOf: (T) -> ArtRef,
    frame: PageFrame,
    sortKey: ((T) -> String)? = null,
    onNew: NewRow? = null,
    tile: @Composable (T, Modifier) -> Unit,
) {
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val density = LocalDensity.current
        val columns = smallColumns(maxWidth)
        val cellPx = with(density) { ((maxWidth - Gutter * 2 - GridSpacing * (columns - 1)) / columns).roundToPx() }
        val rows = remember(things, columns) { things.chunked(columns) }
        // What's above the first row.
        val lead = (if (frame.banner != null) 1 else 0) + (if (onNew != null) 1 else 0)
        val listState = rememberLazyListState()
        val artAt: (Int) -> List<ArtRef> = { index -> rows.getOrNull(index - lead)?.map(artOf).orEmpty() }
        PrefetchArt(listState, cellPx, artAt = artAt)
        val colors = rememberOnScreenArtColors(listState, artAt)
        SideEffect { frame.onColors(colors) }
        val pull = rememberPullToSearch(frame.onSearch)
        val other = stringResource(R.string.fast_scroll_other)
        val entries = remember(things, columns, lead, sortKey != null, other) {
            buildList {
                if (sortKey != null && things.size >= LETTERS_FROM) {
                    letterEntries(this, things.map(sortKey), other) { lead + it / columns }
                }
            }
        }

        LazyColumn(
            state = listState,
            contentPadding = PaddingValues(top = 4.dp, bottom = frame.bottomPadding + Gutter),
            modifier = Modifier.fillMaxSize().pullToSearch(pull, listState),
        ) {
            if (frame.banner != null) item(key = "banner", contentType = "banner") {
                frame.banner.invoke(Modifier.padding(horizontal = Gutter).padding(bottom = 8.dp))
            }
            if (onNew != null) item(key = "new", contentType = "new") {
                UtilityRow(stringResource(onNew.title), { Icon(Icons.Add, contentDescription = null) }, onNew.onClick)
            }
            items(rows, key = { keyOf(it.first()).toString() }, contentType = { "tiles" }) { row ->
                Row(
                    Modifier.padding(horizontal = Gutter).padding(bottom = 12.dp),
                    horizontalArrangement = Arrangement.spacedBy(GridSpacing),
                ) {
                    for (thing in row) tile(thing, Modifier.weight(1f))
                    repeat(columns - row.size) { Spacer(Modifier.weight(1f)) }
                }
            }
        }
        FastScroller(
            listState,
            entries,
            kindOf = { index -> if (index >= lead) "tiles" else null },
            estimate = { kind -> if (kind == "tiles") cellPx + with(density) { TileLabelsHeight.roundToPx() } else null },
            modifier = Modifier.matchParentSize().padding(top = 8.dp, bottom = frame.bottomPadding + 8.dp, end = 2.dp),
        )
        PullToSearchIndicator(pull, Modifier.matchParentSize())
    }
}

/** The empty library's page, which scrolls, so that Sync and Import can be reached in any window. */
@Composable
private fun WelcomePage(frame: PageFrame, actions: SmallHomeActions, importPickers: ImportPickers, inGroup: Boolean) {
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(top = 4.dp, bottom = frame.bottomPadding + Gutter),
    ) {
        frame.banner?.invoke(Modifier.padding(horizontal = Gutter).padding(bottom = 8.dp))
        EmptyLibraryContent(importPickers, inGroup, actions.onSync, actions.onChooseWhatToSync, Modifier.fillMaxWidth())
    }
}

/**
 * What the main list's header has in its menu, as rows, in the same order. A section with no page
 * here has its + too, for making its first playlist or album group, as the main list's heading
 * for it does.
 */
@Composable
private fun UtilitiesPage(
    library: Library,
    frame: PageFrame,
    actions: SmallHomeActions,
    importPickers: ImportPickers,
    importRunning: Boolean,
    syncDot: Boolean,
) {
    Column(
        Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(top = 4.dp, bottom = frame.bottomPadding + Gutter),
    ) {
        frame.banner?.invoke(Modifier.padding(horizontal = Gutter).padding(bottom = 8.dp))
        var importing by remember { mutableStateOf(false) }
        Box {
            UtilityRow(
                stringResource(R.string.import_music),
                { Icon(Icons.Import, contentDescription = null) },
                onClick = { importing = true },
                description = if (importRunning) stringResource(R.string.import_running_title) else null,
                trailing = if (importRunning) {
                    { CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp) }
                } else {
                    null
                },
            )
            ImportMenu(importing, importPickers, onDismiss = { importing = false })
        }
        UtilityRow(stringResource(R.string.search), { Icon(Icons.Search, contentDescription = null) }, actions.onSearch)
        UtilityRow(
            stringResource(R.string.sync),
            { BadgedBox(badge = { if (syncDot) Badge() }) { Icon(Icons.Sync, contentDescription = null) } },
            actions.onSync,
            description = if (syncDot) stringResource(R.string.sync_bin_waiting) else null,
        )
        UtilityRow(stringResource(R.string.stats_title), { Icon(Icons.BarChart, contentDescription = null) }, actions.onStats)
        UtilityRow(stringResource(R.string.settings), { Icon(Icons.Settings, contentDescription = null) }, actions.onSettings)
        val new = actions.onNewList
        if (new != null && !library.isEmpty) {
            if (library.playlists.isEmpty()) {
                UtilityRow(stringResource(R.string.new_playlist_menu), { Icon(Icons.Add, contentDescription = null) }, { new(false) })
            }
            if (library.albumGroups.isEmpty()) {
                UtilityRow(stringResource(R.string.new_album_group_menu), { Icon(Icons.Add, contentDescription = null) }, { new(true) })
            }
        }
    }
}

/** A row of the utilities page: [description], if any, is what TalkBack says in place of [text]. */
@Composable
private fun UtilityRow(
    text: String,
    icon: @Composable () -> Unit,
    onClick: () -> Unit,
    description: String? = null,
    trailing: (@Composable () -> Unit)? = null,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 48.dp)
            .clickable(onClick = onClick)
            .padding(horizontal = 16.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        icon()
        Text(
            text,
            style = MaterialTheme.typography.bodyLarge,
            maxLines = 2,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier
                .weight(1f)
                .padding(start = 16.dp)
                .semantics { if (description != null) contentDescription = description },
        )
        trailing?.invoke()
    }
}
