package ng.kithso.android.ui

import androidx.compose.animation.core.animateFloatAsState
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
import androidx.compose.foundation.layout.wrapContentHeight
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.derivedStateOf
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.hapticfeedback.HapticFeedbackType
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalConfiguration
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
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.text.Normalizer
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

// A fast scroll strip down the right edge of a long list: letters, and markers for the
// list's section headings, to tap or drag along to jump there, with a bubble beside the
// finger saying where it is. It shows while the list scrolls or it's touched.

/**
 * A place in the list the strip can jump to: list item [index], shown as [label] (a letter,
 * or a heading's title). [section] is the heading a letter is under, if the list has them,
 * for the bubble; a [heading] is drawn as a mark in the strip, with its title in the bubble.
 * [through] is the last letter of a run the strip groups into this one place, and a [tick] is
 * a place [groupEntries] added a screen further on, still under the same letter or heading.
 */
@Immutable
data class FastScrollEntry(
    val label: String,
    val index: Int,
    val section: String? = null,
    val heading: Boolean = false,
    val through: String? = null,
    val tick: Boolean = false,
)

/**
 * Where each of [count] items starts down the list, in pixels, from their [heights], and after
 * them where the list ends.
 */
internal fun itemOffsets(count: Int, heights: (Int) -> Int): IntArray {
    val offsets = IntArray(count + 1)
    for (i in 0 until count) offsets[i + 1] = offsets[i] + heights(i)
    return offsets
}

/**
 * [entries] made into places in the strip about [step] pixels of the list apart, by the items'
 * [offsets] (see [itemOffsets]). A run of letters within two thirds of that is one place, at
 * the first, through the last, so that letters a little under a step apart don't leave gaps of
 * nearly two. Where the next entry is further away, as under a letter with a lot of albums or a
 * heading with no letters, a [FastScrollEntry.tick] goes in every [step] on until it's less
 * than a third of a step away. A heading always starts a place; a letter in another section
 * never joins one before it.
 */
internal fun groupEntries(entries: List<FastScrollEntry>, offsets: IntArray, step: Int): List<FastScrollEntry> {
    val groups = ArrayList<FastScrollEntry>()
    var start: FastScrollEntry? = null
    var last: FastScrollEntry? = null
    fun top(entry: FastScrollEntry) = offsets[entry.index.coerceIn(0, offsets.lastIndex)]
    // Ends the place being made, and adds the ticks after it, up to [until] down the list.
    fun close(until: Int) {
        val first = start ?: return
        val end = last!!
        groups += if (!first.heading && end !== first && end.label != first.label) first.copy(through = end.label) else first
        var index = end.index
        var at = top(first) + step
        while (true) {
            // The item [at] is in, which is where a jump can land, so that tall rows don't
            // push the places further than a step apart; else the next one.
            val found = offsets.binarySearch(at, fromIndex = index + 1)
            index = if (found >= 0) found else (-found - 2).coerceAtLeast(index + 1)
            if (index >= offsets.lastIndex || until - offsets[index] < step / 3) break
            groups += end.copy(index = index, through = null, tick = true)
            at = offsets[index] + step
        }
    }
    for (entry in entries) {
        val first = start
        if (first == null || entry.heading || entry.section != first.section || top(entry) - top(first) >= step * 2 / 3) {
            close(top(entry))
            start = entry
        }
        last = entry
    }
    // A jump to the last screen's worth of the list lands in the same place: the end.
    close(offsets.last() - step / 2)
    return groups
}

/**
 * How much of the strip's height each of [places] takes, as fractions adding up to 1: the same
 * for each, except that a section [minShares] names (by its title) with more than its heading
 * takes at least that share of the strip, whatever its size, the other places sharing what's
 * left as before. So on the main screen Albums always takes more of the strip than Artists,
 * however many artists there are. The shares should add up to well under 1.
 */
