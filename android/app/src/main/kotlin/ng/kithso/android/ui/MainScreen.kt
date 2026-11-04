package ng.kithso.android.ui

import androidx.compose.runtime.LaunchedEffect
import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.add
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.LazyItemScope
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.ListItem
import androidx.compose.material3.ListItemDefaults
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.DpOffset
import androidx.compose.ui.unit.dp
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
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.art.PrefetchArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberOnScreenArtColors
import kotlinx.coroutines.launch

enum class Section(@StringRes val title: Int) {
    Radio(R.string.home_row_radio),
    Recents(R.string.section_recents),
    Playlists(R.string.section_playlists),
    AlbumGroups(R.string.section_album_groups),
    Albums(R.string.section_albums),
    Artists(R.string.section_artists),
}

/**
 * One item of the main list. Albums come [MainItem.AlbumRow] at a time, a grid row each; radio
 * stations, recents and album groups are one item each, a [TileRow].
 */
@Immutable
internal sealed interface MainItem {
    val key: Any
    val contentType: Any
    val art: List<ArtRef> get() = emptyList()

    data object Header : MainItem {
        override val key get() = "header"
        override val contentType get() = "header"
    }

    data object Banner : MainItem {
        override val key get() = "banner"
        override val contentType get() = "banner"
    }

    data object Empty : MainItem {
        override val key get() = "empty"
        override val contentType get() = "empty"
    }

    data class Heading(val section: Section) : MainItem {
        override val key get() = "heading:$section"
        override val contentType get() = "heading"
    }

    data class RadioRow(val stations: List<Station>) : MainItem {
        override val key get() = RADIO_ROW_KEY
        override val contentType get() = TILE_ROW_TYPE
        override val art get() = stations.take(ROW_ART).map { it.art }
    }

    data class RecentsRow(val recents: List<Recent>) : MainItem {
        override val key get() = RECENTS_ROW_KEY
        override val contentType get() = TILE_ROW_TYPE
        override val art get() = recents.take(ROW_ART).map { it.art }
    }

    data class PlaylistItem(val playlist: Playlist) : MainItem {
        override val key get() = "playlist:${playlist.uid}"
        override val contentType get() = "playlist"
        override val art get() = listOf(playlist.art)
    }

    data class AlbumGroupsRow(val groups: List<AlbumGroup>) : MainItem {
        override val key get() = ALBUM_GROUPS_ROW_KEY
        override val contentType get() = TILE_ROW_TYPE
        override val art get() = groups.take(ROW_ART).map { it.art }
    }

    data class AlbumRow(val albums: List<Album>) : MainItem {
        override val key get() = "albums:${albums.first().uid}"
        override val contentType get() = "albums"
        override val art get() = albums.map { it.art }
    }

    data class ArtistItem(val artist: Artist) : MainItem {
        override val key get() = "artist:${artist.uid}"
        override val contentType get() = "artist"
        override val art get() = listOf(artist.art)
    }
}

private const val RADIO_ROW_KEY = "row:radio"
private const val RECENTS_ROW_KEY = "row:recents"
private const val ALBUM_GROUPS_ROW_KEY = "row:album-groups"
private const val TILE_ROW_TYPE = "tiles"

/** How many of a row's tiles count towards art prefetching and the backdrop: about a screen's. */
private const val ROW_ART = 6

// Null while the library is being read: just the header, rather than a flash of the empty state.
// With lists to make ([canMake]), Playlists and Album groups show even when empty, for their +
// buttons; with no music, only for what's already there.
internal fun mainItems(library: Library?, columns: Int, banner: Boolean, canMake: Boolean): List<MainItem> = buildList {
    add(MainItem.Header)
    if (banner) add(MainItem.Banner)
    if (library == null) return@buildList
    if (library.isEmpty) add(MainItem.Empty)
    val headings = canMake && !library.isEmpty
    if (library.stations.isNotEmpty()) {
        add(MainItem.Heading(Section.Radio))
        add(MainItem.RadioRow(library.stations))
    }
    if (library.recents.isNotEmpty()) {
        add(MainItem.Heading(Section.Recents))
        add(MainItem.RecentsRow(library.recents))
    }
    if (library.playlists.isNotEmpty() || headings) {
        add(MainItem.Heading(Section.Playlists))
        library.playlists.mapTo(this, MainItem::PlaylistItem)
    }
    if (library.albumGroups.isNotEmpty() || headings) {
        add(MainItem.Heading(Section.AlbumGroups))
        if (library.albumGroups.isNotEmpty()) add(MainItem.AlbumGroupsRow(library.albumGroups))
    }
    if (library.albums.isNotEmpty()) {
        add(MainItem.Heading(Section.Albums))
        library.albums.chunked(columns).mapTo(this, MainItem::AlbumRow)
    }
    if (library.artists.isNotEmpty()) {
        add(MainItem.Heading(Section.Artists))
        library.artists.mapTo(this, MainItem::ArtistItem)
    }
}

