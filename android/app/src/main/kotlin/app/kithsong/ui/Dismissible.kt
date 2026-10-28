package app.kithsong.ui

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
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.SideEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import kotlin.math.abs
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/** Which way a [DismissibleScreen] slides out. */
enum class DismissDirection { Down, Right }

/** Where a [DismissibleScreen] rests: on screen, or slid out. */
private enum class Layer { Shown, Hidden }

/**
 * A full-screen layer that slides in from [direction] when first shown, and slides back out
 * to dismiss: dragged that way past halfway, or flung that way, or on back. Released anywhere
 * else, or if the system takes the touch, it springs back. Predictive back moves it part of
 * the way with the gesture, so the screen beneath shows. [onDismissed] is called once it is
 * off screen; remove the layer then. Only the top layer should have [backEnabled].
 * [content] gets a function that slides the layer out, for its own close button.
 *
 * The drag, the slide in and out, and predictive back all move one [AnchoredDraggableState]:
 * drag steps apply as they come, under the same lock as the animations, so each takes over
 * from the last cleanly and every drag ends settled at one end or the other.
 */
@Composable
fun DismissibleScreen(
    direction: DismissDirection,
    onDismissed: () -> Unit,
    modifier: Modifier = Modifier,
    backEnabled: Boolean = true,
    content: @Composable (dismiss: () -> Unit) -> Unit,
) {
    BoxWithConstraints(modifier.fillMaxSize()) {
        val distance = with(LocalDensity.current) {
            if (direction == DismissDirection.Down) maxHeight.toPx() else maxWidth.toPx()
        }
        val anchors = remember(distance) { DraggableAnchors { Layer.Shown at 0f; Layer.Hidden at distance } }
        val state = remember { AnchoredDraggableState(Layer.Hidden, anchors) }
        // A new size (rotation, a resized window) moves the ends, keeping the layer at its end.
        SideEffect { if (state.anchors != anchors) state.updateAnchors(anchors, state.targetValue) }
        val scope = rememberCoroutineScope()
        val onDismissedNow by rememberUpdatedState(onDismissed)
        // Once only: a fling and the close button can both finish out, and a second call
        // would remove another layer (pop takes the top of the stack).
        var gone by remember { mutableStateOf(false) }
        fun dismissed() {
            if (!gone) { gone = true; onDismissedNow() }
        }
        val motion = spring<Float>(stiffness = Spring.StiffnessMediumLow)

        LaunchedEffect(Unit) { state.animateTo(Layer.Shown, motion) }

        suspend fun dismiss() {
            state.animateTo(Layer.Hidden, motion)
            dismissed()
        }

        PredictiveBackHandler(enabled = backEnabled) { progress ->
            try {
                state.anchoredDrag { progress.collect { dragTo(distance * 0.2f * it.progress) } }
                dismiss()
            } catch (e: CancellationException) {
                scope.launch { state.animateTo(Layer.Shown, motion) }
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
                drawRect(Color.Black, alpha = 0.32f * (1f - state.requireOffset() / distance).coerceIn(0f, 1f))
            },
        )
        Box(
            Modifier
                .fillMaxSize()
                .graphicsLayer {
                    if (direction == DismissDirection.Down) translationY = state.requireOffset()
                    else translationX = state.requireOffset()
                }
                .anchoredDraggable(
                    state,
                    orientation = if (direction == DismissDirection.Down) Orientation.Vertical else Orientation.Horizontal,
                    flingBehavior = flingBehavior,
                ),
        ) {
            content(remember { { scope.launch { dismiss() }; Unit } })
        }
    }
}

/** How fast a release must be moving to count as a fling, whatever the position. */
private val FlingVelocity = 400.dp