internal fun placeShares(places: List<FastScrollEntry>, minShares: Map<String, Float>): FloatArray {
    val n = places.size
    if (n == 0) return FloatArray(0)
    // A heading is in the section it heads, as the letters under it are.
    val sectionOf = places.map { if (it.heading) it.label else it.section }
    val counts = sectionOf.groupingBy { it }.eachCount()
    // A section that's just its heading keeps its place's height; the rest is shared out.
    val alone = counts.count { it.value == 1 }
    // The sections held to their minimum: those under it while the places of the others share
    // what's left equally, until no more are.
    val held = HashMap<String, Float>()
    fun restCount() = n - alone - held.keys.sumOf { counts.getValue(it) }
    fun restShare() = 1f - alone.toFloat() / n - held.values.sum()
    while (true) {
        val restCount = restCount()
        if (restCount <= 0) break
        val restShare = restShare()
        val under = minShares.filter { (section, min) ->
            section !in held && (counts[section] ?: 0) > 1 && counts.getValue(section).toFloat() / restCount * restShare < min
        }
        if (under.isEmpty()) break
        held.putAll(under)
    }
    val restCount = restCount()
    val restShare = restShare().coerceAtLeast(MIN_REST_SHARE)
    val shares = FloatArray(n) { i ->
        val section = sectionOf[i]
        val min = section?.let { held[it] }
        when {
            min != null -> min / counts.getValue(section)
            counts.getValue(section) == 1 -> 1f / n
            else -> restShare / restCount
        }
    }
    val sum = shares.sum()
    for (i in 0 until n) shares[i] /= sum
    return shares
}

/** What's left to the places of sections without a minimum, at least ([placeShares]). */
private const val MIN_REST_SHARE = 0.05f

/** What the strip draws for an entry: its label (or a heading's mark), a dot, or nothing. */
internal enum class Mark { Label, Dot, None }

/**
 * What the strip draws for each entry, given where it [starts] down the strip and the share
 * of it it [spans], as fractions of its height; [minSlot] is [MinSlotHeight] as one and
 * [minTick] [MinTickHeight]. A tick is a dot. So is a letter less than [minSlot] below the last
 * letter, though a heading keeps its mark. Nothing is drawn less than [minTick] below the last
 * mark.
 */
internal fun stripMarks(
    entries: List<FastScrollEntry>,
    starts: FloatArray,
    spans: FloatArray,
    minSlot: Float,
    minTick: Float,
): Array<Mark> {
    var last = Float.NEGATIVE_INFINITY
    var lastLabel = Float.NEGATIVE_INFINITY
    return Array(entries.size) { i ->
        val entry = entries[i]
        val centre = starts[i] + spans[i] / 2
        if (centre - last < minTick) return@Array Mark.None
        last = centre
        if (entry.tick || !entry.heading && centre - lastLabel < minSlot) return@Array Mark.Dot
        lastLabel = centre
        Mark.Label
    }
}

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
private val MinSlotHeight = 14.dp

/** How close together the strip's places can be: fewer than a screen each beyond this. */
private val MinTickHeight = 8.dp
private val BubbleHeight = 64.dp

/** What the strip's parents leave between it and the screen's edge. */
private val StripMargin = 2.dp

/** How far a button at the end of a row stays from the edge, to be clear of the strip. */
internal val FastScrollInset = StripWidth + StripMargin + 4.dp

/** A list row's height until one of its kind has been on screen. */
private val NominalRowHeight = 64.dp

/** How much of the screen's height the strip takes, if there's room. */
private const val STRIP_SCREEN_FRACTION = 0.8f

/** How long the strip stays after the list stops scrolling or it's let go. */
private const val HIDE_AFTER_MS = 1500L

/** The strip's height in an area [maxHeight] tall, on a screen [screenHeight] tall. */
private fun stripHeight(maxHeight: Dp, screenHeight: Dp): Dp = min(maxHeight.value, (screenHeight * STRIP_SCREEN_FRACTION).value).dp

