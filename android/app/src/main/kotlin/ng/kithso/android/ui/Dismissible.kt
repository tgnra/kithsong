package ng.kithso.android.ui

import androidx.activity.compose.PredictiveBackHandler
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.spring
import androidx.compose.foundation.gestures.AnchoredDraggableDefaults
import androidx.compose.foundation.gestures.AnchoredDraggableState
import androidx.compose.foundation.gestures.DraggableAnchors
import androidx.compose.foundation.gestures.FlingBehavior
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.ScrollScope
import androidx.compose.foundation.gestures.anchoredDraggable
import androidx.compose.foundation.gestures.animateTo
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import kotlin.math.abs
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.launch

/** Which way a [DismissibleScreen] slides out. */
enum class DismissDirection { Down, Right, Left }

/** Where a [DismissibleScreen] rests: on screen, or slid out. */
private enum class Layer { Shown, Hidden }

/**
 * A full-screen layer that slides in from [direction] when first shown, and slides back out
 * to dismiss: dragged that way past halfway, or flung that way, or on back. Released anywhere
 * else, or if the system takes the touch, it springs back. Predictive back moves it part of
 * the way with the gesture, so the screen beneath shows. Only the top layer should have
 * [backEnabled].
 *
 * Leaving is one way. Once back or the close button has decided it, nothing brings the layer
 * back: [onLeaving] is called there and then, so that the next press of back, however soon,
 * goes to the layer beneath (see [BackStack]), and the slide out carries on by itself, outside
 * the back handler, which a later press would otherwise cancel. [onDismissed] is called once
 * it is off screen; remove the layer then.
 *
 * [content] gets a function that slides the layer out, for its own close button. A layer that
 * isn't [draggable] only goes on back or that function, where a stray swipe would lose work.
 *
 * The drag, the slide in and out, and predictive back all move one [AnchoredDraggableState]:
 * drag steps apply as they come, under the same lock as the animations, so each takes over
 * from the last cleanly and every drag ends settled at one end or the other.
 *
 * A layer brought back (a rotation, a fold) is simply there, not slid in again. [onShown] is
 * called once it's fully on screen. [onSlide] gets a function saying how far on screen it is, 0 to
 * 1, for what moves along with it, then null when it goes. [scrim] is the dimming of what's beneath.
 */
@Composable
fun DismissibleScreen(
    direction: DismissDirection,
    onDismissed: () -> Unit,
    modifier: Modifier = Modifier,
    onLeaving: () -> Unit = {},
    backEnabled: Boolean = true,
    draggable: Boolean = true,
    scrim: Boolean = true,
    onShown: () -> Unit = {},
    onSlide: ((fraction: (() -> Float)?) -> Unit)? = null,
    content: @Composable (dismiss: () -> Unit) -> Unit,
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val distance = with(LocalDensity.current) {
            if (direction == DismissDirection.Down) maxHeight.toPx() else maxWidth.toPx()
        }
        val anchors = remember(distance) { DraggableAnchors { Layer.Shown at 0f; Layer.Hidden at distance } }
        var seen by rememberSaveable { mutableStateOf(false) }
        val state = remember { AnchoredDraggableState(if (seen) Layer.Shown else Layer.Hidden, anchors) }
        // A new size (rotation, a resized window) moves the ends, keeping the layer at its end.
        SideEffect { if (state.anchors != anchors) state.updateAnchors(anchors, state.targetValue) }
        val scope = rememberCoroutineScope()
        val onLeavingNow by rememberUpdatedState(onLeaving)
        val onDismissedNow by rememberUpdatedState(onDismissed)
        val onShownNow by rememberUpdatedState(onShown)
        val distanceNow by rememberUpdatedState(distance)
        // Each once only: a fling and the close button can both finish out.
        var leaving by remember { mutableStateOf(false) }
        var gone by remember { mutableStateOf(false) }
        fun markLeaving() {
            if (!leaving) { leaving = true; onLeavingNow() }
        }
        fun dismissed() {
            markLeaving()
            if (!gone) { gone = true; onDismissedNow() }
        }
        val motion = spring<Float>(stiffness = Spring.StiffnessMediumLow)

        LaunchedEffect(Unit) {
            state.animateTo(Layer.Shown, motion)
            seen = true
            onShownNow()
        }
        if (onSlide != null) DisposableEffect(Unit) {
            onSlide { (1f - state.requireOffset() / distanceNow).coerceIn(0f, 1f) }
            onDispose { onSlide(null) }
        }

        fun leave() {
            if (leaving) return
            markLeaving()
            scope.launch {
                // A drag let go just now can still take the state over; the slide starts again
                // from where that left it.
                while (true) {
                    try {
                        state.animateTo(Layer.Hidden, motion)
                        break
                    } catch (e: CancellationException) {
                        ensureActive()
                    }
                }
                dismissed()
            }
        }

        PredictiveBackHandler(enabled = backEnabled && !leaving) { progress ->
            try {
                // From where the layer is, as it may still be sliding in.
                val from = state.requireOffset()
                state.anchoredDrag { progress.collect { dragTo(maxOf(from, distance * 0.2f * it.progress)) } }
                leave()
            } catch (e: CancellationException) {
                if (!leaving) scope.launch { state.animateTo(Layer.Shown, motion) }
                throw e
            }
        }

        // On release the layer settles at whichever end is past halfway, or the way it was
        // flung; a cancelled drag settles the same way, with no velocity. Settled out, it's gone.
        // The default fling speed (125dp/s) would count a slow drag let go while still moving
        // as a fling, so slower releases go by position alone.
        val flingVelocity = with(LocalDensity.current) { FlingVelocity.toPx() }
        val settle = AnchoredDraggableDefaults.flingBehavior(state, positionalThreshold = { it / 2 }, animationSpec = motion)
        val flingBehavior = remember(settle, flingVelocity) {
            object : FlingBehavior {
                override suspend fun ScrollScope.performFling(initialVelocity: Float): Float {
                    val velocity = if (abs(initialVelocity) < flingVelocity) 0f else initialVelocity
                    val left = with(settle) { performFling(velocity) }
                    if (state.anchors.closestAnchor(state.requireOffset()) == Layer.Hidden) dismissed()
                    return left
                }
            }
        }

        // Dims the screen beneath, less as this one slides away.
        Box(
            Modifier.fillMaxSize().drawBehind {
                if (scrim) drawRect(Color.Black, alpha = 0.32f * (1f - state.requireOffset() / distance).coerceIn(0f, 1f))
            },
        )
        Box(
            Modifier
                .fillMaxSize()
                .graphicsLayer {
                    when (direction) {
                        DismissDirection.Down -> translationY = state.requireOffset()
                        DismissDirection.Right -> translationX = state.requireOffset()
                        DismissDirection.Left -> translationX = -state.requireOffset()
                    }
                }
                .anchoredDraggable(
                    state,
                    // Said outright, as the default turns a drag round in right-to-left, and the layer doesn't.
                    reverseDirection = direction == DismissDirection.Left,
                    orientation = if (direction == DismissDirection.Down) Orientation.Vertical else Orientation.Horizontal,
                    enabled = draggable && !leaving,
                    flingBehavior = flingBehavior,
                ),
        ) {
            content(remember { { leave() } })
        }
    }
}

/** How fast a release must be moving to count as a fling, whatever the position. */
private val FlingVelocity = 400.dp
