package ng.kithso.android.ui

import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalLayoutDirection
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Density
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import ng.kithso.android.data.Album
import ng.kithso.android.data.CollectionRef

// Grids of tiles that keep off a fold. On a book-style foldable held open, the fold runs down
// the middle of the window, and a tile across it is bent in two (or, half open, split between
// two screens). So the grid's columns are worked out from where the fold is: one gap between
// them is wide enough to take the fold, and the tiles either side are the same width as each
// other. A fold across the window can't be kept off a list that scrolls through it, so only a
// vertical one counts.

/** How far a tile keeps from either edge of a fold: a fold with no width gets a gap of twice this. */
internal val FoldClearance = 12.dp

/**
 * How narrow a tile may get to have tiles on both sides of a fold, rather than none on one: a
 * strip beside the fold with nothing in it looks broken, and slightly smaller tiles don't.
 */
internal val FoldMinTileWidth = 120.dp

/** Where a fold crosses a grid, from its left edge, whatever the layout direction. */
@Immutable
data class FoldSpan(val start: Dp, val end: Dp)

/**
 * A grid's columns: each tile is [tileWidth] wide, and the [starts] of the columns are from the
 * grid's left edge, in order from the left.
 */
@Immutable
data class GridColumns(val tileWidth: Dp, val starts: List<Dp>) {
    val count: Int get() = starts.size
}

/** [width]'s columns as the phone has them: [albumColumns] of them, evenly spaced between the gutters. */
internal fun evenColumns(width: Dp): GridColumns {
    val n = albumColumns(width)
    val tile = (width - BrowseGutter * 2 - TileSpacing * (n - 1)) / n
    return GridColumns(tile, List(n) { BrowseGutter + (tile + TileSpacing) * it })
}

/**
 * The columns of a grid [width] wide that keep clear of [fold], or [evenColumns] when there's
 * no fold in it. Either side of the fold gets as many columns as fit, all of one width, which
 * the more crowded side sets; the other side spreads its spare room evenly between its
 * gutter, its gaps and its edge of the fold, so neither side looks stretched. If that would
 * leave one side with no tiles though it has room for a narrower one, tiles shrink as far as
 * [FoldMinTileWidth] to put some there.
 */
internal fun gridColumns(width: Dp, fold: FoldSpan?): GridColumns {
    if (fold == null || fold.end <= BrowseGutter || fold.start >= width - BrowseGutter) return evenColumns(width)
    val leftStart = BrowseGutter
    val leftRoom = fold.start - FoldClearance - leftStart
    val rightStart = fold.end + FoldClearance
    val rightRoom = width - BrowseGutter - rightStart
    var split = splitColumns(leftRoom, rightRoom, MinTileWidth, both = false)
    if (split == null || split.left == 0 || split.right == 0) {
        split = splitColumns(leftRoom, rightRoom, FoldMinTileWidth, both = true) ?: split
    }
    // Too narrow for a tile beside the fold at all: better across it than none.
    if (split == null) return evenColumns(width)
    val tile = split.tile
    return GridColumns(tile, spread(leftStart, leftRoom, split.left, tile) + spread(rightStart, rightRoom, split.right, tile))
}

/** How many columns go either side of a fold, and how wide their tiles are. */
private data class Split(val left: Int, val right: Int, val tile: Dp)

/**
 * The most columns that fit in [leftRoom] and [rightRoom] with tiles of one width, at least
 * [minTile]; of those, the widest tiles. With [both], each side has a column.
 */
private fun splitColumns(leftRoom: Dp, rightRoom: Dp, minTile: Dp, both: Boolean): Split? {
    var best: Split? = null
    for (left in 0..fitCount(leftRoom, minTile)) {
        for (right in 0..fitCount(rightRoom, minTile)) {
            if (left + right == 0 || (both && (left == 0 || right == 0))) continue
            val tile = minOf(
                if (left > 0) tileFor(leftRoom, left) else Dp.Infinity,
                if (right > 0) tileFor(rightRoom, right) else Dp.Infinity,
            )
            if (tile < minTile) continue
            val b = best
            if (b == null || left + right > b.left + b.right || (left + right == b.left + b.right && tile > b.tile)) {
                best = Split(left, right, tile)
            }
        }
    }
    return best
}