/**
 * The strip for [state]'s list, over the list's right edge: [modifier] places it (the parent
 * box's size, inside the bars and mini player, and below a header with buttons in it). Jumping
 * puts the entry's item at the top, [scrollOffset] pixels in (negative to land below a status
 * bar the list scrolls under).
 *
 * There's a place about every screen of the list, or further apart where the strip hasn't room
 * for that at [MinTickHeight] (see [groupEntries]). Screens are measured in pixels: the list's
 * height against its rows' real heights. [kindOf] says which of its content types item i is,
 * and each kind is as tall as the first of it on screen, or until then [estimate]'s pixels if
 * it knows, so the places stay the same wherever the list is. There's no strip while the list
 * is under [MIN_SCREENS] screens long, or has fewer than two places. [minShares] holds
 * sections (by title) to a share of the strip at least (see [placeShares]).
 */
@Composable
fun FastScroller(
    state: LazyListState,
    entries: List<FastScrollEntry>,
    modifier: Modifier = Modifier,
    scrollOffset: Int = 0,
    minShares: Map<String, Float> = emptyMap(),
    kindOf: (Int) -> Any? = { null },
    estimate: (Any?) -> Int? = { null },
) {
    val density = LocalDensity.current
    val rowPx = with(density) { NominalRowHeight.roundToPx() }
    val heights = remember(state) { mutableStateMapOf<Any, Int>() }
    LaunchedEffect(state) {
        snapshotFlow { state.layoutInfo.visibleItemsInfo }.collect { items ->
            for (item in items) {
                val kind = item.contentType ?: NoKind
                if (kind !in heights) heights[kind] = item.size
            }
        }
    }
    val count by remember(state) { derivedStateOf { state.layoutInfo.totalItemsCount } }
    val screen by remember(state) {
        derivedStateOf {
            val info = state.layoutInfo
            info.viewportSize.height - info.beforeContentPadding - info.afterContentPadding
        }
    }
    if (screen <= 0 || entries.isEmpty()) return
    val screenHeight = LocalConfiguration.current.screenHeightDp.dp
    BoxWithConstraints(modifier) {
        val fits = with(density) { (stripHeight(maxHeight, screenHeight) / MinTickHeight).toInt() }.coerceAtLeast(1)
        val known = heights.toMap()
        val offsets = remember(entries, count, known, rowPx) {
            itemOffsets(count) { i ->
                val kind = kindOf(i)
                known[kind ?: NoKind] ?: estimate(kind) ?: rowPx
            }
        }
        val total = offsets.last()
        val places = remember(entries, offsets, screen, fits) { groupEntries(entries, offsets, max(screen, total / fits)) }
        val shares = remember(places, minShares) { placeShares(places, minShares) }
        if (total <= screen * MIN_SCREENS || places.size < 2) return@BoxWithConstraints
        val scope = rememberCoroutineScope()
        FastScroller(
            places,
            current = { state.firstVisibleItemIndex },
            onJump = { index -> scope.launch { state.scrollToItem(index, scrollOffset) } },
            scrolling = state.isScrollInProgress,
            modifier = Modifier.matchParentSize(),
            shares = shares,
        )
    }
}

/** A list item's kind in [FastScroller] when it has no content type. */
private object NoKind

/**
 * The strip for any list: [entries] in list order, [current] the first item on screen, and
 * [onJump] scrolls to an item. The strip is centred down the right edge of [modifier]'s
 * area, [STRIP_SCREEN_FRACTION] of the screen tall or as fits. Each entry takes its [shares] of
 * its height (fractions adding up to 1, see [placeShares]), or all the same; where they're too
 * close together for every label, some marks are dots or go unmarked ([stripMarks]), and
 * dragging still goes through every entry. It fades in while
 * the list is [scrolling] and out [HIDE_AFTER_MS] after, and while hidden doesn't take
 * touches, which go to the list. Touching it jumps to the entry under the finger and follows
 * it, with a haptic tick at each new one. For accessibility it's an adjustable control, an
 * entry a step, there whether shown or not.
 */
