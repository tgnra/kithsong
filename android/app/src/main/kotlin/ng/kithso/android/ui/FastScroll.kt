package ng.kithso.android.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalHapticFeedback
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.ProgressBarRangeInfo
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.progressBarRangeInfo
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.setProgress
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import kotlinx.coroutines.launch
import java.text.Normalizer
import kotlin.math.floor
import kotlin.math.min
import kotlin.math.roundToInt

// A fast scroll strip down the right edge of a long list: letters, and markers for the
// list's section headings, to tap or drag along to jump there, with a bubble beside the
// finger saying where it is.

/**
 * A place in the list the strip can jump to: list item [index], shown as [label] (a letter,
 * or a heading's title). [section] is the heading a letter is under, if the list has them,
 * for the bubble; a [heading] is drawn as a mark in the strip, with its title in the bubble.
 */
@Immutable
data class FastScrollEntry(
    val label: String,
    val index: Int,
    val section: String? = null,
    val heading: Boolean = false,
)

/**
 * The letter a name filed under [sortKey] goes under: its first letter, in capitals and
 * without accents ("é" is E), or [OTHER_LETTER] for a digit, a symbol, another script or
 * nothing at all. The core orders names by their sort keys a byte at a time ignoring the case
 * of A to Z (SQLite's NOCASE), so a name starting with a digit or a symbol comes before A,
 * and one starting with an accent or another script after Z: see [letterEntries].
 */
internal fun indexLetter(sortKey: String): Char {
    val first = sortKey.firstOrNull() ?: return OTHER_LETTER
    val base = if (first.code < 0x80) first else Normalizer.normalize(first.toString(), Normalizer.Form.NFD).first()
    val upper = base.uppercaseChar()
    return if (upper in 'A'..'Z') upper else OTHER_LETTER
}

/** See [indexLetter]; shown as R.string.fast_scroll_other. */
internal const val OTHER_LETTER = '#'

/**
 * Entries for the letters of a run of names in their list's order, adding to [into]: [keys]
 * are their sort keys (see [indexLetter]), and the name at i is list item [at] (i). Each
 * letter is where its first name is, so the entries follow the list down. One that comes
 * round again later (an accented É after Z, when the E's were before) doesn't move it: those
 * names are found by scrolling on from the letter before. [other] is the label for
 * [OTHER_LETTER].
 */
internal fun letterEntries(
    into: MutableList<FastScrollEntry>,
    keys: List<String>,
    other: String,
    section: String? = null,
    at: (Int) -> Int,
) {
    val seen = HashSet<Char>()
    keys.forEachIndexed { i, key ->
        val letter = indexLetter(key)
        if (seen.add(letter)) into += FastScrollEntry(if (letter == OTHER_LETTER) other else letter.toString(), at(i), section)
    }
}

/** How many screens long a list is before it has a strip. */
private const val MIN_SCREENS = 3

private val StripWidth = 28.dp
private val SlotHeight = 20.dp
private val MinSlotHeight = 14.dp
private val BubbleHeight = 64.dp

/**
 * The strip for [state]'s list, over the list's right edge: [modifier] places it (the parent
 * box's size, inside the bars and mini player). Jumping puts the entry's item at the top,
 * [scrollOffset] pixels in (negative to land below a status bar the list scrolls under).
 * There's none while the list is under [MIN_SCREENS] screens long, or has fewer than two
 * [entries].
 */
@Composable
fun FastScroller(
    state: LazyListState,
    entries: List<FastScrollEntry>,
    modifier: Modifier = Modifier,
    scrollOffset: Int = 0,
) {
    val long by remember(state) {
        derivedStateOf {
            val info = state.layoutInfo
            val visible = info.visibleItemsInfo.size
            visible > 0 && info.totalItemsCount > visible * MIN_SCREENS
        }
    }
    if (!long || entries.size < 2) return
    val scope = rememberCoroutineScope()
    FastScroller(
        entries,
        current = { state.firstVisibleItemIndex },
        onJump = { index -> scope.launch { state.scrollToItem(index, scrollOffset) } },
        modifier = modifier,
    )
}

/**
 * The strip for any list: [entries] in list order, [current] the first item on screen, and
 * [onJump] scrolls to an item. The strip is centred down the right edge of [modifier]'s
 * area, as tall as the entries want or as fits; when they don't all fit, every other slot
 * is a dot, and dragging still goes through every entry. Touching it jumps to the entry
 * under the finger and follows it, with a haptic tick at each new one. For accessibility it's
 * an adjustable control, an entry a step.
 */