private fun fitCount(room: Dp, minTile: Dp): Int =
    if (room < minTile) 0 else ((room + TileSpacing) / (minTile + TileSpacing)).toInt()

private fun tileFor(room: Dp, count: Int): Dp = (room - TileSpacing * (count - 1)) / count

/**
 * The starts of [count] tiles [tile] wide in [room] from [start], what's left over shared
 * between the space before them, the gaps between and the space after.
 */
private fun spread(start: Dp, room: Dp, count: Int, tile: Dp): List<Dp> {
    if (count == 0) return emptyList()
    val extra = ((room - tile * count - TileSpacing * (count - 1)) / (count + 1)).coerceAtLeast(0.dp)
    return List(count) { start + extra + (tile + TileSpacing + extra) * it }
}

/**
 * Where a grid is in the window, for keeping its tiles off the fold: put [modifier] on the
 * element whose width the grid's rows fill, and read [span]. [fold] is the window's, from
 * [rememberWindowShape]; [startPadding] is padding at the element's left the rows start after.
 */
@Stable
internal class GridFold(private val density: Density) {
    var fold: Fold? = null
    var startPadding: Float = 0f

    /** The fold, across the rows, or null if there's none or it's across the window. */
    var span by mutableStateOf<FoldSpan?>(null)
        private set

    val modifier: Modifier = Modifier.onGloballyPositioned { coordinates ->
        val f = fold
        span = if (f == null || !f.vertical || !coordinates.isAttached) {
            null
        } else {
            val bounds = f.boundsIn(coordinates)
            with(density) { FoldSpan((bounds.left - startPadding).toDp(), (bounds.right - startPadding).toDp()) }
        }
    }
}

/** A [GridFold] for the window's [fold], if the grid keeps clear of it; null leaves it out. */
@Composable
internal fun rememberGridFold(fold: Fold?, startPadding: Float = 0f): GridFold {
    val density = LocalDensity.current
    val grid = remember(density) { GridFold(density) }
    grid.fold = fold
    grid.startPadding = startPadding
    return grid
}

/**
 * One row of a grid laid out by [columns]: [count] tiles, each made by [tile], at their
 * columns' places, with the space below that album rows have. Right to left, the first tile
 * takes the rightmost column, while the columns stay where the fold put them.
 */
@Composable
internal fun GridRow(columns: GridColumns, count: Int, tile: @Composable (Int) -> Unit) {
    val rtl = LocalLayoutDirection.current == LayoutDirection.Rtl
    Layout(
        content = { repeat(count.coerceAtMost(columns.count)) { tile(it) } },
        modifier = Modifier.fillMaxWidth().padding(bottom = 16.dp),
    ) { measurables, constraints ->
        val width = columns.tileWidth.roundToPx().coerceAtLeast(0)
        val placeables = measurables.map { it.measure(Constraints.fixedWidth(width)) }
        layout(constraints.maxWidth, placeables.maxOfOrNull { it.height } ?: 0) {
            placeables.forEachIndexed { i, placeable ->
                val column = if (rtl) columns.count - 1 - i else i
                placeable.place(columns.starts[column].roundToPx(), 0)
            }
        }
    }
}

/** [AlbumGridRow] with [columns] that may keep off a fold. */
@Composable
internal fun AlbumGridRow(albums: List<Album>, columns: GridColumns, onOpen: (CollectionRef) -> Unit, onMenu: (Album) -> Unit) {
    GridRow(columns, albums.size) { i ->
        val album = albums[i]
        ArtTile(
            album.title,
            album.artist,
            album.art,
            onLongClick = { onMenu(album) },
            onClick = { onOpen(CollectionRef(CollectionRef.Kind.Album, album.uid)) },
            modifier = Modifier,
            drag = DragItem.Album(album.uid, album.title, album.artist),
        )
    }
}
