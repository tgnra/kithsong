package ng.kithso.android.ui

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.material3.Badge
import androidx.compose.material3.BadgedBox
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.PlainTooltip
import androidx.compose.material3.Text
import androidx.compose.material3.TooltipAnchorPosition
import androidx.compose.material3.TooltipBox
import androidx.compose.material3.TooltipDefaults
import androidx.compose.material3.rememberTooltipState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveableStateHolder
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import ng.kithso.android.data.Library
import kotlin.math.abs

// Home as a strip of columns, each as wide as a pane, with the window showing two at a time: the
// left column (radio, recents, playlists, album groups), the middle one (albums, artists), and
// the pages. At rest it shows left and middle. A page opened from the middle column moves the strip
// over by one, so that column lands in the left pane with what was tapped at the same height.
//
// The strip has no animation of its own: it follows the page that shifted it, by how far on
// screen that is, so a slide in, a drag, predictive back, the system's animator scale and a
// page brought back by a rotation all reach it through DismissibleScreen.

/** What a [MainScreen] shows of the library: all of it in one list, or one column of [HomeLayout]. */
enum class HomeColumn { All, Left, Middle }

/** Where the strip's columns go, for [panes] that aren't exactly equal either side of a hinge. */
@Immutable
internal data class HomeStrip(
    /** Where the middle column rests: the pages' pane. */
    val middle: PaneArea,
    /**
     * How wide its content is. The narrower pane's width, in both: re-laying it out for each pane
     * would change the grid, and move what was tapped.
     */
    val middleContent: Dp,
    /** How far the left column goes to stay clear of the middle one; negative is left. */
    val leftMove: Dp,
    /** How far the middle column goes, to be centred in the main pane. */
    val middleMove: Dp,
)

internal fun homeStrip(panes: Panes): HomeStrip {
    val main = panes.main
    val pages = panes.pages
    val rest = pages.x + pages.width / 2
    val moved = main.x + main.width / 2 - rest
    val leftMove = maxOf(main.width, abs(moved.value).dp)
    return HomeStrip(
        // It clears both sides' insets, as it's at each in turn, and must not move inside itself.
        middle = pages.copy(sides = AllSides),
        middleContent = minOf(main.width, pages.width),
        leftMove = if (main.x < pages.x) -leftMove else leftMove,
        middleMove = moved,
    )
}

/** Keeps how far each page has slid in, for what follows them. */
@Stable
internal class PageSlides {
    private val fractions = mutableStateMapOf<Int, () -> Float>()

    fun set(id: Int, fraction: (() -> Float)?) {
        if (fraction == null) fractions.remove(id) else fractions[id] = fraction
    }

    /**
     * How far [nav]'s shifted pages have home moved over, 0 to 1: as far as the one furthest in,
     * so a page replacing another doesn't take the strip back while it arrives. A page on its way
     * out counts until it's gone, for home to come back with it.
     */
    fun shownOf(nav: BackStack<Page>): Float =
        nav.layers.filter { it.shifted }.maxOfOrNull { fractions[it.id]?.invoke() ?: 0f } ?: 0f
}

/**
 * Home, as [content] draws each [HomeColumn] of it: all in one list, or as columns if [panes] say
 * so. [shift] is how far the strip has moved, 0 to 1; [shifted] says that it's moving or has moved
 * over, and [pagesOpen] that the pages' pane has something in it, which covers the middle column.
 *
 * Each arrangement's scroll positions are saved apart, so going to the other on a fold and back
 * finds them where they were.
 */
