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
import com.materialkolor.ktx.themeColorOrNull
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
    val seed by produceState(ArtSeeds.cached(art), art) { value = ArtSeeds.find(context, art) }
    val dark = isSystemInDarkTheme()
    val base = MaterialTheme.colorScheme
    val scheme = remember(seed, dark, base) {
        seed?.let { dynamicColorScheme(seedColor = it, isDark = dark, style = PaletteStyle.Content) } ?: base
    }
    MaterialTheme(colorScheme = animateColorScheme(scheme), content = content)
}

/** Seed colours of art, remembered, since the mini and full player ask for the same art. */
private object ArtSeeds {
    // Pixels to look at: about what Android scales a wallpaper down to for the same job.
    private const val PX = 128
    private val NONE = Color.Unspecified
    private val cache = LruCache<String, Color>(64)

    private val ArtRef.key get() = if (file != null) "$file:$version" else "art:$uid"

    fun cached(art: ArtRef): Color? = cache[art.key]?.takeIf { it != NONE }

    suspend fun find(context: Context, art: ArtRef): Color? {
        cache[art.key]?.let { return it.takeIf { it != NONE } }
        val bitmap = loadArtBitmap(context, art, PX) ?: return null
        val seed = withContext(Dispatchers.Default) { bitmap.asImageBitmap().themeColorOrNull() }
        cache.put(art.key, seed ?: NONE)
        return seed
    }
}
