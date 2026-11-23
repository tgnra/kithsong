package ng.kithso.android.ui

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.FastOutSlowInEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.GraphicsLayerScope
import androidx.compose.ui.graphics.TransformOrigin
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.Measurable
import androidx.compose.ui.layout.MeasureResult
import androidx.compose.ui.layout.MeasureScope
import androidx.compose.ui.layout.positionInRoot
import androidx.compose.ui.node.LayoutModifierNode
import androidx.compose.ui.node.ModifierNodeElement
import androidx.compose.ui.node.requireLayoutCoordinates
import androidx.compose.ui.unit.Constraints
import androidx.compose.ui.unit.Dp
import kotlin.math.roundToInt

// Pinching the phone's album grid: out for larger tiles, in for smaller ones, which fit a
// column more across ([AppSettings.smallAlbumTiles]). The tiles then slide and scale to their
// new places ([AlbumReflow]) rather than jump there.

/** How far two fingers pinch, as a scale, before the album tiles change size: a fifth either way. */
internal const val PINCH_SCALE = 1.2f

/** The phone's main screen's album columns across [width]: [albumColumns], or one more with [small] tiles. */
internal fun phoneAlbumColumns(width: Dp, small: Boolean): Int = albumColumns(width) + if (small) 1 else 0

/**
 * Whether the tiles should be small after a pinch [zoom]ed this far (its scale since the
 * fingers went down), or null while it's within [PINCH_SCALE] or the tiles are already that
 * size. Zooming in (pinching out) makes them larger.
 */
internal fun pinchedSmall(zoom: Float, small: Boolean): Boolean? = when {
    small && zoom >= PINCH_SCALE -> false
    !small && zoom <= 1 / PINCH_SCALE -> true
    else -> null
}

/** Where a lazy list scrolls to: [index] at the top, [offset] pixels past its start. */
internal data class ListPlace(val index: Int, val offset: Int)

/**
 * Where the main list goes for album [album] (its place in the library's albums) to stay where
 * it was once the grid has [columns] across, its rows from item [firstRow]. Its row was at
 * [rowTop] and [rowHeight] tall, and is about [newRowHeight] tall now; the point [y] in it
 * (the pinch's, say) stays as far down the row as it was, so the album stays under the fingers.
 */
internal fun keptAlbumPlace(album: Int, firstRow: Int, columns: Int, y: Int, rowTop: Int, rowHeight: Int, newRowHeight: Int): ListPlace {
    val fraction = if (rowHeight > 0) ((y - rowTop).toFloat() / rowHeight).coerceIn(0f, 1f) else 0f
    return ListPlace(firstRow + album / columns, (fraction * newRowHeight).roundToInt() - y)
}

/**
 * A two-finger pinch calls [onPinch] once a gesture, with whether the tiles are to be small
 * ([pinchedSmall] of [small]) and where the fingers are, if it starts where [canPinch] says;
 * the latest of each is used, without starting a gesture again. It looks at touches before the
 * list and its tiles do, and leaves one finger's alone, so scrolling, taps, long presses and
 * drags are as they were; from the second finger down it takes the rest of the gesture, so the
 * list doesn't scroll and nothing's tapped, long-pressed or dragged.
 */
@Composable
internal fun Modifier.albumPinch(
    small: Boolean,
    canPinch: (Offset) -> Boolean,
    onPinch: (small: Boolean, at: Offset) -> Unit,
): Modifier {
    val isSmall by rememberUpdatedState(small)
    val can by rememberUpdatedState(canPinch)
    val pinched by rememberUpdatedState(onPinch)
    return pointerInput(Unit) { awaitEachGesture {
        awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
        // Null until a second finger comes down: then whether this gesture is a pinch.
        var pinching: Boolean? = null
        var zoom = 1f
        var done = false
        do {
            val event = awaitPointerEvent(PointerEventPass.Initial)
            if (pinching == null && event.changes.count { it.pressed } >= 2) pinching = can(event.calculateCentroid())
            if (pinching == true) {
                if (!done) {
                    zoom *= event.calculateZoom()
                    pinchedSmall(zoom, isSmall)?.let {
                        done = true
                        pinched(it, event.calculateCentroid())
                    }
                }
                event.changes.forEach { it.consume() }
            }
        } while (event.changes.any { it.pressed })
    } }
}

/** How long the album tiles take to move to their places in the new columns, at 1x animator speed. */
private const val REFLOW_MILLIS = 300

