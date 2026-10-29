package ng.kithso.android.ui.theme

import android.content.Context
import android.util.LruCache
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Immutable
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.toArgb
import ng.kithso.android.data.ArtRef
import ng.kithso.android.ui.art.loadArtBitmap
import com.materialkolor.PaletteStyle
import com.materialkolor.dynamicColorScheme
import com.materialkolor.hct.Hct
import com.materialkolor.ktx.animateColorScheme
import com.materialkolor.ktx.quantize
import com.materialkolor.quantize.QuantizerCelebi
import com.materialkolor.score.Score
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlin.math.abs

/**
 * Colours [content] from [art]: a Material colour scheme from the art's most prominent colour,
 * found as Android finds a wallpaper's, in the Content style, which stays close to that colour.
 * It follows dark mode, and animates when the art changes, but starts in the art's colours
 * when they've been found before. Until the art has been looked at, and for art without a
 * usable colour, the colours are the app's own.
 */
@Composable
fun ArtTheme(art: ArtRef, content: @Composable () -> Unit) {
    val seed = rememberArtColors(art).firstOrNull()?.color
    val dark = isSystemInDarkTheme()
    val base = MaterialTheme.colorScheme
    val scheme = remember(seed, dark, base) {
        seed?.let { dynamicColorScheme(seedColor = it, isDark = dark, style = PaletteStyle.Content) } ?: base
    }
    MaterialTheme(colorScheme = animateColorScheme(scheme), content = content)
}

/**
 * A colour of some art, and its [share] of the art: how much of it, from 0 to 1, is near the
 * colour's hue, or, for a [neutral] one, has hardly any colour at all.
 */
@Immutable
data class ArtColor(val color: Color, val share: Float) {
    val neutral get() = Hct.fromInt(color.toArgb()).chroma < NeutralChroma
}

/**
 * The colours of art, most prominent first, remembered, since the players, track lists and
 * the main list's background ask for the same art. The first is the art's seed colour. After
 * them, when much of the art is grey, black or white, comes one [ArtColor.neutral] colour for
 * that, so a background can be as quiet as the art.
 */
internal object ArtColors {
    // Pixels to look at: about what Android scales a wallpaper down to for the same job.
    private const val PX = 128
    // As many as a background uses.
    private const val COUNT = 4
    // How near in hue, in degrees, a pixel's colour is to count towards a colour's share.
    private const val NEAR_HUE = 30.0
    // How much of the art has to be grey for a neutral colour to be given.
    private const val NEUTRAL_SHARE = 0.2f
    // Enough for a screenful of the main list and a few scrolls of it either way.
    private val cache = LruCache<String, List<ArtColor>>(512)

    private val ArtRef.key get() = if (file != null) "$file:$version" else "art:$uid:$label"

    /** [art]'s colours if they have been found, an empty list for art without any. */
    fun cached(art: ArtRef): List<ArtColor>? = cache[art.key]

    suspend fun find(context: Context, art: ArtRef): List<ArtColor> {
        cache[art.key]?.let { return it }
        val bitmap = loadArtBitmap(context, art, PX) ?: return emptyList()
        // Scored as materialkolor's themeColor does, so the first is the same seed. Kept as
        // soon as they're found, in case the list has scrolled on and the caller has gone.
        return withContext(Dispatchers.Default) {
            val quantized = QuantizerCelebi.quantize(bitmap.asImageBitmap(), 128)
            withShares(quantized, Score.score(quantized, COUNT, null, true)).also { cache.put(art.key, it) }
        }
    }

    /**
     * [scored], from [quantized], with their shares: each of the art's colours counts towards
     * the scored one nearest its hue, if one is near enough, and grey ones towards a neutral
     * colour, given last if there are enough of them. Art with no colour but grey has none.
     */
    private fun withShares(quantized: Map<Int, Int>, scored: List<Int>): List<ArtColor> {
        if (scored.isEmpty()) return emptyList()
        val hues = scored.map { Hct.fromInt(it).hue }
        val counts = IntArray(scored.size)
        var neutralCount = 0
        var neutral: Int? = null
        var neutralMost = 0
        for ((argb, count) in quantized) {
            val hct = Hct.fromInt(argb)
            if (hct.chroma < NeutralChroma) {
                neutralCount += count
                if (count > neutralMost) { neutral = argb; neutralMost = count }
                continue
            }
            val nearest = hues.indices.minBy { hueDistance(hues[it], hct.hue) }
            if (hueDistance(hues[nearest], hct.hue) <= NEAR_HUE) counts[nearest] += count
        }
        val total = quantized.values.sum().coerceAtLeast(1).toFloat()
        val colors = scored.mapIndexed { i, argb -> ArtColor(Color(argb), counts[i] / total) }
        val neutralShare = neutralCount / total
        return if (neutral != null && neutralShare >= NEUTRAL_SHARE) colors + ArtColor(Color(neutral), neutralShare) else colors
    }
}

/** How far apart two hues are, in degrees, the short way round. */
internal fun hueDistance(a: Double, b: Double): Double = 180.0 - abs(abs(a - b) - 180.0)

// Chroma below which a colour reads as grey rather than as a hue: the same cut-off Score uses
// for a colour to count at all, raised a little, since art's darks and lights are often barely
// tinted.
internal const val NeutralChroma = 8.0