@Composable
fun FastScroller(
    entries: List<FastScrollEntry>,
    current: () -> Int,
    onJump: (Int) -> Unit,
    scrolling: Boolean,
    modifier: Modifier = Modifier,
    shares: FloatArray? = null,
) {
    val haptics = LocalHapticFeedback.current
    val density = LocalDensity.current
    // Each entry's share of the strip, and where it starts down it, as fractions of its height.
    val spans = remember(entries, shares) {
        shares?.takeIf { it.size == entries.size } ?: FloatArray(entries.size) { 1f / entries.size }
    }
    val starts = remember(spans) {
        var at = 0f
        FloatArray(spans.size) { i -> at.also { at += spans[i] } }
    }
    val currentEntries by rememberUpdatedState(entries)
    val currentStarts by rememberUpdatedState(starts)
    val jump by rememberUpdatedState(onJump)
    // The entry under the finger while dragging, else -1, and where the finger is in the strip.
    var selected by remember { mutableIntStateOf(-1) }
    var fingerY by remember { mutableFloatStateOf(0f) }
    var shown by remember { mutableStateOf(false) }
    val inUse = scrolling || selected >= 0
    LaunchedEffect(inUse) {
        if (inUse) {
            shown = true
        } else {
            delay(HIDE_AFTER_MS)
            shown = false
        }
    }
    val alpha by animateFloatAsState(if (shown) 1f else 0f, label = "fast scroll")
    val screenHeight = LocalConfiguration.current.screenHeightDp.dp
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
        val stripHeight = stripHeight(maxHeight, screenHeight)
        val minSlot = MinSlotHeight / stripHeight
        val minTick = MinTickHeight / stripHeight
        val marks = remember(entries, starts, spans, minSlot, minTick) { stripMarks(entries, starts, spans, minSlot, minTick) }
        val stripTop = (maxHeight - stripHeight) / 2

        Column(
            Modifier
                .align(Alignment.CenterEnd)
                .width(StripWidth)
                .height(stripHeight)
                .graphicsLayer { this.alpha = alpha }
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
                .then(if (shown) Modifier.pointerInput(Unit) {
                    fun select(y: Float) {
                        val list = currentEntries
                        if (list.isEmpty()) return
                        fingerY = y.coerceIn(0f, size.height.toFloat())
                        // The entry whose part of the strip the finger is in.
                        val at = fingerY / size.height
                        val i = currentStarts.indexOfLast { it <= at }.coerceIn(0, list.lastIndex)
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
                } else Modifier),
        ) {
            for (i in 0 until count) {
                val entry = entries[i]
                // Each entry's part of the strip, as a touch there picks it. A mark may be
                // taller than its part, which the marks' spacing leaves room for.
                Box(Modifier.fillMaxWidth().weight(spans[i]), contentAlignment = Alignment.Center) {
                    val here = i == selected
                    val color = if (here) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant
                    val mark = Modifier.wrapContentHeight(unbounded = true)
                    when {
                        marks[i] == Mark.None -> Unit
                        marks[i] == Mark.Dot -> Box(mark.size(3.dp).background(color, CircleShape))
                        entry.heading -> Box(mark.width(10.dp).height(3.dp).background(color, RoundedCornerShape(2.dp)))
                        else -> Text(
                            entry.label,
                            style = MaterialTheme.typography.labelSmall,
                            fontWeight = if (here) FontWeight.Bold else FontWeight.Medium,
                            color = color,
                            maxLines = 1,
                            modifier = mark,
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
private fun entryText(entry: FastScrollEntry): String {
    val letters = letters(entry)
    return if (entry.heading || entry.section == null) letters
    else stringResource(R.string.fast_scroll_letter_in, letters, entry.section)
}

/** The letter, or run of letters such as "A–C", a place in the strip is. */
@Composable
private fun letters(entry: FastScrollEntry): String =
    entry.through?.let { stringResource(R.string.fast_scroll_letters, entry.label, it) } ?: entry.label

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
                Text(letters(entry), style = MaterialTheme.typography.headlineMedium, maxLines = 1)
                entry.section?.let {
                    Text(it, style = MaterialTheme.typography.labelSmall, maxLines = 1, overflow = TextOverflow.Ellipsis)
                }
            }
        }
    }
}