@Composable
internal fun HomeLayout(
    panes: Panes,
    shift: () -> Float,
    shifted: Boolean,
    pagesOpen: Boolean,
    content: @Composable (HomeColumn) -> Unit,
) {
    val saved = rememberSaveableStateHolder()
    if (!panes.columns) {
        saved.SaveableStateProvider("list") { content(HomeColumn.All) }
    } else {
        saved.SaveableStateProvider("columns") {
            val strip = homeStrip(panes)
            Pane(panes.main) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .coveredIf(shifted)
                        .graphicsLayer { translationX = shift() * strip.leftMove.toPx() },
                ) { content(HomeColumn.Left) }
            }
            Pane(strip.middle) {
                Box(
                    Modifier
                        .fillMaxSize()
                        .coveredIf(pagesOpen && !shifted)
                        .graphicsLayer { translationX = shift() * strip.middleMove.toPx() },
                    contentAlignment = Alignment.Center,
                ) {
                    Box(Modifier.width(strip.middleContent).fillMaxHeight()) { content(HomeColumn.Middle) }
                }
            }
        }
    }
}

/**
 * The left column: what [mainItems] has before the albums, with what's a sideways row there as
 * rows of a grid [columns] wide.
 */
internal fun leftColumnItems(library: Library?, columns: Int, banner: Boolean, canMake: Boolean): List<MainItem> = buildList {
    add(MainItem.Header)
    if (banner) add(MainItem.Banner)
    if (library == null) return@buildList
    if (library.isEmpty) add(MainItem.Empty)
    val headings = canMake && !library.isEmpty
    if (library.stations.isNotEmpty()) {
        add(MainItem.Heading(Section.Radio))
        library.stations.chunked(columns).forEachIndexed { row, stations -> add(MainItem.RadioRow(stations, row)) }
    }
    if (library.recents.isNotEmpty()) {
        add(MainItem.Heading(Section.Recents))
        library.recents.chunked(columns).forEachIndexed { row, recents -> add(MainItem.RecentsRow(recents, row)) }
    }
    if (library.playlists.isNotEmpty() || headings) {
        add(MainItem.Heading(Section.Playlists))
        library.playlists.mapTo(this, MainItem::PlaylistItem)
    }
    if (library.albumGroups.isNotEmpty() || headings) {
        add(MainItem.Heading(Section.AlbumGroups))
        library.albumGroups.chunked(columns).forEachIndexed { row, groups -> add(MainItem.AlbumGroupsRow(groups, row)) }
    }
}

/** The middle column: the albums, then the artists. */
internal fun middleColumnItems(library: Library?, columns: Int): List<MainItem> = buildList {
    add(MainItem.Top)
    if (library == null) return@buildList
    if (library.albums.isNotEmpty()) {
        add(MainItem.Heading(Section.Albums))
        library.albums.chunked(columns).mapTo(this, MainItem::AlbumRow)
    }
    if (library.artists.isNotEmpty()) {
        add(MainItem.Heading(Section.Artists))
        library.artists.mapTo(this, MainItem::ArtistItem)
    }
}

/** [tiles] as a sideways [TileRow], or with a [gridRow], as that row of a grid [columns] wide. */
@Composable
internal fun Tiles(gridRow: Int?, columns: Int, tiles: List<Tile>, state: LazyListState) {
    if (gridRow == null) {
        TileRow(tiles, state = state)
    } else {
        Row(
            Modifier.padding(horizontal = BrowseGutter).padding(bottom = 16.dp),
            horizontalArrangement = Arrangement.spacedBy(TileSpacing),
        ) {
            for (tile in tiles) {
                ArtTile(
                    tile.title,
                    tile.subtitle,
                    tile.shownArt(),
                    onLongClick = tile.onLongClick,
                    onClick = tile.onClick,
                    modifier = Modifier.weight(1f),
                    badge = tile.badge,
                    description = tile.description,
                )
            }
            repeat(columns - tiles.size) { Spacer(Modifier.weight(1f)) }
        }
    }
}

/** A pane with nothing to show: a faint mark, so that it reads as empty rather than broken. */
@Composable
internal fun QuietPane(modifier: Modifier = Modifier) {
    Box(modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
        Icon(
            Icons.LibraryMusic,
            contentDescription = null,
            tint = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.25f),
            modifier = Modifier.size(72.dp),
        )
    }
}

private val HeaderIconSize = 48.dp
private const val HEADER_ICONS = 5
private const val WORDMARK_RATIO = 3.52f
private val FullWordmarkHeight = 48.dp
private val SmallestWordmarkHeight = 36.dp
private val SmallestTile = 32.dp

