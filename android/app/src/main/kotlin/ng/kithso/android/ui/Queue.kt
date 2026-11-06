package ng.kithso.android.ui

import androidx.activity.compose.PredictiveBackHandler
import androidx.compose.animation.core.Spring
import androidx.compose.animation.core.animate
import androidx.compose.animation.core.spring
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.awaitVerticalTouchSlopOrCancellation
import androidx.compose.foundation.gestures.verticalDrag
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.nestedscroll.NestedScrollConnection
import androidx.compose.ui.input.nestedscroll.NestedScrollSource
import androidx.compose.ui.input.nestedscroll.nestedScroll
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.input.pointer.positionChangeIgnoreConsumed
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.input.pointer.util.addPointerInputChange
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.pluralStringResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.paneTitle
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Velocity
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import ng.kithso.android.R
import ng.kithso.android.playback.Playback
import ng.kithso.core.UpNextEntry
import kotlin.math.abs

/**
 * Whether the queue ends up open after a drag let go [progress] of the way there (0 the player,
 * 1 the queue) with the finger moving at [velocity] (negative is up): by a fling at
 * [flingVelocity] or more, else by whether it's past halfway.
 */
internal fun queueOpensOnRelease(progress: Float, velocity: Float, flingVelocity: Float): Boolean =
    if (abs(velocity) >= flingVelocity) velocity < 0 else progress >= 0.5f

/** [this] with the item at [from] moved to [to]. */
internal fun <T> List<T>.moved(from: Int, to: Int): List<T> = toMutableList().apply { add(to, removeAt(from)) }

/** How fast a release must be moving to count as a fling, whatever the position. */
private val QueueFlingVelocity = 400.dp

/**
 * The full player's view sliding up to show the queue beneath it: by its chevrons, by a drag, and
 * back on system back. It's part of the player rather than a page of the [BackStack], as the two
 * are one view that moves.
 */
@Stable
internal class QueueSlide(private val scope: CoroutineScope, open: Boolean) {
    /** How far the view moves: the player's height. */
    var page by mutableFloatStateOf(0f)

    /** How far it has moved: 0 is the player, 1 the queue. */
    var progress by mutableFloatStateOf(if (open) 1f else 0f)
        private set

    /** Where it's going or has come to rest. */
    var open by mutableStateOf(open)
        private set

    /** Whether any of the queue is on screen, or about to be. */
    val shown by derivedStateOf { this.open || progress > 0f }

    internal var flingVelocity = 0f
    private var settling: Job? = null

    fun show() = settle(true, 0f)
    fun hide() = settle(false, 0f)

    /** A finger has taken hold: whatever was sliding stays where it is. */
    fun grab() {
        settling?.cancel()
        settling = null
    }

    /** The finger moved [dy], down being positive. */
    fun dragBy(dy: Float) {
        if (page > 0f) pull(progress - dy / page)
    }

    /** Puts it at [to], as predictive back does. */
    fun pull(to: Float) {
        progress = to.coerceIn(0f, 1f)
    }

    fun release(velocity: Float) = settle(queueOpensOnRelease(progress, velocity, flingVelocity), velocity)

    private fun settle(to: Boolean, velocity: Float) {
        open = to
        settling?.cancel()
        settling = scope.launch {
            val speed = if (page > 0f) -velocity / page else 0f
            animate(progress, if (to) 1f else 0f, speed, spring(stiffness = Spring.StiffnessMediumLow)) { value, _ -> pull(value) }
        }
    }
}

/** A [QueueSlide] that stays open through a rotation. */
@Composable
internal fun rememberQueueSlide(): QueueSlide {
    val scope = rememberCoroutineScope()
    var saved by rememberSaveable { mutableStateOf(false) }
    val slide = remember { QueueSlide(scope, saved) }
    slide.flingVelocity = with(LocalDensity.current) { QueueFlingVelocity.toPx() }
    LaunchedEffect(slide.open) { saved = slide.open }
    return slide
}

/** System back from the queue: back to the player, part of the way with the gesture. */
@Composable
internal fun QueueBackHandler(slide: QueueSlide, enabled: Boolean) {
    PredictiveBackHandler(enabled = enabled && slide.open) { progress ->
        try {
            slide.grab()
            val from = slide.progress
            progress.collect { slide.pull(from - 0.2f * it.progress) }
            slide.hide()
        } catch (e: CancellationException) {
            slide.show()
            throw e
        }
    }
}

