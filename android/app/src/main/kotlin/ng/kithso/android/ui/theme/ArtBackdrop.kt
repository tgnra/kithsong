package ng.kithso.android.ui.theme

import android.os.SystemClock
import androidx.compose.animation.Animatable
import androidx.compose.animation.core.tween
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.platform.LocalContext
import ng.kithso.android.data.ArtRef
import com.materialkolor.hct.Hct
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlin.math.pow

/**
 * A background of soft colour: four glows, one towards each corner, from [colors], shared out
 * by how much of the art each is (see [glowColors]), so one loud colour doesn't take them all.
 * Each is moved to a tone the theme's text reads well on, whatever the art's own. Without
 * colours it's clear, for the surface beneath.
 *
 * It starts in its colours, so a screen sliding in arrives coloured, and fades as they change
 * after that. Colours that come just after it's shown, while its screen is still mostly off
 * screen, are taken at once too; any later than that, the first fade in quickly from clear.
 */
@Composable
fun ArtBackdrop(colors: List<ArtColor>, modifier: Modifier = Modifier) {
    val dark = isSystemInDarkTheme()
    val surface = MaterialTheme.colorScheme.surface
    val shownAt = remember { SystemClock.uptimeMillis() }
    val targets = remember(colors, dark) { glowColors(colors, dark) }
    // Read while drawing, so a fade redraws without recomposing.
    val glows = List(Corners.size) { i ->
        val target = targets.getOrNull(i) ?: surface.copy(alpha = 0f)
        val glow = remember { Animatable(target) }
        // Whether it has had a colour yet: until then, one arrives rather than changes.
        var lit by remember { mutableStateOf(target.alpha != 0f) }
        LaunchedEffect(target) {
            if (glow.targetValue == target) return@LaunchedEffect
            val arriving = !lit
            if (target.alpha != 0f) lit = true
            when {
                !arriving -> glow.animateTo(target, tween(FadeMs))
                SystemClock.uptimeMillis() - shownAt < SnapMs -> glow.snapTo(target)
                else -> glow.animateTo(target, tween(ArriveMs))
            }
        }
        glow
    }
    Box(
        modifier.drawBehind {
            val radius = size.maxDimension * 0.75f
            glows.forEachIndexed { i, glow ->
                val color = glow.value
                if (color.alpha == 0f) return@forEachIndexed
                val corner = Corners[i]
                drawRect(
                    Brush.radialGradient(
                        listOf(color.copy(alpha = color.alpha * Strength), color.copy(alpha = 0f)),
                        center = Offset(corner.x * size.width, corner.y * size.height),
                        radius = radius,
                    ),
                )
            }
        },
    )
}

/**
 * [art]'s colours, most prominent first, for [ArtBackdrop] and [ArtTheme]: at once when
 * they've been found before, as they usually have by the time a screen opens, since the main
 * list finds them for the art on screen and opening a list finds them for its art. Otherwise
 * none until they've been found, or, when [art] changes, the last art's until then.
 */
@Composable
fun rememberArtColors(art: ArtRef?): List<ArtColor> {
    val context = LocalContext.current
    // Read while composing, so a screen's first frame has them.
    val cached = remember(art) { art?.let { ArtColors.cached(it) } }
    var found by remember { mutableStateOf(cached.orEmpty()) }
    LaunchedEffect(art) { found = art?.let { ArtColors.find(context, it) }.orEmpty() }
    return cached ?: found
}

/**
 * Colours for [ArtBackdrop] behind a list: the colours of the art of each item on screen, in
 * order, so they change as it scrolls. Each art counts the same, its colours by their shares
 * of it, so a screenful of art in much the same colour still lets its other colours through.
 * [artAt] gives the art of a list item. Art already looked at counts at once; the rest is
 * added as it's found.
 */
@Composable
fun rememberOnScreenArtColors(state: LazyListState, artAt: (Int) -> List<ArtRef>): List<ArtColor> {
    val context = LocalContext.current
    val currentArtAt by rememberUpdatedState(artAt)
    var colors by remember { mutableStateOf(emptyList<ArtColor>()) }
    LaunchedEffect(state) {
        snapshotFlow { state.layoutInfo.visibleItemsInfo.flatMap { currentArtAt(it.index) } }
            .distinctUntilChanged()
            .collectLatest { arts ->
                val found = arts.mapTo(ArrayList(arts.size)) { ArtColors.cached(it) }
                colors = found.flatMap { it.orEmpty() }
                for (i in arts.indices) {
                    if (found[i] != null) continue
                    found[i] = ArtColors.find(context, arts[i])
                    colors = found.flatMap { it.orEmpty() }
                }
            }
    }
    return colors
}

