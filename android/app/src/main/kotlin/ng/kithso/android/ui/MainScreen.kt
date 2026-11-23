package ng.kithso.android.ui

import androidx.compose.runtime.LaunchedEffect
import androidx.annotation.StringRes
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
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
import androidx.compose.material3.Surface
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
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.collapse
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.expand
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
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
import ng.kithso.android.ui.art.AlbumArt
import ng.kithso.android.ui.art.PrefetchArt
import ng.kithso.android.ui.theme.ArtBackdrop
import ng.kithso.android.ui.theme.rememberOnScreenArtColors
import kotlinx.coroutines.launch
import kotlin.math.roundToInt
import androidx.activity.compose.BackHandler
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.animation.slideInVertically
import androidx.compose.animation.slideOutVertically
import androidx.compose.foundation.layout.ime
import androidx.compose.foundation.layout.union
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.input.ImeAction
import ng.kithso.android.data.FoundTrack
import ng.kithso.android.data.LibrarySource
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.dropWhile
import kotlinx.coroutines.flow.first

enum class Section(@StringRes val title: Int) {
    Recents(R.string.section_recents),
    Playlists(R.string.section_playlists),
    AlbumGroups(R.string.section_album_groups),
    Albums(R.string.section_albums),
    Artists(R.string.section_artists),
}

/**
 * One item of the main list. Albums come [MainItem.AlbumRow] at a time, a grid row each;
 * recents and album groups are one item each, a [TileRow].
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

    /** A row of the album grid, [columns] across. */
    data class AlbumRow(val albums: List<Album>, val columns: Int) : MainItem {
        override val key get() = "albums:${albums.first().uid}"
        override val contentType get() = AlbumRowType(columns)
        override val art get() = albums.map { it.art }
    }

    data class ArtistItem(val artist: Artist) : MainItem {
        override val key get() = "artist:${artist.uid}"
        override val contentType get() = "artist"
        override val art get() = listOf(artist.art)
    }

    /** The row at the end of Artists that opens to show the [count] artists folded away under it. */
    data class CreditedArtistsRow(val count: Int, val open: Boolean) : MainItem {
        override val key get() = "row:credited-artists"
        override val contentType get() = "credited-artists"
    }
}

private const val RECENTS_ROW_KEY = "row:recents"

/** How many recents the phone's row shows; a tablet's sidebar can show more. */
internal const val PHONE_RECENTS = 8
private const val ALBUM_GROUPS_ROW_KEY = "row:album-groups"
private const val TILE_ROW_TYPE = "tiles"

/**
 * An album row's content type: a grid's rows with fewer columns are taller, which the fast
 * scroll strip, measuring each type once, has to know apart.
 */
internal data class AlbumRowType(val columns: Int)

/** How many of a row's tiles count towards art prefetching and the backdrop: about a screen's. */
private const val ROW_ART = 6

// Null while the library is being read: just the header, rather than a flash of the empty state.
// With lists to make ([canMake]), Playlists and Album groups show even when empty, for their +
// buttons; with no music, only for what's already there. The sections come in [order]. With
// [foldCredited], the artists only credited on others' tracks ([Artist.creditOnly]) go at the end
// of Artists, under a row that shows them while [creditedOpen].
internal fun mainItems(
    library: Library?,
    columns: Int,
    banner: Boolean,
    canMake: Boolean,
    order: List<Section> = Section.entries,
    foldCredited: Boolean = false,
    creditedOpen: Boolean = false,
): List<MainItem> = buildList {
    add(MainItem.Header)
    if (banner) add(MainItem.Banner)
    if (library == null) return@buildList
    if (library.isEmpty) add(MainItem.Empty)
    val headings = canMake && !library.isEmpty
    for (section in order) when (section) {
        Section.Recents -> if (library.recents.isNotEmpty()) {
            add(MainItem.Heading(Section.Recents))
            add(MainItem.RecentsRow(library.recents.take(PHONE_RECENTS)))
        }
        Section.Playlists -> if (library.playlists.isNotEmpty() || headings) {
            add(MainItem.Heading(Section.Playlists))
            library.playlists.mapTo(this, MainItem::PlaylistItem)
        }
        Section.AlbumGroups -> if (library.albumGroups.isNotEmpty() || headings) {
            add(MainItem.Heading(Section.AlbumGroups))
            if (library.albumGroups.isNotEmpty()) add(MainItem.AlbumGroupsRow(library.albumGroups))
        }
        Section.Albums -> if (library.albums.isNotEmpty()) {
            add(MainItem.Heading(Section.Albums))
            library.albums.chunked(columns).mapTo(this) { MainItem.AlbumRow(it, columns) }
        }
        Section.Artists -> if (library.artists.isNotEmpty()) {
            add(MainItem.Heading(Section.Artists))
            val (credited, own) = if (foldCredited) library.artists.partition { it.creditOnly } else emptyList<Artist>() to library.artists
            own.mapTo(this, MainItem::ArtistItem)
            if (credited.isNotEmpty()) {
                add(MainItem.CreditedArtistsRow(credited.size, creditedOpen))
                if (creditedOpen) credited.mapTo(this, MainItem::ArtistItem)
            }
        }
    }
}

