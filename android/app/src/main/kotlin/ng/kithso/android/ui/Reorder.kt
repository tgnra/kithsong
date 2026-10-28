package ng.kithso.android.ui

import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.zIndex

/**
 * Dragging a LazyColumn's items by a handle ([dragHandle]) to reorder them. The list's owner
 * keeps the order: while an item is dragged over another, [onMove] is called with the two
 * items' keys and should move the first to where the second is; [onDrop] is called with the
 * dragged item's key once it's let go. Only items whose key [isMovable] take part, so headers
 * stay put. Dragging near the top or bottom scrolls the list.
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

    /** Makes this the handle of the item with [key]. */
    fun Modifier.dragHandle(key: Any): Modifier = pointerInput(key) {
        detectDragGestures(
            onDragStart = { start(key) },
            onDragEnd = { end() },
            onDragCancel = { end() },
            onDrag = { change, amount ->
                change.consume()
                drag(amount.y)
            },
        )
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
        val key = dragged ?: return
        offset += dy
        val layout = listState.layoutInfo
        val item = layout.visibleItemsInfo.firstOrNull { it.key == key } ?: return
        if (awaiting != null && item.index != awaiting) return
        awaiting = null

        val top = item.offset + offset
        val centre = (top + item.size / 2).toInt()
        val target = layout.visibleItemsInfo.firstOrNull {
            it.key != key && isMovable(it.key) && centre in it.offset until it.offset + it.size
        }
        if (target != null) {
            onMove(key, target.key)
            // Its place is now the target's, so it's drawn as far from that.
            offset += item.offset - target.offset
            awaiting = target.index
        }

        // Near an end, scroll by as much as it's over the edge zone.
        val edge = item.size / 2f
        val scroll = when {
            top < layout.viewportStartOffset + edge -> top - (layout.viewportStartOffset + edge)
            top + item.size > layout.viewportEndOffset - edge -> top + item.size - (layout.viewportEndOffset - edge)
            else -> 0f
        }
        if (scroll != 0f) {
            val scrolled = listState.dispatchRawDelta(scroll / 4)
            offset += scrolled
        }
    }
}
