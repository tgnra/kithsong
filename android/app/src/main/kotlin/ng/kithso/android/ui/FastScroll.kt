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
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
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
import androidx.compose.ui.unit.IntOffset
import androidx.compose.ui.unit.dp
import ng.kithso.android.R
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import java.text.Normalizer
import kotlin.math.min
import kotlin.math.roundToInt

// A fast scroll strip down the right edge of a long list: letters, and markers for the
// list's section headings, to tap or drag along to jump there, with a bubble beside the
// finger saying where it is. It shows while the list scrolls or it's touched.

/**
 * A place in the list the strip can jump to: list item [index], shown as [label] (a letter,
 * or a heading's title). [section] is the heading a letter is under, if the list has them,
 * for the bubble; a [heading] is drawn as a mark in the strip, with its title in the bubble.
 * [through] is the last letter of a run the strip groups into this one place (see
 * [groupEntries]).
 */
@Immutable
data class FastScrollEntry(
    val label: String,
    val index: Int,
    val section: String? = null,
    val heading: Boolean = false,
    val through: String? = null,
)

/**
 * [entries] grouped so each place in the strip is about a screen of the list, [perScreen]
 * items: a run of letters closer together than that is one place, at the first, through the
 * last. A heading always starts a place, taking the letters just under it; a letter in another
 * section never joins one before it.
 */
internal fun groupEntries(entries: List<FastScrollEntry>, perScreen: Int): List<FastScrollEntry> {
    val groups = ArrayList<FastScrollEntry>()
    var start: FastScrollEntry? = null
    var last: FastScrollEntry? = null
    fun close() {
        val first = start ?: return
        groups += if (!first.heading && last !== first) first.copy(through = last!!.label) else first
    }
    for (entry in entries) {
        val first = start
        if (first == null || entry.heading || entry.section != first.section || entry.index - first.index >= perScreen) {
            close()
            start = entry
        }
        last = entry
    }
    close()
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
 * of it it [spans], as fractions of its height; [minSlot] is [MinSlotHeight] as one. A mark is
 * drawn only [minSlot] or more below the last; where the entries are closer together than that,
 * every other one drawn is a dot, though a heading keeps its mark.
 */
internal fun stripMarks(entries: List<FastScrollEntry>, starts: FloatArray, spans: FloatArray, minSlot: Float): Array<Mark> {
    var last = Float.NEGATIVE_INFINITY
    var dotNext = false
    return Array(entries.size) { i ->
        val centre = starts[i] + spans[i] / 2
        if (centre - last < minSlot) return@Array Mark.None
        val crowded = spans[i] < minSlot
        val mark = if (crowded && dotNext && !entries[i].heading) Mark.Dot else Mark.Label
        dotNext = crowded && mark == Mark.Label
        last = centre
        mark
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
private val BubbleHeight = 64.dp

/** A typical list row's height, for how many items make a screen. */
private val NominalRowHeight = 64.dp

/** How much of the screen's height the strip takes, if there's room. */
private const val STRIP_SCREEN_FRACTION = 0.8f

/** How long the strip stays after the list stops scrolling or it's let go. */
private const val HIDE_AFTER_MS = 1500L

/**
 * The strip for [state]'s list, over the list's right edge: [modifier] places it (the parent
 * box's size, inside the bars and mini player). Jumping puts the entry's item at the top,
 * [scrollOffset] pixels in (negative to land below a status bar the list scrolls under).
 * [entries] are grouped about a screen to a place (see [groupEntries]), a screen being the
 * list's height in [NominalRowHeight] rows, so the places don't change as it scrolls. There's
 * none while the list is under [MIN_SCREENS] screens long, or has fewer than two places.
 * [minShares] holds sections (by title) to a share of the strip at least (see [placeShares]).
 */
@Composable
fun FastScroller(
    state: LazyListState,
    entries: List<FastScrollEntry>,
    modifier: Modifier = Modifier,
    scrollOffset: Int = 0,
    minShares: Map<String, Float> = emptyMap(),
) {
    // A screen's worth of items, from the list's height and a typical row's rather than the
    // rows on screen, so the places stay the same wherever the list is.
    val rowPx = with(LocalDensity.current) { NominalRowHeight.toPx() }
    val perScreen by remember(state, rowPx) {
        derivedStateOf { (state.layoutInfo.viewportSize.height / rowPx).toInt().coerceAtLeast(1) }
    }
    val long by remember(state) {
        derivedStateOf { state.layoutInfo.totalItemsCount > perScreen * MIN_SCREENS }
    }
    val places = remember(entries, perScreen) { groupEntries(entries, perScreen) }
    val shares = remember(places, minShares) { placeShares(places, minShares) }
    if (!long || places.size < 2) return
    val scope = rememberCoroutineScope()
    FastScroller(
        places,
        current = { state.firstVisibleItemIndex },
        onJump = { index -> scope.launch { state.scrollToItem(index, scrollOffset) } },
        scrolling = state.isScrollInProgress,
        modifier = modifier,
        shares = shares,
    )
}

/**
 * The strip for any list: [entries] in list order, [current] the first item on screen, and
 * [onJump] scrolls to an item. The strip is centred down the right edge of [modifier]'s
 * area, [STRIP_SCREEN_FRACTION] of the screen tall or as fits. Each entry takes its [shares] of
 * its height (fractions adding up to 1, see [placeShares]), or all the same; where they're too
 * close together for every label, every other mark is a dot or some go unmarked ([stripMarks]),
 * and dragging still goes through every entry. It fades in while
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
        val stripHeight = min(maxHeight.value, (screenHeight * STRIP_SCREEN_FRACTION).value).dp
        val minSlot = MinSlotHeight / stripHeight
        val marks = remember(entries, starts, spans, minSlot) { stripMarks(entries, starts, spans, minSlot) }
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