/**
 * Whether [artists] have enough only credited on others' tracks ([Artist.creditOnly]) for folding
 * them away to be worth it: more than a third as many as those with an album or single of their
 * own here, compilations' album artists included. Below that, folding is neither offered on the
 * Customise page nor done, whatever [AppSettings.foldCreditedArtists] says, and that's kept for
 * when there are more. One look at each artist, which the library already has in memory.
 */
internal fun foldsCreditedArtists(artists: List<Artist>): Boolean {
    val credited = artists.count { it.creditOnly }
    return credited * 3 > artists.size - credited
}

/**
 * The sections in a saved order, by name ([AppSettings.sectionOrder]): Recents first whatever
 * it says, then those it names, then any it doesn't (as a newer version's would be) in the usual
 * order. Unknown names are left out.
 */
internal fun orderedSections(saved: List<String>): List<Section> {
    val named = saved.mapNotNull { name -> Section.entries.find { it.name == name } }.distinct() - Section.Recents
    return listOf(Section.Recents) + named + Section.entries.filter { it !in named && it != Section.Recents }
}

/** How many items a section has before the fast scroll strip has its letters as well as its heading. */
private const val LETTERS_FROM = 12

/** How much of the fast scroll strip Albums takes at least, once it has letters (see [placeShares]). */
private const val ALBUMS_MIN_SHARE = 0.6f

/**
 * The fast scroll strip's entries for [items]: each section's heading, then the letters of an
 * alphabetical section long enough for them (playlists by name, albums by title, artists by sort
 * name, as the library orders them). Recents and album groups are a single row, with no
 * places in it for the strip to go. The artists under an open [MainItem.CreditedArtistsRow]
 * have letters of their own after the others', as they start again from A.
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
            // Each item's position, and the sort key of each name in turn; and where in them each
            // alphabetical run starts.
            val at = ArrayList<Int>()
            val keys = ArrayList<String>()
            val runs = arrayListOf(0)
            for (j in start until end) {
                when (val item = items[j]) {
                    is MainItem.PlaylistItem -> { at += j; keys += item.playlist.name }
                    is MainItem.ArtistItem -> { at += j; keys += item.artist.sortName }
                    is MainItem.AlbumRow -> item.albums.forEach { at += j; keys += it.title }
                    is MainItem.CreditedArtistsRow -> runs += keys.size
                    else -> {}
                }
            }
            if (keys.size >= LETTERS_FROM) runs.forEachIndexed { r, from ->
                val until = runs.getOrElse(r + 1) { keys.size }
                letterEntries(this, keys.subList(from, until), other, title) { at[from + it] }
            }
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
 * The Recents heading's + Import and the empty library's button open [importPickers], and what's
 * picked joins an import that's [importRunning]. The header has Sync, in the search field, and Settings. With [onNewList], the Playlists and Album groups headings have a +
 * for a new playlist or (with true) album group.
 *
 * Search is in the header: typing there slides the library down and away and shows [source]'s
 * results in its place, and closing the keyboard (or Back) closes them and brings it back.
 * Pulling the list down from its top (see [pullToSearch]) and [searchRequests] go to the
 * header's field. [onPlayTrack] and [onTrackMenu] are for a found track.
 */
