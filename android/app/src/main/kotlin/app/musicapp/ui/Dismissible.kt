package app.musicapp.ui

import androidx.activity.compose.PredictiveBackHandler
import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.spring
import androidx.compose.animation.core.Spring
import androidx.compose.foundation.gestures.Orientation
import androidx.compose.foundation.gestures.draggable
import androidx.compose.foundation.gestures.rememberDraggableState
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.launch

/** Which way a [DismissibleScreen] slides out. */
enum class DismissDirection { Down, Right }

/**
 * A full-screen layer that slides in from [direction] when first shown, and slides back out
 * to dismiss: dragged that way far or fast enough, or on back. Predictive back moves it part
 * of the way with the gesture, so the screen beneath shows. [onDismissed] is called once it
 * is off screen; remove the layer then. Only the top layer should have [backEnabled].
 * [content] gets a function that slides the layer out, for its own close button.
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
        val density = LocalDensity.current
        val distance = with(density) { if (direction == DismissDirection.Down) maxHeight.toPx() else maxWidth.toPx() }
        val flingVelocity = with(density) { 800.dp.toPx() }
        val offset = remember { Animatable(distance) }
        val scope = rememberCoroutineScope()
        val dismissed by rememberUpdatedState(onDismissed)
        val motion = spring<Float>(stiffness = Spring.StiffnessMediumLow)

        LaunchedEffect(Unit) { offset.animateTo(0f, motion) }

        suspend fun dismiss(velocity: Float = 0f) {
            offset.animateTo(distance, motion, velocity)
            dismissed()
        }

        PredictiveBackHandler(enabled = backEnabled) { progress ->
            try {
                progress.collect { offset.snapTo(distance * 0.2f * it.progress) }
                dismiss()
            } catch (e: CancellationException) {
                scope.launch { offset.animateTo(0f, motion) }
                throw e
            }
        }

        // Dims the screen beneath, less as this one slides away.
        Box(
            Modifier.fillMaxSize().drawBehind {
                drawRect(Color.Black, alpha = 0.32f * (1f - offset.value / distance).coerceIn(0f, 1f))
            },
        )
        Box(
            Modifier
                .fillMaxSize()
                .graphicsLayer {
                    if (direction == DismissDirection.Down) translationY = offset.value
                    else translationX = offset.value
                }
                .draggable(
                    state = rememberDraggableState { delta ->
                        scope.launch { offset.snapTo((offset.value + delta).coerceAtLeast(0f)) }
                    },
                    orientation = if (direction == DismissDirection.Down) Orientation.Vertical else Orientation.Horizontal,
                    onDragStopped = { velocity ->
                        if (offset.value > distance / 3 || velocity > flingVelocity) dismiss(velocity)
                        else offset.animateTo(0f, motion, velocity)
                    },
                ),
        ) {
            content(remember { { scope.launch { dismiss() }; Unit } })
        }
    }
}