/**
 * The glows' colours for [colors], toned, in reading order: one per corner, or none without
 * colours.
 *
 * Colours of near hues count as one, their shares added, and so do all the neutral ones. Each
 * gets corners by its share of the whole, but at most [MaxCorners], so one colour can't fill
 * the background while the art has others, and one too small a part of it gets none; grey
 * gets one at most, when it's most of the art. Corners left over go to the biggest hue
 * again, quieter each time. A colour's corners are across from each other where they can
 * be, and the colours take theirs in the order they first come in [colors], so a list's top
 * colours go to the top.
 */
private fun glowColors(colors: List<ArtColor>, dark: Boolean): List<Color> {
    if (colors.isEmpty()) return emptyList()
    class Group(val color: Color, val hue: Double, val neutral: Boolean) {
        var share = 0f
        var corners = 0
    }
    val groups = ArrayList<Group>()
    for (color in colors) {
        val hue = Hct.fromInt(color.color.toArgb()).hue
        val neutral = color.neutral
        val group = groups.firstOrNull { it.neutral == neutral && (neutral || hueDistance(it.hue, hue) < SameHue) }
            ?: Group(color.color, hue, neutral).also { groups += it }
        group.share += color.share
    }
    // Art whose colours are all too faint to have shares still has colours.
    if (groups.all { it.share == 0f }) groups.forEach { it.share = 1f }
    val whole = groups.sumOf { it.share.toDouble() }.toFloat()
    // Each corner to the colour furthest short of its share of them, if one is; otherwise to
    // the biggest, a hue rather than grey if there's one. Grey only gets a corner, and only
    // one, when there's more of it than of any hue, since it adds no colour.
    val biggest = groups.maxBy { it.share }
    val owed = { group: Group -> group.share / whole * Corners.size - group.corners }
    val mayHave = { group: Group ->
        when {
            group.neutral -> group === biggest && group.corners == 0
            group.corners == 0 -> group.share / whole >= MinShare
            else -> group.corners < MaxCorners && owed(group) > 0f
        }
    }
    repeat(Corners.size) {
        val group = groups.filter(mayHave).maxByOrNull(owed)
            ?: groups.maxWith(compareBy<Group> { !it.neutral }.thenBy { it.share })
        group.corners++
    }
    val glows = arrayOfNulls<Color>(Corners.size)
    for (group in groups) {
        var first = -1
        repeat(group.corners) { n ->
            val across = if (first < 0) -1 else Corners.lastIndex - first
            val corner = if (across >= 0 && glows[across] == null) across else glows.indexOfFirst { it == null }
            if (first < 0) first = corner
            glows[corner] = toned(group.color, dark, n)
        }
    }
    return glows.map { it!! }
}

/**
 * [color] at the tone the background uses, its chroma tamed so it stays a tint. [repeat]
 * counts how many times it's been used already: each time it's a little further from the
 * text's tone and quieter, so a colour that has corners to itself doesn't shout.
 */
private fun toned(color: Color, dark: Boolean, repeat: Int): Color {
    val hct = Hct.fromInt(color.toArgb())
    val tone = if (dark) DarkTone - repeat * 5.0 else LightTone + repeat * 3.0
    val chroma = minOf(hct.chroma, MaxChroma) * RepeatChroma.pow(repeat)
    return Color(Hct.from(hct.hue, chroma, tone).toInt())
}

// Where the glows centre, as fractions of the width and height: top left, top right, bottom
// left, bottom right, each pulled in a little so its brightest part is on screen.
private val Corners = listOf(Offset(0.1f, 0.05f), Offset(0.9f, 0.2f), Offset(0.15f, 0.75f), Offset(0.9f, 0.95f))

// Tones that onSurfaceVariant still reads on at well over 4.5:1, before the glows thin out
// towards the surface.
private const val DarkTone = 30.0
private const val LightTone = 86.0
// Art's loudest colours still shout at those tones above this.
private const val MaxChroma = 40.0
// How much of its chroma a colour keeps each time it's used again.
private const val RepeatChroma = 0.6
// Hues closer than this, in degrees, count as one colour.
private const val SameHue = 25.0
// The most corners one colour gets while there are others, and the share of all the colours
// one needs to get a corner at all.
private const val MaxCorners = 2
private const val MinShare = 0.05f
private const val Strength = 0.8f
private const val FadeMs = 700
// Colours that come this soon after the backdrop is shown are taken at once; later ones from
// clear fade in over ArriveMs.
private const val SnapMs = 150L
private const val ArriveMs = 250
