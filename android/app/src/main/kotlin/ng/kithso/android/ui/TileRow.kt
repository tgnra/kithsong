package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.snapping.SnapPosition
import androidx.compose.foundation.gestures.snapping.rememberSnapFlingBehavior
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.remember
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.snapshots.Snapshot
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.dropWhile
import ng.kithso.android.data.ArtRef
import ng.kithso.android.ui.art.AlbumArt

// A row of tiles scrolling sideways under a heading: art with text underneath, as album tiles
// have. It knows nothing about what the tiles are; the section using it makes a [Tile] of each
// thing it shows.
//
// To use it in a LazyColumn, make the row an item of its own, and keep the row's scroll position
// above the list, as the list drops the item when it scrolls away:
//
//     val row = rememberTileRowState(listState, "row:mixes")   // outside the LazyColumn
//     LazyColumn(state = listState) {
//         item(key = "row:mixes") {
//             TileRow(mixes.map { Tile(it.uid, it.name, it.subtitle, it.art) { play(it) } }, state = row)
//         }
//     }
//
// The second argument must be the row's item key: that's how it knows the row has left the
// screen, and after TILE_ROW_RESET_MS away the row goes back to its start. Leave a section with
// no tiles out altogether. See tileColumns for how many tiles show.

/**
 * What a [TileRow] shows of one thing. [key] stays the same for as long as the thing does, and
 * no two tiles in a row share one: it's how the row keeps its place when tiles come and go.
 * [title] and [subtitle] are one line each, ending in an ellipsis if they don't fit. Without
 * [art] the tile has what a thing with no cover has, a coloured square with the title's first
 * letter. [badge] goes in the top corner of the art, for a small mark; if it means something,
 * give it a content description, which TalkBack reads after the text. [onLongClick] is for a
 * menu. [description] is what TalkBack says in place of the title and subtitle, for a tile they
 * don't explain.
 */
@Immutable
data class Tile(
    val key: String,
    val title: String,
    val subtitle: String? = null,
    val art: ArtRef? = null,
    val onLongClick: (() -> Unit)? = null,
    val badge: (@Composable () -> Unit)? = null,
    val description: String? = null,
    val onClick: () -> Unit,
)

/** How long a row is off screen before it goes back to its start. */
internal const val TILE_ROW_RESET_MS = 3_000L

/** The fewest whole tiles across, on a phone held upright. */
private const val MIN_TILE_COLUMNS = 3

/** How much of the next tile shows at the edge, to say the row scrolls, as part of a tile. */
private const val TILE_SLICE = 0.25f

/** As wide as a tile gets: past this, as on a tablet or sideways, more of them fit instead. */
private val MaxTileWidth = 176.dp

/** How wide a tile is with [columns] whole ones and a slice across [width]. */
internal fun tileWidth(width: Dp, columns: Int): Dp =
    (width - BrowseGutter - TileSpacing * columns) / (columns + TILE_SLICE)

/**
 * How many whole tiles show across [width], with a slice of the next: three, or on a wider
 * screen as many as it takes to keep them to [MaxTileWidth].
 */
internal fun tileColumns(width: Dp): Int {
    var columns = MIN_TILE_COLUMNS
    while (tileWidth(width, columns) > MaxTileWidth) columns++
    return columns
}

/** A tile's width across [width]. It doesn't depend on how many tiles there are. */
internal fun tileWidth(width: Dp): Dp = tileWidth(width, tileColumns(width))

/** The art a tile shows: its own, or a placeholder drawn from its title. */
internal fun Tile.shownArt(): ArtRef = art ?: ArtRef(key, title)

/**
 * Calls [reset] once [onScreen] has been false for [TILE_ROW_RESET_MS]. It only starts counting
 * from the first true: until a row has been on screen, as when a turned phone brings the list
 * back with the row further down, there's nothing to say it has just left.
 */
internal suspend fun resetAfterOffScreen(onScreen: Flow<Boolean>, reset: () -> Unit) {
    onScreen.dropWhile { !it }.distinctUntilChanged().collectLatest { here ->
        if (!here) {
            delay(TILE_ROW_RESET_MS)
            reset()
        }
    }
}

/**
 * The scroll position of a [TileRow] that's item [itemKey] of the list [column]. It outlives the
 * item and survives the phone turning, and goes back to the start once the row has been out of
 * [column] for [TILE_ROW_RESET_MS]. Screens opening over the list don't count: the list is still
 * there beneath them, so coming back finds the row as it was.
 */