/** How many items a section has before the fast scroll strip has its letters as well as its heading. */
private const val LETTERS_FROM = 12

/** How much of the fast scroll strip Albums takes at least, once it has letters (see [placeShares]). */
private const val ALBUMS_MIN_SHARE = 0.6f

/**
 * The fast scroll strip's entries for [items]: each section's heading, then the letters of an
 * alphabetical section long enough for them (playlists by name, albums by title, artists by sort
 * name, as the library orders them). Radio, Recents and album groups are a single row, with no
 * places in it for the strip to go.
 */
internal fun fastScrollEntries(items: List<MainItem>, titles: Map<Section, String>, other: String): List<FastScrollEntry> =
    buildList {
        var i = 0
        while (i < items.size) {
            val heading = items[i] as? MainItem.Heading
            if (heading == null) {
                i++
                continue
            }
            val title = titles.getValue(heading.section)
            add(FastScrollEntry(title, i, heading = true))
            val start = i + 1
            var end = start
            while (end < items.size && items[end] !is MainItem.Heading) end++
            // Each item's position, and the sort key of each name in turn.
            val at = ArrayList<Int>()
            val keys = ArrayList<String>()
            for (j in start until end) {
                when (val item = items[j]) {
                    is MainItem.PlaylistItem -> { at += j; keys += item.playlist.name }
                    is MainItem.ArtistItem -> { at += j; keys += item.artist.sortName }
                    is MainItem.AlbumRow -> item.albums.forEach { at += j; keys += it.title }
                    else -> {}
                }
            }
            if (keys.size >= LETTERS_FROM) letterEntries(this, keys, other, title) { at[it] }
            i = end
        }
    }

/** A section heading's place in the list, for jumping there. */
@Immutable
private data class SectionStart(val section: Section, val index: Int)

private val Gutter = BrowseGutter
private val ListArtSize = 56.dp

/** The header's height under the status bar. */
private val HeaderHeight = 64.dp

/** An album tile's or [TileRow] tile's title and artist, under its art and with the space below the row. */
private val AlbumLabelsHeight = 58.dp

/**
 * The main screen: one lazy list of a header and the library's sections, over a background
 * coloured from the art on screen. [bottomPadding] keeps the last items clear of the mini
 * player and navigation bar. [banner], if any, goes under the header: how an import is going.
 * The header's menu and the empty library's button open [importPickers], unless an import
 * is [importRunning]. [onSearch] is the header's menu's Search, and pulling the list down
 * from its top (see [pullToSearch]). With [onNewList], the Playlists and Album groups headings
 * have a + for a new playlist or (with true) album group.
 */
