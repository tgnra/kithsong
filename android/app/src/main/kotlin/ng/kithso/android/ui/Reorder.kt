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

/**
 * Dragging a LazyColumn's items by a handle ([dragHandle]) to reorder them. The list's owner
 * keeps the order: while an item is dragged over another, [onMove] is called with the two
 * items' keys and should move the first to where the second is; [onDrop] is called with the
 * dragged item's key once it's let go. Only items whose key [isMovable] take part, so headers
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
        val target = layout.visibleItemsInfo.firstOrNull {
            it.key != key && isMovable(it.key) && centre in it.offset until it.offset + it.size
        }
        if (target == null) return item
        onMove(key, target.key)
        // Its place is now the target's, so it's drawn as far from that.
        offset += item.offset - target.offset
        awaiting = target.index
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
