package ng.kithso.android.ui

import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.spring
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitHorizontalTouchSlopOrCancellation
import androidx.compose.foundation.gestures.awaitVerticalTouchSlopOrCancellation
import androidx.compose.foundation.gestures.horizontalDrag
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.systemGestures
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.key
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.pointer.positionChangeIgnoreConsumed
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.input.pointer.util.addPointerInputChange
import androidx.compose.ui.layout.Layout
import androidx.compose.ui.layout.LayoutCoordinates
import androidx.compose.ui.layout.onPlaced
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.unit.LayoutDirection
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import ng.kithso.android.data.Track
import ng.kithso.android.playback.Playback
import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.sign

/** Which way a swipe skips: left is next, right is previous. */
internal enum class Skip { Next, Previous }

/**
 * Where a swipe let go at [offset] (negative is left) moving at [velocity], on a page [page]
 * wide, skips to, if anywhere. It goes by a fling at [flingVelocity] or more, unless that's
 * back the way it came; otherwise by whether it's a third of the way across. There's nothing
 * to skip to where [canNext] or [canPrevious] says so.
 */
internal fun skipOnRelease(
    offset: Float,
    velocity: Float,
    page: Float,
    flingVelocity: Float,
    canNext: Boolean,
    canPrevious: Boolean,
): Skip? {
    val flung = abs(velocity) >= flingVelocity
    if (flung && offset * velocity < 0) return null
    if (!flung && abs(offset) < page * SkipThreshold) return null
    return if ((if (flung) velocity else offset) < 0) Skip.Next.takeIf { canNext } else Skip.Previous.takeIf { canPrevious }
}

/**
 * How far content moves for a drag of [dragged] towards nothing: less and less, up to a limit,
 * as a rubber band pulls back.
 */
internal fun rubberBand(dragged: Float, page: Float): Float {
    val limit = page * RubberBandLimit
    if (limit <= 0f) return 0f
    return dragged.sign * limit * (1f - exp(-abs(dragged) / (limit * 3f)))
}

private const val SkipThreshold = 1f / 3f
private const val RubberBandLimit = 0.12f

/** How long a landed swipe waits for the player to say the track changed. */
private const val TrackChangeTimeoutMs = 2000L

/** How fast a release must be moving to count as a fling, whatever the position. */
private val SkipFlingVelocity = 400.dp

/**
 * A swipe between tracks, for [Modifier.trackSwipe] to drive and [TrackSlides] to show: the
 * content follows the finger, with the next or previous track's coming in from the side, then
 * carries on across or springs back. With nothing to skip to it stretches like a rubber band.
 *
 * The skip goes to the player on release, and the player's answer comes back while the content
 * slides. So the slide holds on to the tracks it started with, and what it lands on stays until
 * the player says the same, or it would flick back to the old track for a moment. The springs
 * run at the system's animator duration scale, as Compose's do: with animations off, a swipe
 * lands at once on release.
 */
@Stable
internal class TrackSwipeState(private val playback: Playback, private val scope: CoroutineScope) {
    /** The width of a page: how far a skip moves the content. */
    var page by mutableFloatStateOf(0f)

    /** How far the content has moved; negative is left, towards the next track. */
    var offset by mutableFloatStateOf(0f)
        private set

    // The finger's drag, which [offset] follows or, towards nothing, stretches.
    private var dragged = 0f

    // While sliding to a skip: the tracks it goes between. The one it goes to may not be known,
    // at the end of a shuffled round, so [sliding] says it's going at all.
    private var sliding by mutableStateOf(false)
    private var slidingFrom by mutableStateOf<Track?>(null)
    private var slidingTo by mutableStateOf<Track?>(null)
    private var landed by mutableStateOf<Track?>(null)
    private var settling: Job? = null

    /** Which side the content has moved to: -1 for left, 1 for right, 0 at rest. */
    val side by derivedStateOf { offset.sign.toInt() }

    /** The track at the centre, moved by [offset]. */
    val current: Track? get() = (if (sliding) slidingFrom else landed) ?: playback.track

    /** Whether a track comes in on the far side of [current]. */
    val neighbourShown: Boolean get() = sliding || (side < 0 && playback.hasNext) || (side > 0 && playback.hasPrevious)

    /** The track coming in; null if it's not known yet. */
    val neighbour: Track?
        get() = when {
            sliding -> slidingTo
            side < 0 -> playback.nextTrack
            else -> playback.previousTrack
        }

    // Set from the window, as where the system's gestures are depends on it.
    internal var inSystemEdge: (Offset) -> Boolean = { false }
    internal var coordinates: LayoutCoordinates? = null
    internal var flingVelocity = 0f

    internal fun startsInSystemEdge(position: Offset): Boolean =
        coordinates?.takeIf { it.isAttached }?.let { inSystemEdge(it.localToWindow(position)) } ?: false

    /** A finger has taken hold: whatever was sliding ends, skipped or not. */
    fun grab() {
        settling?.cancel()
        settling = null
        dragged = offset
    }

    fun dragBy(delta: Float) {
        dragged += delta
        val free = if (dragged < 0) playback.hasNext else playback.hasPrevious
        offset = if (free) dragged else rubberBand(dragged, page)
    }

    fun release(velocity: Float) {
        val skip = skipOnRelease(dragged, velocity, page, flingVelocity, playback.hasNext, playback.hasPrevious)
        if (skip == null) {
            settling = scope.launch { settleAt(0f, velocity) }
            return
        }
        val to = if (skip == Skip.Next) playback.nextTrack else playback.previousTrack
        slidingFrom = current
        slidingTo = to
        sliding = true
        if (skip == Skip.Next) playback.next() else playback.skipToPrevious()
        settling = scope.launch {
            try {
                settleAt(if (skip == Skip.Next) -page else page, velocity)
            } finally {
                land(to)
            }
        }
    }