@Composable
fun MainScreen(
    library: Library?,
    bottomPadding: Dp,
    onOpen: (CollectionRef) -> Unit,
    source: LibrarySource,
    searchRequests: Flow<Unit>,
    onPlayTrack: (FoundTrack) -> Unit,
    onTrackMenu: (FoundTrack) -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
    banner: (@Composable (Modifier) -> Unit)?,
    modifier: Modifier = Modifier,
    // In a group, the empty library offers this phone's sync settings rather than pairing.
    inGroup: Boolean = false,
    onChooseWhatToSync: () -> Unit = {},
    // Long-pressing a recent, a playlist, an album group, an album or an artist opens its menu.
    onRecentMenu: (Recent) -> Unit = {},
    onPlaylistMenu: (Playlist) -> Unit = {},
    onAlbumMenu: (Album) -> Unit = {},
    onArtistMenu: (Artist) -> Unit = {},
    onAlbumGroupMenu: (AlbumGroup) -> Unit = {},
    onNewList: ((isAlbums: Boolean) -> Unit)? = null,
    // A red dot on the header's Sync: deletes wait in the recycle bin while a member is online.
    syncDot: Boolean = false,
    // The sections' order, from the Customise page.
    sectionOrder: List<Section> = Section.entries,
    // Whether the artists only credited on others' tracks are folded away, from the Customise page.
    foldCredited: Boolean = false,
    // Smaller album tiles, a column more across; pinching the grid changes it, with this.
    smallAlbumTiles: Boolean = false,
    onSmallAlbumTiles: ((Boolean) -> Unit)? = null,
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val density = LocalDensity.current
        // The list is inset from a camera cutout or navigation bar at the side, in landscape.
        val sides = WindowInsets.safeDrawing.only(WindowInsetsSides.Horizontal)
        val layoutDirection = LocalLayoutDirection.current
        val width = maxWidth - with(density) { (sides.getLeft(density, layoutDirection) + sides.getRight(density, layoutDirection)).toDp() }
        val columns = phoneAlbumColumns(width, smallAlbumTiles)
        // An album tile's width with n columns across.
        val cellWidth = { n: Int -> with(density) { ((width - Gutter * 2 - TileSpacing * (n - 1)) / n).roundToPx() } }
        val cellPx = cellWidth(columns)
        // The art loads at the larger tiles' size whichever are shown, so one cached cover serves
        // both and a pinch either way finds it there, rather than flashing while it decodes.
        val artPx = cellWidth(phoneAlbumColumns(width, small = false))
        val statusBarPx = WindowInsets.statusBars.getTop(density)

        val hasBanner = banner != null
        val canMake = onNewList != null
        // The folded artists are hidden each time the app starts.
        var creditedOpen by rememberSaveable { mutableStateOf(false) }
        val items = remember(library, columns, hasBanner, canMake, sectionOrder, foldCredited, creditedOpen) {
            mainItems(library, columns, hasBanner, canMake, sectionOrder, foldCredited, creditedOpen)
        }
        val sections = remember(items) {
            items.mapIndexedNotNull { i, item -> (item as? MainItem.Heading)?.let { SectionStart(it.section, i) } }
        }
        val listState = rememberLazyListState()
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
        // The header's search. Its field is the list's first item, so the list is held at its
        // top while searching; [focusWanted] focuses it once it's on screen.
        var query by rememberSaveable { mutableStateOf("") }
        var focused by remember { mutableStateOf(false) }
        var focusWanted by remember { mutableStateOf(false) }
        val searching = focused || query.isNotEmpty()
        val showResults = query.isNotBlank()
        val focusManager = LocalFocusManager.current
        val openSearch: () -> Unit = {
            focusWanted = true
            scope.launch { listState.animateScrollToItem(0) }
        }
        val closeSearch: () -> Unit = {
            query = ""
            focusManager.clearFocus()
        }
        LaunchedEffect(searchRequests) { searchRequests.collect { openSearch() } }
        // Closing the keyboard closes the results. Only once it's been open, so a hardware
        // keyboard, with none on screen, doesn't close them straight away.
        val ime = WindowInsets.ime
        LaunchedEffect(searching) {
            if (!searching) return@LaunchedEffect
            snapshotFlow { ime.getBottom(density) > 0 }.dropWhile { !it }.first { !it }
            closeSearch()
        }
        BackHandler(enabled = searching) { closeSearch() }
        // The library slides down and fades as the results come in over it.
        val slide by animateFloatAsState(if (showResults) 1f else 0f, label = "searchSlide")
        val slidePx = constraints.maxHeight * LIBRARY_SLIDE
        // Pulling down from the top focuses the header's search.
        val pull = rememberPullToSearch(openSearch)
        // Pinching the album grid ([albumPinch]) makes its tiles larger or smaller, keeping the
        // album under the fingers where it was; TalkBack's action on a tile does, for that tile's.
        // The tiles on screen then move to their new places ([AlbumReflow]).
        val reflow = remember { AlbumReflow() }
        val firstAlbumRow = sections.find { it.section == Section.Albums }?.let { it.index + 1 }
        val albumRowAt = { y: Float ->
            listState.layoutInfo.visibleItemsInfo
                .find { y >= it.offset && y < it.offset + it.size }
                ?.takeIf { items.getOrNull(it.index) is MainItem.AlbumRow }
        }
        fun switchTiles(small: Boolean, album: Int, y: Int?) {
            val change = onSmallAlbumTiles ?: return
            val first = firstAlbumRow ?: return
            val newColumns = phoneAlbumColumns(width, small)
            reflow.capture(
                listState.layoutInfo.visibleItemsInfo.flatMapTo(HashSet()) { info ->
                    (items.getOrNull(info.index) as? MainItem.AlbumRow)?.albums.orEmpty().map { it.uid }
                },
            )
            listState.layoutInfo.visibleItemsInfo.find { it.index == first + album / columns }?.let { row ->
                val newRowHeight = cellWidth(newColumns) + with(density) { AlbumLabelsHeight.roundToPx() }
                val place = keptAlbumPlace(album, first, newColumns, y ?: row.offset, row.offset, row.size, newRowHeight)
                listState.requestScrollToItem(place.index, place.offset)
            }
            change(small)
            scope.launch { reflow.play() }
        }
        val canPinch = { at: Offset -> onSmallAlbumTiles != null && !searching && slide == 0f && albumRowAt(at.y) != null }
        val pinchTo = pinch@{ small: Boolean, at: Offset ->
            val first = firstAlbumRow ?: return@pinch
            val info = albumRowAt(at.y) ?: return@pinch
            val row = items[info.index] as MainItem.AlbumRow
            // The column under the fingers, counted from the start, which is the right in RTL.
            val step = cellPx + with(density) { TileSpacing.roundToPx() }
            val fromLeft = ((at.x - sides.getLeft(density, layoutDirection) - with(density) { Gutter.toPx() }) / step).toInt()
            val column = (if (layoutDirection == LayoutDirection.Rtl) columns - 1 - fromLeft else fromLeft).coerceIn(0, row.albums.lastIndex)
            switchTiles(small, (info.index - first) * columns + column, at.y.roundToInt())
        }
        val largerTiles = stringResource(R.string.album_tiles_larger)
        val smallerTiles = stringResource(R.string.album_tiles_smaller)
        val tileActions: ((Album) -> List<CustomAccessibilityAction>)? = if (onSmallAlbumTiles == null) null else { album ->
            listOf(
                CustomAccessibilityAction(if (smallAlbumTiles) largerTiles else smallerTiles) {
                    val i = library?.albums?.indexOfFirst { it.uid == album.uid } ?: -1
                    if (i >= 0) switchTiles(!smallAlbumTiles, i, null)
                    i >= 0
                },
            )
        }

        PrefetchArt(listState, artPx) { index -> items.getOrNull(index)?.art.orEmpty() }
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
            userScrollEnabled = !searching,
            modifier = Modifier
                .fillMaxSize()
                .albumPinch(smallAlbumTiles, canPinch, pinchTo)
                .pullToSearch(pull, listState),
        ) {
            items(items, key = { it.key }, contentType = { it.contentType }) { item ->
                if (item == MainItem.Header) {
                    Header(
                        query = query,
                        onQueryChange = { query = it },
                        onClear = { query = "" },
                        focusWanted = focusWanted,
                        onFocusTaken = { focusWanted = false },
                        focused = focused,
                        onFocusChange = { focused = it },
                        onSync = onSync,
                        onSettings = onSettings,
                        syncDot = syncDot,
                    )
                    return@items
                }
                Box(
                    Modifier.graphicsLayer {
                        translationY = slide * slidePx
                        alpha = 1f - slide
                    },
                ) {
                when (item) {
                    MainItem.Header -> Unit
                    MainItem.Banner -> banner?.invoke(Modifier.padding(horizontal = Gutter).padding(bottom = 8.dp))
                    MainItem.Empty -> EmptyLibrary(importPickers, inGroup, onSync, onChooseWhatToSync)
                    is MainItem.Heading -> SectionHeading(
                        item.section,
                        sections,
                        { current },
                        jumpTo,
                        importPickers = importPickers.takeIf { item.section == Section.Recents },
                        importRunning = importRunning,
                        onNew = onNewList?.let { new ->
                            when (item.section) {
                                Section.Playlists -> ({ new(false) })
                                Section.AlbumGroups -> ({ new(true) })
                                else -> null
                            }
                        },
                    )
                    is MainItem.RecentsRow -> TileRow(
                        item.recents.map { recent ->
                            Tile(
                                "${recent.ref.kind}:${recent.ref.uid}",
                                recent.title,
                                recent.subtitle(),
                                recent.art,
                                onLongClick = { onRecentMenu(recent) },
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
                    is MainItem.AlbumRow -> AlbumGridRow(item.albums, item.columns, onOpen, onAlbumMenu, tileActions, reflow, artPx)
                    is MainItem.ArtistItem -> LibraryRow(
                        item.artist.name,
                        item.artist.subtitle(),
                        item.artist.art,
                        CircleShape,
                        onLongClick = { onArtistMenu(item.artist) },
                    ) { onOpen(CollectionRef(CollectionRef.Kind.Artist, item.artist.uid)) }
                    is MainItem.CreditedArtistsRow -> CreditedArtistsRow(item.count, item.open) { creditedOpen = !creditedOpen }
                }
                }
            }
        }
        if (slide == 0f) FastScroller(
            listState,
            scrollEntries,
            scrollOffset = -statusBarPx,
            // The artists' letters would otherwise take most of the strip.
            minShares = mapOf(titles.getValue(Section.Albums) to ALBUMS_MIN_SHARE),
            kindOf = { index -> items.getOrNull(index)?.contentType },
            // Known before the list reaches them, from the width of their tiles.
            estimate = { kind ->
                when (kind) {
                    is AlbumRowType -> cellPx + with(density) { AlbumLabelsHeight.roundToPx() }
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
        // The last query typed, kept while its results slide away after the field's cleared.
        val kept = remember { object { var query = "" } }
        if (showResults) kept.query = query
        val found = rememberSearchResults(source, kept.query)
        AnimatedVisibility(
            visible = showResults,
            enter = fadeIn() + slideInVertically { -it / 8 },
            exit = fadeOut() + slideOutVertically { -it / 8 },
            modifier = Modifier
                .matchParentSize()
                .padding(top = with(density) { statusBarPx.toDp() } + HeaderHeight),
        ) {
            LazyColumn(
                // Clear of the keyboard as well as the mini player.
                contentPadding = sides
                    .add(WindowInsets(bottom = bottomPadding).union(WindowInsets.ime))
                    .asPaddingValues(),
                modifier = Modifier.fillMaxSize(),
            ) {
                searchResults(
                    kept.query,
                    found,
                    // A page opened from the results closes them, as the keyboard goes with it.
                    onOpen = { ref ->
                        closeSearch()
                        onOpen(ref)
                    },
                    onPlayTrack = onPlayTrack,
                    onTrackMenu = onTrackMenu,
                )
            }
        }
    }
}

/** How far down the library slides as search results come in, of the screen's height. */
private const val LIBRARY_SLIDE = 0.25f

/**
 * The logo, the search field with the name in it and Sync at its end, and Settings. It scrolls
 * away with the list.
 * [focusWanted] focuses the field, and [onFocusTaken] says so.
 */
@Composable
private fun Header(
    query: String,
    onQueryChange: (String) -> Unit,
    onClear: () -> Unit,
    focusWanted: Boolean,
    onFocusTaken: () -> Unit,
    focused: Boolean,
    onFocusChange: (Boolean) -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
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
        Logo(
            height = 48.dp,
            description = stringResource(R.string.app_name),
            modifier = Modifier.offset(x = (-4).dp).semantics { heading() },
        )
        HeaderSearchField(
            query, onQueryChange, onClear, focusWanted, onFocusTaken, focused, onFocusChange, onSync, syncDot,
            Modifier.weight(1f),
        )
        IconButton(onClick = onSettings) {
            Icon(Icons.Settings, contentDescription = stringResource(R.string.settings))
        }
    }
}

/**
 * The header's search field, in Material's search bar's shape and colour. The name stands in
 * for the placeholder until it's focused; screen readers hear Search. At its end, Sync ([onSync],
 * with a dot for [syncDot]), or Clear once something's typed.
 */
@Composable
private fun HeaderSearchField(
    query: String,
    onQueryChange: (String) -> Unit,
    onClear: () -> Unit,
    focusWanted: Boolean,
    onFocusTaken: () -> Unit,
    focused: Boolean,
    onFocusChange: (Boolean) -> Unit,
    onSync: () -> Unit,
    syncDot: Boolean,
    modifier: Modifier = Modifier,
) {
    val focus = remember { FocusRequester() }
    LaunchedEffect(focusWanted) {
        if (focusWanted) {
            focus.requestFocus()
            onFocusTaken()
        }
    }
    val label = stringResource(R.string.search)
    val colors = MaterialTheme.colorScheme
    Surface(
        shape = CircleShape,
        color = colors.surfaceContainerHigh,
        // The whole bar focuses the field, not only its text.
        modifier = modifier
            .height(48.dp)
            .clickable(interactionSource = null, indication = null) { focus.requestFocus() },
    ) {
        Row(Modifier.padding(start = 20.dp, end = 4.dp), verticalAlignment = Alignment.CenterVertically) {
            BasicTextField(
                value = query,
                onValueChange = onQueryChange,
                singleLine = true,
                textStyle = MaterialTheme.typography.bodyLarge.copy(color = colors.onSurface),
                cursorBrush = SolidColor(colors.primary),
                // Search keeps the keyboard up, as closing it closes the results.
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
                keyboardActions = KeyboardActions(onSearch = {}),
                decorationBox = { field ->
                    Box(contentAlignment = Alignment.CenterStart) {
                        if (query.isEmpty()) {
                            if (focused) {
                                Text(
                                    stringResource(R.string.search_placeholder),
                                    style = MaterialTheme.typography.bodyLarge,
                                    color = colors.onSurfaceVariant,
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                            } else {
                                WordmarkName(height = 24.dp, description = null)
                            }
                        }
                        field()
                    }
                },
                modifier = Modifier
                    .weight(1f)
                    .padding(end = 8.dp)
                    .focusRequester(focus)
                    .onFocusChanged { onFocusChange(it.isFocused) }
                    .semantics { contentDescription = label },
            )
            if (query.isNotEmpty()) {
                IconButton(onClick = onClear) {
                    Icon(Icons.Clear, contentDescription = stringResource(R.string.clear))
                }
            } else {
                IconButton(onClick = onSync) {
                    BadgedBox(badge = { if (syncDot) Badge() }) {
                        Icon(
                            Icons.Sync,
                            contentDescription = stringResource(if (syncDot) R.string.sync_bin_waiting else R.string.sync),
                        )
                    }
                }
            }
        }
    }
}

/**
 * A section's heading, a tap target across the whole row. Tapping it opens a list of every
 * section, and picking one scrolls there; [current] is the section at the top of the screen,
 * marked in the list. TalkBack gets the tap, and a jump to each other section as a custom action.
 * With [onNew], a + at the end, clear of the fast scroll strip, makes a new playlist or album
 * group; with [importPickers], + Import there opens their choices, or shows a spinner while an
 * import is [importRunning] (the card under the header shows how it's going).
 */
@Composable
private fun SectionHeading(
    section: Section,
    sections: List<SectionStart>,
    current: () -> Section?,
    onJump: (Int) -> Unit,
    onNew: (() -> Unit)? = null,
    importPickers: ImportPickers? = null,
    importRunning: Boolean = false,
) {
    var open by remember { mutableStateOf(false) }
    val runningText = stringResource(R.string.import_running_title)
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
        if (importPickers != null) {
            var importing by remember { mutableStateOf(false) }
            Box(Modifier.align(Alignment.CenterEnd).padding(end = FastScrollInset)) {
                if (importRunning) {
                    CircularProgressIndicator(
                        Modifier.padding(12.dp).size(24.dp).semantics { contentDescription = runningText },
                        strokeWidth = 2.dp,
                    )
                } else {
                    TextButton(onClick = { importing = true }) {
                        Icon(Icons.Add, contentDescription = null, modifier = Modifier.size(18.dp))
                        Text(stringResource(R.string.import_music), modifier = Modifier.padding(start = 8.dp))
                    }
                }
                ImportMenu(importing, importPickers, onDismiss = { importing = false })
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

/** What a recent shows under its title: an album's artist, or what kind of thing it is. */
@Composable
internal fun Recent.subtitle(): String = artist ?: stringResource(
    when (ref.kind) {
        CollectionRef.Kind.Artist -> R.string.artist
        CollectionRef.Kind.AlbumGroup -> R.string.album_group
        else -> R.string.playlist
    },
)

/** An artist's albums here, or for one only credited on tracks, their tracks. */
@Composable
internal fun Artist.subtitle(): String =
    if (albumCount > 0) pluralStringResource(R.plurals.albums, albumCount, albumCount)
    else pluralStringResource(R.plurals.tracks, trackCount, trackCount)

/** A list row with art, for playlists and artists. A right-click opens its menu, as a long press does. */
@Composable
internal fun LibraryRow(
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
        modifier = Modifier.listItemKeys(ListRowFocusShape, onLongClick).menuClickable(onLongClick, onClick),
    )
}

/**
 * The row at the end of Artists over the [count] artists only credited on others' tracks, which
 * shows them while [open]; tapping it opens or closes it. Its arrow is where the artists' pictures
 * are, clear of the fast scroll strip.
 */
@Composable
private fun CreditedArtistsRow(count: Int, open: Boolean, onToggle: () -> Unit) {
    ListItem(
        headlineContent = { Text(pluralStringResource(R.plurals.credited_artists_more, count, count)) },
        leadingContent = {
            Box(Modifier.size(ListArtSize), contentAlignment = Alignment.Center) {
                Icon(if (open) Icons.ExpandLess else Icons.ExpandMore, contentDescription = null)
            }
        },
        colors = ListItemDefaults.colors(containerColor = Color.Transparent),
        modifier = Modifier
            .listItemKeys(ListRowFocusShape, null)
            .clickable(onClick = onToggle)
            .semantics { if (open) collapse { onToggle(); true } else expand { onToggle(); true } },
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
            ImportMenu(menu, importPickers, onDismiss = { menu = false })
        }
    }
}