@Composable
fun MainScreen(
    library: Library?,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    onStats: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
    banner: (@Composable (Modifier) -> Unit)?,
    modifier: Modifier = Modifier,
    // In a group, the empty library offers this phone's sync settings rather than pairing.
    inGroup: Boolean = false,
    onChooseWhatToSync: () -> Unit = {},
    // Long-pressing a station, a recent, a playlist, an album group, an album or an artist opens its menu.
    onStationMenu: (Station) -> Unit = {},
    onRecentMenu: (Recent) -> Unit = {},
    onPlaylistMenu: (Playlist) -> Unit = {},
    onAlbumMenu: (Album) -> Unit = {},
    onArtistMenu: (Artist) -> Unit = {},
    onAlbumGroupMenu: (AlbumGroup) -> Unit = {},
    onNewList: ((isAlbums: Boolean) -> Unit)? = null,
    // A red dot on the header's menu and its Sync: deletes wait in the recycle bin while a member is online.
    syncDot: Boolean = false,
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val density = LocalDensity.current
        // The list is inset from a camera cutout or navigation bar at the side, in landscape.
        val sides = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
        val layoutDirection = LocalLayoutDirection.current
        val width = maxWidth - with(density) { (sides.getLeft(density, layoutDirection) + sides.getRight(density, layoutDirection)).toDp() }
        val columns = albumColumns(width)
        val cellPx = with(density) { ((width - Gutter * 2 - TileSpacing * (columns - 1)) / columns).roundToPx() }
        val statusBarPx = WindowInsets.statusBars.getTop(density)

        val hasBanner = banner != null
        val canMake = onNewList != null
        val items = remember(library, columns, hasBanner, canMake) { mainItems(library, columns, hasBanner, canMake) }
        val sections = remember(items) {
            items.mapIndexedNotNull { i, item -> (item as? MainItem.Heading)?.let { SectionStart(it.section, i) } }
        }
        val listState = rememberLazyListState()
        val radioRow = rememberTileRowState(listState, RADIO_ROW_KEY)
        val recentsRow = rememberTileRowState(listState, RECENTS_ROW_KEY)
        val albumGroupsRow = rememberTileRowState(listState, ALBUM_GROUPS_ROW_KEY)
        ScrollFrames(listState, "main")
        val scrollStep = scrollStepPx()
        LaunchedEffect(listState) {
            ScrollTest.step.collect { step ->
                if (step == ScrollTest.Step.Main) {
                    runScrollScript(listState, scrollStep, "main")
                    ScrollTest.step.value = ScrollTest.Step.OpenTracks
                }
            }
        }
        // The section at the top of the screen; the header counts as part of the first.
        val current by remember(sections) {
            derivedStateOf {
                (sections.lastOrNull { it.index <= listState.firstVisibleItemIndex } ?: sections.firstOrNull())?.section
            }
        }
        val scope = rememberCoroutineScope()
        // Lands the heading just below the status bar.
        val jumpTo: (Int) -> Unit = remember(listState, statusBarPx) {
            { index -> scope.launch { listState.animateScrollToItem(index, -statusBarPx) } }
        }
        val other = stringResource(R.string.fast_scroll_other)
        val titles = Section.entries.associateWith { stringResource(it.title) }
        val scrollEntries = remember(items, titles, other) { fastScrollEntries(items, titles, other) }
        // Pulling down from the top opens search, as the header's menu does.
        val pull = rememberPullToSearch(onSearch)

        PrefetchArt(listState, cellPx) { index -> items.getOrNull(index)?.art.orEmpty() }
        ArtBackdrop(
            rememberOnScreenArtColors(listState) { index -> items.getOrNull(index)?.art.orEmpty() },
            Modifier.matchParentSize(),
        )

        LazyColumn(
            state = listState,
            // The top is left to the header, so content scrolls under the status bar.
            contentPadding = sides
                .add(WindowInsets(bottom = bottomPadding))
                .asPaddingValues(),
            modifier = Modifier.fillMaxSize().pullToSearch(pull, listState),
        ) {
            items(items, key = { it.key }, contentType = { it.contentType }) { item ->
                when (item) {
                    MainItem.Header -> Header(onSearch, onSync, onSettings, onStats, importPickers, importRunning, syncDot)
                    MainItem.Banner -> banner?.invoke(Modifier.padding(horizontal = Gutter).padding(bottom = 8.dp))
                    MainItem.Empty -> EmptyLibrary(importPickers, importRunning, inGroup, onSync, onChooseWhatToSync)
                    is MainItem.Heading -> SectionHeading(
                        item.section,
                        sections,
                        { current },
                        jumpTo,
                        onNew = onNewList?.let { new ->
                            when (item.section) {
                                Section.Playlists -> ({ new(false) })
                                Section.AlbumGroups -> ({ new(true) })
                                else -> null
                            }
                        },
                    )
                    is MainItem.RadioRow -> TileRow(
                        item.stations.map { station ->
                            val subtitle = station.subtitle()
                            Tile(
                                station.seedUid,
                                station.name,
                                subtitle,
                                station.art,
                                onLongClick = { onStationMenu(station) },
                                badge = { StationBadge() },
                                description = stringResource(R.string.station_label, station.name, subtitle, station.seedTitle),
                            ) { onOpen(CollectionRef(CollectionRef.Kind.Station, station.seedUid)) }
                        },
                        state = radioRow,
                    )
                    is MainItem.RecentsRow -> TileRow(
                        item.recents.map { recent ->
                            val subtitle = recent.subtitle()
                            val station = recent.ref.kind == CollectionRef.Kind.Station
                            Tile(
                                "${recent.ref.kind}:${recent.ref.uid}",
                                recent.title,
                                subtitle,
                                recent.art,
                                onLongClick = { onRecentMenu(recent) },
                                badge = if (station) ({ StationBadge() }) else null,
                                description = if (station) {
                                    stringResource(R.string.station_label, recent.title, subtitle, recent.seedTitle.orEmpty())
                                } else {
                                    null
                                },
                            ) { onOpen(recent.ref) }
                        },
                        state = recentsRow,
                    )
                    is MainItem.PlaylistItem -> LibraryRow(
                        item.playlist.name,
                        pluralStringResource(R.plurals.tracks, item.playlist.trackCount, item.playlist.trackCount),
                        item.playlist.art,
                        RoundedCornerShape(8.dp),
                        onLongClick = { onPlaylistMenu(item.playlist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Playlist, item.playlist.uid)) }
                    is MainItem.AlbumGroupsRow -> TileRow(
                        item.groups.map { group ->
                            Tile(
                                group.uid,
                                group.name,
                                pluralStringResource(R.plurals.albums, group.albumCount, group.albumCount),
                                group.art,
                                onLongClick = { onAlbumGroupMenu(group) },
                            ) { onOpen(CollectionRef(CollectionRef.Kind.AlbumGroup, group.uid)) }
                        },
                        state = albumGroupsRow,
                    )
                    is MainItem.AlbumRow -> AlbumGridRow(item.albums, columns, onOpen, onAlbumMenu)
                    is MainItem.ArtistItem -> LibraryRow(
                        item.artist.name,
                        item.artist.subtitle(),
                        item.artist.art,
                        CircleShape,
                        onLongClick = { onArtistMenu(item.artist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Artist, item.artist.uid)) }
                }
            }
        }
        FastScroller(
            listState,
            scrollEntries,
            scrollOffset = -statusBarPx,
            // The artists' letters would otherwise take most of the strip.
            minShares = mapOf(titles.getValue(Section.Albums) to ALBUMS_MIN_SHARE),
            kindOf = { index -> items.getOrNull(index)?.contentType },
            // Known before the list reaches them, from the width of their tiles.
            estimate = { kind ->
                when (kind) {
                    "albums" -> cellPx + with(density) { AlbumLabelsHeight.roundToPx() }
                    TILE_ROW_TYPE -> with(density) { (tileWidth(width) + AlbumLabelsHeight).roundToPx() }
                    else -> null
                }
            },
            // Below the header, whose buttons it would otherwise be in the way of.
            modifier = Modifier
                .matchParentSize()
                .windowInsetsPadding(sides)
                .padding(top = with(density) { statusBarPx.toDp() } + HeaderHeight + 8.dp, bottom = bottomPadding + 8.dp, end = 2.dp),
        )
        PullToSearchIndicator(
            pull,
            Modifier.matchParentSize().padding(top = with(density) { statusBarPx.toDp() }),
        )
    }
}