/** What goes beside the header's icons in a space [room] wide: all of the wordmark, its tile, or neither. */
internal sealed interface HeaderMark {
    data class Wordmark(val height: Dp) : HeaderMark
    data class Tile(val size: Dp) : HeaderMark
    data object None : HeaderMark
}

internal fun headerMark(room: Dp): HeaderMark {
    // The tile's margin is drawn past the gutter, so a mark takes this much less room than it's wide.
    val tuck = 4.dp
    val height = minOf(FullWordmarkHeight, (room + tuck) / WORDMARK_RATIO)
    return when {
        height >= SmallestWordmarkHeight -> HeaderMark.Wordmark(height)
        room + tuck >= SmallestTile -> HeaderMark.Tile(minOf(FullWordmarkHeight, room + tuck))
        else -> HeaderMark.None
    }
}

/**
 * The header with room for it: everything the phone's header has in its menu is a button of its
 * own. The icons always fit, and the wordmark shrinks and then gives way to its tile to let them.
 * Like the phone's it scrolls away with the list.
 */
@Composable
internal fun WideHeader(
    onSearch: () -> Unit,
    onSync: () -> Unit,
    onSettings: () -> Unit,
    onStats: () -> Unit,
    importPickers: ImportPickers,
    importRunning: Boolean,
    syncDot: Boolean,
) {
    BoxWithConstraints(Modifier.fillMaxWidth().windowInsetsPadding(WindowInsets.statusBars.inPane())) {
        val room = maxWidth - BrowseGutter - 4.dp - HeaderIconSize * HEADER_ICONS
        Row(
            Modifier.fillMaxWidth().height(HeaderHeight).padding(start = BrowseGutter, end = 4.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            val name = stringResource(R.string.app_name)
            val mark = Modifier.offset(x = (-4).dp).semantics { heading() }
            Box(Modifier.weight(1f)) {
                when (val fits = headerMark(room)) {
                    is HeaderMark.Wordmark -> Wordmark(fits.height, name, mark)
                    is HeaderMark.Tile -> WordmarkTile(fits.size, name, mark)
                    HeaderMark.None -> {}
                }
            }
            var importing by remember { mutableStateOf(false) }
            Box {
                HeaderIcon(R.string.import_music, { importing = true }, if (importRunning) stringResource(R.string.import_running_title) else null) {
                    Box(contentAlignment = Alignment.Center) {
                        Icon(Icons.Import, contentDescription = null)
                        if (importRunning) CircularProgressIndicator(Modifier.size(32.dp), strokeWidth = 2.dp)
                    }
                }
                ImportMenu(importing, importPickers, onDismiss = { importing = false })
            }
            HeaderIcon(R.string.search, onSearch) { Icon(Icons.Search, contentDescription = null) }
            HeaderIcon(R.string.sync, onSync, if (syncDot) stringResource(R.string.sync_bin_waiting) else null) {
                BadgedBox(badge = { if (syncDot) Badge() }) { Icon(Icons.Sync, contentDescription = null) }
            }
            HeaderIcon(R.string.stats_title, onStats) { Icon(Icons.BarChart, contentDescription = null) }
            HeaderIcon(R.string.settings, onSettings) { Icon(Icons.Settings, contentDescription = null) }
        }
    }
}

/** A header button: [label] is its tooltip, and what TalkBack says of it, unless there's something to [say] instead. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun HeaderIcon(label: Int, onClick: () -> Unit, say: String? = null, icon: @Composable () -> Unit) {
    val text = stringResource(label)
    TooltipBox(
        positionProvider = TooltipDefaults.rememberTooltipPositionProvider(TooltipAnchorPosition.Below),
        tooltip = { PlainTooltip { Text(text) } },
        state = rememberTooltipState(),
    ) {
        IconButton(
            onClick = onClick,
            modifier = Modifier.size(HeaderIconSize).semantics { contentDescription = say ?: text },
            content = icon,
        )
    }
}
