package ng.kithso.android.ui.theme

import android.content.Context
import android.util.LruCache
import androidx.compose.foundation.isSystemInDarkTheme
import androidx.compose.material3.MaterialTheme
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.platform.LocalContext
import ng.kithso.android.data.ArtRef
import ng.kithso.android.ui.art.loadArtBitmap
import com.materialkolor.PaletteStyle
import com.materialkolor.dynamicColorScheme
import com.materialkolor.ktx.animateColorScheme
import com.materialkolor.ktx.quantize
import com.materialkolor.quantize.QuantizerCelebi
import com.materialkolor.score.Score
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

/**
 * Colours [content] from [art]: a Material colour scheme from the art's most prominent colour,
 * found as Android finds a wallpaper's, in the Content style, which stays close to that colour.
 * It follows dark mode, and animates when the art changes. Until the art has been looked at,
 * and for art without a usable colour, the colours are the app's own.
 */
@Composable
fun ArtTheme(art: ArtRef, content: @Composable () -> Unit) {
    val context = LocalContext.current
    val seed by produceState(ArtColors.cached(art)?.firstOrNull(), art) { value = ArtColors.find(context, art).firstOrNull() }
    val dark = isSystemInDarkTheme()
    val base = MaterialTheme.colorScheme
    val scheme = remember(seed, dark, base) {
        seed?.let { dynamicColorScheme(seedColor = it, isDark = dark, style = PaletteStyle.Content) } ?: base
    }
    MaterialTheme(colorScheme = animateColorScheme(scheme), content = content)
}

/**
 * The colours of art, most prominent first, remembered, since the players, track lists and
 * the main list's background ask for the same art. The first is the art's seed colour.
 */
internal object ArtColors {
    // Pixels to look at: about what Android scales a wallpaper down to for the same job.
    private const val PX = 128
    // As many as a background uses.
    private const val COUNT = 4
    // Enough for a screenful of the main list and a few scrolls of it either way.
    private val cache = LruCache<String, List<Color>>(512)

    private val ArtRef.key get() = if (file != null) "$file:$version" else "art:$uid:$label"

    /** [art]'s colours if they have been found, an empty list for art without any. */
    fun cached(art: ArtRef): List<Color>? = cache[art.key]

    suspend fun find(context: Context, art: ArtRef): List<Color> {
        cache[art.key]?.let { return it }
        val bitmap = loadArtBitmap(context, art, PX) ?: return emptyList()
        // Scored as materialkolor's themeColor does, so the first is the same seed. Kept as
        // soon as they're found, in case the list has scrolled on and the caller has gone.
        return withContext(Dispatchers.Default) {
            val quantized = QuantizerCelebi.quantize(bitmap.asImageBitmap(), 128)
            Score.score(quantized, COUNT, null, true).map(::Color).also { cache.put(art.key, it) }
        }
    }
}