/** The wordmark and the menu. It scrolls away with the list. */
@Composable
private fun Header(
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    onStats: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
    syncDot: Boolean,
) {
    Row(
        Modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.statusBars)
            .height(HeaderHeight)
            .padding(start = Gutter, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // The tile's own margin lines it up with the gutter's content, as the title text did.
        Box(Modifier.weight(1f)) {
            Wordmark(
                height = 48.dp,
                description = stringResource(R.string.app_name),
                modifier = Modifier.offset(x = (-4).dp).semantics { heading() },
            )
        }
        HeaderMenu(onSearch, onSync, onSettings, onStats, importPickers, importRunning, syncDot)
    }
}

/**
 * The header's menu. [syncDot] shows on the button, to be seen with the menu closed, and on Sync
 * inside it. Import's choices take the menu's place, with a way back. While an import runs,
 * Import has a spinner and says so instead of offering choices it couldn't take, as the card
 * under the header shows how it's going.
 */
@Composable
private fun HeaderMenu(
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    onStats: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
    syncDot: Boolean,
) {
    var open by remember { mutableStateOf(false) }
    var importing by remember { mutableStateOf(false) }
    var busy by remember { mutableStateOf(false) }
    val pick: (() -> Unit) -> () -> Unit = { action ->
        {
            open = false
            action()
        }
    }
    Box {
        IconButton(
            onClick = {
                // Here rather than on closing, or the rows would change as the menu fades out.
                importing = false
                open = true
            },
        ) {
            BadgedBox(badge = { if (syncDot) Badge() }) {
                Icon(
                    Icons.MoreHoriz,
                    contentDescription = stringResource(if (syncDot) R.string.main_menu_sync_waiting else R.string.main_menu),
                )
            }
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            if (importing) {
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.back)) },
                    leadingIcon = { Icon(Icons.ArrowBack, contentDescription = null) },
                    onClick = { importing = false },
                )
                ImportChoices(importRunning, importPickers, onBusy = { busy = true }, onDismiss = { open = false })
            } else {
                val runningText = stringResource(R.string.import_running_title)
                DropdownMenuItem(
                    text = {
                        Text(
                            stringResource(R.string.import_music),
                            Modifier.semantics { if (importRunning) contentDescription = runningText },
                        )
                    },
                    leadingIcon = { Icon(Icons.Import, contentDescription = null) },
                    trailingIcon = if (importRunning) {
                        { CircularProgressIndicator(Modifier.size(18.dp), strokeWidth = 2.dp) }
                    } else {
                        null
                    },
                    onClick = {
                        if (importRunning) {
                            open = false
                            busy = true
                        } else {
                            importing = true
                        }
                    },
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.search)) },
                    leadingIcon = { Icon(Icons.Search, contentDescription = null) },
                    onClick = pick(onSearch),
                )
                val waitingText = stringResource(R.string.sync_bin_waiting)
                DropdownMenuItem(
                    text = {
                        Text(
                            stringResource(R.string.sync),
                            Modifier.semantics { if (syncDot) contentDescription = waitingText },
                        )
                    },
                    leadingIcon = {
                        BadgedBox(badge = { if (syncDot) Badge() }) { Icon(Icons.Sync, contentDescription = null) }
                    },
                    onClick = pick(onSync),
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.stats_title)) },
                    leadingIcon = { Icon(Icons.BarChart, contentDescription = null) },
                    onClick = pick(onStats),
                )
                DropdownMenuItem(
                    text = { Text(stringResource(R.string.settings)) },
                    leadingIcon = { Icon(Icons.Settings, contentDescription = null) },
                    onClick = pick(onSettings),
                )
            }
        }
    }
    if (busy) ImportRunningDialog(onDismiss = { busy = false })
}