/**
 * Drags on this element slide the view towards what isn't showing: up from the player, down from
 * the queue. It leaves other gestures theirs, as [trackSwipe] does: a drag the other way is for
 * whatever's after one, such as the full player's swipe down, and one that's more across than up
 * and down, or that a child takes, isn't this. With [swipe], a press in a system gesture area
 * is the system's.
 */
internal fun Modifier.queueDrag(slide: QueueSlide, swipe: TrackSwipeState? = null): Modifier = pointerInput(slide) {
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        if (swipe?.startsInSystemEdge(down.position) == true) return@awaitEachGesture
        val velocity = VelocityTracker()
        velocity.addPointerInputChange(down)
        val start = awaitVerticalTouchSlopOrCancellation(down.id) { change, overSlop ->
            val moved = change.position - down.position
            if ((if (slide.open) overSlop > 0 else overSlop < 0) && abs(moved.y) > abs(moved.x)) {
                change.consume()
                velocity.addPointerInputChange(change)
                slide.grab()
                slide.dragBy(overSlop)
            }
        } ?: return@awaitEachGesture
        val lifted = verticalDrag(start.id) { change ->
            velocity.addPointerInputChange(change)
            slide.dragBy(change.positionChange().y)
            change.consume()
        }
        // The last step comes with the lift, as in [trackSwipe].
        if (lifted) currentEvent.changes.firstOrNull { it.id == start.id }?.let { up ->
            slide.dragBy(up.positionChangeIgnoreConsumed().y)
        }
        slide.release(velocity.calculateVelocity().y)
    }
}

/**
 * A drag down the list past its top moves the view back towards the player, and on the way back
 * up moves the view before the list scrolls.
 */
private class QueueListScroll(private val slide: QueueSlide) : NestedScrollConnection {
    override fun onPreScroll(available: Offset, source: NestedScrollSource): Offset {
        if (source != NestedScrollSource.UserInput || available.y >= 0f || slide.progress >= 1f) return Offset.Zero
        slide.grab()
        slide.dragBy(available.y)
        return available
    }

    override fun onPostScroll(consumed: Offset, available: Offset, source: NestedScrollSource): Offset {
        if (source != NestedScrollSource.UserInput || available.y <= 0f) return Offset.Zero
        slide.grab()
        slide.dragBy(available.y)
        return available
    }

    override suspend fun onPreFling(available: Velocity): Velocity {
        if (slide.progress >= 1f) return Velocity.Zero
        slide.release(available.y)
        return available
    }
}

/**
 * The queue, under the full player: as Windows' popup has it, the heading and Clear, then a row
 * for each entry of up next, in the order it plays, with its title, its artist or how many tracks
 * are left of it, how long it is, a handle to drag it by and an x to take it out. Accessibility
 * services get moving and removing as actions on the row, which is what Windows' keys are there.
 */