@Composable
fun FastScroller(
    entries: List<FastScrollEntry>,
    current: () -> Int,
    onJump: (Int) -> Unit,
    modifier: Modifier = Modifier,
) {
    val haptics = LocalHapticFeedback.current
    val density = LocalDensity.current
    val currentEntries by rememberUpdatedState(entries)
    val jump by rememberUpdatedState(onJump)
    // The entry under the finger while dragging, else -1, and where the finger is in the strip.
    var selected by remember { mutableIntStateOf(-1) }
    var fingerY by remember { mutableFloatStateOf(0f) }
    // The entry the list is at, for accessibility: the last at or above the first item on screen.
    val at by remember(entries) {
        derivedStateOf {
            val first = current()
            entries.indexOfLast { it.index <= first }.coerceAtLeast(0)
        }
    }

    val description = stringResource(R.string.fast_scroll)
    // Read here rather than in the semantics block, so a new entry updates it.
    val atIndex = at
    val state = entries.getOrNull(atIndex)?.let { entryText(it) }.orEmpty()

    BoxWithConstraints(modifier) {
        val count = entries.size
        val stripHeight = min(maxHeight.value, (SlotHeight * count).value).dp
        val slots = min(count, floor(stripHeight / MinSlotHeight).toInt()).coerceAtLeast(1)
        val stripTop = (maxHeight - stripHeight) / 2

        Column(
            Modifier
                .align(Alignment.CenterEnd)
                .width(StripWidth)
                .height(stripHeight)
                .background(
                    if (selected >= 0) MaterialTheme.colorScheme.surfaceContainerHighest.copy(alpha = 0.85f) else Color.Transparent,
                    RoundedCornerShape(StripWidth / 2),
                )
                .semantics {
                    contentDescription = description
                    stateDescription = state
                    progressBarRangeInfo = ProgressBarRangeInfo(atIndex.toFloat(), 0f..(count - 1).toFloat(), steps = (count - 2).coerceAtLeast(0))
                    setProgress { value ->
                        val entry = currentEntries.getOrNull(value.roundToInt().coerceIn(0, currentEntries.lastIndex))
                        entry?.let { jump(it.index) }
                        entry != null
                    }
                }
                .pointerInput(Unit) {
                    fun select(y: Float) {
                        val list = currentEntries
                        if (list.isEmpty()) return
                        fingerY = y.coerceIn(0f, size.height.toFloat())
                        val i = floor(fingerY / size.height * list.size).toInt().coerceIn(0, list.lastIndex)
                        if (i != selected) {
                            selected = i
                            haptics.performHapticFeedback(HapticFeedbackType.SegmentTick)
                            jump(list[i].index)
                        }
                    }
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        down.consume()
                        try {
                            select(down.position.y)
                            while (true) {
                                val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: break
                                if (!change.pressed) {
                                    change.consume()
                                    break
                                }
                                change.consume()
                                select(change.position.y)
                            }
                        } finally {
                            selected = -1
                        }
                    }
                },
        ) {
            for (slot in 0 until slots) {
                // The entry at the slot's middle, as a touch there picks.
                val i = ((slot + 0.5f) * count / slots).toInt().coerceIn(0, count - 1)
                val entry = entries[i]
                Box(Modifier.fillMaxWidth().weight(1f), contentAlignment = Alignment.Center) {
                    val here = i == selected
                    val color = if (here) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant
                    when {
                        slots < count && slot % 2 == 1 -> Box(Modifier.size(3.dp).background(color, CircleShape))
                        entry.heading -> Box(Modifier.width(10.dp).height(3.dp).background(color, RoundedCornerShape(2.dp)))
                        else -> Text(
                            entry.label,
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = if (here) FontWeight.Bold else FontWeight.Medium,
                            color = color,
                            maxLines = 1,
                        )
                    }
                }
            }
        }

        val shown = entries.getOrNull(selected)
        if (shown != null) {
            val half = with(density) { (BubbleHeight / 2).toPx() }
            val top = with(density) { stripTop.toPx() }
            Bubble(
                shown,
                Modifier
                    .align(Alignment.TopEnd)
                    .padding(end = StripWidth + 12.dp)
                    .offset { IntOffset(0, (top + fingerY - half).roundToInt().coerceAtLeast(0)) },
            )
        }
    }
}

/** What the bubble says, and the strip's state for accessibility: "K, Artists", or a heading. */
@Composable
private fun entryText(entry: FastScrollEntry): String =
    if (entry.heading || entry.section == null) entry.label
    else stringResource(R.string.fast_scroll_letter_in, entry.label, entry.section)

/** Beside the finger: the letter it's on, with its section under it, or a heading's title. */
@Composable
private fun Bubble(entry: FastScrollEntry, modifier: Modifier) {
    Surface(
        shape = RoundedCornerShape(BubbleHeight / 2),
        color = MaterialTheme.colorScheme.primaryContainer,
        contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
        shadowElevation = 6.dp,
        modifier = modifier.widthIn(min = BubbleHeight, max = 240.dp),
    ) {
        Column(
            Modifier.heightIn(min = BubbleHeight).padding(horizontal = 16.dp, vertical = 6.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            if (entry.heading) {
                Text(entry.label, style = MaterialTheme.typography.titleMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            } else {
                Text(entry.label, style = MaterialTheme.typography.headlineMedium, maxLines = 1)
                entry.section?.let {
                    Text(it, style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}
