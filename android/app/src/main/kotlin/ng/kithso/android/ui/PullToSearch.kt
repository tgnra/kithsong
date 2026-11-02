package ng.kithso.android.ui

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.shadow
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedback
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.pointer.PointerEventPass
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.unit.Velocity
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

// Pulling the main list down from its top opens search. It takes a drag that starts with the
// list already at its top and goes on past a threshold: a search button comes down from the
// top as it's pulled, fills in with a tick once letting go would open search, and springs back
// up either way. Flings that reach the top don't pull, nor does a drag that started further down.

/** How far the finger moves for each pixel the indicator comes down. */
private const val DRAG_RESISTANCE = 0.5f

/** How far the indicator comes down before letting go opens search. */
private val Threshold = 72.dp

/** How far it comes down at most. */
private val MaxPull = 112.dp

private val IndicatorSize = 40.dp

/**
 * A pull down from the top of a list, which calls [onSearch] when let go past [Threshold]: see
 * [pullToSearch] for the list and [PullToSearchIndicator] for what shows it.
 */
@Stable
class PullToSearchState internal constructor(
    private val scope: CoroutineScope,
    private val threshold: Float,
    private val max: Float,
    private val haptics: () -> HapticFeedback,
    private val onSearch: () -> Unit,
) {
    private var pulled by mutableFloatStateOf(0f)
    private var springBack: Job? = null

    /** How far the indicator is down, in pixels. */
    val offset: Float get() = pulled

    /** How far through the pull it is, 1 at the threshold. */
    val progress: Float get() = (pulled / threshold).coerceIn(0f, 1f)

    /** Whether letting go now opens search. */
    val armed: Boolean get() = pulled >= threshold

    // Whether the touch that's down started with the list at its top; only such a drag pulls.
    internal var fromTop = false

    private fun pullTo(value: Float) {
        springBack?.cancel()
        val was = armed
        pulled = value.coerceIn(0f, max)
        if (armed && !was) haptics().performHapticFeedback(HapticFeedbackType.GestureThresholdActivate)
    }

    internal val connection = object : NestedScrollConnection {
        // Pushing back up takes back the pull first, before the list scrolls.
        override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
            if (source != NestedScrollSource.UserInput || available.y >= 0f || pulled <= 0f) return Offset.Zero
            val before = pulled
            pullTo(before + available.y * DRAG_RESISTANCE)
            return Offset(0f, (pulled - before) / DRAG_RESISTANCE)
        }

        // What's left of a drag down once the list is at its top pulls, if it started there.
        // Taking it all also keeps the list's overscroll stretch out of the way.
        override fun onPostScroll(consumed: Offset, available: Offset, source: NestedScrollSource): Offset {
            if (source != NestedScrollSource.UserInput || available.y <= 0f || !fromTop) return Offset.Zero
            pullTo(pulled + available.y * DRAG_RESISTANCE)
            return Offset(0f, available.y)
        }

        // Let go: open search if far enough, and spring back up either way. A release while
        // pulled doesn't fling the list.
        override suspend fun onPreFling(available: Velocity): Velocity {
            if (pulled <= 0f) return Velocity.Zero
            if (armed) onSearch()
            springBack?.cancel()
            springBack = scope.launch {
                animate(pulled, 0f, animationSpec = spring(stiffness = Spring.StiffnessMediumLow)) { value, _ ->
                    pulled = value
                }
            }
            return available
        }
    }
}

@Composable
fun rememberPullToSearch(onSearch: () -> Unit): PullToSearchState {
    val density = LocalDensity.current
    val scope = rememberCoroutineScope()
    val haptics by rememberUpdatedState(LocalHapticFeedback.current)
    val search by rememberUpdatedState(onSearch)
    return remember(density) {
        with(density) {
            PullToSearchState(scope, Threshold.toPx(), MaxPull.toPx(), { haptics }, { search() })
        }
    }
}

/**
 * Lets [state] pull [list], which this modifies: notes whether each touch starts with the list
 * at its top, without taking the touch, and takes the list's drags while pulling.
 */
fun Modifier.pullToSearch(state: PullToSearchState, list: LazyListState): Modifier =
    pointerInput(state, list) {
        awaitEachGesture {
            awaitFirstDown(requireUnconsumed = false, pass = PointerEventPass.Initial)
            state.fromTop = !list.canScrollBackward
        }
    }.nestedScroll(state.connection)

/**
 * The search button that comes down from the top of [modifier]'s area as [state] is pulled,
 * growing and fading in on the way, and fills in with the primary colour once letting go would
 * open search.
 */
@Composable
fun PullToSearchIndicator(state: PullToSearchState, modifier: Modifier = Modifier) {
    val sizePx = with(LocalDensity.current) { IndicatorSize.toPx() }
    // Derived, so the pull itself only redraws, in the layer below.
    val armed by remember(state) { derivedStateOf { state.armed } }
    val container by animateColorAsState(
        if (armed) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.surfaceContainerHigh,
        label = "pull to search",
    )
    val content by animateColorAsState(
        if (armed) MaterialTheme.colorScheme.onPrimary else MaterialTheme.colorScheme.onSurfaceVariant,
        label = "pull to search",
    )
    val scale by animateFloatAsState(if (armed) 1.1f else 1f, label = "pull to search")
    // Boxes rather than a Surface, which would take touches at the top of the list.
    Box(modifier, contentAlignment = Alignment.TopCenter) {
        Box(
            Modifier
                .size(IndicatorSize)
                .graphicsLayer {
                    val progress = state.progress
                    // From just above the area down to the pull: unseen until pulled.
                    translationY = state.offset - sizePx
                    alpha = progress
                    val grow = (0.6f + 0.4f * progress) * scale
                    scaleX = grow
                    scaleY = grow
                }
                .shadow(6.dp, CircleShape)
                .background(container, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Icon(Icons.Search, contentDescription = null, tint = content)
        }
    }
}