/**
 * A section's heading, a tap target across the whole row. Tapping it opens a list of every
 * section, and picking one scrolls there; [current] is the section at the top of the screen,
 * marked in the list. TalkBack gets the tap, and a jump to each other section as a custom action.
 * With [onNew], a + at the end, clear of the fast scroll strip, makes a new playlist or album
 * group.
 */
@Composable
private fun SectionHeading(
    section: Section,
    sections: List<SectionStart>,
    current: () -> Section?,
    onJump: (Int) -> Unit,
    onNew: (() -> Unit)? = null,
) {
    var open by remember { mutableStateOf(false) }
    val jumpLabel = stringResource(R.string.jump_to_section)
    val jumpActions = sections.filter { it.section != section }.map { start ->
        val label = stringResource(R.string.jump_to_named_section, stringResource(start.section.title))
        CustomAccessibilityAction(label) { onJump(start.index); true }
    }
    Box(Modifier.padding(top = 12.dp)) {
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = 52.dp)
                .clickable(onClickLabel = jumpLabel) { open = true }
                .semantics(mergeDescendants = true) {
                    heading()
                    customActions = jumpActions
                }
                .padding(horizontal = Gutter, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(stringResource(section.title), style = MaterialTheme.typography.titleLarge)
            Icon(Icons.ExpandMore, contentDescription = null, modifier = Modifier.padding(start = 4.dp))
        }
        if (onNew != null) {
            IconButton(
                onClick = onNew,
                modifier = Modifier.align(Alignment.CenterEnd).padding(end = FastScrollInset),
            ) {
                Icon(
                    Icons.Add,
                    contentDescription = stringResource(
                        if (section == Section.AlbumGroups) R.string.new_album_group_menu else R.string.new_playlist_menu,
                    ),
                )
            }
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }, offset = DpOffset(Gutter - 8.dp, 0.dp)) {
            val here = current()
            for (start in sections) {
                DropdownMenuItem(
                    text = {
                        Text(
                            stringResource(start.section.title),
                            color = if (start.section == here) MaterialTheme.colorScheme.primary else Color.Unspecified,
                            fontWeight = if (start.section == here) FontWeight.Bold else null,
                        )
                    },
                    onClick = {
                        open = false
                        onJump(start.index)
                    },
                )
            }
        }
    }
}

