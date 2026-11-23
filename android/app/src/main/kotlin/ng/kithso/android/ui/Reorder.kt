package ng.kithso.android.ui

import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.lazy.LazyListItemInfo
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.zIndex
import kotlinx.coroutines.Job
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch

/** A dragged item's move to [target]'s place in the list, where it'll be laid out at [offset]. */
internal data class ReorderMove(val target: LazyListItemInfo, val offset: Int)

/**
 * Where the dragged [item] moves with its middle at [centre], among the list's [visible] items:
 * past the furthest one that leaves its middle within its new place, or null to stay. An item as
 * tall as it is passed once its middle's over that. Over a taller one that alone would have it
 * moved back at once, as its middle would still be over the other, and so on back and forth; it
 * has to be dragged further over, to where its own new place is.
 */
internal fun reorderMove(
    item: LazyListItemInfo,
    visible: List<LazyListItemInfo>,
    centre: Int,
    isMovable: (Any) -> Boolean,
): ReorderMove? {
    // Moved down, it ends where the target does now; moved up, it starts where the target does.
    visible.lastOrNull {
        it.index > item.index && isMovable(it.key) && centre >= maxOf(it.offset, it.offset + it.size - item.size)
    }?.let { return ReorderMove(it, it.offset + it.size - item.size) }
    return visible.firstOrNull {
        it.index < item.index && isMovable(it.key) && centre < it.offset + minOf(it.size, item.size)
    }?.let { ReorderMove(it, it.offset) }
}

/**
 * Dragging a LazyColumn's items by a handle ([dragHandle]) to reorder them. The list's owner
 * keeps the order: [onStart] is called with an item's key as its drag starts; while it's dragged
 * over another, [onMove] is called with the two items' keys and should move the first to where
 * the second is; [onDrop] is called with the dragged item's key once it's let go. The items
 * needn't be of one height ([reorderMove]). Only items whose key [isMovable] take part, so headers
 * stay put. Holding the item near the top or bottom scrolls the list.
 */
@Stable
class ReorderState(private val listState: LazyListState) {
    /** The key of the item being dragged, or null. */
    var dragged by mutableStateOf<Any?>(null)
        private set

    // How far the dragged item is drawn from its place in the list.
    private var offset by mutableFloatStateOf(0f)
    // After a move, the dragged item's index in the next layout; no other move is made until
    // it's been laid out there, as the layout seen until then is the old one.
    private var awaiting: Int? = null

    var isMovable: (Any) -> Boolean = { true }
    var onStart: (Any) -> Unit = {}
    var onMove: (from: Any, to: Any) -> Unit = { _, _ -> }
    var onDrop: (Any) -> Unit = {}

    /** For the dragged item: drawn over the others where the finger has it. */
    fun Modifier.dragged(key: Any): Modifier =
        if (key == dragged) zIndex(1f).graphicsLayer { translationY = offset } else this

    /**
     * Makes this the handle of the item with [key]. While it's held near an end of the list,
     * the list scrolls each frame, whether or not the finger moves.
     */
    fun Modifier.dragHandle(key: Any): Modifier = pointerInput(key) {
        coroutineScope {
            var scrolling: Job? = null
            val stop = {
                scrolling?.cancel()
                scrolling = null
                end()
            }
            detectDragGestures(
                onDragStart = {
                    start(key)
                    scrolling = launch { autoScroll(key) }
                },
                onDragEnd = stop,
                onDragCancel = stop,
                onDrag = { change, amount ->
                    change.consume()
                    drag(amount.y)
                },
            )
        }
    }

    private fun start(key: Any) {
        dragged = key
        offset = 0f
        awaiting = null
        onStart(key)
    }

    private fun end() {
        val key = dragged ?: return
        dragged = null
        offset = 0f
        awaiting = null
        onDrop(key)
    }

    private fun drag(dy: Float) {
        if (dragged == null) return
        offset += dy
        follow()
    }

    // Moves the dragged item over the one its middle is now over, if any, and returns it as
    // laid out; null if it isn't laid out yet.
    private fun follow(): LazyListItemInfo? {
        val key = dragged ?: return null
        val layout = listState.layoutInfo
        val item = layout.visibleItemsInfo.firstOrNull { it.key == key } ?: return null
        if (awaiting != null && item.index != awaiting) return null
        awaiting = null

        val centre = (item.offset + offset + item.size / 2).toInt()
        val move = reorderMove(item, layout.visibleItemsInfo, centre, isMovable) ?: return item
        onMove(key, move.target.key)
        // Its place changes, so it's drawn as far from the new one.
        offset += item.offset - move.offset
        awaiting = move.target.index
        return null
    }

    // Once a frame while [key] is dragged: near an end, scroll by a quarter of how far it's
    // into the edge zone (at most an eighth of the item), then keep it under the finger. Each
    // frame sees the layout of the scroll before, so the item and [offset] agree.
    private suspend fun autoScroll(key: Any) {
        while (dragged == key) {
            withFrameNanos {}
            if (dragged != key) break
            val item = follow() ?: continue
            val layout = listState.layoutInfo
            val top = item.offset + offset
            val edge = item.size / 2f
            val over = when {
                top < layout.viewportStartOffset + edge -> top - (layout.viewportStartOffset + edge)
                top + item.size > layout.viewportEndOffset - edge -> top + item.size - (layout.viewportEndOffset - edge)
                else -> 0f
            }
            if (over != 0f) {
                val max = item.size / 8f
                offset += listState.dispatchRawDelta((over / 4).coerceIn(-max, max))
            }
        }
    }
}
