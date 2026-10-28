package ng.kithso.android.ui.theme

import androidx.compose.animation.animateColorAsState
import androidx.compose.animation.core.tween
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.lazy.LazyListState
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
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

/**
 * A background of soft colour: four glows, one towards each corner, in reading order, from
 * [colors], spread across them when there are more and repeated in deeper or paler tones when
 * there are fewer. Each is moved to a tone the theme's text reads well on, whatever the art's
 * own, and fades as it changes. Without colours it's clear, for the surface beneath.
 */
@Composable
fun ArtBackdrop(colors: List<Color>, modifier: Modifier = Modifier) {
    val dark = isSystemInDarkTheme()
    val surface = MaterialTheme.colorScheme.surface
    // Read while drawing, so a fade redraws without recomposing.
    val glows = List(Corners.size) { i ->
        val target = if (colors.isEmpty()) surface.copy(alpha = 0f) else {
            val n = colors.size
            if (n > Corners.size) toned(colors[(i * 2 + 1) * n / (Corners.size * 2)], dark, 0)
            else toned(colors[i % n], dark, i / n)
        }
        animateColorAsState(target, tween(FadeMs), label = "glow")
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

/** [art]'s colours, most prominent first, for [ArtBackdrop]; none until they've been found. */
@Composable
fun rememberArtColors(art: ArtRef?): List<Color> {
    val context = LocalContext.current
    val colors by produceState(art?.let { ArtColors.cached(it) }.orEmpty(), art) {
        value = art?.let { ArtColors.find(context, it) }.orEmpty()
    }
    return colors
}

/**
 * Colours for [ArtBackdrop] behind a list: the most prominent colour of the art of each item
 * on screen, in order, so they change as it scrolls. [artAt] gives the art of a list item.
 * Art already looked at counts at once; the rest is added as it's found.
 */
@Composable
fun rememberOnScreenArtColors(state: LazyListState, artAt: (Int) -> List<ArtRef>): List<Color> {
    val context = LocalContext.current
    val currentArtAt by rememberUpdatedState(artAt)
    var colors by remember { mutableStateOf(emptyList<Color>()) }
    LaunchedEffect(state) {
        snapshotFlow { state.layoutInfo.visibleItemsInfo.flatMap { currentArtAt(it.index) } }
            .distinctUntilChanged()
            .collectLatest { arts ->
                val found = arts.mapTo(ArrayList(arts.size)) { ArtColors.cached(it) }
                colors = found.mapNotNull { it?.firstOrNull() }
                for (i in arts.indices) {
                    if (found[i] != null) continue
                    found[i] = ArtColors.find(context, arts[i])
                    colors = found.mapNotNull { it?.firstOrNull() }
                }
            }
    }
    return colors
}

/**
 * [color] at the tone the background uses, its chroma tamed so it stays a tint. [repeat]
 * counts how many times it's been used already, each a little further from the text's tone.
 */
private fun toned(color: Color, dark: Boolean, repeat: Int): Color {
    val hct = Hct.fromInt(color.toArgb())
    val tone = if (dark) DarkTone - repeat * 5.0 else LightTone + repeat * 3.0
    return Color(Hct.from(hct.hue, minOf(hct.chroma, MaxChroma), tone).toInt())
}

// Where the glows centre, as fractions of the width and height: top left, top right, bottom
// left, bottom right, each pulled in a little so its brightest part is on screen.
private val Corners = listOf(Offset(0.1f, 0.05f), Offset(0.9f, 0.2f), Offset(0.15f, 0.75f), Offset(0.9f, 0.95f))

// Tones that onSurfaceVariant still reads on at well over 4.5:1, before the glows thin out
// towards the surface.
private const val DarkTone = 30.0
private const val LightTone = 86.0
private const val MaxChroma = 48.0
private const val Strength = 0.8f
private const val FadeMs = 700
