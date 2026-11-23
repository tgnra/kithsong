package ng.kithso.android.ui

import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.unit.Dp
import kotlin.math.roundToInt

// Pinching the phone's album grid: out for larger tiles, in for smaller ones, which fit a
// column more across ([AppSettings.smallAlbumTiles]).

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