@Composable
fun rememberTileRowState(column: LazyListState, itemKey: Any): LazyListState {
    val row = rememberLazyListState()
    LaunchedEffect(column, itemKey, row) {
        resetAfterOffScreen(snapshotFlow { column.layoutInfo.visibleItemsInfo.any { it.key == itemKey } }) {
            // Not scrollToItem, which would wait for a row that isn't laid out.
            row.requestScrollToItem(0)
        }
    }
    return row
}

/**
 * [tiles] in a row scrolling sideways, starting at the gutter, and with the space below it that
 * album rows have. [tileColumns] whole tiles fit across, with a slice of the next while there
 * are more; a shorter row has tiles of the same width and stops where they do. Flings land with
 * a tile at the gutter. In a list, take [state] from [rememberTileRowState].
 */
@Composable
fun TileRow(tiles: List<Tile>, modifier: Modifier = Modifier, state: LazyListState = rememberLazyListState()) {
    // A tile arriving at the front of a row at its start shows there. The list would otherwise
    // stay with the tile that was first, leaving the new one out of sight to the left. This has
    // to be here, before the row's next layout, which is when the list follows the old tile.
    val front = tiles.firstOrNull()?.key
    remember(front) {
        Snapshot.withoutReadObservation {
            if (state.firstVisibleItemIndex == 0 && state.firstVisibleItemScrollOffset == 0) state.requestScrollToItem(0)
        }
        front
    }
    // All the tiles or none keep a line for the sub line, so the row's height stays the same.
    val subtitles = tiles.any { it.subtitle != null }
    BoxWithConstraints(modifier.fillMaxWidth().padding(bottom = 16.dp)) {
        val width = tileWidth(maxWidth)
        LazyRow(
            state = state,
            contentPadding = PaddingValues(horizontal = BrowseGutter),
            horizontalArrangement = Arrangement.spacedBy(TileSpacing),
            flingBehavior = rememberSnapFlingBehavior(state, SnapPosition.Start),
        ) {
            items(tiles, key = { it.key }, contentType = { "tile" }) { tile ->
                ArtTile(
                    tile.title,
                    tile.subtitle ?: "".takeIf { subtitles },
                    tile.shownArt(),
                    onLongClick = tile.onLongClick,
                    onClick = tile.onClick,
                    modifier = Modifier.width(width),
                    badge = tile.badge,
                    description = tile.description,
                )
            }
        }
    }
}

/** A radio station's mark on its art, so it doesn't read as the album it's showing. */
@Composable
internal fun StationBadge() {
    Box(
        Modifier.size(28.dp).background(MaterialTheme.colorScheme.primary, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Icon(Icons.Radio, contentDescription = null, tint = MaterialTheme.colorScheme.onPrimary, modifier = Modifier.size(18.dp))
    }
}

/**
 * Art with a title and, unless it's null, a [subtitle] under it, as a [modifier]-wide tap target;
 * the [badge] goes in the art's top corner. [description], if any, is what TalkBack says of it.
 * A right-click opens its menu too, and in the tablet layout it drags [drag], if given.
 */
@Composable
internal fun ArtTile(
    title: String,
    subtitle: String?,
    art: ArtRef,
    onLongClick: (() -> Unit)?,
    onClick: () -> Unit,
    modifier: Modifier,
    badge: (@Composable () -> Unit)? = null,
    description: String? = null,
    drag: DragItem? = null,
) {
    val said = description ?: if (subtitle.isNullOrEmpty()) title else "$title, $subtitle"
    Column(
        modifier
            .clip(RoundedCornerShape(12.dp))
            .listItemKeys(RoundedCornerShape(12.dp), onLongClick)
            .menuClickable(onLongClick, onClick, drag)
            .semantics { contentDescription = said },
    ) {
        Box {
            AlbumArt(
                art,
                contentDescription = null,
                shape = RoundedCornerShape(12.dp),
                modifier = Modifier.fillMaxWidth().aspectRatio(1f),
            )
            if (badge != null) Box(Modifier.align(Alignment.TopEnd).padding(6.dp)) { badge() }
        }
        Text(
            title,
            style = MaterialTheme.typography.titleSmall,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            // The tile's description says this; TalkBack would otherwise read it twice.
            modifier = Modifier.padding(top = 6.dp).clearAndSetSemantics {},
        )
        if (subtitle != null) {
            Text(
                subtitle,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.clearAndSetSemantics {},
            )
        }
    }
}