/**
 * Where a tile is drawn, part way ([progress], 0 to 1) from [from] to where it's laid out now:
 * [to], [toWidth] wide. It's moved by [dx] and [dy] and scaled by [scale] about its top left,
 * so its width is the old one's at 0 and its own at 1; the height scales with it, so the art
 * stays square.
 */
internal data class TileShift(val dx: Float, val dy: Float, val scale: Float)

internal fun tileShift(from: Rect, to: Offset, toWidth: Float, progress: Float): TileShift {
    val left = 1 - progress
    val scale = if (toWidth > 0) from.width / toWidth else 1f
    return TileShift((from.left - to.x) * left, (from.top - to.y) * left, scale + (1 - scale) * progress)
}

/**
 * Moves the main screen's album tiles from where they were to their places in the new columns
 * when a pinch changes them, so the grid reflows rather than jumps. [capture] notes where the
 * tiles on screen are, just before the change; [play] then slides and scales each tile
 * ([albumReflow]) from there, and fades in those that weren't on screen. It's all in the
 * tiles' layers, read as they're drawn, so nothing's composed or measured again as it plays.
 * It runs at the system's animator speed, so with animations off the tiles jump as before.
 */
@Stable
internal class AlbumReflow {
    // The tiles composed now, and where those on screen were when last captured, by album uid.
    internal val tiles = HashSet<AlbumReflowNode>()
    internal var from: Map<String, Rect> = emptyMap()
    internal val progress = Animatable(1f)

    /** Notes where the tiles of the albums [onScreen] are drawn, part way through a reflow too. */
    fun capture(onScreen: Set<String>) {
        from = tiles.mapNotNull { tile -> tile.takeIf { it.uid in onScreen }?.drawn()?.let { tile.uid to it } }.toMap()
    }

    /** Moves the tiles from where [capture] found them to where they are now. */
    suspend fun play() {
        progress.snapTo(0f)
        progress.animateTo(1f, tween(REFLOW_MILLIS, easing = FastOutSlowInEasing))
    }
}

/** Moves the tile of album [uid] as [reflow] plays. */
internal fun Modifier.albumReflow(reflow: AlbumReflow, uid: String): Modifier = this then AlbumReflowElement(reflow, uid)

private data class AlbumReflowElement(val reflow: AlbumReflow, val uid: String) : ModifierNodeElement<AlbumReflowNode>() {
    override fun create() = AlbumReflowNode(reflow, uid)

    override fun update(node: AlbumReflowNode) {
        if (node.reflow !== reflow && node.isAttached) {
            node.reflow.tiles -= node
            reflow.tiles += node
        }
        node.reflow = reflow
        node.uid = uid
    }
}

internal class AlbumReflowNode(var reflow: AlbumReflow, var uid: String) : Modifier.Node(), LayoutModifierNode {
    private var width = 0
    private var height = 0

    override fun onAttach() {
        reflow.tiles += this
    }

    override fun onDetach() {
        reflow.tiles -= this
    }

    /** Where the tile's laid out, in the root's coordinates; its own layer doesn't move it. */
    private fun laidOut(): Offset? = if (isAttached) requireLayoutCoordinates().takeIf { it.isAttached }?.positionInRoot() else null

    /** Where the tile's drawn now, in the root's coordinates. */
    fun drawn(): Rect? {
        val at = laidOut() ?: return null
        val from = reflow.from[uid]
        val p = reflow.progress.value
        if (from == null || p >= 1f) return Rect(at.x, at.y, at.x + width, at.y + height)
        val shift = tileShift(from, at, width.toFloat(), p)
        return Rect(at.x + shift.dx, at.y + shift.dy, at.x + shift.dx + width * shift.scale, at.y + shift.dy + height * shift.scale)
    }

    // Read as the layer's drawn: once a frame while a reflow plays, otherwise not again.
    private val layer: GraphicsLayerScope.() -> Unit = layer@{
        val p = reflow.progress.value
        if (p >= 1f) return@layer
        val at = laidOut() ?: return@layer
        val from = reflow.from[uid]
        if (from == null) {
            alpha = p
            return@layer
        }
        val shift = tileShift(from, at, width.toFloat(), p)
        translationX = shift.dx
        translationY = shift.dy
        scaleX = shift.scale
        scaleY = shift.scale
        transformOrigin = TransformOrigin(0f, 0f)
    }

    override fun MeasureScope.measure(measurable: Measurable, constraints: Constraints): MeasureResult {
        val placeable = measurable.measure(constraints)
        return layout(placeable.width, placeable.height) {
            width = placeable.width
            height = placeable.height
            placeable.placeWithLayer(0, 0, layerBlock = layer)
        }
    }
}