/** What a recent shows under its title: an album's artist, a station's line, or what kind of thing it is. */
@Composable
internal fun Recent.subtitle(): String = artist ?: sub ?: stringResource(
    when (ref.kind) {
        CollectionRef.Kind.Artist -> R.string.artist
        CollectionRef.Kind.AlbumGroup -> R.string.album_group
        CollectionRef.Kind.Station -> R.string.home_station_sub
        else -> R.string.playlist
    },
)

/** What a station shows under its name. */
@Composable
internal fun Station.subtitle(): String = sub ?: stringResource(R.string.home_station_sub)

/** An artist's albums here, or for one only credited on tracks, their tracks. */
@Composable
internal fun Artist.subtitle(): String =
    if (albumCount > 0) pluralStringResource(R.plurals.albums, albumCount, albumCount)
    else pluralStringResource(R.plurals.tracks, trackCount, trackCount)

/** A list row with art, for playlists and artists. */
@Composable
private fun LibraryRow(
    title: String,
    subtitle: String,
    art: ArtRef,
    artShape: Shape,
    onLongClick: (() -> Unit)? = null,
    onClick: () -> Unit,
) {
    ListItem(
        headlineContent = { Text(title, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        supportingContent = { Text(subtitle, maxLines = 1, overflow = TextOverflow.Ellipsis) },
        leadingContent = { AlbumArt(art, contentDescription = null, shape = artShape, modifier = Modifier.size(ListArtSize)) },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier.combinedClickable(onLongClick = onLongClick, onClick = onClick),
    )
}

/**
 * No music yet: where it can come from. A phone's music usually comes from a computer, so Sync
 * comes first, with Import beneath it. While this phone is [inGroup] its sync settings hold
 * nothing, so Choose what to sync takes Sync's place.
 */
@Composable
private fun LazyItemScope.EmptyLibrary(
    importPickers: ImportPickers,
    importRunning: Boolean,
    inGroup: Boolean,
    onSync: () -> Unit,
    onChooseWhatToSync: () -> Unit,
) {
    Column(
        Modifier.fillParentMaxWidth().fillParentMaxHeight(0.7f).padding(32.dp),
        verticalArrangement = Arrangement.Center,
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Icon(
            Icons.LibraryMusic,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.size(72.dp),
        )
        Text(
            stringResource(R.string.empty_title),
            style = MaterialTheme.typography.titleLarge,
            modifier = Modifier.padding(top = 16.dp),
        )
        Text(
            stringResource(if (inGroup) R.string.empty_text_choose else R.string.empty_text_import),
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            textAlign = TextAlign.Center,
            modifier = Modifier.padding(top = 4.dp),
        )
        Button(onClick = if (inGroup) onChooseWhatToSync else onSync, modifier = Modifier.padding(top = 24.dp)) {
            if (!inGroup) Icon(Icons.Sync, contentDescription = null, modifier = Modifier.padding(end = 8.dp).size(18.dp))
            Text(stringResource(if (inGroup) R.string.choose_what_to_sync else R.string.sync))
        }
        var menu by remember { mutableStateOf(false) }
        Box(Modifier.padding(top = 8.dp)) {
            TextButton(onClick = { menu = true }) {
                Icon(Icons.Import, contentDescription = null, modifier = Modifier.padding(end = 8.dp).size(18.dp))
                Text(stringResource(R.string.import_music))
            }
            ImportMenu(menu, importRunning, importPickers, onDismiss = { menu = false })
        }
    }
}
