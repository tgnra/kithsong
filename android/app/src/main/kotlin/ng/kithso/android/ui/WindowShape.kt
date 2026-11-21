package ng.kithso.android.ui

import androidx.compose.material3.adaptive.currentWindowAdaptiveInfo
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.layout.LayoutCoordinates
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.unit.Dp

/**
 * The window's size and its fold, if it has one, for layouts that change with them: the tablet
 * layout, the album grid that keeps its tiles off the fold, and the player in tabletop.
 */
@Immutable
data class WindowShape(
    val width: Dp,
    val height: Dp,
    /** The fold across the window, while it's in it; a foldable that's shut, or a tablet, has none. */
    val fold: Fold?,
) {
    /** Half open with the fold across, the top half standing up: the player's tabletop layout. */
    val tabletop: Boolean get() = fold != null && !fold.vertical && !fold.flat
}

/**
 * A fold, or the hinge between two screens. [bounds] are in the window, in pixels; a fold with
 * no width (a flexible screen) has a line for bounds. [separating] is true when the window is
 * in two halves, half open or across two screens, where nothing should cross it.
 */
@Immutable
data class Fold(
    val bounds: Rect,
    val vertical: Boolean,
    val flat: Boolean,
    val separating: Boolean,
) {
    /** [bounds] in [coordinates]' own space, for placing things around the fold. */
    fun boundsIn(coordinates: LayoutCoordinates): Rect {
        val topLeft = coordinates.windowToLocal(bounds.topLeft)
        return Rect(topLeft, bounds.size)
    }
}

/** This window's [WindowShape], which changes as it's resized, folded or unfolded. */
@Composable
fun rememberWindowShape(): WindowShape {
    val size = LocalWindowInfo.current.containerSize
    val density = LocalDensity.current
    val hinge = currentWindowAdaptiveInfo().windowPosture.hingeList.firstOrNull()
    return remember(size, density, hinge) {
        with(density) {
            WindowShape(
                width = size.width.toDp(),
                height = size.height.toDp(),
                fold = hinge?.let {
                    Fold(it.bounds, vertical = it.isVertical, flat = it.isFlat, separating = it.isSeparating)
                },
            )
        }
    }
}