    private suspend fun settleAt(target: Float, velocity: Float) {
        animate(offset, target, velocity, spring(stiffness = Spring.StiffnessMediumLow)) { value, _ -> offset = value }
    }

    // [to] moves to the centre from where it's drawn, a page away at the end of the slide.
    private fun land(to: Track?) {
        landed = to
        sliding = false
        slidingFrom = null
        slidingTo = null
        offset = 0f
        dragged = 0f
        if (to != null) scope.launch {
            withTimeoutOrNull(TrackChangeTimeoutMs) { snapshotFlow { playback.track?.uid }.first { it == to.uid } }
            if (landed === to) landed = null
        }
    }
}

/** A [TrackSwipeState] for [playback], in this window. */
@Composable
internal fun rememberTrackSwipe(playback: Playback): TrackSwipeState {
    val scope = rememberCoroutineScope()
    val state = remember(playback) { TrackSwipeState(playback, scope) }
    val density = LocalDensity.current
    val window = LocalWindowInfo.current
    val gestures = WindowInsets.systemGestures
    state.flingVelocity = with(density) { SkipFlingVelocity.toPx() }
    state.inSystemEdge = { at ->
        val size = window.containerSize
        at.x < gestures.getLeft(density, LayoutDirection.Ltr) ||
            at.x > size.width - gestures.getRight(density, LayoutDirection.Ltr) ||
            at.y > size.height - gestures.getBottom(density)
    }
    return state
}

/**
 * Swipes on this element skip tracks. It leaves other gestures theirs: a press in a system
 * gesture area is the system's (back from the sides, home along the bottom), a drag that's
 * more up and down than across is for whatever's after one, such as the full player's swipe
 * down, and a child that takes the drag keeps it, as a seekbar does. A tap goes through, as
 * nothing here happens until the finger has moved.
 */
internal fun Modifier.trackSwipe(swipe: TrackSwipeState): Modifier = this
    .onSizeChanged { swipe.page = it.width.toFloat() }
    .onPlaced { swipe.coordinates = it }
    .pointerInput(swipe) {
        awaitEachGesture {
            val down = awaitFirstDown(requireUnconsumed = false)
            if (swipe.startsInSystemEdge(down.position)) return@awaitEachGesture
            val velocity = VelocityTracker()
            velocity.addPointerInputChange(down)
            val start = awaitHorizontalTouchSlopOrCancellation(down.id) { change, overSlop ->
                val moved = change.position - down.position
                if (abs(moved.x) > abs(moved.y)) {
                    change.consume()
                    velocity.addPointerInputChange(change)
                    swipe.grab()
                    swipe.dragBy(overSlop)
                }
            } ?: return@awaitEachGesture
            val lifted = horizontalDrag(start.id) { change ->
                velocity.addPointerInputChange(change)
                swipe.dragBy(change.positionChange().x)
                change.consume()
            }
            // The last step comes with the lift, which the drag above doesn't pass on; in a quick
            // swipe on a busy phone that can be much of the way.
            if (lifted) currentEvent.changes.firstOrNull { it.id == start.id }?.let { up ->
                swipe.dragBy(up.positionChangeIgnoreConsumed().x)
            }
            swipe.release(velocity.calculateVelocity().x)
        }
    }

/**
 * A drag that's more up and down than across ends any press under this element, as it would if
 * something took it. For an element with a [trackSwipe] and nothing after up and down drags;
 * without this a slow one would still be a press, and held long enough a long press. Goes before
 * [trackSwipe] in the chain, so it only sees what the swipe has left.
 */
internal fun Modifier.verticalDragEndsPress(): Modifier = pointerInput(Unit) {
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        awaitVerticalTouchSlopOrCancellation(down.id) { change, _ ->
            val moved = change.position - down.position
            if (abs(moved.y) > abs(moved.x)) change.consume()
        }
    }
}

/**
 * Shows [content] for the current track, moved by [swipe], and the track coming in beside it,
 * a page away. It takes the size of the current track's, so a longer title coming in doesn't
 * move what's beneath before it's arrived. A slide is keyed by its track, so the one that comes
 * in stays as it is when it lands. [content] gets null for a track not known yet.
 */
@Composable
internal fun TrackSlides(swipe: TrackSwipeState, modifier: Modifier = Modifier, content: @Composable (Track?) -> Unit) {
    val current = swipe.current
    val slides = if (swipe.neighbourShown) listOf(current, swipe.neighbour) else listOf(current)
    Layout(
        content = {
            slides.forEachIndexed { i, track ->
                // A queue can hold a track twice in a row, and two slides can't share a key.
                key(if (i == 1 && track?.uid == current?.uid) "again" else track?.uid) {
                    val slide = if (i == 0) Modifier else Modifier.clearAndSetSemantics {}
                    Box(
                        slide.graphicsLayer {
                            translationX = if (i == 0) swipe.offset else swipe.offset - swipe.side * swipe.page
                        },
                    ) { content(track) }
                }
            }
        },
        modifier = modifier,
    ) { measurables, constraints ->
        val placeables = measurables.map { it.measure(constraints) }
        val size = placeables.first()
        layout(size.width, size.height) { placeables.forEach { it.place(0, 0) } }
    }
}