@Composable
internal fun QueuePage(playback: Playback, slide: QueueSlide, modifier: Modifier = Modifier) {
    val entries = playback.upNext
    val listState = rememberLazyListState()
    val reorder = remember(listState) { ReorderState(listState) }
    // The order while a row is dragged, which only goes to the player when it's dropped.
    var dragging by remember { mutableStateOf<List<UpNextEntry>?>(null) }
    val rows = dragging ?: entries
    reorder.onMove = { from, to ->
        val i = rows.indexOfFirst { it.id == from }
        val j = rows.indexOfFirst { it.id == to }
        if (i >= 0 && j >= 0) dragging = rows.moved(i, j)
    }
    reorder.onDrop = { key ->
        val to = rows.indexOfFirst { it.id == key }
        if (to >= 0 && to != entries.indexOfFirst { it.id == key }) playback.moveUpNext(key as Int, to)
        dragging = null
    }
    val title = stringResource(R.string.queue)
    val scroll = remember(slide) { QueueListScroll(slide) }
    Column(
        modifier.semantics { paneTitle = title },
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Row(
            Modifier.widthIn(max = MaxQueueWidth).fillMaxWidth().padding(top = 4.dp, start = 12.dp, end = 24.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            IconButton(onClick = slide::hide) {
                Icon(Icons.ExpandLess, contentDescription = stringResource(R.string.close_queue))
            }
            Text(
                title,
                style = MaterialTheme.typography.titleLarge,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                modifier = Modifier.weight(1f).padding(start = 4.dp).semantics { heading() },
            )
            OutlinedButton(onClick = playback::clearUpNext, enabled = rows.isNotEmpty()) {
                Text(stringResource(R.string.queue_clear))
            }
        }
        if (rows.isEmpty()) {
            Box(Modifier.weight(1f).fillMaxWidth(), contentAlignment = Alignment.Center) {
                Text(
                    stringResource(R.string.queue_empty),
                    style = MaterialTheme.typography.bodyLarge,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        } else {
            LazyColumn(
                Modifier.weight(1f).widthIn(max = MaxQueueWidth).fillMaxWidth().nestedScroll(scroll),
                state = listState,
            ) {
                itemsIndexed(rows, key = { _, entry -> entry.id }) { i, entry ->
                    val moveTo = { to: Int -> playback.moveUpNext(entry.id, to) }
                    QueueRow(
                        entry,
                        onMoveUp = if (i > 0) ({ moveTo(i - 1) }) else null,
                        onMoveDown = if (i < rows.lastIndex) ({ moveTo(i + 1) }) else null,
                        onRemove = { playback.removeUpNext(entry.id) },
                        handle = with(reorder) { Modifier.dragHandle(entry.id) },
                        lifted = entry.id == reorder.dragged,
                        modifier = if (entry.id == reorder.dragged) with(reorder) { Modifier.dragged(entry.id) } else Modifier.animateItem(),
                    )
                }
            }
        }
    }
}

@Composable
private fun QueueRow(
    entry: UpNextEntry,
    onMoveUp: (() -> Unit)?,
    onMoveDown: (() -> Unit)?,
    onRemove: () -> Unit,
    handle: Modifier,
    lifted: Boolean,
    modifier: Modifier,
) {
    val moveUp = stringResource(R.string.move_up)
    val moveDown = stringResource(R.string.move_down)
    val remove = stringResource(R.string.queue_remove)
    val byline = entry.artist ?: if (entry.block) pluralStringResource(R.plurals.tracks, entry.tracks, entry.tracks) else null
    val secondary = MaterialTheme.colorScheme.onSurfaceVariant
    Row(
        modifier
            .fillMaxWidth()
            .heightIn(min = 64.dp)
            .padding(horizontal = 8.dp)
            .clip(RoundedCornerShape(12.dp))
            .background(if (lifted) MaterialTheme.colorScheme.surfaceContainerHigh else Color.Transparent)
            .semantics(mergeDescendants = true) {
                customActions = listOfNotNull(
                    onMoveUp?.let { move -> CustomAccessibilityAction(moveUp) { move(); true } },
                    onMoveDown?.let { move -> CustomAccessibilityAction(moveDown) { move(); true } },
                    CustomAccessibilityAction(remove) { onRemove(); true },
                )
            },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        // The row says what can be done with it, so the handle needn't be a stop of its own.
        Box(handle.size(48.dp), contentAlignment = Alignment.Center) {
            Icon(Icons.DragHandle, contentDescription = null, tint = secondary)
        }
        Column(Modifier.weight(1f).padding(vertical = 8.dp), verticalArrangement = Arrangement.Center) {
            Text(entry.title, style = MaterialTheme.typography.bodyLarge, maxLines = 1, overflow = TextOverflow.Ellipsis)
            if (byline != null) {
                Text(byline, style = MaterialTheme.typography.bodyMedium, color = secondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        if (entry.durationMs > 0) {
            // To the nearest second, as Windows' rows have it.
            Text(
                formatTime(entry.durationMs + 500),
                style = MaterialTheme.typography.bodyMedium,
                color = secondary,
                modifier = Modifier.padding(start = 8.dp),
            )
        }
        IconButton(onClick = onRemove) {
            Icon(Icons.Clear, contentDescription = remove, tint = secondary)
        }
    }
}

/** As wide as the mini player, on a tablet or in landscape. */
private val MaxQueueWidth = 640.dp
